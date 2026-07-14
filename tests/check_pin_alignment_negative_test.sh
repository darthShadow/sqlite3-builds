#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "$0")/.." && pwd -P)"
tmp_parent="${TMPDIR:-/tmp}"
tmp_root="$(mktemp -d "${tmp_parent%/}/check-pin-alignment-negative.XXXXXX" 2>/dev/null || mktemp -d /tmp/check-pin-alignment-negative.XXXXXX)"

cleanup() {
  rm -rf "$tmp_root"
}
trap cleanup EXIT

fail() {
  echo "FATAL: $*" >&2
  exit 1
}

stage_scratch() {
  scratch="$1"
  mkdir -p \
    "$scratch/tests" \
    "$scratch/pins" \
    "$scratch/.github/workflows" \
    "$scratch/build" \
    "$scratch/docker-cli" \
    "$scratch/docker-library" \
    "$scratch/docker-build-base" \
    "$scratch/docs" \
    "$scratch/src" \
    "$scratch/tools/ci" \
    "$scratch/tools/lsio-mod" \
    "$scratch/tools" \
    "$scratch/scripts"
  mkdir -p "$scratch/docs/runbooks/query-measure/families"

  cp "$repo_root/tests/check_pin_alignment.sh" "$scratch/tests/check_pin_alignment.sh"
  cp "$repo_root/pins/versions.env" "$scratch/pins/versions.env"
  cp "$repo_root/pins/library-compat-groups.tsv" "$scratch/pins/library-compat-groups.tsv"
  cp "$repo_root/pins/runtime-support.tsv" "$scratch/pins/runtime-support.tsv"
  cp "$repo_root/.github/workflows/sqlite-build.yml" "$scratch/.github/workflows/sqlite-build.yml"
  cp "$repo_root/.github/workflows/base.yml" "$scratch/.github/workflows/base.yml"
  cp "$repo_root/build/Build.sh" "$scratch/build/Build.sh"
  cp "$repo_root/build/build_static_sqlite.sh" "$scratch/build/build_static_sqlite.sh"
  cp "$repo_root/build/base_image_ref.sh" "$scratch/build/base_image_ref.sh"
  cp "$repo_root/build/expected-sqlite-config-count.txt" "$scratch/build/expected-sqlite-config-count.txt"
  cp "$repo_root/build/expected-sqlite-dbconfig-count.txt" "$scratch/build/expected-sqlite-dbconfig-count.txt"
  cp "$repo_root/docker-cli/Dockerfile" "$scratch/docker-cli/Dockerfile"
  cp "$repo_root/docker-library/Dockerfile" "$scratch/docker-library/Dockerfile"
  cp "$repo_root/docker-build-base/Dockerfile" "$scratch/docker-build-base/Dockerfile"
  cp "$repo_root/docker-build-base/ubuntu-toolchain-r-test.asc" "$scratch/docker-build-base/ubuntu-toolchain-r-test.asc"
  cp "$repo_root/CLAUDE.md" "$scratch/CLAUDE.md"
  cp "$repo_root/docs/env-vars.md" "$scratch/docs/env-vars.md"
  cp "$repo_root/src/auto_extension.c" "$scratch/src/auto_extension.c"
  cp "$repo_root/src/emby_fts_rewrite.c" "$scratch/src/emby_fts_rewrite.c"
  cp "$repo_root/src/fts_lex.c" "$scratch/src/fts_lex.c"
  cp "$repo_root/src/observability.c" "$scratch/src/observability.c"
  cp "$repo_root/src/observability.h" "$scratch/src/observability.h"
  cp "$repo_root/src/plex_fts_rewrite.c" "$scratch/src/plex_fts_rewrite.c"
  cp "$repo_root/src/rewrite_modes.h" "$scratch/src/rewrite_modes.h"
  cp "$repo_root/src/runtime_optimize.c" "$scratch/src/runtime_optimize.c"
  cp "$repo_root/src/slow_query_tracker.c" "$scratch/src/slow_query_tracker.c"
  cp "$repo_root/docs/runbooks/query-measure/families/plex-guid-like.sh" "$scratch/docs/runbooks/query-measure/families/plex-guid-like.sh"
  cp "$repo_root/docs/runbooks/query-measure/families/emby-fanout.sh" "$scratch/docs/runbooks/query-measure/families/emby-fanout.sh"
  cp "$repo_root/docs/runbooks/query-measure/families/emby-search.sh" "$scratch/docs/runbooks/query-measure/families/emby-search.sh"
  cp "$repo_root/docs/runbooks/query-measure/families/emby-dashboard.sh" "$scratch/docs/runbooks/query-measure/families/emby-dashboard.sh"
  cp "$repo_root/tests/abi_obsolete_config_ops_test.sh" "$scratch/tests/abi_obsolete_config_ops_test.sh"
  cp "$repo_root/tools/ci/mod-bake-smoke.sh" "$scratch/tools/ci/mod-bake-smoke.sh"
  cp "$repo_root/tools/lsio-mod/render-lsio-mod-baked-pins.sh" "$scratch/tools/lsio-mod/render-lsio-mod-baked-pins.sh"
  cp "$repo_root/scripts/optimize_media_servers.sh" "$scratch/scripts/optimize_media_servers.sh"
}

