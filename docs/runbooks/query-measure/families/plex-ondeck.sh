# shellcheck shell=bash disable=SC2034
# Query-family data for Plex On-Deck. Sourced by query-measure.sh.

family_configure() {
  require_env PLEX_DB_SOURCE
  SOURCE_DB=$PLEX_DB_SOURCE
  IDLIST_SIZE=${IDLIST_SIZE:-50}
  THRESHOLD_RANK=${THRESHOLD_RANK:-100}
  PREPARE_PLEX_INDEXES=${PREPARE_PLEX_INDEXES:-0}
  require_uint IDLIST_SIZE
  require_uint THRESHOLD_RANK
  require_uint PREPARE_PLEX_INDEXES
  [ "$IDLIST_SIZE" -ge 1 ] || die 'IDLIST_SIZE must be at least 1'
  [ "$THRESHOLD_RANK" -ge 1 ] || die 'THRESHOLD_RANK must be at least 1'
  case "$PREPARE_PLEX_INDEXES" in 0|1) ;; *) die 'PREPARE_PLEX_INDEXES must be 0 or 1' ;; esac
  ACCOUNT_ID=
  SECTION_ID=
  IDLIST=
  THRESHOLD=
}

family_cases() {
  printf '%s\n' id-list-cross id-list-join threshold-cross threshold-join
}

family_case_role() {
  case "$1" in
    id-list-join|threshold-join) printf 'SHIPPED\n' ;;
    id-list-cross|threshold-cross) printf 'COMPARISON\n' ;;
    *) die "unknown Plex On-Deck case role: $1" ;;
  esac
}

