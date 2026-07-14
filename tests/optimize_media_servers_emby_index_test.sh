#!/usr/bin/env bash
set -euo pipefail

. ./tests/optimize_media_servers_index_test_lib.sh
index_test_init "emby-index"

EMBY_INDEX_NAME="idx_dshadow_mediaitems_parent_type"
EMBY_LATEST_INDEX_NAME="idx_dshadow_emby_latest_gk_dc"
EMBY_LATEST_MOVIES_INDEX_NAME="idx_dshadow_emby_latest_movies_dcn_puk"
EMBY_LATEST_MOVIES_PUK_DC_INDEX_NAME="idx_dshadow_emby_latest_movies_puk_dc_cover"

create_mediaitems_db() {
  local db page_size
  db="$1"
  page_size="${2:-4096}"

  "$real_sqlite" "$db" <<EOF_SQL
PRAGMA page_size=${page_size};
VACUUM;
PRAGMA user_version=4101;
PRAGMA application_id=123456789;
CREATE TABLE MediaItems(
  Id INTEGER PRIMARY KEY, ParentId INTEGER NOT NULL, Type INTEGER NOT NULL,
  Name TEXT, Path TEXT, ProductionYear INTEGER, RunTimeTicks INTEGER, Images TEXT,
  DateCreated INTEGER, SeriesPresentationUniqueKey TEXT,
  PresentationUniqueKey TEXT, UserDataKeyId INTEGER
);
CREATE TABLE AncestorIds2(itemid INTEGER, AncestorId INTEGER, Distance INTEGER);
CREATE TABLE UserDatas(
  UserDataKeyId INTEGER, UserId INTEGER, IsFavorite INTEGER, Played INTEGER,
  PlaybackPositionTicks INTEGER, AudioStreamIndex INTEGER, SubtitleStreamIndex INTEGER,
  PRIMARY KEY (UserDataKeyId, UserId)
) WITHOUT ROWID;
WITH RECURSIVE seq(x) AS (VALUES(1) UNION ALL SELECT x + 1 FROM seq WHERE x < 5000)
INSERT INTO MediaItems(Id,ParentId,Type,Name,Path,ProductionYear,RunTimeTicks,Images,DateCreated,SeriesPresentationUniqueKey,PresentationUniqueKey,UserDataKeyId)
SELECT x, CASE WHEN x <= 4500 THEN x % 25 ELSE x END,
       CASE WHEN x % 20 = 0 THEN 8 ELSE 1 END,
       'item-'||x, '/item/'||x, 1900+(x%120), x*1000, 'img-'||x,
       2000000-x, CASE WHEN x%20=0 THEN 'series-'||(x%7) ELSE NULL END,
       'presentation-'||(x%11), 500000+x
FROM seq;
WITH RECURSIVE m(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM m WHERE x<24)
INSERT INTO MediaItems(Id,ParentId,Type,Name,Path,ProductionYear,RunTimeTicks,Images,DateCreated,PresentationUniqueKey,UserDataKeyId)
SELECT 6200+x,9000+x,5,'movie-'||x,'/movie/'||x,2000+(x%20),6200000+x,'movie-img-'||x,300000-x,'movie-puk-'||x,7200+x FROM m;
INSERT INTO MediaItems(Id,ParentId,Type,Name,Path,ProductionYear,RunTimeTicks,Images,DateCreated,PresentationUniqueKey,UserDataKeyId) VALUES
(6101,1,5,'singleton','/m/6101',2001,6101,'i6101',900,'p-single',7101),
(6102,1,5,'dup-min','/m/6102',2002,6102,'i6102',700,'p-dup',7102),
(6103,1,5,'dup-max','/m/6103',2003,6103,'i6103',800,'p-dup',7103),
(6104,1,5,'equal-min-id','/m/6104',2004,6104,'i6104',600,'p-equal',7104),
(6105,1,5,'equal-other','/m/6105',2005,6105,'i6105',600,'p-equal',7105),
(6106,1,5,'mixed-null','',NULL,6106,NULL,NULL,'p-mixed',7106),
(6107,1,5,'mixed-value','/m/6107',2007,6107,'i6107',500,'p-mixed',7107),
(6108,1,5,'all-null-min','/m/6108',2008,6108,'i6108',NULL,'p-all-null',7108),
(6109,1,5,'all-null-other','/m/6109',2009,6109,'i6109',NULL,'p-all-null',7109),
(6110,1,5,'null-puk-min','/m/6110',2010,6110,'i6110',400,NULL,7110),
(6111,1,5,'null-puk-other','/m/6111',2011,6111,'i6111',450,NULL,7111),
(6112,1,5,'missing-userdata','/m/6112',2012,6112,'i6112',390,'p-missing',7112),
(6113,1,5,'played-zero','/m/6113',2013,6113,'i6113',380,'p-played-zero',7113),
(6114,1,5,'played-one','/m/6114',2014,6114,'i6114',370,'p-played-one',7114),
(6115,1,5,'xb-invisible-min','/m/6115',2015,6115,'i6115',100,'p-xb',7115),
(6116,1,5,'xb-visible-next','/m/6116',2016,6116,'i6116',200,'p-xb',7116),
(6117,1,5,'ub-played-min','/m/6117',2017,6117,'i6117',110,'p-ub',7117),
(6118,1,5,'ub-unplayed-next','/m/6118',2018,6118,'i6118',210,'p-ub',7118);
INSERT INTO AncestorIds2 SELECT Id,100,0 FROM MediaItems WHERE Type=5 AND Id<>6115;
INSERT INTO AncestorIds2 VALUES(6115,999,0);
INSERT INTO UserDatas(UserDataKeyId,UserId,IsFavorite,Played,PlaybackPositionTicks,AudioStreamIndex,SubtitleStreamIndex)
SELECT UserDataKeyId,42,Id%2,CASE WHEN Id=6114 OR Id=6117 THEN 1 ELSE 0 END,Id*10,Id%3,Id%4
FROM MediaItems WHERE Type=5 AND Id<>6112;
CREATE TABLE SyncJobs2(Id INTEGER PRIMARY KEY, Name TEXT);
INSERT INTO SyncJobs2(Id, Name) VALUES(1, 'fixture');
EOF_SQL
}