assert_rejected() {
  name="$1"
  injected_path="$2"
  scratch="$tmp_root/$name"
  stage_scratch "$scratch"

  case "$injected_path" in
    pins/versions.env)
      printf '\nPLEX_IMAGE_TAG=foo\n' >> "$scratch/$injected_path"
      ;;
    scripts/reintroduced-pin.sh)
      printf '%s\n' 'PLEX_IMAGE_TAG=foo' > "$scratch/$injected_path"
      ;;
    docker-build-base/reintroduced-pin.sh)
      printf '%s\n' 'PLEX_IMAGE_TAG=foo' > "$scratch/$injected_path"
      ;;
    *)
      fail "unsupported injected path: $injected_path"
      ;;
  esac

  set +e
  output="$(cd "$scratch" && bash tests/check_pin_alignment.sh 2>&1)"
  status=$?
  set -e

  if [ "$status" -eq 0 ]; then
    printf '%s\n' "$output" >&2
    fail "$name: check_pin_alignment.sh accepted injected retired scalar"
  fi

  if ! printf '%s\n' "$output" | grep -Fq "$injected_path:"; then
    printf '%s\n' "$output" >&2
    fail "$name: missing injected path in guard output"
  fi

  if ! printf '%s\n' "$output" | grep -Fq 'FATAL: retired scalar pin reappeared'; then
    printf '%s\n' "$output" >&2
    fail "$name: missing retired-scalar fatal message"
  fi
}

assert_fails_with() {
  name="$1"
  expected_text="$2"
  scratch="$tmp_root/$name"
  stage_scratch "$scratch"

  case "$name" in
    missing-base-ref-script-input)
      grep -Fv 'printf '\''%s'\'' "${CMAKE_SHA256_AARCH64}"' \
        "$scratch/build/base_image_ref.sh" > "$scratch/build/base_image_ref.sh.tmp"
      mv "$scratch/build/base_image_ref.sh.tmp" "$scratch/build/base_image_ref.sh"
      ;;
    inline-generic-base)
      awk '
        $0 == "FROM ${BASE_IMAGE} AS base-generic" {
          print "FROM ubuntu:18.04 AS base-generic"
          print "RUN add-apt-repository ppa:ubuntu-toolchain-r/test"
          next
        }
        { print }
      ' "$scratch/docker-library/Dockerfile" > "$scratch/docker-library/Dockerfile.tmp"
      mv "$scratch/docker-library/Dockerfile.tmp" "$scratch/docker-library/Dockerfile"
      ;;
    library-copy-before-dependencies)
      python3 - "$scratch/docker-library/Dockerfile" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