# Reconstruct the production candidate byte stream from the live C fragments.
# The harness adds one file-framing LF; comparison arms change only the
# source-selected metadata_item_views JOIN line.
ondeck_source_fragment() {
  local source=$1 name=$2
  awk -v declaration="static const char ${name}[] =" '
    function consume(raw, line, terminal) {
      line=raw
      if (line !~ /^[[:space:]]*"/) return -1
      sub(/^[[:space:]]*"/, "", line)
      terminal=(line ~ /";[[:space:]]*$/)
      if (terminal) sub(/";[[:space:]]*$/, "", line)
      else if (line ~ /"[[:space:]]*$/) sub(/"[[:space:]]*$/, "", line)
      else return -1
      gsub(/\\"/, "\"", line)
      print line
      if (terminal) {
        complete=1
        return 1
      }
      return 0
    }
    index($0,declaration) {
      rest=substr($0,index($0,declaration)+length(declaration))
      found=1
      reading=1
      if (rest ~ /^[[:space:]]*"/) {
        status=consume(rest)
        if (status < 0) exit 2
        if (status > 0) exit
      }
      next
    }
    reading {
      status=consume($0)
      if (status < 0) exit 2
      if (status > 0) exit
    }
    END { if (!found || !complete) exit 1 }
  ' "$source" | while IFS= read -r fragment; do printf '%b' "$fragment"; done
}

ondeck_capture_source_fragment() {
  local source=$1 name=$2 outvar=$3 marker=__DSHADOW_ONDECK_FRAGMENT_END__ value
  value=$(ondeck_source_fragment "$source" "$name" || exit 1; printf '%s' "$marker") || return 1
  case "$value" in *"$marker") ;; *) return 1 ;; esac
  value=${value%"$marker"}
  printf -v "$outvar" '%s' "$value"
}

ondeck_source_append_sequence() {
  awk '
    /^static char \*plex_build_ondeck_sql\(/ { in_function=1 }
    in_function && /^    p = rewritten;$/ { in_appends=1; next }
    in_appends && /^    rewritten\[out_len\] = 0;$/ { complete=1; exit }
    in_appends && /plex_append_/ {
      if (index($0,"plex_append_bytes(&p, tpl0,")) print "tpl0"
      else if (index($0,"plex_append_span(&p, sql, section_id)")) print "section_id"
      else if (index($0,"plex_append_bytes(&p, selector_open,")) print "selector_open"
      else if (index($0,"plex_append_span(&p, sql, selector_value)")) print "selector_value"
      else if (index($0,"plex_append_bytes(&p, selector_close,")) print "selector_close"
      else if (index($0,"plex_append_bytes(&p, predicate_tail,")) print "predicate_tail"
      else if (index($0,"plex_append_span(&p, sql, account_id)")) print "account_id"
      else if (index($0,"plex_append_bytes(&p, tpl3,")) print "tpl3"
      else print "UNKNOWN"
    }
    END { if (!complete) exit 1 }
  ' "$1"
}

ondeck_source_selector_sequence() {
  awk '
    /^static char \*plex_build_ondeck_sql\(/ { in_function=1 }
    in_function && /if \(selector->kind == PLEX_ONDECK_SELECTOR_IDS\)/ {
      in_selectors=1
      print "id-list"
      next
    }
    in_selectors && /} else if \(selector->kind == PLEX_ONDECK_SELECTOR_THRESHOLD\)/ {
      print "threshold"
      next
    }
    in_selectors && /^    } else {$/ {
      print "else"
      complete=1
      exit
    }
    in_selectors && /selector_open = ids_open;/ { print "open=ids_open"; next }
    in_selectors && /selector_close = ids_close;/ { print "close=ids_close"; next }
    in_selectors && /selector_value = &selector->value.id_list;/ { print "value=id_list"; next }
    in_selectors && /selector_open = threshold_open;/ { print "open=threshold_open"; next }
    in_selectors && /selector_close = "";/ { print "close=empty"; next }
    in_selectors && /selector_value = &selector->value.threshold;/ { print "value=threshold"; next }
    in_selectors && /selector_(open|close|value)[[:space:]]*=/ { print "UNKNOWN" }
    END { if (!complete) exit 1 }
  ' "$1"
}

ondeck_source_candidate() {
  local source=$1 source_form=$2 form=$3 selector=$4 section=$5 selector_value=$6 account=$7
  local tpl0 ids_open ids_close threshold_open predicate_tail tpl3 selector_open selector_close
  local join_line cross_line replaced

  ondeck_capture_source_fragment "$source" tpl0 tpl0 || return 1
  ondeck_capture_source_fragment "$source" ids_open ids_open || return 1
  ondeck_capture_source_fragment "$source" ids_close ids_close || return 1
  ondeck_capture_source_fragment "$source" threshold_open threshold_open || return 1
  ondeck_capture_source_fragment "$source" predicate_tail predicate_tail || return 1
  ondeck_capture_source_fragment "$source" tpl3 tpl3 || return 1

  join_line=$'  JOIN metadata_item_views\n'
  cross_line=$'  CROSS JOIN metadata_item_views\n'
  case "$source_form:$form" in
    join:join|cross:cross) ;;
    join:cross)
      replaced=${tpl0/"$join_line"/"$cross_line"}
      [ "$replaced" != "$tpl0" ] || return 1
      tpl0=$replaced
      ;;
    cross:join)
      replaced=${tpl0/"$cross_line"/"$join_line"}
      [ "$replaced" != "$tpl0" ] || return 1
      tpl0=$replaced
      ;;
    *) return 1 ;;
  esac
  case "$selector" in
    id-list) selector_open=$ids_open; selector_close=$ids_close ;;
    threshold) selector_open=$threshold_open; selector_close= ;;
    *) return 1 ;;
  esac
  printf '%s%s%s%s%s%s%s%s' "$tpl0" "$section" "$selector_open" \
    "$selector_value" "$selector_close" "$predicate_tail" "$account" "$tpl3"
}

