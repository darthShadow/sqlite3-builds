#!/usr/bin/env bash
# shellcheck disable=SC2016
set -euo pipefail

workflow=".github/workflows/sqlite-build.yml"
mod_bake_script="tools/ci/mod-bake-smoke.sh"

for forbidden in \
  SQLITE_LIBRARY_PINS \
  update-sqlite-library.sh \
  regen-deploy-pins \
  assemble-library-pins \
  cont-init-patch-plex-pool \
  /etc/cont-init.d \
  sqlite-pins-pre \
  "PMS first-init smoke (Plex, pool-patched)" \
  "Fetch prior library pins" \
  "Regenerate deploy script pins" \
  "Assemble deploy library pins" \
  "DOCKER_MODS"
do
  if grep -Fq "$forbidden" "$workflow"; then
    echo "FATAL: forbidden workflow deploy surface remains: $forbidden" >&2
    exit 1
  fi
done

grep -Eq '^[[:space:]]+release:' "$workflow"
grep -Eq '^[[:space:]]+mod-build:' "$workflow"
grep -Eq '^[[:space:]]+mod-publish:' "$workflow"
grep -Fq 'ubuntu-24.04-arm' "$workflow"
grep -Fq 'linuxserver-mod-sqlite3-plex' "$workflow"
grep -Fq 'linuxserver-mod-sqlite3-emby' "$workflow"
grep -Fq 'tools/lsio-mod/render-lsio-mod-baked-pins.sh' "$mod_bake_script"
grep -Fq 'tools/lsio-mod/stage-lsio-mod.sh' "$mod_bake_script"
grep -Fq 'bash tools/ci/plex-icu-smoke.sh' "$workflow"
grep -Fq 'bash tools/ci/plex-pms-first-init-smoke.sh' "$workflow"
grep -Fq 'bash tools/ci/plex-pms-killswitch-smoke.sh' "$workflow"
grep -Fq 'bash tools/ci/emby-first-init-smoke.sh' "$workflow"
grep -Fq 'bash tools/ci/slow-query-smoke.sh' "$workflow"
grep -Fq 'bash tools/ci/emby-killswitch-smoke.sh' "$workflow"
grep -Fq 'bash tools/ci/mod-bake-smoke.sh' "$workflow"
grep -Fq 'bash tests/ci_log_assertions_test.sh' "$workflow"
grep -Fq 'bash tests/mod_bake_assertions_test.sh' "$workflow"
grep -Fq 'compat_group_field_for_group "$compat_group" artifact_stem' "$workflow"
grep -Fq 'library_dir="library-${artifact_stem}"' "$workflow"
grep -Fq 'render_args+=(--artifact "${arch}:${compat_group}:${artifact_name}:${artifact_path}:${target_path}")' "$mod_bake_script"
grep -Fq 'stage_args+=(--artifact "${arch}:${compat_group}:${artifact_path}")' "$mod_bake_script"
grep -Fq 'COPY root-fs /' "$mod_bake_script"
grep -Fq 'root-fs/etc/s6-overlay/s6-rc.d' "$mod_bake_script"
grep -Fq '${GITHUB_RUN_ID}-${MATRIX_MOD}-${MATRIX_ARCH_SUFFIX}-${server_tag}-smoke' "$mod_bake_script"
grep -Fq 'mod-image-${{ matrix.mod }}-${{ matrix.arch_suffix }}' "$workflow"

if grep -Fq ':latest' "$workflow"; then
  echo "FATAL: workflow publishes latest tag" >&2
  exit 1
fi
if grep -Fq -- '--platform' "$workflow"; then
  echo "FATAL: workflow uses docker --platform instead of native runners" >&2
  exit 1
fi

python3 - <<'PY'
import re
from pathlib import Path