copy_line = "COPY src/observability.c /app/observability.c\n"
anchor = "ARG ICU_SOURCE_VERSION\n"
if text.count(copy_line) != 1 or text.count(anchor) != 1:
    raise SystemExit("unexpected Dockerfile fixture shape")
text = text.replace(copy_line, "", 1)
text = text.replace(anchor, copy_line + "\n" + anchor, 1)
path.write_text(text)
PY
      ;;
    flagged-library-copy-before-dependencies)
      python3 - "$scratch/docker-library/Dockerfile" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
copy_line = "COPY --chown=1000:1000 src/observability.c /app/observability.c\n"
marker = "ENV MIMALLOC_LIB=/opt/mimalloc/lib/libmimalloc.a\n"
if text.count(copy_line) != 0 or text.count(marker) != 1:
    raise SystemExit("unexpected flagged Dockerfile fixture shape")
text = text.replace(marker, copy_line + marker, 1)
path.write_text(text)
PY
      ;;
    gha-cache-backend)
      python3 - "$scratch/.github/workflows/sqlite-build.yml" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
old = '--cache-from type=registry,ref="${event_cache_ref}"'
if old not in text:
    raise SystemExit("registry event-cache import fixture missing")
path.write_text(text.replace(old, '--cache-from type=gha,scope="$cache_scope"', 1))
PY
      ;;
    pr-cache-writer)
      python3 - "$scratch/.github/workflows/sqlite-build.yml" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
old = "  CACHE_EXPORT_ENABLED: ${{ (github.event_name == 'push' && github.ref == 'refs/heads/main' && github.repository == 'darthshadow/sqlite3-builds') || (github.event_name == 'pull_request' && github.event.pull_request.head.repo.full_name == github.repository) }}"
new = "  CACHE_EXPORT_ENABLED: ${{ (github.event_name == 'push' && github.ref == 'refs/heads/main' && github.repository == 'darthshadow/sqlite3-builds') || github.event_name == 'pull_request' }}"
if text.count(old) != 1:
    raise SystemExit("fork-PR cache-writer gate fixture missing")
path.write_text(text.replace(old, new, 1))
PY
      ;;
    pr-baseline-cache-writer)
      python3 - "$scratch/.github/workflows/sqlite-build.yml" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
old = "  CACHE_EVENT_NAME: ${{ github.event_name == 'pull_request' && format('pr-{0}', github.event.pull_request.number) || 'baseline' }}"
new = "  CACHE_EVENT_NAME: baseline"
if text.count(old) != 1:
    raise SystemExit("event-scoped cache-name fixture missing")
path.write_text(text.replace(old, new, 1))
PY
      ;;
    concurrency-pr-source-repo-collision)
      python3 - "$scratch/.github/workflows/sqlite-build.yml" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
contexts = [
    ("fork-one/sqlite3-builds", "feature", 101),
    ("fork-two/sqlite3-builds", "feature", 202),
]
legacy_keys = [head_ref for _source_repo, head_ref, _pr_number in contexts]
pr_keys = [f"pr-{pr_number}" for _source_repo, _head_ref, pr_number in contexts]
if contexts[0][0] == contexts[1][0] or legacy_keys[0] != legacy_keys[1] or pr_keys[0] == pr_keys[1]:
    raise SystemExit("cross-repository same-branch concurrency fixture invalid")
old = "  group: ${{ github.workflow }}-${{ github.event_name == 'pull_request' && format('pr-{0}', github.event.pull_request.number) || github.ref || github.run_id }}\n"
new = "  group: ${{ github.workflow }}-${{ github.head_ref || github.ref || github.run_id }}\n"
if text.count(old) != 1:
    raise SystemExit("PR-number concurrency fixture missing")
path.write_text(text.replace(old, new, 1))
PY
      ;;
    moved-cli-event-cache-import-to-generic)
      python3 - "$scratch/.github/workflows/sqlite-build.yml" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
