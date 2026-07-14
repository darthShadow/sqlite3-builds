#!/usr/bin/env bash
set -euo pipefail

umask 077
export LC_ALL=C

HARNESS_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
MODE=${1:-}
FAMILY=${2:-}
ENV_FILE=${3:-${HARNESS_DIR}/env}
REPO_ROOT=${REPO_ROOT:-$(cd -- "${HARNESS_DIR}/../../.." && pwd)}

usage() {
  cat >&2 <<'USAGE'
usage: bash query-measure.sh <measure|canary> <family> [env-file]

families:
  plex-ondeck     Plex On-Deck id-list/threshold x CROSS JOIN/JOIN
  plex-guid-like  Plex GUID LIKE NULL guard work and non-NULL plan identity
  emby-search     Emby FTS search candidates x type/presentation x match cells
  emby-fanout     Emby resume fan-out production shapes
  emby-dashboard  Emby movies-Latest and episodes-Latest
  plex-taggings   Plex taggings membership count and grouped-id shapes
USAGE
  exit 2
}

case "$MODE" in measure|canary) ;; *) usage ;; esac
case "$FAMILY" in plex-ondeck|plex-guid-like|emby-search|emby-fanout|emby-dashboard|plex-taggings) ;; *) usage ;; esac
[ -f "$ENV_FILE" ] || {
  printf 'error: missing %s; copy %s and fill host-local paths\n' "$ENV_FILE" "${HARNESS_DIR}/env.example" >&2
  exit 2
}
# shellcheck source=/dev/null
. "$ENV_FILE"

die() {
  printf 'error: %s\n' "$*" >&2
  exit 1
}

log() {
  printf '%s\n' "$*" >&2
}

require_env() {
  local name value
  for name in "$@"; do
    value=${!name-}
    [ -n "$value" ] || die "$name is required in $ENV_FILE"
  done
}

require_uint() {
  local name=$1 value=${!1-}
  case "$value" in ''|*[!0-9]*) die "$name must be a non-negative integer, got ${value:-empty}" ;; esac
}

