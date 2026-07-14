#!/usr/bin/env bash
set -euo pipefail

fail() {
  printf 'FATAL: %s\n' "$*" >&2
  exit 1
}

[ "$#" -eq 2 ] || fail "usage: $0 <calver-tag> <github-output-delimiter>"
release_tag="$1"
delimiter="$2"
calver_re='^[0-9]{4}\.[0-9]{2}\.[0-9]{2}-r[1-9][0-9]*$'

[[ "$release_tag" =~ $calver_re ]] || fail "release tag does not match YYYY.MM.DD-rN: $release_tag"
[ -n "$delimiter" ] || fail "GitHub output delimiter must not be empty"
[[ "$delimiter" != *$'\n'* ]] || fail "GitHub output delimiter must be one line"

git rev-parse --verify "${release_tag}^{commit}" >/dev/null || \
  fail "release tag does not resolve to a commit: $release_tag"

previous_tag=''
release_tag_seen=0
while IFS= read -r candidate; do
  [[ "$candidate" =~ $calver_re ]] || continue
  if [ "$candidate" = "$release_tag" ]; then
    release_tag_seen=1
    break
  fi
  previous_tag="$candidate"
done < <(git tag --merged "${release_tag}^{commit}" --sort=version:refname)
[ "$release_tag_seen" -eq 1 ] || \
  fail "release tag is not present in reachable CalVer tag set: $release_tag"

if [ -z "$previous_tag" ]; then
  rendered=$'Changes:\n- Initial CalVer release.'
else
  subjects="$(git log --no-merges --reverse --pretty=format:'- %s' \
    "${previous_tag}^{commit}..${release_tag}^{commit}" --)"
  if [ -z "$subjects" ]; then
    rendered="Changes since ${previous_tag}:"$'\n- No source changes; release revision only.'
  else
    rendered="Changes since ${previous_tag}:"$'\n'"${subjects}"
  fi
fi

while IFS= read -r line || [ -n "$line" ]; do
  [ "$line" != "$delimiter" ] || \
    fail "rendered release-note line equals GitHub output delimiter: $delimiter"
done <<< "$rendered"

printf '%s\n' "$rendered"