cli_start = text.index("      - name: Build sqlite CLI\n")
cli_end = text.index("      - name: Export sqlite CLI build cache\n", cli_start)
generic_start = text.index("      - name: Build sqlite library\n", cli_end)
generic_end = text.index("      - name: Export sqlite generic library build cache\n", generic_start)
cli_block = text[cli_start:cli_end]
generic_block = text[generic_start:generic_end]
line = '            --cache-from type=registry,ref="${event_cache_ref}" \\\n'
anchor = '            --cache-from type=registry,ref="${baseline_cache_ref}" \\\n'
if cli_block.count(line) != 1 or generic_block.count(line) != 1 or generic_block.count(anchor) != 1:
    raise SystemExit("cross-family event-cache import move fixture missing")
cli_block = cli_block.replace(line, "", 1)
generic_block = generic_block.replace(anchor, line + anchor, 1)
text = text[:cli_start] + cli_block + text[cli_end:generic_start] + generic_block + text[generic_end:]
path.write_text(text)
PY
      ;;
    moved-plex-cache-export-to-generic)
      python3 - "$scratch/.github/workflows/sqlite-build.yml" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
generic_start = text.index("      - name: Export sqlite generic library build cache\n")
generic_end = text.index("      - name: Extract binary from docker image\n", generic_start)
plex_start = text.index("      - name: Export sqlite Plex library build cache\n", generic_end)
plex_end = text.index("      - name: Extract Plex library from docker image\n", plex_start)
generic_block = text[generic_start:generic_end]
plex_block = text[plex_start:plex_end]
line = '            --cache-to type=registry,ref="${event_cache_ref}",mode=max,oci-mediatypes=true,image-manifest=true,compression=zstd \\\n'
if generic_block.count(line) != 1 or plex_block.count(line) != 1:
    raise SystemExit("cross-family event-cache export move fixture missing")
generic_block = generic_block.replace(line, line + line, 1)
plex_block = plex_block.replace(line, "", 1)
text = text[:generic_start] + generic_block + text[generic_end:plex_start] + plex_block + text[plex_end:]
path.write_text(text)
PY
      ;;
    missing-generic-event-cache-import)
      python3 - "$scratch/.github/workflows/sqlite-build.yml" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
start = text.index("      - name: Build sqlite library\n")
end = text.index("      - name: Export sqlite generic library build cache\n", start)
block = text[start:end]
line = '            --cache-from type=registry,ref="${event_cache_ref}" \\\n'
if block.count(line) != 1:
    raise SystemExit("generic event-cache import fixture missing")
text = text[:start] + block.replace(line, "", 1) + text[end:]
path.write_text(text)
PY
      ;;
    missing-plex-cache-export)
      python3 - "$scratch/.github/workflows/sqlite-build.yml" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
start = text.index("      - name: Export sqlite Plex library build cache\n")
end = text.index("      - name: Extract Plex library from docker image\n", start)
block = text[start:end]
line = '            --cache-to type=registry,ref="${event_cache_ref}",mode=max,oci-mediatypes=true,image-manifest=true,compression=zstd \\\n'
if block.count(line) != 1:
    raise SystemExit("Plex event-cache export fixture missing")
text = text[:start] + block.replace(line, "", 1) + text[end:]
path.write_text(text)
PY
      ;;
    cache-export-not-best-effort)
      python3 - "$scratch/.github/workflows/sqlite-build.yml" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
start = text.index("      - name: Export sqlite generic library build cache\n")
end = text.index("      - name: Extract binary from docker image\n", start)
block = text[start:end]
line = "        continue-on-error: true\n"
if block.count(line) != 1:
    raise SystemExit("generic best-effort export fixture missing")
text = text[:start] + block.replace(line, "", 1) + text[end:]
path.write_text(text)
PY
      ;;
    concurrency-tag-cancel-enabled)
      python3 - "$scratch/.github/workflows/sqlite-build.yml" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