validate_segment() {
  case "$1" in ''|*/*|*..*) die "unsafe path segment: $1" ;; esac
}

TIMEOUT_S=${TIMEOUT_S:-10}
SETUP_TIMEOUT_S=${SETUP_TIMEOUT_S:-300}
TIMED_ITERS=${TIMED_ITERS:-5}
DISK_MARGIN_BYTES=${DISK_MARGIN_BYTES:-1073741824}
CANARY_MAX_RATIO=${CANARY_MAX_RATIO:-1.25}
ANALYZE_COPY=${ANALYZE_COPY:-0}
KEEP_OUTPUTS=${KEEP_OUTPUTS:-0}
for _n in TIMEOUT_S SETUP_TIMEOUT_S TIMED_ITERS DISK_MARGIN_BYTES ANALYZE_COPY KEEP_OUTPUTS; do require_uint "$_n"; done
[ "$TIMEOUT_S" -ge 1 ] || die 'TIMEOUT_S must be at least 1'
[ "$SETUP_TIMEOUT_S" -ge 1 ] || die 'SETUP_TIMEOUT_S must be at least 1'
[ "$TIMED_ITERS" -ge 3 ] || die 'TIMED_ITERS must be at least 3'
case "$ANALYZE_COPY" in 0|1) ;; *) die 'ANALYZE_COPY must be 0 or 1' ;; esac
case "$KEEP_OUTPUTS" in 0|1) ;; *) die 'KEEP_OUTPUTS must be 0 or 1' ;; esac
awk -v r="$CANARY_MAX_RATIO" 'BEGIN { exit !(r+0==r && r>=1) }' || die 'CANARY_MAX_RATIO must be a number >= 1'

for _cmd in awk bash cat cmp cp date df grep mkdir mktemp paste rm sed sha256sum sort stat tee timeout tr wc; do
  command -v "$_cmd" >/dev/null 2>&1 || die "required command not found: $_cmd"
done

family_case_role() { printf 'SHIPPED\n'; }
family_canary_policy() { printf 'LATENCY\n'; }
family_validate_engine() { :; }
family_work_pass() { return 1; }

# shellcheck source=/dev/null
. "${HARNESS_DIR}/families/${FAMILY}.sh"
family_configure
require_env SQLITE_BIN SOURCE_DB SCRATCH_ROOT
[ -x "$SQLITE_BIN" ] || die "sqlite3 binary is not executable: $SQLITE_BIN"
[ -f "$SOURCE_DB" ] || die "database source is not a file: $SOURCE_DB"
[ -r "$SOURCE_DB" ] || die "database source is not readable: $SOURCE_DB"
SOURCE_WAL=${SOURCE_DB}-wal
if [ -e "$SOURCE_WAL" ]; then
  [ -f "$SOURCE_WAL" ] || die "database WAL source is not a file: $SOURCE_WAL"
  [ -r "$SOURCE_WAL" ] || die "database WAL source is not readable: $SOURCE_WAL"
fi

# Source-level contract test: provider SQL data may not call SQLite, and every
# engine-owned direct CLI invocation must show timeout on the same line.
# shellcheck disable=SC2016
_ss=$(grep -nF '"$SQLITE_BIN" -batch' "${BASH_SOURCE[0]}" | grep -vF '_ss=' | grep -vF 'timeout "${' || true)
[ -z "$_ss" ] || die "startup self-check found sqlite invocation without timeout: $_ss"
# shellcheck disable=SC2016
if grep -R -nE '\$SQLITE_BIN|sqlite3[[:space:]]+-batch' "${HARNESS_DIR}/families" >/dev/null 2>&1; then
  die 'family data files must not invoke sqlite; the shared engine owns execution'
fi
family_contract_check

RUN_ID=${RUN_ID:-query-measure-${FAMILY}-$(date -u +%Y%m%dT%H%M%SZ)-$$}
validate_segment "$RUN_ID"
mkdir -p "$SCRATCH_ROOT"
RUN_DIR=$(mktemp -d "${SCRATCH_ROOT%/}/${RUN_ID}.XXXXXX")
DB_COPY=$RUN_DIR/database.copy
SQL_DIR=$RUN_DIR/sql
OUT_DIR=$RUN_DIR/out
MANIFEST=$OUT_DIR/manifest.tsv
LITERALS=$OUT_DIR/literals.tsv
IDENTITY=$OUT_DIR/identity.tsv
EQP=$OUT_DIR/eqp.tsv
TIMINGS=$OUT_DIR/timings.tsv
MEDIANS=$OUT_DIR/medians.tsv
PLANS=$OUT_DIR/plans.tsv
WORK=$OUT_DIR/work.tsv
VERDICTS=$OUT_DIR/verdicts.tsv
STATUS_FILE=$OUT_DIR/status.tsv
cleanup_done=0

cleanup() {
  local rc=$?
  trap - EXIT INT TERM HUP
  if [ "${cleanup_done:-0}" -eq 0 ] && [ -n "${RUN_DIR:-}" ]; then
    case "$RUN_DIR" in
      "${SCRATCH_ROOT%/}"/*)
        if [ "$KEEP_OUTPUTS" -eq 1 ]; then
          timeout "${SETUP_TIMEOUT_S}s" rm -f "$DB_COPY" "${DB_COPY}-wal" "${DB_COPY}-shm" >/dev/null 2>&1 || rc=1
        else
          timeout "${SETUP_TIMEOUT_S}s" rm -rf "$RUN_DIR" >/dev/null 2>&1 || rc=1
        fi
        ;;
      *) printf 'error: refusing unsafe cleanup path %s\n' "$RUN_DIR" >&2; rc=1 ;;
    esac
  fi
  cleanup_done=1
  exit "$rc"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

file_size() {
  stat -c %s "$1" 2>/dev/null || stat -f %z "$1"
}

run_setup_sql() {
  local phase=$1 db=$2 sql=$3 out=$4 rc
  log "PHASE phase=$phase cap=${SETUP_TIMEOUT_S}s"
  set +e
  timeout "${SETUP_TIMEOUT_S}s" "$SQLITE_BIN" -batch "$db" < "$sql" > "$out" 2>&1
  rc=$?
  set -e
  [ "$rc" -eq 0 ] && return 0
  [ "$rc" -eq 124 ] && die "BLOCKED setup timeout phase=$phase cap=${SETUP_TIMEOUT_S}s"
  sed -n '1,40p' "$out" >&2
  die "setup sqlite failed phase=$phase rc=$rc out=$out"
}

run_query_sql() {
  local phase=$1 db=$2 sql=$3 out=$4 rc
  log "PHASE phase=$phase cap=${TIMEOUT_S}s"
  set +e
  timeout "${TIMEOUT_S}s" "$SQLITE_BIN" -batch "$db" < "$sql" > "$out" 2>&1
  rc=$?
  set -e
  return "$rc"
}

disk_preflight() {
  local source_bytes source_wal_bytes=0 snapshot_bytes free_bytes required_bytes
  mkdir -p "$SQL_DIR" "$OUT_DIR"
  source_bytes=$(file_size "$SOURCE_DB")
  if [ -f "$SOURCE_WAL" ]; then
    source_wal_bytes=$(file_size "$SOURCE_WAL")
  fi
  snapshot_bytes=$((source_bytes + source_wal_bytes))
  free_bytes=$(df -Pk "$RUN_DIR" | awk 'NR==2 {printf "%.0f\n", $4 * 1024}')
  required_bytes=$((snapshot_bytes * 2 + DISK_MARGIN_BYTES))
  printf 'source_bytes\t%s\nsource_wal_bytes\t%s\nsnapshot_bytes\t%s\nfree_bytes\t%s\nrequired_bytes\t%s\n' \
    "$source_bytes" "$source_wal_bytes" "$snapshot_bytes" "$free_bytes" "$required_bytes" > "$MANIFEST"
  [ "$free_bytes" -ge "$required_bytes" ] || die "BLOCKED disk preflight free=$free_bytes required=$required_bytes"
}

copy_database() {
  local rc
  log "PHASE phase=copy cap=${SETUP_TIMEOUT_S}s"
  set +e
  timeout "${SETUP_TIMEOUT_S}s" cp -- "$SOURCE_DB" "$DB_COPY"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { [ "$rc" -eq 124 ] && die 'BLOCKED copy timeout'; die "copy failed rc=$rc"; }
  [ "$(file_size "$SOURCE_DB")" = "$(file_size "$DB_COPY")" ] || die 'copy size mismatch'
  if [ -f "$SOURCE_WAL" ]; then
    set +e
    timeout "${SETUP_TIMEOUT_S}s" cp -- "$SOURCE_WAL" "${DB_COPY}-wal"
    rc=$?
    set -e
    [ "$rc" -eq 0 ] || { [ "$rc" -eq 124 ] && die 'BLOCKED WAL copy timeout'; die "WAL copy failed rc=$rc"; }
    [ "$(file_size "$SOURCE_WAL")" = "$(file_size "${DB_COPY}-wal")" ] || die 'WAL copy size mismatch'
  fi
}

write_readonly_preamble() {
  cat <<'SQL'
.bail on
.headers off
.mode tabs
.nullvalue <NULL>
.explain off
SQL
  family_parameters
  printf 'PRAGMA query_only=1;\n'
}

probe_stat_rows() {
  local table=$1 probe=$2 label sql out
  case "$table" in sqlite_stat1|sqlite_stat4) ;; *) die "invalid stats table: $table" ;; esac
  label=${table#sqlite_}
  if grep -Fqx "${label}_present"$'\t''1' "$probe"; then
    sql=$SQL_DIR/probe-${label}.sql
    out=$OUT_DIR/probe-${label}.out
    { write_readonly_preamble; printf "SELECT '%s_rows', count(*) FROM %s;\n" "$label" "$table"; } > "$sql"
    run_setup_sql "probe_${label}" "$DB_COPY" "$sql" "$out"
    cat "$out" >> "$probe"
  else
    printf '%s_rows\tABSENT\n' "$label" >> "$probe"
  fi
}

probe_copy() {
  local sql=$SQL_DIR/probe.sql out=$OUT_DIR/probe.out source_wal_sha256=ABSENT
  {
    write_readonly_preamble
    cat <<'SQL'
SELECT 'sqlite_version', sqlite_version();
SELECT 'compile_option', compile_options FROM pragma_compile_options ORDER BY compile_options;
SELECT 'function', name, builtin, narg FROM pragma_function_list WHERE name='icu_load_collation';
.print cache_size
PRAGMA cache_size;
.print mmap_size
PRAGMA mmap_size;
.print page_size
PRAGMA page_size;
SELECT 'stat1_present', count(*) FROM sqlite_schema WHERE type='table' AND name='sqlite_stat1';
SELECT 'stat4_present', count(*) FROM sqlite_schema WHERE type='table' AND name='sqlite_stat4';
SQL
  } > "$sql"
  run_setup_sql probe "$DB_COPY" "$sql" "$out"
  probe_stat_rows sqlite_stat1 "$out"
  probe_stat_rows sqlite_stat4 "$out"
  if [ -f "$SOURCE_WAL" ]; then
    source_wal_sha256=$(sha256sum "$SOURCE_WAL" | awk '{print $1}')
  fi
  {
    printf 'family\t%s\nmode\t%s\nsource_sha256\t%s\nsource_wal_sha256\t%s\nbinary_sha256\t%s\n' \
      "$FAMILY" "$MODE" "$(sha256sum "$SOURCE_DB" | awk '{print $1}')" "$source_wal_sha256" "$(sha256sum "$SQLITE_BIN" | awk '{print $1}')"
    printf 'policy\ttimeout_s=%s\tsetup_timeout_s=%s\ttimed_iters=%s\tanalyze_copy=%s\n' "$TIMEOUT_S" "$SETUP_TIMEOUT_S" "$TIMED_ITERS" "$ANALYZE_COPY"
    cat "$out"
  } >> "$MANIFEST"
  family_validate_engine "$out"
}

derive_cells() {
  local sql=$SQL_DIR/derive.sql out=$OUT_DIR/derive.out
  { write_readonly_preamble; family_derive_sql; } > "$sql"
  run_setup_sql derive "$DB_COPY" "$sql" "$out"
  family_parse_derived "$out"
  family_record_literals > "$LITERALS"
}

prepare_copy() {
  local sql=$SQL_DIR/prepare-copy.sql out=$OUT_DIR/prepare-copy.out
  family_prepare_sql > "$sql"
  if [ -s "$sql" ]; then
    run_setup_sql family_prepare "$DB_COPY" "$sql" "$out"
  fi
  if [ "$ANALYZE_COPY" -eq 1 ]; then
    printf '.bail on\nPRAGMA analysis_limit=0;\nANALYZE main;\n' > "$sql"
    run_setup_sql analyze_copy "$DB_COPY" "$sql" "$out"
  fi
}

materialize_queries() {
  local case_id
  while IFS= read -r case_id; do
    [ -n "$case_id" ] || continue
    validate_segment "$case_id"
    family_render "$case_id" vendor > "$SQL_DIR/$case_id-vendor.query.sql"
    family_render "$case_id" candidate > "$SQL_DIR/$case_id-candidate.query.sql"
    for _arm in vendor candidate; do
      _query=$SQL_DIR/$case_id-${_arm}.query.sql
      [ -s "$_query" ] || die "empty query case=$case_id arm=$_arm"
      _head=$(awk '$0 ~ /^[[:space:]]*--/ {next} NF {print; exit}' "$_query")
      printf '%s\n' "$_head" | grep -Eiq '^[[:space:]]*(select|with)([[:space:]]|$)' || die "query is not RAW prepare form case=$case_id arm=$_arm"
      grep -Eiq 'create[[:space:]]+temp|create[[:space:]]+table' "$_query" && die "query is wrapped before prepare case=$case_id arm=$_arm"
    done
  done < <(family_cases)
  return 0
}

emit_sql_statement() {
  awk '
    { line[NR]=$0 }
    END {
      if (NR==0) exit 1
      for (i=1; i<=NR; i++) print line[i]
      last=line[NR]
      sub(/[[:space:]]+$/, "", last)
      if (last !~ /;$/) print ";"
    }
  ' "$1"
}

run_identity_gate() {
  local case_id sql out rc failures=0
  printf 'family\tcase\tstatus\tdetail\n' > "$IDENTITY"
  while IFS= read -r case_id; do
    [ -n "$case_id" ] || continue
    CURRENT_CASE=$case_id
    sql=$SQL_DIR/$case_id-identity.sql
    out=$OUT_DIR/$case_id-identity.out
    family_identity_sql "$case_id" "$SQL_DIR/$case_id-vendor.query.sql" "$SQL_DIR/$case_id-candidate.query.sql" > "$sql"
    if run_query_sql "identity_${case_id}" "$DB_COPY" "$sql" "$out"; then
      if family_identity_pass "$case_id" "$out"; then
        printf '%s\t%s\tPASS\t%s\n' "$FAMILY" "$case_id" "$(family_identity_detail "$case_id" "$out")" >> "$IDENTITY"
      else
        printf '%s\t%s\tFAIL\t%s\n' "$FAMILY" "$case_id" "$(family_identity_detail "$case_id" "$out")" >> "$IDENTITY"
        failures=$((failures + 1))
      fi
    else
      rc=$?
      if [ "$rc" -eq 124 ]; then
        printf '%s\t%s\tTIMEOUT\tcap=%ss\n' "$FAMILY" "$case_id" "$TIMEOUT_S" >> "$IDENTITY"
      else
        sed -n '1,40p' "$out" >&2
        printf '%s\t%s\tERROR\trc=%s out=%s\n' "$FAMILY" "$case_id" "$rc" "$out" >> "$IDENTITY"
      fi
      failures=$((failures + 1))
    fi
  done < <(family_cases)
  if [ "$failures" -ne 0 ]; then
    cat "$IDENTITY" >&2
    printf 'STATUS\tFAIL\treason=identity_gate\tfailures=%s\n' "$failures" > "$STATUS_FILE"
    die 'identity hard gate failed; EQP, work, and timing are blocked'
  fi
  log "IDENTITY_GATE status=PASS family=$FAMILY"
}

run_eqp() {
  local case_id arm query sql out rc
  printf 'family\tcase\tarm\tdetail\n' > "$EQP"
  while IFS= read -r case_id; do
    [ -n "$case_id" ] || continue
    CURRENT_CASE=$case_id
    for arm in vendor candidate; do
      query=$SQL_DIR/$case_id-$arm.query.sql
      sql=$SQL_DIR/$case_id-$arm.eqp.sql
      out=$OUT_DIR/$case_id-$arm.eqp.out
      { write_readonly_preamble; printf 'EXPLAIN QUERY PLAN\n'; emit_sql_statement "$query"; } > "$sql"
      if run_query_sql "eqp_${case_id}_${arm}" "$DB_COPY" "$sql" "$out"; then
        awk -F '\t' -v f="$FAMILY" -v c="$case_id" -v a="$arm" 'NF>=4 {d=$4; for(i=5;i<=NF;i++) d=d FS $i; print f FS c FS a FS d}' "$out" >> "$EQP"
      else
        rc=$?
        [ "$rc" -eq 124 ] && die "EQP timeout case=$case_id arm=$arm"
        die "EQP failed case=$case_id arm=$arm rc=$rc out=$out"
      fi
    done
  done < <(family_cases)
}

case_policy() {
  local policy
  policy=$(family_canary_policy "$1")
  case "$policy" in
    LATENCY|PLAN|WORK) printf '%s\n' "$policy" ;;
    *) die "invalid canary policy case=$1 policy=$policy" ;;
  esac
}

run_plan_gates() {
  local case_id policy vendor candidate vendor_rows candidate_rows failures=0
  printf 'family\tcase\tstatus\tdetail\n' > "$PLANS"
  while IFS= read -r case_id; do
    [ -n "$case_id" ] || continue
    policy=$(case_policy "$case_id")
    if [ "$policy" != PLAN ]; then
      printf '%s\t%s\tNOT_APPLICABLE\tpolicy=%s\n' "$FAMILY" "$case_id" "$policy" >> "$PLANS"
      continue
    fi
    vendor=$OUT_DIR/$case_id-vendor.plan
    candidate=$OUT_DIR/$case_id-candidate.plan
    awk -F '\t' -v f="$FAMILY" -v c="$case_id" '$1==f && $2==c && $3=="vendor" {d=$4; for(i=5;i<=NF;i++) d=d FS $i; print d}' "$EQP" > "$vendor"
    awk -F '\t' -v f="$FAMILY" -v c="$case_id" '$1==f && $2==c && $3=="candidate" {d=$4; for(i=5;i<=NF;i++) d=d FS $i; print d}' "$EQP" > "$candidate"
    vendor_rows=$(wc -l < "$vendor" | tr -d ' ')
    candidate_rows=$(wc -l < "$candidate" | tr -d ' ')
    if [ "$vendor_rows" -gt 0 ] && [ "$candidate_rows" -gt 0 ] && cmp -s "$vendor" "$candidate"; then
      printf '%s\t%s\tPASS\tvendor_rows=%s candidate_rows=%s exact_detail_match=1\n' "$FAMILY" "$case_id" "$vendor_rows" "$candidate_rows" >> "$PLANS"
    else
      printf '%s\t%s\tFAIL\tvendor_rows=%s candidate_rows=%s exact_detail_match=0 vendor=%s candidate=%s\n' "$FAMILY" "$case_id" "$vendor_rows" "$candidate_rows" "$vendor" "$candidate" >> "$PLANS"
      failures=$((failures + 1))
    fi
  done < <(family_cases)
  if [ "$failures" -ne 0 ]; then
    cat "$PLANS" >&2
    printf 'STATUS\tFAIL\treason=plan_gate\tfailures=%s\n' "$failures" > "$STATUS_FILE"
    die 'plan identity hard gate failed; work and timing are blocked'
  fi
}

write_work_sql() {
  local query=$1
  write_readonly_preamble
  printf '.stats stmt\n'
  emit_sql_statement "$query"
  printf '.stats off\n'
}

extract_stmt_counter() {
  local key=$1 out=$2
  awk -F ':' -v key="$key" '
    {
      label=$1
      gsub(/^[[:space:]]+|[[:space:]]+$/, "", label)
      if (label==key) {
        value=$2
        gsub(/^[[:space:]]+|[[:space:]]+$/, "", value)
        if (value !~ /^[0-9]+$/) exit 2
        found++
      }
    }
    END { if (found!=1) exit 1; print value }
  ' "$out"
}

run_work_case() {
  local case_id=$1 arm query sql out rc fullscan vm
  local vendor_fullscan=NA candidate_fullscan=NA vendor_vm=NA candidate_vm=NA status=PASS detail
  CURRENT_CASE=$case_id
  for arm in vendor candidate; do
    query=$SQL_DIR/$case_id-$arm.query.sql
    sql=$SQL_DIR/$case_id-$arm.work.sql
    out=$OUT_DIR/$case_id-$arm.work.out
    write_work_sql "$query" > "$sql"
    if run_query_sql "work_${case_id}_${arm}" "$DB_COPY" "$sql" "$out"; then
      fullscan=$(extract_stmt_counter 'Fullscan Steps' "$out") || die "missing Fullscan Steps counter case=$case_id arm=$arm out=$out"
      vm=$(extract_stmt_counter 'Virtual Machine Steps' "$out") || die "missing Virtual Machine Steps counter case=$case_id arm=$arm out=$out"
      if [ "$arm" = vendor ]; then
        vendor_fullscan=$fullscan
        vendor_vm=$vm
      else
        candidate_fullscan=$fullscan
        candidate_vm=$vm
      fi
    else
      rc=$?
      if [ "$rc" -eq 124 ]; then
        status=TIMEOUT
      else
        sed -n '1,40p' "$out" >&2
        die "work query failed case=$case_id arm=$arm rc=$rc out=$out"
      fi
    fi
  done
  if [ "$status" = PASS ]; then
    if family_work_pass "$case_id" "$vendor_fullscan" "$candidate_fullscan" "$vendor_vm" "$candidate_vm"; then
      detail='family_assertion=pass'
    else
      status=FAIL
      detail='family_assertion=fail'
    fi
  else
    detail="counter_collection=$status"
  fi
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$FAMILY" "$case_id" "$vendor_fullscan" "$candidate_fullscan" "$vendor_vm" "$candidate_vm" "$status" "$detail" >> "$WORK"
}

run_work_assertions() {
  local case_id policy
  printf 'family\tcase\tvendor_fullscan_steps\tcandidate_fullscan_steps\tvendor_vm_steps\tcandidate_vm_steps\tstatus\tdetail\n' > "$WORK"
  while IFS= read -r case_id; do
    [ -n "$case_id" ] || continue
    policy=$(case_policy "$case_id")
    if [ "$policy" = WORK ]; then
      run_work_case "$case_id"
    fi
  done < <(family_cases)
}

prewarm_copy() {
  local case_id=$1 rc
  log "PHASE phase=prewarm_${case_id} cap=${SETUP_TIMEOUT_S}s"
  set +e
  timeout "${SETUP_TIMEOUT_S}s" cat "$DB_COPY" "${DB_COPY}-wal" "${DB_COPY}-shm" >/dev/null 2>&1
  rc=$?
  set -e
  case "$rc" in 0|1) ;; 124) die "BLOCKED prewarm timeout case=$case_id" ;; *) die "prewarm failed case=$case_id rc=$rc" ;; esac
}

write_execution_sql() {
  local query=$1 timer=$2
  write_readonly_preamble
  if [ "$timer" -eq 1 ]; then printf '.timer on\n'; fi
  emit_sql_statement "$query"
  if [ "$timer" -eq 1 ]; then printf '.timer off\n'; fi
}

extract_real() {
  awk '/Run Time:/ {for(i=1;i<=NF;i++) if($i=="real") {print $(i+1); found=1}} END{if(!found) exit 1}' "$1"
}

median_values() {
  sort -n "$1" | awk '{a[++n]=$1} END {if(!n) exit 1; if(n%2) printf "%.6f\n",a[(n+1)/2]; else printf "%.6f\n",(a[n/2]+a[n/2+1])/2}'
}

run_timing_case() {
  local case_id=$1 pair=$OUT_DIR/timing-$1 arm query sql out rc real iter order
  local vendor_timeout=0 candidate_timeout=0 vendor_median candidate_median ratio verdict role
  # Used by sourced family data when parameters vary per case.
  # shellcheck disable=SC2034
  CURRENT_CASE=$case_id
  mkdir -p "$pair"
  : > "$pair/vendor.values"
  : > "$pair/candidate.values"
  prewarm_copy "$case_id"
  for arm in vendor candidate; do
    query=$SQL_DIR/$case_id-$arm.query.sql
    sql=$pair/$arm-warm.sql
    out=$pair/$arm-warm.out
    write_execution_sql "$query" 0 > "$sql"
    if run_query_sql "warm_${case_id}_${arm}" "$DB_COPY" "$sql" "$out"; then
      printf 'WARM\t%s\t%s\t0\tOK\tNA\t%s\n' "$case_id" "$arm" "$out" >> "$TIMINGS"
    else
      rc=$?
      [ "$rc" -eq 124 ] || die "warm failed case=$case_id arm=$arm rc=$rc"
      [ "$arm" = vendor ] && vendor_timeout=1 || candidate_timeout=1
      printf 'TIMEOUT\t%s\t%s\t0\tTIMEOUT\tNA\t%s\n' "$case_id" "$arm" "$out" >> "$TIMINGS"
    fi
  done
  iter=1
  while [ "$iter" -le "$TIMED_ITERS" ]; do
    [ $((iter % 2)) -eq 1 ] && order='vendor candidate' || order='candidate vendor'
    for arm in $order; do
      if { [ "$arm" = vendor ] && [ "$vendor_timeout" -eq 1 ]; } || { [ "$arm" = candidate ] && [ "$candidate_timeout" -eq 1 ]; }; then
        printf 'SKIP\t%s\t%s\t%s\tPRIOR_TIMEOUT\tNA\tNA\n' "$case_id" "$arm" "$iter" >> "$TIMINGS"
        continue
      fi
      query=$SQL_DIR/$case_id-$arm.query.sql
      sql=$pair/$arm-$iter.sql
      out=$pair/$arm-$iter.out
      write_execution_sql "$query" 1 > "$sql"
      if run_query_sql "timed_${case_id}_${arm}_${iter}" "$DB_COPY" "$sql" "$out"; then
        real=$(extract_real "$out") || die "missing timer case=$case_id arm=$arm iter=$iter"
        printf '%s\n' "$real" >> "$pair/$arm.values"
        printf 'TIME\t%s\t%s\t%s\tOK\t%s\t%s\n' "$case_id" "$arm" "$iter" "$real" "$out" >> "$TIMINGS"
      else
        rc=$?
        [ "$rc" -eq 124 ] || die "timed query failed case=$case_id arm=$arm iter=$iter rc=$rc"
        [ "$arm" = vendor ] && vendor_timeout=1 || candidate_timeout=1
        printf 'TIMEOUT\t%s\t%s\t%s\tTIMEOUT\tNA\t%s\n' "$case_id" "$arm" "$iter" "$out" >> "$TIMINGS"
      fi
    done
    [ "$vendor_timeout" -eq 1 ] && [ "$candidate_timeout" -eq 1 ] && break
    iter=$((iter + 1))
  done
  [ "$vendor_timeout" -eq 1 ] && vendor_median=TIMEOUT || vendor_median=$(median_values "$pair/vendor.values")
  [ "$candidate_timeout" -eq 1 ] && candidate_median=TIMEOUT || candidate_median=$(median_values "$pair/candidate.values")
  if [ "$candidate_median" = TIMEOUT ] && [ "$vendor_median" != TIMEOUT ]; then ratio=NA; verdict=REGRESSION
  elif [ "$vendor_median" = TIMEOUT ] && [ "$candidate_median" != TIMEOUT ]; then ratio=NA; verdict=WIN
  elif [ "$vendor_median" = TIMEOUT ]; then ratio=NA; verdict=INCONCLUSIVE_BOTH_SLOW
  else ratio=$(awk -v c="$candidate_median" -v v="$vendor_median" 'BEGIN{if(v==0)print "NA";else printf "%.3f",c/v}'); verdict=MEASURED
  fi
  role=$(family_case_role "$case_id")
  case "$role" in SHIPPED|COMPARISON) ;; *) die "invalid case role case=$case_id role=$role" ;; esac
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$FAMILY" "$case_id" "$vendor_median" "$candidate_median" "$ratio" "$verdict" "$role" >> "$MEDIANS"
}

run_timings() {
  local case_id
  printf 'record\tcase\tarm\titeration\tstatus\treal_seconds\toutput\n' > "$TIMINGS"
  printf 'family\tcase\tvendor_median_seconds\tcandidate_median_seconds\tcandidate_over_vendor\tverdict\trole\n' > "$MEDIANS"
  while IFS= read -r case_id; do [ -n "$case_id" ] && run_timing_case "$case_id"; done < <(family_cases)
}

write_case_verdicts() {
  local case_id policy role row vendor_median candidate_median ratio timing_verdict _timing_role status detail
  printf 'family\tcase\tpolicy\trole\tstatus\tdetail\n' > "$VERDICTS"
  while IFS= read -r case_id; do
    [ -n "$case_id" ] || continue
    policy=$(case_policy "$case_id")
    role=$(family_case_role "$case_id")
    case "$role" in SHIPPED|COMPARISON) ;; *) die "invalid case role case=$case_id role=$role" ;; esac
    case "$policy" in
      LATENCY)
        row=$(awk -F '\t' -v c="$case_id" '$2==c {print; exit}' "$MEDIANS")
        [ -n "$row" ] || die "missing median verdict input case=$case_id"
        IFS=$'\t' read -r _family _case vendor_median candidate_median ratio timing_verdict _timing_role <<< "$row"
        if [ "$candidate_median" = TIMEOUT ] || [ "$ratio" = NA ] || awk -v r="$ratio" -v max="$CANARY_MAX_RATIO" 'BEGIN { exit !(r+0>max) }'; then
          status=FAIL
        else
          status=PASS
        fi
        detail="candidate_over_vendor=$ratio max_ratio=$CANARY_MAX_RATIO timing_verdict=$timing_verdict"
        ;;
      PLAN)
        status=$(awk -F '\t' -v c="$case_id" '$2==c {print $3; exit}' "$PLANS")
        [ "$status" = PASS ] || die "missing successful plan gate case=$case_id status=${status:-empty}"
        detail='exact_eqp_detail_match=1 latency=informational'
        ;;
      WORK)
        status=$(awk -F '\t' -v c="$case_id" '$2==c {print $7; exit}' "$WORK")
        [ -n "$status" ] || die "missing work verdict case=$case_id"
        detail=$(awk -F '\t' -v c="$case_id" '$2==c {printf "vendor_fullscan=%s candidate_fullscan=%s vendor_vm=%s candidate_vm=%s",$3,$4,$5,$6; exit}' "$WORK")
        ;;
    esac
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$FAMILY" "$case_id" "$policy" "$role" "$status" "$detail" >> "$VERDICTS"
  done < <(family_cases)
}

write_status() {
  local failures=0 comparison_failures=0
  if [ "$MODE" = canary ]; then
    failures=$(awk -F '\t' 'NR>1 && $4=="SHIPPED" && $5!="PASS" {n++} END{print n+0}' "$VERDICTS")
    comparison_failures=$(awk -F '\t' 'NR>1 && $4=="COMPARISON" && $5!="PASS" {n++} END{print n+0}' "$VERDICTS")
    if [ "$failures" -eq 0 ]; then
      printf 'STATUS\tPASS\tmode=canary\tfamily=%s\tcomparison_failures=%s\tmax_ratio=%s\n' "$FAMILY" "$comparison_failures" "$CANARY_MAX_RATIO" > "$STATUS_FILE"
    else
      printf 'STATUS\tFAIL\tmode=canary\tfamily=%s\tfailures=%s\tcomparison_failures=%s\tmax_ratio=%s\n' "$FAMILY" "$failures" "$comparison_failures" "$CANARY_MAX_RATIO" > "$STATUS_FILE"
    fi
  else
    printf 'STATUS\tDONE\tmode=measure\tfamily=%s\n' "$FAMILY" > "$STATUS_FILE"
  fi
  cat "$STATUS_FILE"
  [ "$MODE" != canary ] || [ "$failures" -eq 0 ]
}

emit_outputs() {
  local name file
  for name in manifest literals identity eqp plans work timings medians verdicts status; do
    file=$OUT_DIR/$name.tsv
    printf 'BEGIN_OUTPUT\t%s\n' "$name"
    cat "$file"
    printf 'END_OUTPUT\t%s\n' "$name"
  done
}

main() {
  local status_rc=0
  log "START mode=$MODE family=$FAMILY run_dir=$RUN_DIR"
  disk_preflight
  log 'PHASE_DONE phase=disk_preflight'
  copy_database
  log 'PHASE_DONE phase=copy'
  probe_copy
  log 'PHASE_DONE phase=probe'
  derive_cells
  log 'PHASE_DONE phase=derive'
  prepare_copy
  log 'PHASE_DONE phase=prepare_copy'
  materialize_queries
  log 'PHASE_DONE phase=materialize_queries'
  family_fixture_check
  log 'PHASE_DONE phase=fixture'
  run_identity_gate
  run_eqp
  run_plan_gates
  run_work_assertions
  run_timings
  write_case_verdicts
  write_status || status_rc=$?
  emit_outputs
  log "STATUS DONE mode=$MODE family=$FAMILY cleanup=trap"
  return "$status_rc"
}

main