family_contract_check() {
  local source=${REPO_ROOT}/src/plex_fts_rewrite.c selectors cases template source_join source_cross shipped_form selector form role expected
  local selector_sequence append_sequence selector_value source_candidate harness_candidate
  local saved_idlist saved_threshold
  local marker=__DSHADOW_ONDECK_CANDIDATE_END__
  [ -f "$source" ] || die "missing rewrite source for contract check: $source"
  selectors=$(sed -n '/typedef enum plex_ondeck_selector_kind/,/} plex_ondeck_selector_kind/p' "$source" | grep -Ec 'PLEX_ONDECK_SELECTOR_[A-Z_]+[[:space:]]*=')
  [ "$selectors" -eq 2 ] || die "On-Deck selector contract changed: source_count=$selectors measured_expected=2"
  cases=$(family_cases | wc -l | tr -d ' ')
  [ "$cases" -eq 4 ] || die "On-Deck arm matrix incomplete: selectors=2 join_forms=2 cases=$cases"
  template=$(sed -n '/static const char tpl0\[\]/,/static const char ids_open\[\]/p' "$source")
  source_join=$(printf '%s\n' "$template" | awk 'index($0,"\"  JOIN metadata_item_views\\n\""){n++} END{print n+0}')
  source_cross=$(printf '%s\n' "$template" | awk 'index($0,"\"  CROSS JOIN metadata_item_views\\n\""){n++} END{print n+0}')
  if [ "$source_join" -eq 1 ] && [ "$source_cross" -eq 0 ]; then
    shipped_form='join'
  elif [ "$source_cross" -eq 1 ] && [ "$source_join" -eq 0 ]; then
    shipped_form='cross'
  else
    die "On-Deck shipped join-form source contract changed: join=$source_join cross=$source_cross"
  fi
  selector_sequence=$(ondeck_source_selector_sequence "$source") || die 'On-Deck source selector mapping missing'
  [ "$selector_sequence" = $'id-list\nopen=ids_open\nclose=ids_close\nvalue=id_list\nthreshold\nopen=threshold_open\nclose=empty\nvalue=threshold\nelse' ] || die "On-Deck source selector mapping drifted: $selector_sequence"
  append_sequence=$(ondeck_source_append_sequence "$source") || die 'On-Deck source append sequence missing'
  [ "$append_sequence" = $'tpl0\nsection_id\nselector_open\nselector_value\nselector_close\npredicate_tail\naccount_id\ntpl3' ] || die "On-Deck source append sequence drifted: $append_sequence"
  saved_idlist=${IDLIST-}
  saved_threshold=${THRESHOLD-}
  IDLIST=900003,900004
  THRESHOLD=900005
  for selector in id-list threshold; do
    for form in cross join; do
      family_cases | grep -Fx "$selector-$form" >/dev/null || die "missing On-Deck measurement arm: $selector-$form"
      role=$(family_case_role "$selector-$form")
      expected=COMPARISON
      [ "$form" = "$shipped_form" ] && expected=SHIPPED
      [ "$role" = "$expected" ] || die "On-Deck shipped-arm declaration drift: case=$selector-$form source_form=$shipped_form declared=$role expected=$expected"
      case "$selector" in
        id-list) selector_value=900003,900004 ;;
        threshold) selector_value=900005 ;;
      esac
      source_candidate=$(
        ondeck_source_candidate "$source" "$shipped_form" "$form" "$selector" \
          '?1' "$selector_value" '?2' || exit 1
        printf '\n%s' "$marker"
      ) || die "On-Deck source candidate derivation failed: case=$selector-$form"
      harness_candidate=$(
        family_render "$selector-$form" candidate
        printf '%s' "$marker"
      )
      [ "$harness_candidate" = "$source_candidate" ] || die "On-Deck candidate SQL source contract drifted: case=$selector-$form"
    done
  done
  IDLIST=$saved_idlist
  THRESHOLD=$saved_threshold
}

family_parameters() {
  printf '.parameter init\n'
  if [ -n "${SECTION_ID:-}" ]; then
    printf '.parameter set ?1 %s\n.parameter set ?2 %s\n' "$SECTION_ID" "$ACCOUNT_ID"
  fi
}

