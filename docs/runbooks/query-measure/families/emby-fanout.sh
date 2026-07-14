# shellcheck shell=bash disable=SC2034
# Query-family data for Emby resume fan-out shapes. Sourced by query-measure.sh.

family_configure() {
  require_env EMBY_DB_SOURCE
  SOURCE_DB=$EMBY_DB_SOURCE
  FANOUT_ANCESTOR_SIZE=${FANOUT_ANCESTOR_SIZE:-20}
  require_uint FANOUT_ANCESTOR_SIZE
  [ "$FANOUT_ANCESTOR_SIZE" -ge 1 ] || die 'FANOUT_ANCESTOR_SIZE must be at least 1'
  FANOUT_USER_ID=
  FANOUT_ANCESTORS=
}
family_all_cases() { printf '%s\n' resume-12 resume-24 resume-empty resume-two-group resume-large-group; }
family_cases() {
  local -a selected
  if [ -n "${CASES:-}" ]; then read -r -a selected <<< "$CASES"; printf '%s\n' "${selected[@]}"; else family_all_cases; fi
}

family_contract_check() {
  local file actual expected
  grep -Fx '    X(EMBY_RESUME, "emby", "fanout+resume", "emby_fts_rewrite", 0) \' "${REPO_ROOT}/src/rewrite_modes.h" >/dev/null || die 'Emby resume mode catalogue contract drifted'
  grep -Fx '    X(EMBY_RESUME_SIMPLE, "emby", "fanout+resume_simple", "emby_fts_rewrite", 0) \' "${REPO_ROOT}/src/rewrite_modes.h" >/dev/null || die 'Emby resume-simple mode catalogue contract drifted'
  [ "$(family_all_cases | wc -l | tr -d ' ')" -eq 5 ] || die 'Emby fan-out case inventory incomplete'
  for file in c-empty.sql c-large-group.sql c-two-group.sql; do
    case "$file" in
      c-empty.sql) expected=b304d3dfd9a6b67cf5d23e1930af97414b42e280196f3c8429b4b840e7fcbde3 ;;
      c-large-group.sql) expected=d2cea8a83f882f512e5fee17df0f3f193f2dcd1a3dada2819b0ce742b2b181e9 ;;
      c-two-group.sql) expected=c6d9e049a666d363514f3e0752597191728dd2da7676dd89479fdc073a0b6126 ;;
    esac
    actual=$(sha256sum "${HARNESS_DIR}/sql/emby-fanout/$file" | awk '{print $1}')
    [ "$actual" = "$expected" ] || die "Emby fan-out static capture hash mismatch: $file"
  done
  while IFS= read -r _case; do family_all_cases | grep -Fx "$_case" >/dev/null || die "unknown Emby fan-out case: $_case"; done < <(family_cases)
}
family_parameters() { :; }

family_derive_sql() {
  cat <<SQL
SELECT 'USER',UserId,count(*) AS n FROM UserDatas WHERE playbackPositionTicks>0 GROUP BY UserId ORDER BY n DESC,UserId LIMIT 1;
SELECT 'ANCESTOR',X.AncestorId,count(*) AS n FROM AncestorIds2 AS X JOIN MediaItems AS A ON A.Id=X.ItemId WHERE A.Type IN (5,8) GROUP BY X.AncestorId ORDER BY n DESC,X.AncestorId LIMIT ${FANOUT_ANCESTOR_SIZE};
SQL
}
family_parse_derived() {
  local out=$1
  FANOUT_USER_ID=$(awk -F '\t' '$1=="USER" && $2~/^[0-9]+$/{print $2;exit}' "$out")
  FANOUT_ANCESTORS=$(awk -F '\t' '$1=="ANCESTOR" && $2~/^[0-9]+$/{print $2}' "$out" | paste -sd, -)
  [ -n "$FANOUT_USER_ID" ] || die 'Emby fan-out user derivation returned no numeric user'
  [ -n "$FANOUT_ANCESTORS" ] || die 'Emby fan-out ancestor derivation returned no numeric ancestors'
}
family_record_literals() {
  printf 'user\tid=%s\tderived=1\n' "$FANOUT_USER_ID"
  printf 'ancestors\tcount=%s\tvalues=%s\tderived=1\n' "$(printf '%s' "$FANOUT_ANCESTORS" | awk -F, '{print NF}')" "$FANOUT_ANCESTORS"
}
family_prepare_sql() { :; }