old = "  cancel-in-progress: ${{ !startsWith(github.ref, 'refs/tags/') }}\n"
new = "  cancel-in-progress: true\n"
if text.count(old) != 1:
    raise SystemExit("tag concurrency exception fixture missing")
path.write_text(text.replace(old, new, 1))
PY
      ;;
    cache-export-ungated)
      python3 - "$scratch/.github/workflows/sqlite-build.yml" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
start = text.index("      - name: Export sqlite Plex library build cache\n")
end = text.index("      - name: Extract Plex library from docker image\n", start)
block = text[start:end]
line = "        if: env.CACHE_EXPORT_ENABLED == 'true'\n"
if block.count(line) != 1:
    raise SystemExit("Plex cache event-gate fixture missing")
text = text[:start] + block.replace(line, "", 1) + text[end:]
path.write_text(text)
PY
      ;;
    rewrite-catalogue-missing-guid)
      python3 - "$scratch/src/rewrite_modes.h" <<'PY'
from pathlib import Path
import sys
path = Path(sys.argv[1])
text = path.read_text()
line = '    X(PLEX_GUID_LIKE, "plex", "guid+like-null", "plex_fts_rewrite", 0) \\\n'
if text.count(line) != 1:
    raise SystemExit("PLEX_GUID_LIKE catalogue fixture missing")
path.write_text(text.replace(line, "", 1))
PY
      ;;
    rewrite-catalogue-browse-eligible)
      python3 - "$scratch/src/rewrite_modes.h" <<'PY'
from pathlib import Path
import sys
path = Path(sys.argv[1])
text = path.read_text()
old = '    X(EMBY_BROWSE, "emby", "fanout+browse", "emby_fts_rewrite", 0) \\\n'
new = '    X(EMBY_BROWSE, "emby", "fanout+browse", "emby_fts_rewrite", 1) \\\n'
if text.count(old) != 1:
    raise SystemExit("EMBY_BROWSE eligibility fixture missing")
path.write_text(text.replace(old, new, 1))
PY
      ;;
    rewrite-resume-wrong-valid-token)
      python3 - "$scratch/src/emby_fts_rewrite.c" <<'PY'
from pathlib import Path
import sys
path = Path(sys.argv[1])
text = path.read_text()
old = "    candidate->mode = OBS_MODE_EMBY_RESUME;\n"
new = "    candidate->mode = OBS_MODE_EMBY_FTS;\n"
if text.count(old) != 1:
    raise SystemExit("Emby resume producer fixture missing")
path.write_text(text.replace(old, new, 1))
PY
      ;;
    rewrite-guid-raw-producer)
      python3 - "$scratch/src/plex_fts_rewrite.c" <<'PY'
from pathlib import Path
import sys
path = Path(sys.argv[1])
text = path.read_text()
old = "                OBS_MODE_PLEX_GUID_LIKE\n"
new = '                "guid+like-null"\n'
if text.count(old) != 1:
    raise SystemExit("Plex GUID producer fixture missing")
path.write_text(text.replace(old, new, 1))
PY
      ;;
    rewrite-raw-wire-slow-query)
      printf '%s\n' 'candidate->mode = "guid+like-null";' >> "$scratch/src/slow_query_tracker.c"
      ;;
    rewrite-docker-copy-missing)
      python3 - "$scratch/docker-library/Dockerfile" <<'PY'
from pathlib import Path
import sys
path = Path(sys.argv[1])
text = path.read_text()
line = "COPY src/rewrite_modes.h /app/rewrite_modes.h\n"
if text.count(line) != 1:
    raise SystemExit("rewrite_modes Docker COPY fixture missing")
path.write_text(text.replace(line, "", 1))
PY
      ;;
    rewrite-docker-count-restored-18)
      python3 - "$scratch/tests/check_pin_alignment.sh" <<'PY'
