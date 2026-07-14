# shellcheck shell=bash disable=SC2016,SC2034
# Query-family data for the Plex GUID LIKE NULL guard.
# This family requires an ICU-enabled SQLite engine because ICU overloads like().

family_configure() {
  require_env PLEX_DB_SOURCE
  SOURCE_DB=$PLEX_DB_SOURCE
  GUID_VENDOR_SQL='SELECT mt.`id` FROM metadata_items mt WHERE (mt.`guid` LIKE :1) LIMIT :2'
  GUID_CANDIDATE_SQL='SELECT mt.`id` FROM metadata_items mt WHERE :1 IS NOT NULL AND (mt.`guid` LIKE :1) LIMIT :2'
  GUID_PATTERN_HEX=
  GUID_PATTERN_TEXT=
  GUID_PATTERN_ROWS=
  GUID_PATTERN_LENGTH=
  GUID_LIMIT=
}

family_all_cases() { printf '%s\n' null-pattern prefix-pattern; }
family_cases() { family_all_cases; }

family_case_role() {
  case "$1" in
    null-pattern|prefix-pattern) printf 'SHIPPED\n' ;;
    *) die "unknown Plex GUID LIKE case role: $1" ;;
  esac
}

family_canary_policy() {
  case "$1" in
    null-pattern) printf 'WORK\n' ;;
    prefix-pattern) printf 'PLAN\n' ;;
    *) die "unknown Plex GUID LIKE canary policy: $1" ;;
  esac
}

guid_source_string() {
  local source=$1 name=$2
  awk -v name="$name" '
    $0 ~ "static const char " name "\\[\\] =" {
      getline
      line=$0
      sub(/^[[:space:]]*"/, "", line)
      sub(/";[[:space:]]*$/, "", line)
      print line
      found=1
      exit
    }
    END { if (!found) exit 1 }
  ' "$source"
}

family_contract_check() {
  local source=${REPO_ROOT}/src/plex_fts_rewrite.c source_vendor source_candidate gate_block case_id policy role cases
  [ -f "$source" ] || die "missing rewrite source for contract check: $source"
  source_vendor=$(guid_source_string "$source" PLEX_GUID_LIKE_SQL) || die 'Plex GUID LIKE vendor SQL source contract missing'
  source_candidate=$(guid_source_string "$source" PLEX_GUID_LIKE_REWRITTEN_SQL) || die 'Plex GUID LIKE candidate SQL source contract missing'
  [ "$source_vendor" = "$GUID_VENDOR_SQL" ] || die 'Plex GUID LIKE vendor SQL source contract drifted'
  [ "$source_candidate" = "$GUID_CANDIDATE_SQL" ] || die 'Plex GUID LIKE candidate SQL source contract drifted'
  grep -Fx '    X(PLEX_GUID_LIKE, "plex", "guid+like-null", "plex_fts_rewrite", 0) \' "${REPO_ROOT}/src/rewrite_modes.h" >/dev/null || die 'Plex GUID LIKE mode catalogue contract drifted'
  gate_block=$(sed -n '/static void plex_guid_like_rewrite_init_once/,/^}/p' "$source")
  printf '%s\n' "$gate_block" | grep -F 'getenv("SQLITE3_DISABLE_PLEX_GUID_LIKE_REWRITE")' >/dev/null || die 'Plex GUID LIKE gate source contract missing'
  printf '%s\n' "$gate_block" | grep -F '(value && strcmp(value, "0") == 0) ? 1 : 0' >/dev/null || die 'Plex GUID LIKE opt-in gate source contract drifted'
  cases=$(family_all_cases | wc -l | tr -d ' ')
  [ "$cases" -eq 2 ] || die "Plex GUID LIKE arm matrix incomplete: expected=2 actual=$cases"
  for case_id in null-pattern prefix-pattern; do
    family_all_cases | grep -Fx "$case_id" >/dev/null || die "missing Plex GUID LIKE measurement arm: $case_id"
    policy=$(family_canary_policy "$case_id")
    role=$(family_case_role "$case_id")
    case "$case_id:$policy:$role" in
      null-pattern:WORK:SHIPPED|prefix-pattern:PLAN:SHIPPED) ;;
      *) die "Plex GUID LIKE arm declaration drift: case=$case_id policy=$policy role=$role" ;;
    esac
  done
}