run_rebuild_capture() {
  local name db page_size optimize_sql
  name="$1"
  db="$2"
  page_size="$3"
  optimize_sql="$4"

  index_test_run_rebuild_capture "$name" sqlite3 "$db" "$page_size" "SELECT 1 FROM SyncJobs2 LIMIT 1;" "$optimize_sql"
}

index_count() {
  local db
  db="$1"
  index_test_index_count "$db" "$EMBY_INDEX_NAME"
}

latest_index_count() {
  local db
  db="$1"
  index_test_index_count "$db" "$EMBY_LATEST_INDEX_NAME"
}

movies_index_count() {
  local db
  db="$1"
  index_test_index_count "$db" "$EMBY_LATEST_MOVIES_INDEX_NAME"
}

movies_puk_dc_index_count() {
  local db
  db="$1"
  index_test_index_count "$db" "$EMBY_LATEST_MOVIES_PUK_DC_INDEX_NAME"
}

movies_vendor_sql() {
  printf '%s' "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) )select A.Id,A.Name,A.Path,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.Images,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex from mediaitems A left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=42 where A.Type=5 AND Coalesce(UserDatas.played, 0)=0 AND A.Id in WithAncestors Group by A.PresentationUniqueKey ORDER BY A.DateCreated DESC LIMIT 12"
}

movies_c2_sql() {
  printf '%s' "WITH ranked(id, dc, puk) AS MATERIALIZED (SELECT A.Id,A.DateCreated,A.PresentationUniqueKey FROM MediaItems AS A INDEXED BY $EMBY_LATEST_MOVIES_INDEX_NAME WHERE A.Type=5 AND EXISTS (SELECT 1 FROM AncestorIds2 AS X WHERE X.ItemId=A.Id AND X.AncestorId IN (100)) AND NOT EXISTS (SELECT 1 FROM UserDatas AS U0 WHERE U0.UserDataKeyId=A.UserDataKeyId AND U0.UserId=42 AND U0.played<>0) AND NOT EXISTS (SELECT 1 FROM MediaItems AS B INDEXED BY $EMBY_LATEST_MOVIES_PUK_DC_INDEX_NAME WHERE B.Type=5 AND B.PresentationUniqueKey IS A.PresentationUniqueKey AND ((B.DateCreated IS NOT NULL AND A.DateCreated IS NULL) OR B.DateCreated>A.DateCreated OR (B.DateCreated IS A.DateCreated AND B.Id<A.Id)) AND EXISTS (SELECT 1 FROM AncestorIds2 AS XB WHERE XB.ItemId=B.Id AND XB.AncestorId IN (100)) AND NOT EXISTS (SELECT 1 FROM UserDatas AS UB WHERE UB.UserDataKeyId=B.UserDataKeyId AND UB.UserId=42 AND UB.played<>0)) ORDER BY (A.DateCreated IS NULL),A.DateCreated DESC,A.PresentationUniqueKey LIMIT 12) SELECT A.Id,A.Name,A.Path,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.Images,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex FROM ranked AS R JOIN MediaItems AS A ON A.Id=R.id LEFT JOIN UserDatas ON A.UserDataKeyId=UserDatas.UserDataKeyId AND UserDatas.UserId=42 ORDER BY (R.dc IS NULL),R.dc DESC,R.puk LIMIT 12"
}