from pathlib import Path
import sys
path = Path(sys.argv[1])
text = path.read_text()
old = "  19 \\\n  'library dependency layers before all project COPY lines'"
new = "  18 \\\n  'library dependency layers before all project COPY lines'"
if text.count(old) != 1:
    raise SystemExit("19-copy checker fixture missing")
path.write_text(text.replace(old, new, 1))
PY
      ;;
    rewrite-runbook-row-drift)
      python3 - "$scratch/docs/runbooks/query-measure/families/emby-search.sh" <<'PY'
from pathlib import Path
import sys
path = Path(sys.argv[1])
text = path.read_text()
old = "Emby People mode catalogue contract drifted"
new = "Emby people mode catalogue contract drifted"
if text.count(old) != 1:
    raise SystemExit("Emby People runbook row fixture missing")
path.write_text(text.replace(old, new, 1))
PY
      ;;
    rewrite-abi-observability-header-missing)
      grep -Fv 'cp "${repo_root}/src/observability.h" "${tmpdir}/observability.h"' \
        "$scratch/tests/abi_obsolete_config_ops_test.sh" > "$scratch/tests/abi_obsolete_config_ops_test.sh.tmp"
      mv "$scratch/tests/abi_obsolete_config_ops_test.sh.tmp" "$scratch/tests/abi_obsolete_config_ops_test.sh"
      ;;
    rewrite-abi-mode-header-missing)
      grep -Fv 'cp "${repo_root}/src/rewrite_modes.h" "${tmpdir}/rewrite_modes.h"' \
        "$scratch/tests/abi_obsolete_config_ops_test.sh" > "$scratch/tests/abi_obsolete_config_ops_test.sh.tmp"
      mv "$scratch/tests/abi_obsolete_config_ops_test.sh.tmp" "$scratch/tests/abi_obsolete_config_ops_test.sh"
      ;;
    stmt-sampling-source-literal)
      awk '
        $0 == "        (stmt_sampling && strcmp(stmt_sampling, \"1\") == 0) ? 1 : 0," {
          print "        (stmt_sampling && strcmp(stmt_sampling, \"0\") == 0) ? 1 : 0,"
          next
        }
        { print }
      ' "$scratch/src/observability.c" > "$scratch/src/observability.c.tmp"
      mv "$scratch/src/observability.c.tmp" "$scratch/src/observability.c"
      ;;
    stmt-sampling-doc-literal)
      awk '
        index($0, "| `SQLITE3_DISABLE_STMT_TRACE_SAMPLING` |") == 1 {
          sub("Literal `1` logs every", "Literal `0` logs every")
        }
        { print }
      ' "$scratch/docs/env-vars.md" > "$scratch/docs/env-vars.md.tmp"
      mv "$scratch/docs/env-vars.md.tmp" "$scratch/docs/env-vars.md"
      ;;
    stmt-sampling-claude-literal)
      awk '
        { gsub("SQLITE3_DISABLE_STMT_TRACE_SAMPLING=1", "SQLITE3_DISABLE_STMT_TRACE_SAMPLING=0"); print }
      ' "$scratch/CLAUDE.md" > "$scratch/CLAUDE.md.tmp"
      mv "$scratch/CLAUDE.md.tmp" "$scratch/CLAUDE.md"
      ;;
    *)
      fail "unsupported failure case: $name"
      ;;
  esac

  set +e
  output="$(cd "$scratch" && bash tests/check_pin_alignment.sh 2>&1)"
  status=$?
  set -e

  if [ "$status" -eq 0 ]; then
    printf '%s\n' "$output" >&2
    fail "$name: check_pin_alignment.sh accepted invalid fixture"
  fi

  if ! printf '%s\n' "$output" | grep -Fq "$expected_text"; then
    printf '%s\n' "$output" >&2
    fail "$name: missing expected failure text: $expected_text"
  fi
}