family_validate_engine() {
  local probe=$1
  awk -F '\t' '
    $1=="compile_option" && $2=="ENABLE_ICU" {compile=1}
    $1=="function" && $2=="icu_load_collation" && $4==2 {fn=1}
    END {exit !(compile && fn)}
  ' "$probe" || die 'plex-guid-like requires a runtime ICU-enabled SQL engine; configured SQLITE_BIN lacks ENABLE_ICU or icu_load_collation()'
}

family_parameters() {
  local escaped_pattern
  printf '.parameter init\n'
  [ -n "${CURRENT_CASE:-}" ] || return 0
  [ -n "${GUID_LIMIT:-}" ] || die "GUID LIKE parameters requested before derivation case=$CURRENT_CASE"
  case "$CURRENT_CASE" in
    null-pattern) printf '.parameter set :1 null\n' ;;
    prefix-pattern)
      escaped_pattern=${GUID_PATTERN_TEXT//\'/\'\'}
      printf ".parameter set :1 '%s'\n" "$escaped_pattern"
      ;;
    *) die "unknown Plex GUID LIKE parameter case: $CURRENT_CASE" ;;
  esac
  printf '.parameter set :2 %s\n' "$GUID_LIMIT"
}

family_derive_sql() {
  cat <<'SQL'
WITH prefixes AS (
  SELECT substr(guid,1,instr(guid,'://')+2)||'%' AS pattern
  FROM metadata_items
  WHERE guid IS NOT NULL AND instr(guid,'://')>0
), cell AS (
  SELECT pattern,count(*) AS source_rows
  FROM prefixes
  GROUP BY pattern
  ORDER BY source_rows DESC,pattern
  LIMIT 1
)
SELECT 'GUID_CELL',hex(pattern),
       (SELECT count(*) FROM metadata_items WHERE guid LIKE pattern) AS match_rows,
       length(pattern) AS pattern_length,
       (SELECT count(*) FROM (SELECT mt.id FROM metadata_items mt WHERE mt.guid LIKE pattern LIMIT 1)) AS limit_value,
       pattern
FROM cell;
SQL
}

family_parse_derived() {
  local out=$1 extra hex_tail hex_byte
  IFS=$'\t' read -r _tag GUID_PATTERN_HEX GUID_PATTERN_ROWS GUID_PATTERN_LENGTH GUID_LIMIT GUID_PATTERN_TEXT extra < <(awk -F '\t' '$1=="GUID_CELL" {print; exit}' "$out")
  [ -z "${extra:-}" ] || die 'Plex GUID LIKE derivation returned extra columns'
  [ "${_tag:-}" = GUID_CELL ] || die 'Plex GUID LIKE derivation returned no prefix cell'
  case "$GUID_PATTERN_HEX" in ''|*[!0-9A-F]*) die 'Plex GUID LIKE derivation returned an unsafe hex pattern' ;; esac
  [ $(( ${#GUID_PATTERN_HEX} % 2 )) -eq 0 ] || die 'Plex GUID LIKE derivation returned odd-length hex'
  hex_tail=$GUID_PATTERN_HEX
  while [ -n "$hex_tail" ]; do
    hex_byte=${hex_tail:0:2}
    case "$hex_byte" in 0[0-9A-F]|1[0-9A-F]|7F) die 'Plex GUID LIKE derivation returned a control character' ;; esac
    hex_tail=${hex_tail:2}
  done
  for _v in "$GUID_PATTERN_ROWS" "$GUID_PATTERN_LENGTH" "$GUID_LIMIT"; do
    case "$_v" in ''|*[!0-9]*) die 'Plex GUID LIKE derivation returned non-integer metadata' ;; esac
  done
  [ -n "$GUID_PATTERN_TEXT" ] || die 'Plex GUID LIKE derivation returned empty pattern text'
  [ "$GUID_PATTERN_ROWS" -gt 0 ] || die 'Plex GUID LIKE prefix cell matches no rows'
  [ "$GUID_PATTERN_LENGTH" -gt 1 ] || die 'Plex GUID LIKE prefix cell is empty'
  [ "$GUID_LIMIT" -gt 0 ] || die 'Plex GUID LIKE derived LIMIT is not positive'
}

family_record_literals() {
  printf 'guid_prefix\tpattern_hex=%s\tmatch_rows=%s\tpattern_length=%s\tderived=1\n' "$GUID_PATTERN_HEX" "$GUID_PATTERN_ROWS" "$GUID_PATTERN_LENGTH"
  printf 'guid_limit\tvalue=%s\tderived=1\n' "$GUID_LIMIT"
  printf 'null_pattern\tvalue=NULL\tproduction_semantic=1\n'
}

family_prepare_sql() { :; }

family_render() {
  local case_id=$1 arm=$2
  case "$case_id" in null-pattern|prefix-pattern) ;; *) die "unknown Plex GUID LIKE case: $case_id" ;; esac
  case "$arm" in
    vendor) printf '%s\n' "$GUID_VENDOR_SQL" ;;
    candidate) printf '%s\n' "$GUID_CANDIDATE_SQL" ;;
    *) die "unknown Plex GUID LIKE arm: $arm" ;;
  esac
}

