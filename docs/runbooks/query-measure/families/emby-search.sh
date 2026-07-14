# shellcheck shell=bash disable=SC2034
# Query-family data for the Emby FTS search/fan-out candidate matrix.

family_configure() {
  require_env EMBY_DB_SOURCE
  SOURCE_DB=$EMBY_DB_SOURCE
  SEARCH_CANDIDATES=${SEARCH_CANDIDATES:-'COMBINED B8 B1 B2 B3 B4 B5 B5_INLINE B7'}
  SEARCH_VARIANTS=${SEARCH_VARIANTS:-'type presentation'}
  SEARCH_MATCH_CASES=${SEARCH_MATCH_CASES:-'case1_or case1_and case2_or case2_and'}
  SEARCH_USER_ID=
  SEARCH_TOKEN_1=
  SEARCH_TOKEN_2=
}

family_all_cases() {
  local variant match candidate
  for variant in type presentation; do
    for match in case1_or case1_and case2_or case2_and; do
      for candidate in COMBINED B8 B1 B2 B3 B4 B5 B5_INLINE B7; do
        printf '%s-%s-%s\n' "$variant" "$match" "$candidate"
      done
    done
  done
}

family_cases() {
  local variant match candidate
  for variant in $SEARCH_VARIANTS; do
    for match in $SEARCH_MATCH_CASES; do
      for candidate in $SEARCH_CANDIDATES; do printf '%s-%s-%s\n' "$variant" "$match" "$candidate"; done
    done
  done
}

family_contract_check() {
  local n
  n=$(family_all_cases | wc -l | tr -d ' ')
  [ "$n" -eq 72 ] || die "Emby search matrix contract changed: expected=72 actual=$n"
  grep -Fx '    X(EMBY_PEOPLE, "emby", "fanout+people", "emby_fts_rewrite", 0) \' "${REPO_ROOT}/src/rewrite_modes.h" >/dev/null || die 'Emby People mode catalogue contract drifted'
  grep -Fx '    X(EMBY_LINKS_SEARCH, "emby", "fanout+links_search", "emby_fts_rewrite", 0) \' "${REPO_ROOT}/src/rewrite_modes.h" >/dev/null || die 'Emby links-search mode catalogue contract drifted'
  while IFS= read -r _case; do family_all_cases | grep -Fx "$_case" >/dev/null || die "unknown Emby search case: $_case"; done < <(family_cases)
}

search_case_parts() {
  local case_id=$1
  IFS=- read -r SEARCH_VARIANT SEARCH_MATCH_CASE SEARCH_CANDIDATE <<< "$case_id"
  case "$SEARCH_VARIANT" in type|presentation) ;; *) die "unknown Emby search variant: $SEARCH_VARIANT" ;; esac
  case "$SEARCH_MATCH_CASE" in
    case1_or) SEARCH_MATCH="Name:${SEARCH_TOKEN_1}* OR Name:${SEARCH_TOKEN_2}*" ;;
    case1_and) SEARCH_MATCH="Name:${SEARCH_TOKEN_1}* AND Name:${SEARCH_TOKEN_2}*" ;;
    case2_or) SEARCH_MATCH="Name:${SEARCH_TOKEN_1}* OR SeriesName:${SEARCH_TOKEN_2}*" ;;
    case2_and) SEARCH_MATCH="Name:${SEARCH_TOKEN_1}* AND SeriesName:${SEARCH_TOKEN_2}*" ;;
    *) die "unknown Emby search match case: $SEARCH_MATCH_CASE" ;;
  esac
}

family_parameters() {
  [ -n "${CURRENT_CASE:-}" ] || return 0
  search_case_parts "$CURRENT_CASE"
  printf '.parameter init\n.parameter set ?1 %s\n.parameter set ?2 %s\n' "$SEARCH_USER_ID" "'${SEARCH_MATCH}'"
}