family_derive_sql() {
  cat <<SQL
WITH eligible AS (
  SELECT V.account_id, V.library_section_id, G.id AS grandparent_id, V.viewed_at, V.id AS view_id
  FROM metadata_item_views AS V
  JOIN metadata_items AS G ON G.guid=V.grandparent_guid
  JOIN metadata_item_settings AS S ON S.guid=V.guid AND S.account_id=V.account_id
  JOIN metadata_item_settings AS GS ON GS.guid=V.grandparent_guid AND GS.account_id=V.account_id
  WHERE S.view_count>0 AND V.account_id IS NOT NULL AND V.library_section_id IS NOT NULL
), cell AS (
  SELECT account_id, library_section_id, count(*) AS joined_rows,
         count(DISTINCT grandparent_id) AS distinct_grandparents, count(viewed_at) AS nonnull_viewed_at
  FROM eligible GROUP BY account_id, library_section_id
  HAVING count(DISTINCT grandparent_id)>0 AND count(viewed_at)>0
  ORDER BY distinct_grandparents DESC, joined_rows DESC, account_id, library_section_id LIMIT 1
)
SELECT 'CELL', account_id, library_section_id, joined_rows, distinct_grandparents, nonnull_viewed_at FROM cell;
WITH eligible AS (
  SELECT V.account_id, V.library_section_id, G.id AS grandparent_id, V.viewed_at
  FROM metadata_item_views AS V
  JOIN metadata_items AS G ON G.guid=V.grandparent_guid
  JOIN metadata_item_settings AS S ON S.guid=V.guid AND S.account_id=V.account_id
  JOIN metadata_item_settings AS GS ON GS.guid=V.grandparent_guid AND GS.account_id=V.account_id
  WHERE S.view_count>0 AND V.account_id IS NOT NULL AND V.library_section_id IS NOT NULL
), cell AS (
  SELECT account_id, library_section_id FROM eligible GROUP BY account_id, library_section_id
  HAVING count(DISTINCT grandparent_id)>0 AND count(viewed_at)>0
  ORDER BY count(DISTINCT grandparent_id) DESC, count(*) DESC, account_id, library_section_id LIMIT 1
)
SELECT 'ID', E.grandparent_id
FROM eligible AS E JOIN cell AS C USING(account_id,library_section_id)
GROUP BY E.grandparent_id ORDER BY max(E.viewed_at) DESC, E.grandparent_id DESC LIMIT ${IDLIST_SIZE};
WITH eligible AS (
  SELECT V.account_id, V.library_section_id, V.viewed_at, V.id AS view_id, G.id AS grandparent_id
  FROM metadata_item_views AS V
  JOIN metadata_items AS G ON G.guid=V.grandparent_guid
  JOIN metadata_item_settings AS S ON S.guid=V.guid AND S.account_id=V.account_id
  JOIN metadata_item_settings AS GS ON GS.guid=V.grandparent_guid AND GS.account_id=V.account_id
  WHERE S.view_count>0 AND V.account_id IS NOT NULL AND V.library_section_id IS NOT NULL AND V.viewed_at IS NOT NULL
), cell AS (
  SELECT account_id, library_section_id FROM eligible GROUP BY account_id, library_section_id
  HAVING count(DISTINCT grandparent_id)>0 AND count(viewed_at)>0
  ORDER BY count(DISTINCT grandparent_id) DESC, count(*) DESC, account_id, library_section_id LIMIT 1
), distinct_values AS (
  SELECT E.viewed_at, max(E.view_id) AS representative_view_id
  FROM eligible AS E JOIN cell AS C USING(account_id,library_section_id) GROUP BY E.viewed_at
), ranked AS (
  SELECT viewed_at, representative_view_id, row_number() OVER (ORDER BY viewed_at DESC) AS rn, count(*) OVER () AS n
  FROM distinct_values
)
SELECT 'THRESHOLD', viewed_at, rn, n, representative_view_id FROM ranked WHERE rn=min(n, ${THRESHOLD_RANK});
SQL
}