guid_like_core() { sed '$s/;[[:space:]]*$//' "$1"; }

family_identity_sql() {
  local case_id=$1 vendor=$2 candidate=$3 require_nonempty
  case "$case_id" in
    null-pattern) require_nonempty=0 ;;
    prefix-pattern) require_nonempty=1 ;;
    *) die "unknown Plex GUID LIKE identity case: $case_id" ;;
  esac
  write_readonly_preamble
  cat <<SQL
WITH vendor_identity AS MATERIALIZED (
  SELECT id,count(*) AS identity_n FROM ($(guid_like_core "$vendor")) GROUP BY id
), candidate_identity AS MATERIALIZED (
  SELECT id,count(*) AS identity_n FROM ($(guid_like_core "$candidate")) GROUP BY id
), metrics AS (
  SELECT (SELECT count(*) FROM vendor_identity) AS vendor_rows,
         (SELECT count(*) FROM candidate_identity) AS candidate_rows,
         (SELECT count(*) FROM (SELECT * FROM vendor_identity EXCEPT SELECT * FROM candidate_identity)) AS missing_candidate,
         (SELECT count(*) FROM (SELECT * FROM candidate_identity EXCEPT SELECT * FROM vendor_identity)) AS extra_candidate
)
SELECT 'IDENTITY',
       CASE WHEN vendor_rows=candidate_rows AND missing_candidate=0 AND extra_candidate=0
                  AND (${require_nonempty}=0 OR vendor_rows>0)
            THEN 'PASS' ELSE 'FAIL' END,
       vendor_rows,candidate_rows,missing_candidate,extra_candidate,${require_nonempty} AS require_nonempty
FROM metrics;
SQL
}

family_identity_pass() { awk -F '\t' '$1=="IDENTITY" && $2=="PASS" {ok=1} END {exit !ok}' "$2"; }
family_identity_detail() { tr '\n' ';' < "$2" | sed 's/;$//'; }

family_work_pass() {
  local case_id=$1 vendor_fullscan=$2 candidate_fullscan=$3 vendor_vm=$4 candidate_vm=$5
  [ "$case_id" = null-pattern ] || die "unexpected Plex GUID LIKE work assertion case: $case_id"
  awk -v vf="$vendor_fullscan" -v cf="$candidate_fullscan" -v vv="$vendor_vm" -v cv="$candidate_vm" '
    BEGIN { exit !(vf>0 && cf==0 && vv>0 && cv>0 && cv*100<=vv) }
  '
}

family_fixture_check() {
  local sql=$SQL_DIR/plex-guid-like-null-fixture.sql out=$OUT_DIR/plex-guid-like-null-fixture.out
  cat > "$sql" <<'SQL'
.bail on
.headers off
.mode tabs
PRAGMA query_only=1;
WITH metadata_items(id,guid) AS (VALUES(1,'x')),
vendor AS (SELECT id FROM metadata_items WHERE guid LIKE NULL),
candidate AS (SELECT id FROM metadata_items WHERE NULL IS NOT NULL AND guid LIKE NULL)
SELECT 'GUID_NULL_FIXTURE',
       CASE WHEN (SELECT count(*) FROM vendor)=0 AND (SELECT count(*) FROM candidate)=0 THEN 'PASS' ELSE 'FAIL' END,
       (SELECT count(*) FROM vendor),(SELECT count(*) FROM candidate);
SQL
  run_query_sql guid_null_fixture :memory: "$sql" "$out" || die 'Plex GUID LIKE NULL fixture execution failed'
  grep -Fx $'GUID_NULL_FIXTURE\tPASS\t0\t0' "$out" >/dev/null || die "Plex GUID LIKE NULL empty-identity fixture did not fire: $out"
}