movies_rows() {
  local db sql
  db="$1"
  sql="$2"
  "$real_sqlite" -quote "$db" "$sql"
}

movies_vendor_eqp() {
  local db
  db="$1"
  "$real_sqlite" "$db" "EXPLAIN QUERY PLAN $(movies_vendor_sql)" | tr '\r\n' ' '
}

movies_c2_eqp() {
  local db
  db="$1"
  "$real_sqlite" "$db" "EXPLAIN QUERY PLAN $(movies_c2_sql)" | tr '\r\n' ' '
}

mediaitems_eqp() {
  local db
  db="$1"
  "$real_sqlite" "$db" "EXPLAIN QUERY PLAN SELECT Id FROM MediaItems WHERE ParentId=4600 AND Type=8;" | tr '\r\n' ' '
}

latest_eqp() {
  local db
  db="$1"
  "$real_sqlite" "$db" "EXPLAIN QUERY PLAN SELECT Id FROM MediaItems WHERE Type=8 AND coalesce(SeriesPresentationUniqueKey, PresentationUniqueKey)='series-6' ORDER BY DateCreated DESC LIMIT 1;" | tr '\r\n' ' '
}

assert_eqp_uses_emby_index() {
  local eqp phase
  eqp="$1"
  phase="$2"
  assert_contains "$eqp" "SEARCH" "$phase EQP search"
  assert_contains "$eqp" "$EMBY_INDEX_NAME" "$phase EQP index"
  assert_not_contains "$eqp" "SCAN" "$phase EQP no scan"
}

assert_eqp_uses_latest_index() {
  local eqp phase
  eqp="$1"
  phase="$2"
  assert_contains "$eqp" "SEARCH" "$phase latest EQP search"
  assert_contains "$eqp" "$EMBY_LATEST_INDEX_NAME" "$phase latest EQP index"
  assert_not_contains "$eqp" "USE TEMP B-TREE" "$phase latest EQP no temp sort"
}

assert_movies_c2_eqp() {
  local eqp phase
  eqp="$1"
  phase="$2"
  assert_contains "$eqp" "USING COVERING INDEX $EMBY_LATEST_MOVIES_INDEX_NAME" "$phase movies outer covering index"
  assert_contains "$eqp" "USING COVERING INDEX $EMBY_LATEST_MOVIES_PUK_DC_INDEX_NAME" "$phase movies inner covering index"
  assert_not_contains "$eqp" "idx_MediaItems47b2" "$phase native index absent"
}

assert_movies_vendor_eqp() {
  local eqp phase
  eqp="$1"
  phase="$2"
  case "$eqp" in
    *"$EMBY_LATEST_MOVIES_INDEX_NAME"*)
      printf 'INFO: %s vendor EQP adopts %s\n' "$phase" "$EMBY_LATEST_MOVIES_INDEX_NAME"
      ;;
    *)
      printf 'INFO: %s vendor EQP does not adopt %s\n' "$phase" "$EMBY_LATEST_MOVIES_INDEX_NAME"
      ;;
  esac
  case "$eqp" in
    *"$EMBY_LATEST_MOVIES_PUK_DC_INDEX_NAME"*)
      printf 'INFO: %s vendor EQP adopts %s\n' "$phase" "$EMBY_LATEST_MOVIES_PUK_DC_INDEX_NAME"
      ;;
    *)
      printf 'INFO: %s vendor EQP does not adopt %s\n' "$phase" "$EMBY_LATEST_MOVIES_PUK_DC_INDEX_NAME"
      ;;
  esac
}

optimize_sql="$(build_emby_optimize_sql)"