family_parse_derived() {
  local out=$1 extra
  IFS=$'\t' read -r _tag ACCOUNT_ID SECTION_ID JOINED_ROWS DISTINCT_G NONNULL_V extra < <(awk -F '\t' '$1=="CELL"{print;exit}' "$out")
  [ -z "${extra:-}" ] || die 'Plex cell derivation returned extra columns'
  for _v in "$ACCOUNT_ID" "$SECTION_ID" "$JOINED_ROWS" "$DISTINCT_G" "$NONNULL_V"; do case "$_v" in ''|*[!0-9]*) die 'Plex cell derivation returned non-integer data' ;; esac; done
  IDLIST=$(awk -F '\t' '$1=="ID" && $2~/^[0-9]+$/{print $2}' "$out" | paste -sd, -)
  [ -n "$IDLIST" ] || die 'Plex id-list derivation returned no ids'
  [ "$(awk -F '\t' '$1=="ID"{n++} END{print n+0}' "$out")" -eq "$(awk -F '\t' '$1=="ID" && $2~/^[0-9]+$/{n++} END{print n+0}' "$out")" ] || die 'Plex id-list derivation returned non-decimal data'
  IFS=$'\t' read -r _tag THRESHOLD THRESHOLD_ORDINAL THRESHOLD_TOTAL THRESHOLD_VIEW_ID extra < <(awk -F '\t' '$1=="THRESHOLD"{print;exit}' "$out")
  [ -z "${extra:-}" ] || die 'Plex threshold derivation returned extra columns'
  for _v in "$THRESHOLD" "$THRESHOLD_ORDINAL" "$THRESHOLD_TOTAL" "$THRESHOLD_VIEW_ID"; do case "$_v" in ''|*[!0-9]*) die 'Plex threshold derivation returned non-integer data' ;; esac; done
  [ "$THRESHOLD_TOTAL" -ge 2 ] || die 'Plex threshold cell has fewer than two distinct viewed_at values'
}

family_record_literals() {
  printf 'cell\taccount_id=%s\tlibrary_section_id=%s\tjoined_rows=%s\tdistinct_grandparents=%s\tnonnull_viewed_at=%s\tderived=1\n' "$ACCOUNT_ID" "$SECTION_ID" "$JOINED_ROWS" "$DISTINCT_G" "$NONNULL_V"
  printf 'id_list\tcount=%s\tvalues=%s\tderived=1\n' "$(printf '%s' "$IDLIST" | awk -F, '{print NF}')" "$IDLIST"
  printf 'threshold\tvalue=%s\tordinal=%s\tdistinct_values=%s\trepresentative_view_id=%s\tderived=1\n' "$THRESHOLD" "$THRESHOLD_ORDINAL" "$THRESHOLD_TOTAL" "$THRESHOLD_VIEW_ID"
}

family_prepare_sql() {
  [ "$PREPARE_PLEX_INDEXES" -eq 1 ] || return 0
  cat <<'SQL'
.bail on
CREATE INDEX IF NOT EXISTS idx_dshadow_metadata_item_views_account_grandparent_guid ON metadata_item_views (account_id, grandparent_guid);
SQL
}

ondeck_case_parts() {
  local case_id=$1
  case "$case_id" in
    id-list-cross) SELECTOR=id_list; JOIN_FORM='CROSS JOIN' ;;
    id-list-join) SELECTOR=id_list; JOIN_FORM='JOIN' ;;
    threshold-cross) SELECTOR=threshold; JOIN_FORM='CROSS JOIN' ;;
    threshold-join) SELECTOR=threshold; JOIN_FORM='JOIN' ;;
    *) die "unknown Plex On-Deck case: $case_id" ;;
  esac
  case "$SELECTOR" in
    id_list) VENDOR_SELECTOR=" and grandparents.id in ($IDLIST)"; CANDIDATE_SELECTOR=$'\n    AND grandparents.id IN ('"$IDLIST"')' ;;
    threshold) VENDOR_SELECTOR=" and viewed_at > $THRESHOLD"; CANDIDATE_SELECTOR=$'\n    AND metadata_item_views.viewed_at > '"$THRESHOLD" ;;
  esac
}

