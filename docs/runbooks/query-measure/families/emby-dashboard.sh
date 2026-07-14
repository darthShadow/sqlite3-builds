# shellcheck shell=bash disable=SC2034
# Query-family data for Emby dashboard Latest. Sourced by query-measure.sh.

family_configure() {
  require_env EMBY_DB_SOURCE
  SOURCE_DB=$EMBY_DB_SOURCE
  DASHBOARD_ANCESTOR_SIZE=${DASHBOARD_ANCESTOR_SIZE:-12}
  PREPARE_DASHBOARD_INDEXES=${PREPARE_DASHBOARD_INDEXES:-0}
  require_uint DASHBOARD_ANCESTOR_SIZE
  require_uint PREPARE_DASHBOARD_INDEXES
  [ "$DASHBOARD_ANCESTOR_SIZE" -ge 1 ] || die 'DASHBOARD_ANCESTOR_SIZE must be at least 1'
  case "$PREPARE_DASHBOARD_INDEXES" in 0|1) ;; *) die 'PREPARE_DASHBOARD_INDEXES must be 0 or 1' ;; esac
  DASHBOARD_USER_ID=
  DASHBOARD_ANCESTORS=
}

family_all_cases() { printf '%s\n' movies-latest episodes-latest; }
family_cases() {
  local -a selected
  if [ -n "${CASES:-}" ]; then read -r -a selected <<< "$CASES"; printf '%s\n' "${selected[@]}"; else family_all_cases; fi
}

family_contract_check() {
  grep -Fx '    X(EMBY_EPISODES_LATEST, "emby", "dashboard+episodes_latest", "emby_fts_rewrite", 1) \' "${REPO_ROOT}/src/rewrite_modes.h" >/dev/null || die 'Emby Episodes Latest mode catalogue contract drifted'
  grep -Fx '    X(EMBY_MOVIES_LATEST, "emby", "dashboard+movies_latest", "emby_fts_rewrite", 1)' "${REPO_ROOT}/src/rewrite_modes.h" >/dev/null || die 'Emby movies Latest mode catalogue contract drifted'
  for _case in movies-latest episodes-latest; do
    family_all_cases | grep -Fx "$_case" >/dev/null || die "missing Emby dashboard measurement arm: $_case"
  done
  while IFS= read -r _case; do family_all_cases | grep -Fx "$_case" >/dev/null || die "unknown CASES entry for emby-dashboard: $_case"; done < <(family_cases)
}

# These production matcher shapes reject bind parameters. The derived user and
# ancestor cells therefore remain literals in the raw prepare form by contract.
family_parameters() { :; }

family_derive_sql() {
  cat <<SQL
SELECT 'USER',UserId,count(*) AS n FROM UserDatas GROUP BY UserId ORDER BY n DESC,UserId LIMIT 1;
SELECT 'ANCESTOR',X.AncestorId,count(*) AS n
FROM AncestorIds2 AS X JOIN MediaItems AS A ON A.Id=X.ItemId
WHERE A.Type IN (5,8)
GROUP BY X.AncestorId ORDER BY n DESC,X.AncestorId LIMIT ${DASHBOARD_ANCESTOR_SIZE};
SQL
}

family_parse_derived() {
  local out=$1
  DASHBOARD_USER_ID=$(awk -F '\t' '$1=="USER" && $2~/^[0-9]+$/{print $2;exit}' "$out")
  DASHBOARD_ANCESTORS=$(awk -F '\t' '$1=="ANCESTOR" && $2~/^[0-9]+$/{print $2}' "$out" | paste -sd, -)
  [ -n "$DASHBOARD_USER_ID" ] || die 'Emby dashboard user derivation returned no numeric user'
  [ -n "$DASHBOARD_ANCESTORS" ] || die 'Emby dashboard ancestor derivation returned no numeric ancestors'
  [ "$(awk -F '\t' '$1=="ANCESTOR"{n++} END{print n+0}' "$out")" -eq "$(awk -F '\t' '$1=="ANCESTOR" && $2~/^[0-9]+$/{n++} END{print n+0}' "$out")" ] || die 'Emby dashboard ancestor derivation returned non-numeric data'
}

family_record_literals() {
  printf 'user\tid=%s\tderived=1\tselection=most_UserDatas_rows\n' "$DASHBOARD_USER_ID"
  printf 'ancestors\tcount=%s\tvalues=%s\tderived=1\tselection=most_type_5_or_8_items\n' "$(printf '%s' "$DASHBOARD_ANCESTORS" | awk -F, '{print NF}')" "$DASHBOARD_ANCESTORS"
}