text = Path(".github/workflows/sqlite-build.yml").read_text()
mod_bake_script = Path("tools/ci/mod-bake-smoke.sh").read_text()
preflight = text.split("\n  preflight:", 1)[1].split("\n  build-cli:", 1)[0]
build_cli = text.split("\n  build-cli:", 1)[1].split("\n  build-generic:", 1)[0]
build_generic = text.split("\n  build-generic:", 1)[1].split("\n  build-plex:", 1)[0]
build_plex = text.split("\n  build-plex:", 1)[1].split("\n  mod-static-tests:", 1)[0]
release = text.split("\n  release:", 1)[1].split("\n  mod-build:", 1)[0]
mod_build = text.split("\n  mod-build:", 1)[1].split("\n  mod-publish:", 1)[0]
mod_publish = text.split("\n  mod-publish:", 1)[1]

if "needs: [build-cli, build-generic, build-plex, mod-static-tests]" not in release:
    raise SystemExit("release job does not gate on artifacts and mod-static-tests")
if "needs: [build-generic, build-plex]" not in mod_build:
    raise SystemExit("mod-build job missing generic/Plex artifact needs")
if "needs: release" in mod_build:
    raise SystemExit("mod-build job still depends on release")
if "startsWith(github.ref, 'refs/tags/')" in mod_build:
    raise SystemExit("mod-build job is still tag-gated")
if "pattern: sqlite-*-library-*" not in mod_build:
    raise SystemExit("mod-build artifact download is not library-only")
if "actions/upload-artifact@" not in mod_build:
    raise SystemExit("mod-build job missing upload-artifact step")
if "${{ matrix.platform }}" in mod_build:
    raise SystemExit("mod-build still uses matrix.platform in image refs")
if "DOCKER_MODS" in mod_build:
    raise SystemExit("mod-build still uses registry-fetch smoke")
if "cont-init" in mod_build:
    raise SystemExit("mod-build still references legacy cont-init")
if 'bash tools/ci/mod-bake-smoke.sh' not in mod_build:
    raise SystemExit("mod-build missing extracted bake-smoke invocation")
if "printf 'FROM %s\\nCOPY root-fs /\\n' \"$lsio_image\"" not in mod_bake_script:
    raise SystemExit("mod-bake script missing bake-in smoke Dockerfile")
if 'docker build --rm -t "$mod_ref" "$staged"' not in mod_bake_script:
    raise SystemExit("mod-bake script missing real scratch image build")
if "docker save \"$mod_ref\"" not in mod_bake_script:
    raise SystemExit("mod-bake script missing real image export")
if "docker run --rm --entrypoint sha256sum" not in mod_bake_script:
    raise SystemExit("mod-bake script missing runtime pre-SHA derivation")
if "assert_runtime_load" not in mod_bake_script:
    raise SystemExit("mod-bake script missing app runtime-load assertion")
if 'target_path_for_mod' in mod_bake_script:
    raise SystemExit("mod-bake script still uses per-mod target path")
if '${arch}:${compat_group}:${artifact_name}:${artifact_path}:${target_path}' not in mod_bake_script:
    raise SystemExit("mod-bake script missing group-aware render artifact tuple")
if '${arch}:${compat_group}:${artifact_path}' not in mod_bake_script:
    raise SystemExit("mod-bake script missing group-aware stage artifact tuple")

for command in [
    "bash tests/check_pin_alignment.sh",
    "bash tests/check_multi_version_pin_alignment.sh",
    "bash tests/check_build_sh_prechecks.sh",
]:
    if text.count(command) != 1 or command not in preflight:
        raise SystemExit(f"preflight must own one guard invocation: {command}")

if "needs: preflight" not in build_cli:
    raise SystemExit("build-cli job missing preflight need")
if "needs: [preflight, base]" in build_cli:
    raise SystemExit("build-cli job unnecessarily waits for base")
for name, section in [("build-generic", build_generic), ("build-plex", build_plex)]:
    if "needs: [preflight, base]" not in section:
        raise SystemExit(f"{name} job missing preflight/base needs")
for name, section in [("build-cli", build_cli), ("build-generic", build_generic), ("build-plex", build_plex)]:
    if "steps.library_artifact_stems.outputs" in section:
        raise SystemExit(f"{name} still references producer-local preflight step outputs")

