#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "$0")/.." && pwd -P)"
tmp_root="$(mktemp -d "${TMPDIR:-/tmp}/check-multi-version-negative.XXXXXX")"
trap 'rm -rf "$tmp_root"' EXIT

fail() {
  printf 'FATAL: %s\n' "$*" >&2
  exit 1
}

stage_scratch() {
  local scratch="$1"
  mkdir -p "$scratch/tests" "$scratch/pins" "$scratch/.github/workflows" "$scratch/tools/ci"
  cp "$repo_root/tests/check_multi_version_pin_alignment.sh" "$scratch/tests/"
  cp "$repo_root/pins/runtime-support.tsv" "$scratch/pins/"
  cp "$repo_root/pins/library-compat-groups.tsv" "$scratch/pins/"
  cp "$repo_root/pins/runtime-baselines.tsv" "$scratch/pins/"
  cp "$repo_root/pins/emby-detector-evidence.tsv" "$scratch/pins/"
  cp "$repo_root/pins/plex-pool-patch-sites.tsv" "$scratch/pins/"
  cp "$repo_root/pins/plex-pool-patch-reviews.tsv" "$scratch/pins/"
  cp "$repo_root/.github/workflows/sqlite-build.yml" "$scratch/.github/workflows/"
  cp "$repo_root/tools/ci/mod-bake-smoke.sh" "$scratch/tools/ci/"
}

mutate_emby_493_to_lscr() {
  local scratch="$1" status="$2"
  python3 - "$scratch" "$status" <<'PY'
from pathlib import Path
import sys

root = Path(sys.argv[1])
status = sys.argv[2]

support = root / "pins/runtime-support.tsv"
rows = []
for raw in support.read_text().splitlines():
    cols = raw.split("\t")
    if len(cols) == 7 and cols[1] == "emby-4.9.3":
        cols[2] = cols[2].replace("ghcr.io/", "lscr.io/", 1)
        cols[4] = status
    rows.append("\t".join(cols))
support.write_text("\n".join(rows) + "\n")

baselines = root / "pins/runtime-baselines.tsv"
rows = []
for raw in baselines.read_text().splitlines():
    cols = raw.split("\t")
    if len(cols) == 12 and cols[0] == "pre" and cols[2] == "emby-4.9.3":
        cols[5] = cols[5].replace("ghcr.io/", "lscr.io/", 1)
    rows.append("\t".join(cols))
baselines.write_text("\n".join(rows) + "\n")
PY
}

mutate_emby_evidence_to_lscr() {
  local scratch="$1"
  python3 - "$scratch/pins/emby-detector-evidence.tsv" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
text = text.replace(
    "ghcr.io/linuxserver/emby:version-4.9.3.0",
    "lscr.io/linuxserver/emby:version-4.9.3.0",
)
path.write_text(text)
PY
}

assert_rejected() {
  local name="$1" status="$2" expected="$3" scratch output rc
  scratch="$tmp_root/$name"
  stage_scratch "$scratch"
  mutate_emby_493_to_lscr "$scratch" "$status"
  set +e
  output="$(cd "$scratch" && bash tests/check_multi_version_pin_alignment.sh 2>&1)"
  rc=$?
  set -e
  [ "$rc" -ne 0 ] || fail "$name: synchronized lscr.io regression was accepted"
  printf '%s\n' "$output" | grep -Fq "$expected" || {
    printf '%s\n' "$output" >&2
    fail "$name: missing expected failure: $expected"
  }
}

assert_evidence_rejected() {
  local name="$1" scratch output rc
  scratch="$tmp_root/$name"
  stage_scratch "$scratch"
  mutate_emby_evidence_to_lscr "$scratch"
  set +e
  output="$(cd "$scratch" && bash tests/check_multi_version_pin_alignment.sh 2>&1)"
  rc=$?
  set -e
  [ "$rc" -ne 0 ] || fail "$name: Emby evidence registry regression was accepted"
  printf '%s\n' "$output" | grep -Fq 'Emby detector evidence image_ref mismatch' || {
    printf '%s\n' "$output" >&2
    fail "$name: missing expected evidence image-ref failure"
  }
}

positive="$tmp_root/positive"
stage_scratch "$positive"
(cd "$positive" && bash tests/check_multi_version_pin_alignment.sh >/dev/null)

assert_rejected support-registry-regression supported \
  'supported runtime image_ref must use canonical GHCR repository'
assert_rejected baseline-registry-regression unsupported \
  'runtime baseline image_ref must use canonical GHCR repository'
assert_evidence_rejected emby-evidence-registry-regression

printf 'multi-version registry negative checks passed\n'