family_prepare_sql() {
  [ "$PREPARE_DASHBOARD_INDEXES" -eq 1 ] || return 0
  cat <<'SQL'
.bail on
CREATE INDEX IF NOT EXISTS idx_dshadow_emby_latest_gk_dc ON MediaItems (coalesce(SeriesPresentationUniqueKey, PresentationUniqueKey), DateCreated DESC, Id, UserDataKeyId) WHERE Type = 8;
CREATE INDEX IF NOT EXISTS idx_dshadow_emby_latest_movies_dcn_puk ON MediaItems ((DateCreated IS NULL), DateCreated DESC, PresentationUniqueKey, Id, UserDataKeyId) WHERE Type = 5;
CREATE INDEX IF NOT EXISTS idx_dshadow_emby_latest_movies_puk_dc_cover ON MediaItems (PresentationUniqueKey, DateCreated, Id, UserDataKeyId) WHERE Type = 5;
SQL
}

dashboard_projection() {
  case "$1" in
    movies-latest) printf '%s' 'A.Id,A.Name,A.Path,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.Images,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex ' ;;
    episodes-latest) printf '%s' 'A.Id,A.EndDate,A.IndexNumber,A.Name,A.Path,A.ParentIndexNumber,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.SeriesName,A.AlbumId,A.SeriesId,A.Images,A.SortIndexNumber,A.SortParentIndexNumber,A.IndexNumberEnd,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex ' ;;
    *) die "unknown Emby dashboard case: $1" ;;
  esac
}

family_render() {
  local case_id=$1 arm=$2 projection
  projection=$(dashboard_projection "$case_id")
  case "$case_id:$arm" in
    movies-latest:vendor)
      printf 'with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (%s) )select %sfrom mediaitems A left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=%s where A.Type=5 AND Coalesce(UserDatas.played, 0)=0 AND A.Id in WithAncestors Group by A.PresentationUniqueKey ORDER BY A.DateCreated DESC LIMIT 12;\n' "$DASHBOARD_ANCESTORS" "$projection" "$DASHBOARD_USER_ID"
      ;;
    movies-latest:candidate)
      cat <<SQL
WITH ranked(id, dc, puk) AS MATERIALIZED (
  SELECT A.Id,A.DateCreated,A.PresentationUniqueKey
  FROM MediaItems AS A INDEXED BY idx_dshadow_emby_latest_movies_dcn_puk
  WHERE A.Type=5
    AND EXISTS (SELECT 1 FROM AncestorIds2 AS X WHERE X.ItemId=A.Id AND X.AncestorId IN ($DASHBOARD_ANCESTORS))
    AND NOT EXISTS (SELECT 1 FROM UserDatas AS U0 WHERE U0.UserDataKeyId=A.UserDataKeyId AND U0.UserId=$DASHBOARD_USER_ID AND U0.played<>0)
    AND NOT EXISTS (
      SELECT 1 FROM MediaItems AS B INDEXED BY idx_dshadow_emby_latest_movies_puk_dc_cover
      WHERE B.Type=5 AND B.PresentationUniqueKey IS A.PresentationUniqueKey
        AND ((B.DateCreated IS NOT NULL AND A.DateCreated IS NULL) OR B.DateCreated>A.DateCreated OR (B.DateCreated IS A.DateCreated AND B.Id<A.Id))
        AND EXISTS (SELECT 1 FROM AncestorIds2 AS XB WHERE XB.ItemId=B.Id AND XB.AncestorId IN ($DASHBOARD_ANCESTORS))
        AND NOT EXISTS (SELECT 1 FROM UserDatas AS UB WHERE UB.UserDataKeyId=B.UserDataKeyId AND UB.UserId=$DASHBOARD_USER_ID AND UB.played<>0)
    )
  ORDER BY (A.DateCreated IS NULL) ASC,A.DateCreated DESC,A.PresentationUniqueKey ASC LIMIT 12
)
SELECT ${projection}FROM ranked AS R JOIN MediaItems AS A ON A.Id=R.id
LEFT JOIN UserDatas ON A.UserDataKeyId=UserDatas.UserDataKeyId AND UserDatas.UserId=$DASHBOARD_USER_ID
ORDER BY (R.dc IS NULL) ASC,R.dc DESC,R.puk ASC LIMIT 12;
SQL
      ;;
    episodes-latest:vendor)
      printf 'with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (%s) )select %sfrom mediaitems A left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=%s where A.Type=8 AND Coalesce(UserDatas.played, 0)=0 AND A.Id in WithAncestors Group by coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey) ORDER BY MAX(A.DateCreated) DESC LIMIT 12;\n' "$DASHBOARD_ANCESTORS" "$projection" "$DASHBOARD_USER_ID"
      ;;
    episodes-latest:candidate)
      cat <<SQL