producer_scripts = {
    "build-cli": (build_cli, ["tests/plex_fts_stat4_eqp_repro_test.sh"]),
    "build-generic": (
        build_generic,
        [
            "tools/ci/emby-first-init-smoke.sh",
            "tools/ci/emby-killswitch-smoke.sh",
            "tools/ci/slow-query-smoke.sh",
            "tests/check_obs_counts.sh",
            "tests/abi_obsolete_config_ops_test.sh",
        ],
    ),
    "build-plex": (
        build_plex,
        [
            "tools/ci/plex-icu-smoke.sh",
            "tools/ci/plex-pms-first-init-smoke.sh",
            "tools/ci/plex-pms-killswitch-smoke.sh",
        ],
    ),
}
for job, (section, scripts) in producer_scripts.items():
    for script in scripts:
        if f"bash {script}" not in section:
            raise SystemExit(f"{job} missing producer-owned script: {script}")

for script in producer_scripts["build-generic"][1]:
    if script in build_plex:
        raise SystemExit(f"build-plex contains generic-owned script: {script}")
for script in producer_scripts["build-plex"][1]:
    if script in build_generic:
        raise SystemExit(f"build-generic contains Plex-owned script: {script}")

static_tests = text.split("\n  mod-static-tests:", 1)[1].split("\n  release:", 1)[0]
if "bash tests/ci_log_assertions_test.sh" not in static_tests:
    raise SystemExit("mod-static-tests missing ci log assertions test")
if "bash tests/mod_bake_assertions_test.sh" not in static_tests:
    raise SystemExit("mod-static-tests missing mod bake assertions test")

needs_match = re.search(r'(?ms)^\s+needs:\s*(\[[^\n]+\]|(?:\n(?:\s+- .+\n?)+))', mod_publish)
if not needs_match:
    raise SystemExit("mod-publish job missing needs block")
needs_block = needs_match.group(1)
if "release" not in needs_block or "mod-build" not in needs_block:
    raise SystemExit("mod-publish job missing release/mod-build dependency gate")
if "startsWith(github.ref, 'refs/tags/')" not in mod_publish:
    raise SystemExit("mod-publish job missing tag gate")
if "actions/download-artifact@" not in mod_publish:
    raise SystemExit("mod-publish job missing download-artifact step")
if "docker build" in mod_publish or "docker run" in mod_publish:
    raise SystemExit("mod-publish rebuilds or re-smokes images")

if "SHA256SUMS" not in release:
    raise SystemExit("release job missing SHA256SUMS")
if "runtime_support_window<<EOF_RUNTIME_SUPPORT" not in release:
    raise SystemExit("release job missing runtime support window output")
if '${{ steps.release_compat.outputs.runtime_support_window }}' not in release:
    raise SystemExit("release body missing enumerated runtime support window")
if '$5 == "supported"' not in release:
    raise SystemExit("release support window must enumerate supported runtime rows only")
if "fetch-depth: 0" not in release:
    raise SystemExit("release checkout missing full history")
if 'bash tools/ci/render-release-notes.sh "${GITHUB_REF_NAME}" "${RELEASE_NOTES_DELIMITER}"' not in release:
    raise SystemExit("release job missing tested renderer invocation")
if "changes<<%s\\n" not in release or "EOF_RELEASE_NOTES_CHANGES" not in release:
    raise SystemExit("release job missing guarded changes heredoc")
if "${{ steps.commit_summaries.outputs.changes }}" not in release:
    raise SystemExit("release body missing rendered commit summaries")
if "body_path:" in release:
    raise SystemExit("release job replaced the inline compatibility body")
if "icu-${{ github.ref_name }}-" in release:
    raise SystemExit("release job still publishes ICU tarball")
for forbidden in ["SQLITE_LIBRARY_PINS", "release-assets/update-sqlite-library.sh"]:
    if forbidden in release:
        raise SystemExit(f"release job contains forbidden asset {forbidden}")
print("workflow mod-only static checks passed")
PY
