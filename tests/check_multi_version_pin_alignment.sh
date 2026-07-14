#!/bin/sh
# shellcheck disable=SC2016
set -eu

unset CDPATH
repo_root="$(cd -- "$(dirname -- "$0")/.." && pwd -P)"
cd "${repo_root}"

fail() {
  printf 'FATAL: %s\n' "$1" >&2
  exit 1
}

require_line() {
  file="$1"
  line="$2"
  grep -Fxq "$line" "$file" || fail "$file missing exact line: $line"
}

require_pattern() {
  file="$1"
  pattern="$2"
  label="$3"
  grep -Eq "$pattern" "$file" || fail "$label missing from $file"
}

reject_pattern() {
  file="$1"
  pattern="$2"
  if grep -Eq "$pattern" "$file"; then
    fail "$file contains rejected pattern: $pattern"
  fi
}

artifact_stems_for_mod() {
  mod="$1"
  awk -F '\t' -v want_mod="$mod" '
    $0 ~ /^[[:space:]]*($|#)/ { next }
    $1 == "compat_group" { next }
    $2 == want_mod {
      if (seen[$1]++) {
        printf "FATAL: duplicate compat_group for mod: %s %s\n", want_mod, $1 > "/dev/stderr"
        exit 2
      }
      count++
      print $1 "=" $4
    }
    END {
      if (count == 0) {
        printf "FATAL: missing artifact_stem rows for mod: %s\n", want_mod > "/dev/stderr"
        exit 1
      }
    }
  ' pins/library-compat-groups.tsv
}

for file in \
  pins/runtime-support.tsv \
  pins/library-compat-groups.tsv \
  pins/runtime-baselines.tsv \
  pins/emby-detector-evidence.tsv \
  pins/plex-pool-patch-sites.tsv \
  pins/plex-pool-patch-reviews.tsv \
  .github/workflows/sqlite-build.yml \
  tools/ci/mod-bake-smoke.sh
do
  [ -r "$file" ] || fail "$file not readable"
done

awk -F '\t' \
  -v compat_file="pins/library-compat-groups.tsv" \
  -v support_file="pins/runtime-support.tsv" \
  -v baseline_file="pins/runtime-baselines.tsv" \
  -v emby_evidence_file="pins/emby-detector-evidence.tsv" \
  -v pool_sites_file="pins/plex-pool-patch-sites.tsv" \
  -v pool_reviews_file="pins/plex-pool-patch-reviews.tsv" '
  function die(message) {
    printf "FATAL: %s\n", message > "/dev/stderr"
    exit 1
  }
  function valid_mod(value) {
    return value == "plex" || value == "emby"
  }
  function valid_arch(value) {
    return value == "linux-x86_64-v2" || value == "linux-x86_64-v3" || value == "linux-arm64"
  }
  function nonempty(label, value) {
    if (value == "") {
      die(label " is empty")
    }
  }
  FILENAME == compat_file {
    if ($0 ~ /^[[:space:]]*($|#)/ || $1 == "compat_group") {
      next
    }
    if (NF != 9) {
      die("malformed compat group row at line " FNR)
    }
    nonempty("compat_group at line " FNR, $1)
    nonempty("compat group mod at line " FNR, $2)
    nonempty("artifact_stem at line " FNR, $4)
    nonempty("smoke_server_id at line " FNR, $9)
    if (!valid_mod($2)) {
      die("invalid compat group mod at line " FNR ": " $2)
    }
    if ($1 in compat_mod) {
      die("duplicate compat_group: " $1)
    }
    if ($4 in stem_owner) {
      die("duplicate artifact_stem across compat groups: " $4)
    }
    stem_owner[$4] = $1
    compat_mod[$1] = $2
    compat_stem[$1] = $4
    compat_smoke[$1] = $9
    next
  }
  FILENAME == support_file {
    if ($0 ~ /^[[:space:]]*($|#)/ || $1 == "mod") {
      next
    }
    if (NF != 7) {
      die("malformed runtime support row at line " FNR)
    }
    for (i = 1; i <= 7; i++) {
      nonempty("runtime support field " i " at line " FNR, $i)
    }
    if (!valid_mod($1)) {
      die("invalid runtime support mod at line " FNR ": " $1)
    }
    if ($5 == "supported" && $3 !~ ("^ghcr[.]io/linuxserver/" $1 ":")) {
      die("supported runtime image_ref must use canonical GHCR repository at line " FNR ": " $3)
    }
    if (!($4 in compat_mod)) {
      die("runtime support references unknown compat_group at line " FNR ": " $4)
    }
    if (compat_mod[$4] != $1) {
      die("runtime support compat_group/mod mismatch at line " FNR ": " $4)
    }
    if ($2 in server_mod) {
      die("duplicate runtime support server_id: " $2)
    }
    server_mod[$2] = $1
    server_image[$2] = $3
    server_compat[$2] = $4
    server_status[$2] = $5
    compat_used[$4]++
    next
  }
  FILENAME == baseline_file {
    if ($0 ~ /^[[:space:]]*($|#)/ || $1 == "kind") {
      next
    }
    if (NF != 12) {
      die("malformed runtime baseline row at line " FNR)
    }
    if (!valid_arch($4)) {
      die("invalid runtime baseline arch at line " FNR ": " $4)
    }
    if ($1 == "detect" || $1 == "pre") {
      if (!($3 in server_mod)) {
        die("runtime baseline references unknown server_id at line " FNR ": " $3)
      }
      if ($2 != server_mod[$3]) {
        die("runtime baseline mod mismatch at line " FNR ": " $2)
      }
      if ($1 == "pre" && $6 !~ ("^ghcr[.]io/linuxserver/" $2 ":")) {
        die("runtime baseline image_ref must use canonical GHCR repository at line " FNR ": " $6)
      }
      if ($1 == "pre" && $6 != server_image[$3]) {
        die("runtime baseline image_ref mismatch at line " FNR ": " $3)
      }
      baseline_seen[$3]++
      if ($9 != "-" && !($9 in compat_mod)) {
        die("runtime baseline references unknown compat_group at line " FNR ": " $9)
      }
      if ($1 == "detect" && $5 ~ /:pristine$/) {
        pristine_sha[$3 SUBSEP $4 SUBSEP $8] = $12
      }
      if ($2 == "emby") {
        evidence_key = $2 SUBSEP $3 SUBSEP $4 SUBSEP $1 SUBSEP $5
        if (evidence_key in baseline_evidence_path) {
          die("duplicate Emby evidence source tuple at line " FNR ": " $3 " " $4 " " $1 " " $5)
        }
        baseline_evidence_path[evidence_key] = $8
        baseline_evidence_sha[evidence_key] = $12
      }
      next
    }
    if ($1 == "icu-runtime") {
      if (!($9 in compat_mod)) {
        die("icu-runtime references unknown compat_group at line " FNR ": " $9)
      }
      if (compat_mod[$9] != "plex") {
        die("icu-runtime compat_group is not Plex at line " FNR ": " $9)
      }
      next
    }
    die("unknown runtime baseline kind at line " FNR ": " $1)
  }
  FILENAME == emby_evidence_file {
    if ($0 ~ /^[[:space:]]*($|#)/ || $1 == "mod") {
      next
    }
    if (NF != 9) {
      die("malformed Emby detector evidence row at line " FNR)
    }
    if ($1 != "emby" || !($2 in server_mod) || server_mod[$2] != "emby") {
      die("Emby detector evidence server/mod mismatch at line " FNR ": " $1 " " $2)
    }
    if (!valid_arch($3)) {
      die("invalid Emby detector evidence arch at line " FNR ": " $3)
    }
    if ($4 != "detect" && $4 != "pre") {
      die("invalid Emby detector evidence kind at line " FNR ": " $4)
    }
    evidence_key = $1 SUBSEP $2 SUBSEP $3 SUBSEP $4 SUBSEP $5
    if (!(evidence_key in baseline_evidence_path)) {
      die("Emby detector evidence has no runtime baseline tuple at line " FNR ": " $2 " " $3 " " $4 " " $5)
    }
    if (evidence_seen[evidence_key]++) {
      die("duplicate Emby detector evidence tuple at line " FNR ": " $2 " " $3 " " $4 " " $5)
    }
    if ($6 != server_image[$2]) {
      die("Emby detector evidence image_ref mismatch at line " FNR ": " $2)
    }
    if ($7 != baseline_evidence_path[evidence_key] || $8 != baseline_evidence_sha[evidence_key]) {
      die("Emby detector evidence path/SHA mismatch at line " FNR ": " $2 " " $3 " " $4 " " $5)
    }
    if ($9 != "pins/runtime-baselines.tsv") {
      die("Emby detector evidence source mismatch at line " FNR ": " $9)
    }
    next
  }
  FILENAME == pool_sites_file {
    if ($0 ~ /^[[:space:]]*($|#)/ || $1 == "server_id") {
      next
    }
    if (NF != 9) {
      die("malformed Plex pool patch site row at line " FNR)
    }
    if (!($1 in server_mod)) {
      die("pool patch site references unknown server_id at line " FNR ": " $1)
    }
    if (server_mod[$1] != "plex") {
      die("pool patch site references non-Plex server_id at line " FNR ": " $1)
    }
    if (!valid_arch($2)) {
      die("invalid pool patch arch at line " FNR ": " $2)
    }
    key = $1 SUBSEP $2 SUBSEP $3
    if (!(key in pristine_sha)) {
      die("pool patch site has no pristine detector baseline at line " FNR ": " $1 " " $2 " " $3)
    }
    if (pristine_sha[key] != $4) {
      die("pool patch site baseline SHA mismatch at line " FNR ": " $1 " " $2 " " $3)
    }
    site_key = $1 SUBSEP $2 SUBSEP $3 SUBSEP $5 SUBSEP $6
    exact_key = site_key SUBSEP $7 SUBSEP $8 SUBSEP $9
    if (site_key in pool_site_identity_seen) {
      die("duplicate pool patch site key at line " FNR ": " $1 " " $2 " " $3 " " $5 " " $6)
    }
    pool_site_identity_seen[site_key] = 1
    pool_site_tuple[exact_key] = 1
    pool_site_desc[exact_key] = $1 " " $2 " " $3 " " $5 " " $6
    pool_site_seen[exact_key]++
    pool_server_seen[$1]++
    next
  }
  FILENAME == pool_reviews_file {
    if ($0 ~ /^[[:space:]]*($|#)/ || $1 == "server_id") {
      next
    }
    if (NF != 11) {
      die("malformed Plex pool patch review row at line " FNR)
    }
    if (!($1 in server_mod)) {
      die("pool patch review references unknown server_id at line " FNR ": " $1)
    }
    if (server_mod[$1] != "plex") {
      die("pool patch review references non-Plex server_id at line " FNR ": " $1)
    }
    if (!valid_arch($2)) {
      die("invalid pool patch review arch at line " FNR ": " $2)
    }
    site_key = $1 SUBSEP $2 SUBSEP $3 SUBSEP $4 SUBSEP $5 SUBSEP $6 SUBSEP $7 SUBSEP $8
    if (!(site_key in pool_site_tuple)) {
      die("pool patch review has no pool-site row at line " FNR ": " $1 " " $2 " " $3 " " $4 " " $5)
    }
    if ($11 != "approved") {
      die("pool patch review is not approved at line " FNR ": " $11)
    }
    exact_key = site_key
    reviewer_key = exact_key SUBSEP $10
    if (!(reviewer_key in pool_review_seen)) {
      pool_review_seen[reviewer_key] = 1
      pool_review_count[exact_key]++
    }
    next
  }
  END {
    for (evidence_key in baseline_evidence_path) {
      if (!(evidence_key in evidence_seen)) {
        die("Emby runtime baseline tuple has no detector evidence row: " evidence_key)
      }
    }
    for (compat in compat_mod) {
      if (!(compat in compat_used)) {
        die("orphan compat_group has no runtime-support row: " compat)
      }
      smoke = compat_smoke[compat]
      if (!(smoke in server_mod)) {
        die("compat_group smoke_server_id does not resolve in runtime-support: " compat " -> " smoke)
      }
      if (server_compat[smoke] != compat) {
        die("compat_group smoke_server_id uses another compat_group: " compat " -> " smoke)
      }
    }
    for (server_id in server_mod) {
      if (!(server_id in baseline_seen)) {
        die("runtime-support server_id has no runtime-baselines rows: " server_id)
      }
      if (server_mod[server_id] == "plex" && server_status[server_id] == "supported" && !(server_id in pool_server_seen)) {
        die("supported Plex server_id has no pool-site rows: " server_id)
      }
    }
    for (pool_key in pool_site_seen) {
      if (pool_review_count[pool_key] < 2) {
        die("pool-site row has fewer than 2 approved review rows: " pool_site_desc[pool_key])
      }
    }
  }
' \
  pins/library-compat-groups.tsv \
  pins/runtime-support.tsv \
  pins/runtime-baselines.tsv \
  pins/emby-detector-evidence.tsv \
  pins/plex-pool-patch-sites.tsv \
  pins/plex-pool-patch-reviews.tsv

plex_stems="$(artifact_stems_for_mod plex)"
emby_stems="$(artifact_stems_for_mod emby)"
printf '%s\n' "$plex_stems" | grep -Fxq 'icu69=plex-icu69' || fail "Plex icu69 artifact_stem missing from group set: observed=$plex_stems"
printf '%s\n' "$emby_stems" | grep -Fxq 'generic=generic' || fail "Emby generic artifact_stem missing from group set: observed=$emby_stems"

require_line .github/workflows/sqlite-build.yml '          compat_group_field_for_group() {'
require_line .github/workflows/sqlite-build.yml '          first_compat_group_for_mod() {'
require_line .github/workflows/sqlite-build.yml '          name: sqlite-${{env.SQLITE_VERSION}}-library-${{ needs.preflight.outputs.emby_artifact_stem }}-${{ matrix.arch_suffix }}'
require_line .github/workflows/sqlite-build.yml '          name: sqlite-${{env.SQLITE_VERSION}}-library-${{ needs.preflight.outputs.plex_artifact_stem }}-${{ matrix.arch_suffix }}'
reject_pattern .github/workflows/sqlite-build.yml 'name: sqlite-\$\{\{env\.SQLITE_VERSION\}\}-library-\$\{\{ matrix\.arch_suffix \}\}'
reject_pattern .github/workflows/sqlite-build.yml 'name: sqlite-\$\{\{env\.SQLITE_VERSION\}\}-library-plex-\$\{\{ matrix\.arch_suffix \}\}'
reject_pattern .github/workflows/sqlite-build.yml 'destination: library$'
reject_pattern .github/workflows/sqlite-build.yml 'destination: library-plex$'

require_line tools/ci/mod-bake-smoke.sh '    render_args+=(--artifact "${arch}:${compat_group}:${artifact_name}:${artifact_path}:${target_path}")'
require_line tools/ci/mod-bake-smoke.sh '    stage_args+=(--artifact "${arch}:${compat_group}:${artifact_path}")'
reject_pattern tools/ci/mod-bake-smoke.sh 'artifact_kind="library-(plex|generic|plex-icu69)"'

require_line .github/workflows/sqlite-build.yml '      - name: Record support image digests'
require_pattern .github/workflows/sqlite-build.yml 'pins/runtime-support\.tsv' 'support image digest runtime-support source'
require_pattern .github/workflows/sqlite-build.yml 'RepoDigests' 'support image digest RepoDigests inspection'
require_pattern .github/workflows/sqlite-build.yml 'support-image-digests-\$\{\{ matrix\.mod \}\}-\$\{\{ matrix\.arch_suffix \}\}' 'support image digest artifact'

printf 'multi-version pins aligned: plex_groups=%s emby_groups=%s\n' "$plex_stems" "$emby_stems"