WITH keys(gk) AS MATERIALIZED (
  SELECT DISTINCT coalesce(A0.SeriesPresentationUniqueKey,A0.PresentationUniqueKey)
  FROM (SELECT DISTINCT itemid FROM AncestorIds2 WHERE AncestorId IN ($DASHBOARD_ANCESTORS)) AS W
  CROSS JOIN MediaItems AS A0
  WHERE A0.Id=W.itemid AND A0.Type=8
    AND NOT EXISTS (SELECT 1 FROM UserDatas AS U0 WHERE U0.UserDataKeyId=A0.UserDataKeyId AND U0.UserId=$DASHBOARD_USER_ID AND U0.played<>0)
), picked AS MATERIALIZED (
  SELECT K.gk,(SELECT A2.Id FROM MediaItems AS A2 WHERE A2.Type=8
    AND coalesce(A2.SeriesPresentationUniqueKey,A2.PresentationUniqueKey) IS K.gk
    AND EXISTS (SELECT 1 FROM AncestorIds2 AS X WHERE X.ItemId=A2.Id AND X.AncestorId IN ($DASHBOARD_ANCESTORS))
    AND NOT EXISTS (SELECT 1 FROM UserDatas AS U2 WHERE U2.UserDataKeyId=A2.UserDataKeyId AND U2.UserId=$DASHBOARD_USER_ID AND U2.played<>0)
    ORDER BY A2.DateCreated DESC LIMIT 1) AS id FROM keys AS K
), exact_groups AS MATERIALIZED (
  SELECT P.gk,P.id,Amax.DateCreated AS maxdc FROM picked AS P JOIN MediaItems AS Amax ON Amax.Id=P.id WHERE P.id IS NOT NULL
), ranked AS MATERIALIZED (SELECT gk,id,maxdc FROM exact_groups ORDER BY maxdc DESC LIMIT 12)
SELECT ${projection}FROM ranked AS R JOIN MediaItems AS A ON A.Id=R.id
LEFT JOIN UserDatas ON A.UserDataKeyId=UserDatas.UserDataKeyId AND UserDatas.UserId=$DASHBOARD_USER_ID
ORDER BY R.maxdc DESC LIMIT 12;
SQL
      ;;
    *) die "unknown Emby dashboard arm: $case_id/$arm" ;;
  esac
}

dashboard_query_core() { sed '$s/;[[:space:]]*$//' "$1"; }

# This strict canary can false-fail when max-DateCreated ties cross LIMIT 12:
# neither arm has a deterministic group tiebreaker, so tied subsets can differ.
# It can also pass on all-singleton groups without exercising representative
# selection; tests/fixtures/emby-fts-rewrite/ is authoritative for that drift.
family_identity_sql() {
  local _case_id=$1 vendor_file=$2 candidate_file=$3
  write_readonly_preamble
  cat <<SQL
WITH vendor_identity AS MATERIALIZED ($(dashboard_query_core "$vendor_file")),
candidate_identity AS MATERIALIZED ($(dashboard_query_core "$candidate_file")),
metrics AS (
 SELECT (SELECT count(*) FROM vendor_identity) vendor_rows,
        (SELECT count(*) FROM candidate_identity) candidate_rows,
        (SELECT count(*) FROM (SELECT * FROM vendor_identity EXCEPT SELECT * FROM candidate_identity)) missing_candidate,
        (SELECT count(*) FROM (SELECT * FROM candidate_identity EXCEPT SELECT * FROM vendor_identity)) extra_candidate
)
SELECT 'IDENTITY',CASE WHEN vendor_rows>0 AND vendor_rows=candidate_rows AND missing_candidate=0 AND extra_candidate=0 THEN 'PASS' ELSE 'FAIL' END,
       vendor_rows,candidate_rows,missing_candidate,extra_candidate FROM metrics;
SQL
}

family_identity_pass() { awk -F '\t' '$1=="IDENTITY" && $2=="PASS"{ok=1} END{exit !ok}' "$2"; }
family_identity_detail() { tr '\n' ';' < "$2" | sed 's/;$//'; }
family_fixture_check() { :; }