fanout_replace() {
  local old=$1 new=$2
  awk -v old="$old" -v new="$new" 'BEGIN{if(old=="")exit 2}{out="";rest=$0;while((p=index(rest,old))>0){out=out substr(rest,1,p-1)new;rest=substr(rest,p+length(old))}print out rest}'
}
fanout_exists() { printf 'EXISTS (SELECT 1 FROM AncestorIds2 WHERE AncestorIds2.itemid = A.Id AND AncestorIds2.AncestorId in (%s))' "$FANOUT_ANCESTORS"; }
fanout_conjunct() { printf 'AND ((A.Type=5 AND A.UserDataKeyId IN (SELECT UserDataKeyId FROM UserDatas WHERE UserId=%s AND playbackPositionTicks>0)) OR (A.Type=8 AND A.SeriesPresentationUniqueKey IN (SELECT N2.SeriesPresentationUniqueKey FROM MediaItems N2 JOIN UserDatas UN2 ON N2.UserDataKeyId=UN2.UserDataKeyId AND UN2.UserId=%s WHERE N2.Type=8 AND Coalesce(N2.SortParentIndexNumber,N2.ParentIndexNumber,-1) <> 0 AND (UN2.Played=1 OR UN2.playbackPositionTicks>0))))' "$FANOUT_USER_ID" "$FANOUT_USER_ID"; }

fanout_vendor() {
  local case_id=$1 capture sql exists
  case "$case_id" in
    resume-12)
      printf 'with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (%s) )select count(*) OVER() AS TotalRecordCount,A.type,A.Id,A.EndDate,A.IndexNumber,A.Name,A.Path,A.ParentIndexNumber,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.SeriesName,A.AlbumId,A.SeriesId,A.Images,A.SortIndexNumber,A.SortParentIndexNumber,A.IndexNumberEnd,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex from mediaitems A join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=%s where A.Type in (5,8) AND UserDatas.playbackPositionTicks > 0 AND Coalesce(UserDatas.HideFromResume,0)=0 AND COALESCE((select hidefromresume from userdatas where userdatas.userid=%s and userdatas.userdatakeyid=(select userdatakeyid from mediaitems where mediaitems.id=A.seriesid)),0)=0 AND A.Id in WithAncestors Group by A.PresentationUniqueKey ORDER BY UserDatas.LastPlayedDateInt DESC LIMIT 12;\n' "$FANOUT_ANCESTORS" "$FANOUT_USER_ID" "$FANOUT_USER_ID"
      ;;
    resume-24)
      printf 'with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (%s) )select A.type,A.Id,A.EndDate,A.CommunityRating,A.IndexNumber,A.Name,A.Path,A.PremiereDate,A.ParentIndexNumber,A.ProductionYear,A.OfficialRating,A.RunTimeTicks,A.guid,A.ParentId,A.CriticRating,A.SeriesName,A.AlbumId,A.SeriesId,A.Images,A.SortIndexNumber,A.SortParentIndexNumber,A.IndexNumberEnd,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex from mediaitems A join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=%s where A.Type in (5,8) AND UserDatas.playbackPositionTicks > 0 AND Coalesce(UserDatas.HideFromResume,0)=0 AND COALESCE((select hidefromresume from userdatas where userdatas.userid=%s and userdatas.userdatakeyid=(select userdatakeyid from mediaitems where mediaitems.id=A.seriesid)),0)=0 AND A.Id in WithAncestors Group by A.PresentationUniqueKey ORDER BY UserDatas.LastPlayedDateInt DESC LIMIT 24;\n' "$FANOUT_ANCESTORS" "$FANOUT_USER_ID" "$FANOUT_USER_ID"
      ;;
    resume-empty|resume-large-group|resume-two-group)
      case "$case_id" in resume-empty) capture=c-empty.sql ;; resume-large-group) capture=c-large-group.sql ;; *) capture=c-two-group.sql ;; esac
      sql=$(sed -e "s/__EMBY_USER_ID__/${FANOUT_USER_ID}/g" -e "s/__ANCESTORS__/${FANOUT_ANCESTORS}/g" "${HARNESS_DIR}/sql/emby-fanout/$capture")
      exists=$(fanout_exists)
      printf '%s\n' "$sql" | fanout_replace "$exists" 'A.Id in WithAncestors'
      ;;
    *) die "unknown Emby fan-out case: $case_id" ;;
  esac
}

family_render() {
  local case_id=$1 arm=$2 exists conjunct
  if [ "$arm" = vendor ]; then fanout_vendor "$case_id"; return; fi
  exists=$(fanout_exists)
  if [ "$case_id" = resume-12 ] || [ "$case_id" = resume-24 ]; then
    fanout_vendor "$case_id" | fanout_replace 'A.Id in WithAncestors' "$exists"
  else
    conjunct=$(fanout_conjunct)
    fanout_vendor "$case_id" | fanout_replace 'A.Id in WithAncestors' "$exists" | fanout_replace ' Group by coalesce(' " $conjunct Group by coalesce("
  fi
}

fanout_core() { sed '$s/;[[:space:]]*$//' "$1"; }
family_identity_sql() {
  local _case=$1 vendor=$2 candidate=$3
  write_readonly_preamble
  cat <<SQL
WITH vendor_identity AS MATERIALIZED ($(fanout_core "$vendor")),
candidate_identity AS MATERIALIZED ($(fanout_core "$candidate")),
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