published="$tmp/published.db"
create_mediaitems_db "$published"
run_rebuild_capture published-first "$published" 4096 "$optimize_sql"
assert_eq 0 "$(cat "$tmp/published-first.rc")" "first Emby index pipeline rc"
assert_eq "1" "$(index_count "$published")" "published Emby index count"
assert_eq "1" "$(latest_index_count "$published")" "published Emby latest index count"
assert_eq "1" "$(movies_index_count "$published")" "published Emby movies outer index count"
assert_eq "1" "$(movies_puk_dc_index_count "$published")" "published Emby movies inner index count"
assert_eq "CREATE INDEX $EMBY_LATEST_MOVIES_INDEX_NAME ON MediaItems ((DateCreated IS NULL), DateCreated DESC, PresentationUniqueKey, Id, UserDataKeyId) WHERE Type = 5" "$($real_sqlite "$published" "SELECT sql FROM sqlite_master WHERE name='$EMBY_LATEST_MOVIES_INDEX_NAME';")" "published movies outer sqlite_master.sql"
assert_eq "CREATE INDEX $EMBY_LATEST_MOVIES_PUK_DC_INDEX_NAME ON MediaItems (PresentationUniqueKey, DateCreated, Id, UserDataKeyId) WHERE Type = 5" "$($real_sqlite "$published" "SELECT sql FROM sqlite_master WHERE name='$EMBY_LATEST_MOVIES_PUK_DC_INDEX_NAME';")" "published movies inner sqlite_master.sql"
stat1_count="$("$real_sqlite" "$published" "SELECT COUNT(*) FROM sqlite_stat1;")"
assert_int_gt "$stat1_count" 0 "published sqlite_stat1 row count"
stat1_mediaitems_count="$("$real_sqlite" "$published" "SELECT COUNT(*) FROM sqlite_stat1 WHERE tbl='MediaItems' OR idx IN ('$EMBY_INDEX_NAME','$EMBY_LATEST_INDEX_NAME','$EMBY_LATEST_MOVIES_INDEX_NAME','$EMBY_LATEST_MOVIES_PUK_DC_INDEX_NAME');")"
assert_int_gt "$stat1_mediaitems_count" 0 "published sqlite_stat1 MediaItems/index row count"
stat4_exists="$("$real_sqlite" "$published" "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='sqlite_stat4';")"
if [ "$stat4_exists" = "1" ]; then
  stat4_count="$("$real_sqlite" "$published" "SELECT COUNT(*) FROM sqlite_stat4 WHERE tbl='MediaItems' OR idx IN ('$EMBY_INDEX_NAME','$EMBY_LATEST_INDEX_NAME','$EMBY_LATEST_MOVIES_INDEX_NAME','$EMBY_LATEST_MOVIES_PUK_DC_INDEX_NAME');")"
  printf 'sqlite_stat4 MediaItems/index rows: %s\n' "$stat4_count"
else
  printf 'SKIP: sqlite_stat4 unavailable in this sqlite3 build\n'
fi
assert_eqp_uses_emby_index "$(mediaitems_eqp "$published")" "published post-ANALYZE"
assert_eqp_uses_latest_index "$(latest_eqp "$published")" "published post-ANALYZE"
assert_movies_c2_eqp "$(movies_c2_eqp "$published")" "published post-ANALYZE"
assert_movies_vendor_eqp "$(movies_vendor_eqp "$published")" "published post-ANALYZE"

run_rebuild_capture published-second "$published" 4096 "$optimize_sql"
assert_eq 0 "$(cat "$tmp/published-second.rc")" "second Emby index pipeline rc"
assert_eq "" "$(cat "$tmp/published-second.err")" "second Emby index pipeline stderr"
assert_eq "1" "$(index_count "$published")" "idempotent Emby index count"
assert_eq "1" "$(latest_index_count "$published")" "idempotent Emby latest index count"
assert_eq "1" "$(movies_index_count "$published")" "idempotent Emby movies outer index count"
assert_eq "1" "$(movies_puk_dc_index_count "$published")" "idempotent Emby movies inner index count"

warn_db="$tmp/warn-not-abort.db"
create_mediaitems_db "$warn_db" 1024
assert_eq "1024" "$("$real_sqlite" "$warn_db" "PRAGMA page_size;")" "warn-not-abort source page_size"
export INDEX_TEST_FAIL_SQL=1
export INDEX_TEST_FAIL_NEEDLE="$EMBY_INDEX_NAME"
run_rebuild_capture warn-not-abort "$warn_db" 4096 "$optimize_sql"
unset INDEX_TEST_FAIL_SQL INDEX_TEST_FAIL_NEEDLE
assert_eq 0 "$(cat "$tmp/warn-not-abort.rc")" "warn-not-abort rc"
assert_contains "$(cat "$tmp/warn-not-abort.err")" "WARNING: staged maintenance SQL failed" "warn-not-abort warning"
assert_eq "4096" "$("$real_sqlite" "$warn_db" "PRAGMA page_size;")" "warn-not-abort published page_size"
assert_eq "0" "$(index_count "$warn_db")" "warn-not-abort no index"
assert_eq "0" "$(latest_index_count "$warn_db")" "warn-not-abort no latest index"
assert_eq "0" "$(movies_index_count "$warn_db")" "warn-not-abort no movies outer index"
assert_eq "0" "$(movies_puk_dc_index_count "$warn_db")" "warn-not-abort no movies inner index"