assert_rejected pins-source pins/versions.env
assert_rejected scripts-source scripts/reintroduced-pin.sh
assert_rejected retired-scalar-under-base-context docker-build-base/reintroduced-pin.sh
assert_fails_with missing-base-ref-script-input CMAKE_SHA256_AARCH64
assert_fails_with inline-generic-base BASE_IMAGE
assert_fails_with library-copy-before-dependencies 'library dependency layers before all project COPY lines invalid'
assert_fails_with flagged-library-copy-before-dependencies 'library dependency layers before all project COPY lines invalid'
assert_fails_with gha-cache-backend 'contains rejected pattern: type=gha'
assert_fails_with pr-cache-writer "missing exact line:   CACHE_EXPORT_ENABLED: \${{ (github.event_name == 'push' && github.ref == 'refs/heads/main' && github.repository == 'darthshadow/sqlite3-builds') || (github.event_name == 'pull_request' && github.event.pull_request.head.repo.full_name == github.repository) }}"
assert_fails_with pr-baseline-cache-writer 'contains rejected exact line:   CACHE_EVENT_NAME: baseline'
assert_fails_with concurrency-pr-source-repo-collision \
  "missing exact line:   group: \${{ github.workflow }}-\${{ github.event_name == 'pull_request' && format('pr-{0}', github.event.pull_request.number) || github.ref || github.run_id }}"
assert_fails_with moved-cli-event-cache-import-to-generic \
  'CLI event cache import exact-line count drift: observed=0 expected=1'
assert_fails_with moved-plex-cache-export-to-generic \
  'generic event-only cache export exact-line count drift: observed=2 expected=1'
assert_fails_with missing-generic-event-cache-import 'event cache import count drift: observed=2 expected=3'
assert_fails_with missing-plex-cache-export 'event cache export count drift: observed=2 expected=3'
assert_fails_with cache-export-not-best-effort 'best-effort cache export count drift: observed=2 expected=3'
assert_fails_with concurrency-tag-cancel-enabled \
  "missing exact line:   cancel-in-progress: \${{ !startsWith(github.ref, 'refs/tags/') }}"
assert_fails_with cache-export-ungated 'Plex cache event gate exact-line count drift: observed=0 expected=1'
assert_fails_with rewrite-catalogue-missing-guid \
  'src/rewrite_modes.h missing exact line:     X(PLEX_GUID_LIKE'
assert_fails_with rewrite-catalogue-browse-eligible \
  'src/rewrite_modes.h missing exact line:     X(EMBY_BROWSE'
assert_fails_with rewrite-resume-wrong-valid-token \
  'Emby resume producer manifest invalid'
assert_fails_with rewrite-guid-raw-producer \
  'Plex prepare producer manifest OBS_MODE_PLEX_GUID_LIKE invalid'
assert_fails_with rewrite-raw-wire-slow-query \
  'src/slow_query_tracker.c contains rejected pattern:'
assert_fails_with rewrite-docker-copy-missing \
  'library dependency layers before all project COPY lines invalid'
assert_fails_with rewrite-docker-count-restored-18 \
  'library dependency layers before all project COPY lines invalid'
assert_fails_with rewrite-runbook-row-drift \
  'docs/runbooks/query-measure/families/emby-search.sh missing exact line:'
assert_fails_with rewrite-abi-observability-header-missing \
  'tests/abi_obsolete_config_ops_test.sh missing exact line:   cp "${repo_root}/src/observability.h"'
assert_fails_with rewrite-abi-mode-header-missing \
  'tests/abi_obsolete_config_ops_test.sh missing exact line:   cp "${repo_root}/src/rewrite_modes.h"'
assert_fails_with stmt-sampling-source-literal 'STMT trace sampling literal-1 override missing'
assert_fails_with stmt-sampling-doc-literal 'docs/env-vars.md missing exact line'
assert_fails_with stmt-sampling-claude-literal 'CLAUDE STMT sampling knob missing from CLAUDE.md'

printf 'negative pin-alignment checks passed\n'