family_derive_sql() {
  cat <<'SQL'
SELECT 'USER',UserId,count(*) AS n FROM UserDatas GROUP BY UserId ORDER BY n DESC,UserId LIMIT 1;
SELECT 'ANCESTOR',AncestorId,count(*) AS n FROM AncestorIds2 GROUP BY AncestorId ORDER BY n DESC,AncestorId LIMIT 20;
SELECT 'TOKEN',lower(Name) FROM fts_search9
WHERE Name IS NOT NULL AND length(Name) BETWEEN 3 AND 16 AND Name NOT GLOB '*[^A-Za-z0-9]*'
GROUP BY lower(Name) LIMIT 2;
SQL
}

family_parse_derived() {
  local out=$1
  SEARCH_USER_ID=$(awk -F '\t' '$1=="USER" && $2~/^[0-9]+$/{print $2;exit}' "$out")
  SEARCH_ANCESTORS=$(awk -F '\t' '$1=="ANCESTOR" && $2~/^[0-9]+$/{print $2}' "$out" | paste -sd, -)
  SEARCH_TOKEN_1=$(awk -F '\t' '$1=="TOKEN" && $2~/^[a-z0-9]+$/{print $2;exit}' "$out")
  SEARCH_TOKEN_2=$(awk -F '\t' '$1=="TOKEN" && $2~/^[a-z0-9]+$/{n++;if(n==2){print $2;exit}}' "$out")
  [ -n "$SEARCH_USER_ID" ] || die 'Emby search user derivation returned no numeric user'
  [ -n "$SEARCH_ANCESTORS" ] || die 'Emby search ancestor derivation returned no numeric ancestors'
  [ -n "$SEARCH_TOKEN_1" ] && [ -n "$SEARCH_TOKEN_2" ] || die 'Emby search token derivation returned fewer than two alphanumeric cells'
}

family_record_literals() {
  printf 'user\tid=%s\tderived=1\n' "$SEARCH_USER_ID"
  printf 'ancestors\tcount=%s\tvalues=%s\tderived=1\n' "$(printf '%s' "$SEARCH_ANCESTORS" | awk -F, '{print NF}')" "$SEARCH_ANCESTORS"
  printf 'match_token\tordinal=1\tvalue=%s\tderived=1\n' "$SEARCH_TOKEN_1"
  printf 'match_token\tordinal=2\tvalue=%s\tderived=1\n' "$SEARCH_TOKEN_2"
}

family_prepare_sql() { :; }

replace_fixed() {
  local old=$1 new=$2
  awk -v old="$old" -v new="$new" 'BEGIN{if(old=="")exit 2}{out="";rest=$0;while((p=index(rest,old))>0){out=out substr(rest,1,p-1) new;rest=substr(rest,p+length(old))}print out rest}'
}