gate_db="$tmp/staged-gate.db"
create_mediaitems_db "$gate_db"
gate_hash_before="$(sha256_file "$gate_db")"
export INDEX_TEST_CORRUPT_STAGED_ON_INTEGRITY=1
export INDEX_TEST_CORRUPT_STAGED_DB="$gate_db.new"
run_rebuild_capture staged-gate "$gate_db" 4096 "$optimize_sql"
unset INDEX_TEST_CORRUPT_STAGED_ON_INTEGRITY INDEX_TEST_CORRUPT_STAGED_DB
assert_eq 1 "$(cat "$tmp/staged-gate.rc")" "staged integrity gate rc"
assert_eq "$gate_hash_before" "$(sha256_file "$gate_db")" "staged integrity gate live hash"
[ ! -e "$gate_db.new" ] || fail "staged integrity gate cleanup" "no staged file" "$gate_db.new exists"
assert_contains "$(cat "$tmp/staged-gate.err")" "integrity check failed" "staged integrity gate diagnostic"

eqp_db="$tmp/eqp.db"
create_mediaitems_db "$eqp_db"
vendor_rows_before="$(movies_rows "$eqp_db" "$(movies_vendor_sql)")"
vendor_eqp_before="$(movies_vendor_eqp "$eqp_db")"
for ddl in "${_EMBY_INDEXES[@]}"; do
  "$real_sqlite" "$eqp_db" "$ddl"
done
assert_eqp_uses_emby_index "$(mediaitems_eqp "$eqp_db")" "pre-ANALYZE"
assert_eqp_uses_latest_index "$(latest_eqp "$eqp_db")" "pre-ANALYZE"
assert_movies_c2_eqp "$(movies_c2_eqp "$eqp_db")" "pre-ANALYZE"
assert_movies_vendor_eqp "$(movies_vendor_eqp "$eqp_db")" "pre-ANALYZE"
assert_movies_vendor_eqp "$vendor_eqp_before" "before indexes"
assert_eq "$vendor_rows_before" "$(movies_rows "$eqp_db" "$(movies_vendor_sql)")" "vendor rows stable before ANALYZE"
assert_eq "$vendor_rows_before" "$(movies_rows "$eqp_db" "$(movies_c2_sql)")" "C2/vendor full-row identity before ANALYZE"
"$real_sqlite" "$eqp_db" "ANALYZE;"
assert_eqp_uses_emby_index "$(mediaitems_eqp "$eqp_db")" "post-ANALYZE"
assert_eqp_uses_latest_index "$(latest_eqp "$eqp_db")" "post-ANALYZE"
assert_movies_c2_eqp "$(movies_c2_eqp "$eqp_db")" "post-ANALYZE"
assert_movies_vendor_eqp "$(movies_vendor_eqp "$eqp_db")" "post-ANALYZE"
assert_eq "$vendor_rows_before" "$(movies_rows "$eqp_db" "$(movies_vendor_sql)")" "vendor rows stable after ANALYZE"
assert_eq "$vendor_rows_before" "$(movies_rows "$eqp_db" "$(movies_c2_sql)")" "C2/vendor full-row identity after ANALYZE"
assert_eq "1" "$("$real_sqlite" "$eqp_db" "SELECT COUNT(*) FROM MediaItems INDEXED BY $EMBY_INDEX_NAME WHERE ParentId=4600 AND Type=8;")" "INDEXED BY usability probe"
latest_indexed_count="$("$real_sqlite" "$eqp_db" "SELECT COUNT(*) FROM MediaItems INDEXED BY $EMBY_LATEST_INDEX_NAME WHERE Type=8 AND coalesce(SeriesPresentationUniqueKey, PresentationUniqueKey)='series-6';")"
assert_int_gt "$latest_indexed_count" 0 "latest INDEXED BY usability probe"
assert_int_gt "$($real_sqlite "$eqp_db" "SELECT COUNT(*) FROM MediaItems INDEXED BY $EMBY_LATEST_MOVIES_INDEX_NAME WHERE Type=5;")" 0 "movies outer INDEXED BY usability probe"
assert_int_gt "$($real_sqlite "$eqp_db" "SELECT COUNT(*) FROM MediaItems INDEXED BY $EMBY_LATEST_MOVIES_PUK_DC_INDEX_NAME WHERE Type=5;")" 0 "movies inner INDEXED BY usability probe"
assert_eq "0" "$($real_sqlite "$eqp_db" "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name='idx_MediaItems47b2';")" "native movies index absent"

printf 'optimize_media_servers Emby index tests passed\n'
