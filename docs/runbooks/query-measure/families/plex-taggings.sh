# shellcheck shell=bash disable=SC2034
# Query-family data for Plex taggings membership. Sourced by query-measure.sh.

family_configure() {
  require_env PLEX_DB_SOURCE
  SOURCE_DB=$PLEX_DB_SOURCE
  PREPARE_PLEX_INDEXES=${PREPARE_PLEX_INDEXES:-0}
  require_uint PREPARE_PLEX_INDEXES
  case "$PREPARE_PLEX_INDEXES" in 0|1) ;; *) die 'PREPARE_PLEX_INDEXES must be 0 or 1' ;; esac
  TAG_ID=
  TAG_SECTION_ID=
}
family_all_cases() { printf '%s\n' taggings-count taggings-grouped; }
family_cases() { family_all_cases; }

family_contract_check() {
  local source=${REPO_ROOT}/src/plex_fts_rewrite.c
  grep -F 'mode=taggings' "$source" >/dev/null || grep -F '"taggings"' "$source" >/dev/null || die 'Plex taggings source contract missing'
  [ "$(family_all_cases | wc -l | tr -d ' ')" -eq 2 ] || die 'Plex taggings arm matrix incomplete'
}

family_parameters() { :; }
family_derive_sql() {
  cat <<'SQL'
SELECT 'TAG_CELL',T.tag_id,M.library_section_id,count(*) AS n
FROM taggings AS T JOIN metadata_items AS M ON M.id=T.metadata_item_id
WHERE T.tag_id IS NOT NULL AND M.library_section_id IS NOT NULL
GROUP BY T.tag_id,M.library_section_id ORDER BY n DESC,T.tag_id,M.library_section_id LIMIT 1;
SQL
}
family_parse_derived() {
  local out=$1 extra
  IFS=$'\t' read -r _tag TAG_ID TAG_SECTION_ID TAG_ROWS extra < "$out"
  [ -z "${extra:-}" ] || die 'Plex taggings derivation returned extra columns'
  for _v in "$TAG_ID" "$TAG_SECTION_ID" "$TAG_ROWS"; do case "$_v" in ''|*[!0-9]*) die 'Plex taggings derivation returned non-integer data' ;; esac; done
}
family_record_literals() { printf 'tag_cell\ttag_id=%s\tlibrary_section_id=%s\trows=%s\tderived=1\n' "$TAG_ID" "$TAG_SECTION_ID" "$TAG_ROWS"; }
family_prepare_sql() {
  [ "$PREPARE_PLEX_INDEXES" -eq 1 ] || return 0
  cat <<'SQL'
.bail on
CREATE INDEX IF NOT EXISTS idx_dshadow_taggings_tag_id_metadata_item_id ON taggings (tag_id, metadata_item_id);
SQL
}

family_render() {
  local case_id=$1 arm=$2
  case "$case_id" in
    taggings-count|taggings-grouped) ;;
    *) die "unknown Plex taggings case: $case_id" ;;
  esac
  if [ "$case_id" = taggings-count ]; then
    if [ "$arm" = vendor ]; then
      printf 'select count(*) from (select distinct(metadata_items.id) from metadata_items left join taggings on taggings.metadata_item_id=metadata_items.id left join tags on taggings.tag_id=tags.id where metadata_items.library_section_id in (%s) and (metadata_items.metadata_type=1 and tags.id=%s));\n' "$TAG_SECTION_ID" "$TAG_ID"
    else
      printf 'select count(*) from (select distinct(metadata_items.id) from metadata_items left join taggings on taggings.metadata_item_id=metadata_items.id left join tags on taggings.tag_id=tags.id where metadata_items.library_section_id in (%s) and (metadata_items.metadata_type=1 and tags.id=%s AND metadata_items.id IN (SELECT metadata_item_id FROM taggings WHERE tag_id=%s)));\n' "$TAG_SECTION_ID" "$TAG_ID" "$TAG_ID"
    fi
  elif [ "$arm" = vendor ]; then
    printf 'select metadata_items.id from metadata_items left join taggings on taggings.metadata_item_id=metadata_items.id left join tags on taggings.tag_id=tags.id where metadata_items.library_section_id in (%s) and (metadata_items.metadata_type=1 and tags.id=%s) order by metadata_items.id;\n' "$TAG_SECTION_ID" "$TAG_ID"
  else
    printf 'select metadata_items.id from metadata_items left join taggings on taggings.metadata_item_id=metadata_items.id left join tags on taggings.tag_id=tags.id where metadata_items.library_section_id in (%s) and (metadata_items.metadata_type=1 and tags.id=%s AND metadata_items.id IN (SELECT metadata_item_id FROM taggings WHERE tag_id=%s)) order by metadata_items.id;\n' "$TAG_SECTION_ID" "$TAG_ID" "$TAG_ID"
  fi
}

taggings_core() { sed '$s/;[[:space:]]*$//' "$1"; }
family_identity_sql() {
  local _case=$1 vendor=$2 candidate=$3
  write_readonly_preamble
  cat <<SQL
WITH vendor_identity AS MATERIALIZED (SELECT *,count(*) AS identity_n FROM ($(taggings_core "$vendor")) GROUP BY 1),
candidate_identity AS MATERIALIZED (SELECT *,count(*) AS identity_n FROM ($(taggings_core "$candidate")) GROUP BY 1),
metrics AS (
 SELECT (SELECT count(*) FROM vendor_identity) vendor_rows,(SELECT count(*) FROM candidate_identity) candidate_rows,
 (SELECT count(*) FROM (SELECT * FROM vendor_identity EXCEPT SELECT * FROM candidate_identity)) missing_candidate,
 (SELECT count(*) FROM (SELECT * FROM candidate_identity EXCEPT SELECT * FROM vendor_identity)) extra_candidate
)
SELECT 'IDENTITY',CASE WHEN vendor_rows>0 AND vendor_rows=candidate_rows AND missing_candidate=0 AND extra_candidate=0 THEN 'PASS' ELSE 'FAIL' END,vendor_rows,candidate_rows,missing_candidate,extra_candidate FROM metrics;
SQL
}
family_identity_pass() { awk -F '\t' '$1=="IDENTITY" && $2=="PASS"{ok=1} END{exit !ok}' "$2"; }
family_identity_detail() { tr '\n' ';' < "$2" | sed 's/;$//'; }
family_fixture_check() { :; }