OLD_WITH_ITEM='WithItemLinkItemIds AS (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (6,4,7,10,3,2,0,1,5) union select ItemLinks2TwoLevel.LinkedId from ItemLinks2 ItemLinks2TwoLevel where itemid in (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (7,0,1,5,6,4,2)))'
NEW_WITH_ITEM_UNION_ALL='WithItemLinkItemIds AS (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (6,4,7,10,3,2,0,1,5) union all select ItemLinks2TwoLevel.LinkedId from ItemLinks2 ItemLinks2TwoLevel where itemid in (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (7,0,1,5,6,4,2)))'
OLD_B2='A.Id in (select PersonId from itemPeople2 where ItemId in WithAncestors)'
NEW_B2='EXISTS (SELECT 1 FROM itemPeople2 WHERE itemPeople2.PersonId = A.Id AND itemPeople2.ItemId in WithAncestors)'
OLD_B3='A.Id in (select ListItemsExemptionForPlaylists.ListId from ListItems ListItemsExemptionForPlaylists join ancestorids2 AncestorIdExemptionForPlaylists on ListItemsExemptionForPlaylists.ListItemId=AncestorIdExemptionForPlaylists.itemid and AncestorIdExemptionForPlaylists.AncestorId in (__ANCESTORS__))'
NEW_B3='EXISTS (SELECT 1 FROM ListItems ListItemsExemptionForPlaylists JOIN ancestorids2 AncestorIdExemptionForPlaylists ON ListItemsExemptionForPlaylists.ListItemId=AncestorIdExemptionForPlaylists.itemid AND AncestorIdExemptionForPlaylists.AncestorId in (__ANCESTORS__) WHERE ListItemsExemptionForPlaylists.ListId = A.Id)'
OLD_B4='A.Id in WithAncestors'
NEW_B4='EXISTS (SELECT 1 FROM WithAncestors WHERE WithAncestors.itemid = A.Id)'
OLD_B5='A.Id in WithItemLinkItemIds'
NEW_B5='EXISTS (SELECT 1 FROM WithItemLinkItemIds WHERE WithItemLinkItemIds.LinkedId = A.Id)'
OLD_WITH_ITEM_COMMA=",${OLD_WITH_ITEM}"
NEW_B5_INLINE='(EXISTS (SELECT 1 FROM ItemLinks2 JOIN WithAncestors ON WithAncestors.itemid = ItemLinks2.itemid WHERE ItemLinks2.LinkedId = A.Id AND ItemLinks2.Type in (6,4,7,10,3,2,0,1,5)) OR EXISTS (SELECT 1 FROM ItemLinks2 ItemLinks2TwoLevel JOIN ItemLinks2 ItemLinks2Seed ON ItemLinks2Seed.LinkedId = ItemLinks2TwoLevel.itemid JOIN WithAncestors ON WithAncestors.itemid = ItemLinks2Seed.itemid WHERE ItemLinks2TwoLevel.LinkedId = A.Id AND ItemLinks2Seed.Type in (7,0,1,5,6,4,2)))'
OLD_CTE_END=')))select count(*) OVER()'
NEW_CTE_END=",FtsCandidates AS MATERIALIZED (SELECT rowid AS RowId, rank AS Rank FROM fts_search9 WHERE fts_search9 MATCH ?2)select count(*) OVER()"
OLD_FTS_FROM="from mediaitems A join fts_search9 on A.Id=fts_search9.RowId and fts_search9 match '__MATCH_LITERAL__'"
NEW_FTS_FROM='from FtsCandidates join mediaitems A on A.Id=FtsCandidates.RowId'

apply_search_candidate() {
  case "$1" in
    BASE) cat ;;
    B1) replace_fixed "$OLD_WITH_ITEM" "$NEW_WITH_ITEM_UNION_ALL" ;;
    B2) replace_fixed "$OLD_B2" "$NEW_B2" ;;
    B3) replace_fixed "$OLD_B3" "$NEW_B3" ;;
    B4) replace_fixed "$OLD_B4" "$NEW_B4" ;;
    B5) replace_fixed "$OLD_B5" "$NEW_B5" ;;
    B5_INLINE) replace_fixed "$OLD_WITH_ITEM_COMMA" '' | replace_fixed "$OLD_B5" "$NEW_B5_INLINE" ;;
    B7) replace_fixed 'WithAncestors AS (' 'WithAncestors AS NOT MATERIALIZED (' | replace_fixed 'WithItemLinkItemIds AS (' 'WithItemLinkItemIds AS NOT MATERIALIZED (' ;;
    B8) replace_fixed "$OLD_CTE_END" "$NEW_CTE_END" | replace_fixed "$OLD_FTS_FROM" "$NEW_FTS_FROM" ;;
    COMBINED) replace_fixed "$OLD_WITH_ITEM" "$NEW_WITH_ITEM_UNION_ALL" | replace_fixed "$OLD_B2" "$NEW_B2" | replace_fixed "$OLD_B3" "$NEW_B3" | replace_fixed "$OLD_B4" "$NEW_B4" | replace_fixed "$OLD_B5" "$NEW_B5" | replace_fixed 'WithAncestors AS (' 'WithAncestors AS NOT MATERIALIZED (' | replace_fixed 'WithItemLinkItemIds AS (' 'WithItemLinkItemIds AS NOT MATERIALIZED (' ;;
    *) die "unknown Emby search candidate: $1" ;;
  esac
}

