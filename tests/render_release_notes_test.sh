#!/usr/bin/env bash
set -euo pipefail

export GIT_CONFIG_GLOBAL=/dev/null
export GIT_CONFIG_SYSTEM=/dev/null

repo_root="$(cd -- "$(dirname -- "$0")/.." && pwd -P)"
renderer="$repo_root/tools/ci/render-release-notes.sh"
tmp_root="$(mktemp -d "${TMPDIR:-/tmp}/render-release-notes.XXXXXX")"
trap 'rm -rf "$tmp_root"' EXIT

fail() {
  printf 'FATAL: %s\n' "$*" >&2
  exit 1
}

assert_eq() {
  local label="$1" expected="$2" actual="$3"
  [ "$actual" = "$expected" ] || {
    printf 'FATAL: %s\nexpected:\n%s\nactual:\n%s\n' "$label" "$expected" "$actual" >&2
    exit 1
  }
}

new_repo() {
  local name="$1" repo
  repo="$tmp_root/$name"
  git init -q -b main "$repo"
  git -C "$repo" config user.name "Release Test"
  git -C "$repo" config user.email "release-test@example.invalid"
  printf '%s\n' "$repo"
}

commit_subject() {
  local repo="$1" subject="$2" count
  count="$(git -C "$repo" rev-list --all --count 2>/dev/null || printf '0')"
  printf '%s\n' "$subject" > "$repo/commit-${count}.txt"
  git -C "$repo" add .
  git -C "$repo" commit -q -m "$subject"
}

annotated_tag() {
  git -C "$1" tag -a "$2" -m "$2"
}

render() {
  local repo="$1" tag="$2" delimiter="${3:-EOF_RELEASE_NOTES_CHANGES}"
  (cd "$repo" && "$renderer" "$tag" "$delimiter")
}

[ -x "$renderer" ] || fail "renderer is not executable: $renderer"
grep -Fq 'git rev-parse --verify "${release_tag}^{commit}"' "$renderer" || \
  fail "renderer missing annotated-tag commit dereference"
grep -Fq 'git tag --merged "${release_tag}^{commit}" --sort=version:refname' "$renderer" || \
  fail "renderer missing reachable version-sorted tag selection"
grep -Fq 'if [ "$candidate" = "$release_tag" ]; then' "$renderer" || \
  fail "renderer missing strict lower-than-release boundary"
grep -Fq '[ "$release_tag_seen" -eq 1 ]' "$renderer" || \
  fail "renderer missing release-tag membership assertion"
grep -Fq '"${previous_tag}^{commit}..${release_tag}^{commit}" --)' "$renderer" || \
  fail "renderer missing revision separator"
if grep -Fq 'git describe' "$renderer"; then
  fail "renderer uses forbidden git describe predecessor selection"
fi

normal="$(new_repo normal)"
commit_subject "$normal" "baseline"
annotated_tag "$normal" 2026.01.01-r1
commit_subject "$normal" "alpha: preserve [punctuation]"
git -C "$normal" tag non-calver-noise
commit_subject "$normal" "beta subject"
annotated_tag "$normal" 2026.01.02-r1
assert_eq normal-range $'Changes since 2026.01.01-r1:\n- alpha: preserve [punctuation]\n- beta subject' \
  "$(render "$normal" 2026.01.02-r1)"

same_commit="$(new_repo same-commit)"
commit_subject "$same_commit" "baseline only"
annotated_tag "$same_commit" 2026.02.01-r1
annotated_tag "$same_commit" 2026.02.01-r2
assert_eq same-commit-r2 $'Changes since 2026.02.01-r1:\n- No source changes; release revision only.' \
  "$(render "$same_commit" 2026.02.01-r2)"

r10="$(new_repo r10)"
commit_subject "$r10" "r9 baseline"
annotated_tag "$r10" 2026.03.01-r9
commit_subject "$r10" "r10 change"
annotated_tag "$r10" 2026.03.01-r10
assert_eq r9-r10 $'Changes since 2026.03.01-r9:\n- r10 change' \
  "$(render "$r10" 2026.03.01-r10)"

initial="$(new_repo initial)"
commit_subject "$initial" "repository history"
git -C "$initial" tag v1.0.0
annotated_tag "$initial" 2026.04.01-r1
assert_eq initial-release $'Changes:\n- Initial CalVer release.' \
  "$(render "$initial" 2026.04.01-r1)"

reachable="$(new_repo reachable)"
commit_subject "$reachable" "reachable baseline"
annotated_tag "$reachable" 2026.05.01-r1
git -C "$reachable" checkout -q -b unreachable
commit_subject "$reachable" "unreachable subject"
annotated_tag "$reachable" 2099.12.31-r9
git -C "$reachable" checkout -q main
commit_subject "$reachable" "reachable subject"
annotated_tag "$reachable" 2026.05.02-r1
assert_eq reachable-predecessor $'Changes since 2026.05.01-r1:\n- reachable subject' \
  "$(render "$reachable" 2026.05.02-r1)"

reachable_higher="$(new_repo reachable-higher)"
commit_subject "$reachable_higher" "lower baseline"
annotated_tag "$reachable_higher" 2026.05.10-r1
commit_subject "$reachable_higher" "higher-version ancestor"
annotated_tag "$reachable_higher" 2099.12.31-r9
commit_subject "$reachable_higher" "backdated release change"
annotated_tag "$reachable_higher" 2026.05.11-r1
assert_eq reachable-higher-predecessor $'Changes since 2026.05.10-r1:\n- higher-version ancestor\n- backdated release change' \
  "$(render "$reachable_higher" 2026.05.11-r1)"

merge_repo="$(new_repo merge)"
commit_subject "$merge_repo" "merge baseline"
annotated_tag "$merge_repo" 2026.06.01-r1
git -C "$merge_repo" checkout -q -b topic
commit_subject "$merge_repo" "topic subject"
git -C "$merge_repo" checkout -q main
commit_subject "$merge_repo" "main subject"
git -C "$merge_repo" merge -q --no-ff topic -m "merge subject"
annotated_tag "$merge_repo" 2026.06.02-r1
merge_output="$(render "$merge_repo" 2026.06.02-r1)"
printf '%s\n' "$merge_output" | grep -Fq -- '- topic subject' || fail "topic subject missing"
printf '%s\n' "$merge_output" | grep -Fq -- '- main subject' || fail "main subject missing"
if printf '%s\n' "$merge_output" | grep -Fq -- '- merge subject'; then
  fail "merge subject was rendered"
fi

set +e
malformed_output="$(render "$normal" v2026.01.02 2>&1)"
malformed_rc=$?
set -e
[ "$malformed_rc" -ne 0 ] || fail "malformed tag was accepted"
printf '%s\n' "$malformed_output" | grep -Fq 'release tag does not match YYYY.MM.DD-rN' || \
  fail "malformed tag failure was not specific"

collision="$(new_repo collision)"
commit_subject "$collision" "collision baseline"
annotated_tag "$collision" 2026.07.01-r1
commit_subject "$collision" "collision"
annotated_tag "$collision" 2026.07.02-r1
set +e
collision_output="$(render "$collision" 2026.07.02-r1 '- collision' 2>&1)"
collision_rc=$?
set -e
[ "$collision_rc" -ne 0 ] || fail "rendered delimiter collision was accepted"
printf '%s\n' "$collision_output" | grep -Fq 'rendered release-note line equals GitHub output delimiter' || \
  fail "delimiter collision failure was not specific"

printf 'release notes renderer tests passed\n'