family_render() {
  local case_id=$1 arm=$2
  ondeck_case_parts "$case_id"
  if [ "$arm" = vendor ]; then
    cat <<SQL
select grandparents.id,metadata_item_views.originally_available_at,metadata_item_views.parent_index,metadata_item_views.\`index\`,max(viewed_at),grandparents.library_section_id,grandparentsSettings.extra_data from metadata_item_views indexed by index_metadata_item_views_on_guid join metadata_items as grandparents indexed by index_metadata_items_on_guid on grandparents.guid=grandparent_guid join metadata_item_settings indexed by index_metadata_item_settings_on_account_id on metadata_item_settings.guid=metadata_item_views.guid and metadata_item_views.account_id=metadata_item_settings.account_id join metadata_item_settings as grandparentsSettings indexed by index_metadata_item_settings_on_guid on grandparentsSettings.guid=metadata_item_views.grandparent_guid and metadata_item_views.account_id=grandparentsSettings.account_id where metadata_item_views.library_section_id=?1${VENDOR_SELECTOR} and metadata_item_settings.view_count>0  and metadata_item_views.account_id=?2 group by grandparents.id order by viewed_at desc;
SQL
  else
    cat <<SQL
SELECT grandparents_id AS id,
       originally_available_at AS originally_available_at,
       parent_index AS parent_index,
       metadata_item_views_index AS "index",
       +viewed_at AS "max(viewed_at)",
       library_section_id AS library_section_id,
       grandparents_extra_data AS extra_data
FROM (
  SELECT grandparents.id AS grandparents_id,
         metadata_item_views.originally_available_at AS originally_available_at,
         metadata_item_views.parent_index AS parent_index,
         metadata_item_views.\`index\` AS metadata_item_views_index,
         metadata_item_views.viewed_at AS viewed_at,
         grandparents.library_section_id AS library_section_id,
         grandparentsSettings.extra_data AS grandparents_extra_data,
         row_number() OVER (PARTITION BY grandparents.id ORDER BY metadata_item_views.viewed_at DESC, metadata_item_views.id DESC, grandparentsSettings.id DESC, metadata_item_settings.id DESC) AS dshadow_on_deck_rank
  FROM metadata_items AS grandparents
  ${JOIN_FORM} metadata_item_views
  JOIN metadata_item_settings
  JOIN metadata_item_settings AS grandparentsSettings
  WHERE grandparents.guid=metadata_item_views.grandparent_guid
    AND metadata_item_settings.guid=metadata_item_views.guid
    AND metadata_item_views.account_id=metadata_item_settings.account_id
    AND grandparentsSettings.guid=metadata_item_views.grandparent_guid
    AND metadata_item_views.account_id=grandparentsSettings.account_id
    AND metadata_item_views.library_section_id=?1${CANDIDATE_SELECTOR}
    AND metadata_item_settings.view_count>0
    AND metadata_item_views.account_id=?2
) AS dshadow_on_deck_ranked
WHERE dshadow_on_deck_rank=1
ORDER BY viewed_at DESC, grandparents_id DESC;
SQL
  fi
}

emit_query_core() { sed '$s/;[[:space:]]*$//' "$1"; }

family_identity_sql() {
  local case_id=$1 vendor_file=$2 candidate_file=$3
  ondeck_case_parts "$case_id"
  write_readonly_preamble
  cat <<SQL
WITH vendor(id,originally_available_at,parent_index,item_index,max_viewed_at,library_section_id,extra_data) AS MATERIALIZED (
$(emit_query_core "$vendor_file")
), candidate(id,originally_available_at,parent_index,item_index,max_viewed_at,library_section_id,extra_data) AS MATERIALIZED (
$(emit_query_core "$candidate_file")
), eligible AS MATERIALIZED (
  SELECT grandparents.id, metadata_item_views.originally_available_at, metadata_item_views.parent_index,
         metadata_item_views.\`index\` AS item_index, metadata_item_views.viewed_at,
         grandparents.library_section_id, grandparentsSettings.extra_data,
         metadata_item_views.id AS view_id, grandparentsSettings.id AS gs_id, metadata_item_settings.id AS s_id
  FROM metadata_items AS grandparents ${JOIN_FORM} metadata_item_views
  JOIN metadata_item_settings JOIN metadata_item_settings AS grandparentsSettings
  WHERE grandparents.guid=metadata_item_views.grandparent_guid
    AND metadata_item_settings.guid=metadata_item_views.guid
    AND metadata_item_views.account_id=metadata_item_settings.account_id
    AND grandparentsSettings.guid=metadata_item_views.grandparent_guid
    AND metadata_item_views.account_id=grandparentsSettings.account_id
    AND metadata_item_views.library_section_id=?1${CANDIDATE_SELECTOR}
    AND metadata_item_settings.view_count>0 AND metadata_item_views.account_id=?2
), oracle AS MATERIALIZED (
  SELECT id,originally_available_at,parent_index,item_index,viewed_at AS max_viewed_at,library_section_id,extra_data
  FROM (SELECT eligible.*,row_number() OVER(PARTITION BY id ORDER BY viewed_at DESC,view_id DESC,gs_id DESC,s_id DESC) rn FROM eligible) WHERE rn=1
), ties AS MATERIALIZED (
  SELECT o.id,count(*) AS n FROM oracle o JOIN eligible e ON e.id=o.id AND e.viewed_at IS o.max_viewed_at GROUP BY o.id
), metrics AS (
  SELECT (SELECT count(*) FROM vendor) vendor_rows,(SELECT count(*) FROM candidate) candidate_rows,
    (SELECT count(*) FROM vendor v LEFT JOIN candidate c USING(id) WHERE c.id IS NULL) missing_candidate,
    (SELECT count(*) FROM candidate c LEFT JOIN vendor v USING(id) WHERE v.id IS NULL) extra_candidate,
    (SELECT count(*) FROM vendor v JOIN candidate c USING(id) WHERE NOT(v.max_viewed_at IS c.max_viewed_at)) max_mismatch,
    (SELECT count(*) FROM candidate c JOIN oracle o USING(id) WHERE NOT(c.originally_available_at IS o.originally_available_at AND c.parent_index IS o.parent_index AND c.item_index IS o.item_index AND c.max_viewed_at IS o.max_viewed_at AND c.library_section_id IS o.library_section_id AND c.extra_data IS o.extra_data)) candidate_oracle_mismatch,
    (SELECT count(*) FROM vendor v JOIN candidate c USING(id) LEFT JOIN ties t USING(id) WHERE NOT(v.originally_available_at IS c.originally_available_at AND v.parent_index IS c.parent_index AND v.item_index IS c.item_index AND v.max_viewed_at IS c.max_viewed_at AND v.library_section_id IS c.library_section_id AND v.extra_data IS c.extra_data) AND coalesce(t.n,0)<=1) untied_projection_diff,
    (SELECT count(*) FROM vendor v JOIN candidate c USING(id) JOIN ties t USING(id) WHERE NOT(v.originally_available_at IS c.originally_available_at AND v.parent_index IS c.parent_index AND v.item_index IS c.item_index AND v.max_viewed_at IS c.max_viewed_at AND v.library_section_id IS c.library_section_id AND v.extra_data IS c.extra_data) AND t.n>1 AND NOT EXISTS(SELECT 1 FROM eligible e WHERE e.id=v.id AND e.viewed_at IS v.max_viewed_at AND e.originally_available_at IS v.originally_available_at AND e.parent_index IS v.parent_index AND e.item_index IS v.item_index AND e.library_section_id IS v.library_section_id AND e.extra_data IS v.extra_data)) invalid_tie_divergence,
    (SELECT count(*) FROM vendor v JOIN candidate c USING(id) JOIN ties t USING(id) WHERE NOT(v.originally_available_at IS c.originally_available_at AND v.parent_index IS c.parent_index AND v.item_index IS c.item_index AND v.max_viewed_at IS c.max_viewed_at AND v.library_section_id IS c.library_section_id AND v.extra_data IS c.extra_data) AND t.n>1 AND EXISTS(SELECT 1 FROM eligible e WHERE e.id=v.id AND e.viewed_at IS v.max_viewed_at AND e.originally_available_at IS v.originally_available_at AND e.parent_index IS v.parent_index AND e.item_index IS v.item_index AND e.library_section_id IS v.library_section_id AND e.extra_data IS v.extra_data)) accepted_tie_divergence
)
SELECT 'IDENTITY',CASE WHEN vendor_rows>0 AND candidate_rows>0 AND missing_candidate=0 AND extra_candidate=0 AND max_mismatch=0 AND candidate_oracle_mismatch=0 AND untied_projection_diff=0 AND invalid_tie_divergence=0 THEN 'PASS' ELSE 'FAIL' END,
 vendor_rows,candidate_rows,missing_candidate,extra_candidate,max_mismatch,candidate_oracle_mismatch,untied_projection_diff,invalid_tie_divergence,accepted_tie_divergence FROM metrics;
SQL
}

family_identity_pass() { awk -F '\t' '$1=="IDENTITY" && $2=="PASS"{ok=1} END{exit !ok}' "$2"; }
family_identity_detail() { tr '\n' ';' < "$2" | sed 's/;$//'; }

family_fixture_check() {
  local sql=$SQL_DIR/plex-ondeck-tie-fixture.sql out=$OUT_DIR/plex-ondeck-tie-fixture.out
  cat > "$sql" <<'SQL'
.bail on
.headers off
.mode tabs
PRAGMA query_only=1;
WITH vendor(id,a,b,m) AS (VALUES(1,'vendor',10,100)),
 candidate(id,a,b,m) AS (VALUES(1,'ranked',20,100)),
 eligible(id,a,b,m) AS (VALUES(1,'vendor',10,100),(1,'ranked',20,100)),
 ties(id,n) AS (SELECT id,count(*) FROM eligible WHERE m=100 GROUP BY id),
 metrics AS (
  SELECT (SELECT count(*) FROM vendor v JOIN candidate c USING(id) JOIN ties t USING(id)
          WHERE NOT(v.a IS c.a AND v.b IS c.b AND v.m IS c.m) AND t.n>1
            AND EXISTS(SELECT 1 FROM eligible e WHERE e.id=v.id AND e.m IS v.m AND e.a IS v.a AND e.b IS v.b)) accepted,
         (SELECT count(*) FROM vendor v JOIN candidate c USING(id) JOIN ties t USING(id)
          WHERE NOT(v.a IS c.a AND v.b IS c.b AND v.m IS c.m) AND t.n>1
            AND NOT EXISTS(SELECT 1 FROM eligible e WHERE e.id=v.id AND e.m IS v.m AND e.a IS v.a AND e.b IS v.b)) invalid
 )
SELECT 'TIE_FIXTURE',CASE WHEN accepted=1 AND invalid=0 THEN 'PASS' ELSE 'FAIL' END,accepted,invalid FROM metrics;
SQL
  run_query_sql tie_fixture :memory: "$sql" "$out" || die 'Plex On-Deck tie fixture execution failed'
  grep -Fx $'TIE_FIXTURE\tPASS\t1\t0' "$out" >/dev/null || die "Plex On-Deck declared divergence fixture did not fire: $out"
}