render_search_query() {
  local variant=$1 candidate=$2 template=${HARNESS_DIR}/sql/base-$1.sql
  [ -f "$template" ] || die "missing Emby search template: $template"
  apply_search_candidate "$candidate" < "$template" |
    replace_fixed '__ANCESTORS__' "$SEARCH_ANCESTORS" |
    replace_fixed '__EMBY_USER_ID__' '?1' |
    replace_fixed "'__MATCH_LITERAL__'" '?2'
}

family_render() {
  local case_id=$1 arm=$2
  search_case_parts "$case_id"
  if [ "$arm" = vendor ]; then render_search_query "$SEARCH_VARIANT" BASE; else render_search_query "$SEARCH_VARIANT" "$SEARCH_CANDIDATE"; fi
}

search_identity_core() {
  local variant=$1 candidate=$2
  if [ "$variant" = type ]; then
    render_search_query "$variant" "$candidate" |
      replace_fixed 'select count(*) OVER() AS TotalRecordCount,A.type,(Select ShareLevel' 'select A.Id AS id,(Select ShareLevel' |
      replace_fixed ' Group by A.Type ORDER BY Rank ASC LIMIT 50' ''
  else
    render_search_query "$variant" "$candidate" |
      replace_fixed 'select count(*) OVER() AS TotalRecordCount,A.type,A.Id,A.EndDate,A.IndexNumber,A.Name,A.Path,A.ParentIndexNumber,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.SeriesName,A.AlbumId,A.SeriesId,A.PresentationUniqueKey,A.Images,A.Status,A.SortIndexNumber,A.SortParentIndexNumber,A.IndexNumberEnd,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex,(Select ShareLevel' 'select A.Id AS id,(Select ShareLevel' |
      replace_fixed ' Group by A.PresentationUniqueKey ORDER BY Rank ASC LIMIT 50' ''
  fi
}

family_identity_sql() {
  local case_id=$1 _vendor=$2 _candidate=$3
  search_case_parts "$case_id"
  write_readonly_preamble
  cat <<SQL
WITH vendor_identity AS MATERIALIZED (SELECT id,count(*) AS n FROM ($(search_identity_core "$SEARCH_VARIANT" BASE)) GROUP BY id),
candidate_identity AS MATERIALIZED (SELECT id,count(*) AS n FROM ($(search_identity_core "$SEARCH_VARIANT" "$SEARCH_CANDIDATE")) GROUP BY id),
metrics AS (
 SELECT (SELECT count(*) FROM vendor_identity) vendor_ids,(SELECT coalesce(sum(n),0) FROM vendor_identity) vendor_rows,
        (SELECT count(*) FROM candidate_identity) candidate_ids,(SELECT coalesce(sum(n),0) FROM candidate_identity) candidate_rows,
        (SELECT count(*) FROM (SELECT * FROM vendor_identity EXCEPT SELECT * FROM candidate_identity)) missing_candidate,
        (SELECT count(*) FROM (SELECT * FROM candidate_identity EXCEPT SELECT * FROM vendor_identity)) extra_candidate
)
SELECT 'IDENTITY',CASE WHEN vendor_ids>0 AND vendor_ids=candidate_ids AND vendor_rows=candidate_rows AND missing_candidate=0 AND extra_candidate=0 THEN 'PASS' ELSE 'FAIL' END,
 vendor_ids,vendor_rows,candidate_ids,candidate_rows,missing_candidate,extra_candidate FROM metrics;
SQL
}

family_identity_pass() { awk -F '\t' '$1=="IDENTITY" && $2=="PASS"{ok=1} END{exit !ok}' "$2"; }
family_identity_detail() { tr '\n' ';' < "$2" | sed 's/;$//'; }
family_fixture_check() { :; }
