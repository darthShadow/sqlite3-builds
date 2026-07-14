#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "$0")/.." && pwd -P)"
cd "$repo_root"

. ./scripts/optimize_media_servers.sh

if grep -E '^[[:space:]]*"PRAGMA temp_store=(MEMORY|2);"[[:space:]]*\\$' scripts/optimize_media_servers.sh >/dev/null; then
  printf 'FATAL: maintenance script still forces MEMORY temp storage for VACUUM\n' >&2
  exit 1
fi

tmp_parent="${TMPDIR:-/tmp}"
tmp="$(mktemp -d "${tmp_parent%/}/sqlite3-optimize-pipeline.XXXXXX" 2>/dev/null || mktemp -d /tmp/sqlite3-optimize-pipeline.XXXXXX)"
trap 'rm -rf "$tmp"' EXIT

fail() {
  local message expected actual
  message="$1"
  expected="$2"
  actual="$3"
  printf 'FATAL: %s: expected [%s], actual [%s]\n' "$message" "$expected" "$actual" >&2
  exit 1
}

assert_eq() {
  local expected actual message
  expected="$1"
  actual="$2"
  message="$3"
  [ "$actual" = "$expected" ] || fail "$message" "$expected" "$actual"
}

assert_contains() {
  local haystack needle message
  haystack="$1"
  needle="$2"
  message="$3"
  case "$haystack" in
    *"$needle"*) ;;
    *) fail "$message" "contains [$needle]" "$haystack" ;;
  esac
}

assert_not_contains() {
  local haystack needle message
  haystack="$1"
  needle="$2"
  message="$3"
  case "$haystack" in
    *"$needle"*) fail "$message" "not contains [$needle]" "$haystack" ;;
  esac
}

sha256_file() {
  sha256sum "$1" | awk '{print $1}'
}

count_log_occurrences() {
  local needle file
  needle="$1"
  file="$2"
  awk -v needle="$needle" 'index($0, needle) { count++ } END { print count + 0 }' "$file"
}

assert_line_lt() {
  local left right message
  left="$1"
  right="$2"
  message="$3"
  [ -n "$left" ] || fail "$message" "left line number set" "$left:$right"
  [ -n "$right" ] || fail "$message" "right line number set" "$left:$right"
  [ "$left" -lt "$right" ] || fail "$message" "$left < $right" "$left >= $right"
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

movies_vendor_sql() {
  printf '%s' "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) )select A.Id,A.Name,A.Path,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.Images,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex from mediaitems A left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=42 where A.Type=5 AND Coalesce(UserDatas.played, 0)=0 AND A.Id in WithAncestors Group by A.PresentationUniqueKey ORDER BY A.DateCreated DESC LIMIT 12"
}

movies_c2_sql() {
  printf '%s' "WITH ranked(id, dc, puk) AS MATERIALIZED (SELECT A.Id,A.DateCreated,A.PresentationUniqueKey FROM MediaItems AS A INDEXED BY idx_dshadow_emby_latest_movies_dcn_puk WHERE A.Type=5 AND EXISTS (SELECT 1 FROM AncestorIds2 AS X WHERE X.ItemId=A.Id AND X.AncestorId IN (100)) AND NOT EXISTS (SELECT 1 FROM UserDatas AS U0 WHERE U0.UserDataKeyId=A.UserDataKeyId AND U0.UserId=42 AND U0.played<>0) AND NOT EXISTS (SELECT 1 FROM MediaItems AS B INDEXED BY idx_dshadow_emby_latest_movies_puk_dc_cover WHERE B.Type=5 AND B.PresentationUniqueKey IS A.PresentationUniqueKey AND ((B.DateCreated IS NOT NULL AND A.DateCreated IS NULL) OR B.DateCreated>A.DateCreated OR (B.DateCreated IS A.DateCreated AND B.Id<A.Id)) AND EXISTS (SELECT 1 FROM AncestorIds2 AS XB WHERE XB.ItemId=B.Id AND XB.AncestorId IN (100)) AND NOT EXISTS (SELECT 1 FROM UserDatas AS UB WHERE UB.UserDataKeyId=B.UserDataKeyId AND UB.UserId=42 AND UB.played<>0)) ORDER BY (A.DateCreated IS NULL),A.DateCreated DESC,A.PresentationUniqueKey LIMIT 12) SELECT A.Id,A.Name,A.Path,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.Images,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex FROM ranked AS R JOIN MediaItems AS A ON A.Id=R.id LEFT JOIN UserDatas ON A.UserDataKeyId=UserDatas.UserDataKeyId AND UserDatas.UserId=42 ORDER BY (R.dc IS NULL),R.dc DESC,R.puk LIMIT 12"
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

assert_movies_c2_eqp() {
  local eqp phase
  eqp="$1"
  phase="$2"
  assert_contains "$eqp" "USING COVERING INDEX idx_dshadow_emby_latest_movies_dcn_puk" "$phase movies outer covering index"
  assert_contains "$eqp" "USING COVERING INDEX idx_dshadow_emby_latest_movies_puk_dc_cover" "$phase movies inner covering index"
  assert_not_contains "$eqp" "idx_MediaItems47b2" "$phase native index absent"
}

assert_movies_vendor_eqp() {
  local eqp phase
  eqp="$1"
  phase="$2"
  case "$eqp" in
    *"idx_dshadow_emby_latest_movies_dcn_puk"*)
      printf 'INFO: %s vendor EQP adopts %s\n' "$phase" "idx_dshadow_emby_latest_movies_dcn_puk"
      ;;
    *)
      printf 'INFO: %s vendor EQP does not adopt %s\n' "$phase" "idx_dshadow_emby_latest_movies_dcn_puk"
      ;;
  esac
  case "$eqp" in
    *"idx_dshadow_emby_latest_movies_puk_dc_cover"*)
      printf 'INFO: %s vendor EQP adopts %s\n' "$phase" "idx_dshadow_emby_latest_movies_puk_dc_cover"
      ;;
    *)
      printf 'INFO: %s vendor EQP does not adopt %s\n' "$phase" "idx_dshadow_emby_latest_movies_puk_dc_cover"
      ;;
  esac
}

assert_eqp_uses_emby_index() {
  local eqp phase
  eqp="$1"
  phase="$2"
  assert_contains "$eqp" "SEARCH" "$phase EQP search"
  assert_contains "$eqp" "idx_dshadow_mediaitems_parent_type" "$phase EQP index"
  assert_not_contains "$eqp" "SCAN" "$phase EQP no scan"
}

assert_eqp_uses_latest_index() {
  local eqp phase
  eqp="$1"
  phase="$2"
  assert_contains "$eqp" "SEARCH" "$phase latest EQP search"
  assert_contains "$eqp" "idx_dshadow_emby_latest_gk_dc" "$phase latest EQP index"
  assert_not_contains "$eqp" "USE TEMP B-TREE" "$phase latest EQP no temp sort"
}

real_sqlite="$(command -v sqlite3 || true)"
[ -n "$real_sqlite" ] || fail "sqlite3 availability" "sqlite3 in PATH" "missing"

mkdir -p "$tmp/bin"
cat > "$tmp/bin/sqlite3" <<'EOF_SQLITE'
#!/usr/bin/env bash
sql_text=""
stdin_sql=""
if [ ! -t 0 ]; then
  stdin_sql="$(cat)"
fi
for arg in "$@"; do
  sql_text="${sql_text}${arg}
"
done
sql_text="${sql_text}${stdin_sql}"

case "$sql_text" in
  *"VACUUM"*)
    printf '%s\n' "$sql_text" | tr '\n' ' ' >> "$SQLITE_VACUUM_LOG"
    printf '\n' >> "$SQLITE_VACUUM_LOG"
    ;;
esac

case "$sql_text" in
  *"VALUES('rebuild')"*|*"VALUES('optimize')"*)
    printf '%s\n' "$sql_text" | tr '\n' ' ' | sed 's/[[:space:]][[:space:]]*/ /g; s/[[:space:]]$//' >> "$SQLITE_CALL_LOG"
    printf '\n' >> "$SQLITE_CALL_LOG"
    ;;
esac

case "$sql_text" in
  *"PRAGMA integrity_check;"*|*"PRAGMA integrity_check(1);"*)
    printf '%s\n' "$sql_text" | tr '\n' ' ' | sed 's/[[:space:]][[:space:]]*/ /g; s/[[:space:]]$//' >> "$SQLITE_INTEGRITY_LOG"
    printf '\n' >> "$SQLITE_INTEGRITY_LOG"
    ;;
esac

case "$sql_text" in
  *"PRAGMA integrity_check;"*) printf 'integrity\n' >> "$SQLITE_PIPELINE_ORDER_LOG" ;;
  *'DELETE FROM "fts4_tag_titles_icu"'*) printf 'recurate\n' >> "$SQLITE_PIPELINE_ORDER_LOG" ;;
  *"name GLOB 'fts4_tag_titles_icu_"*) printf 'shadow-discovery\n' >> "$SQLITE_PIPELINE_ORDER_LOG" ;;
  *'PRAGMA integrity_check("fts4_tag_titles_icu_'*) printf 'shadow-integrity\n' >> "$SQLITE_PIPELINE_ORDER_LOG" ;;
esac

case "$sql_text" in
  *"name GLOB 'fts4_tag_titles_icu_"*) printf 'shadow-discovery\n' >> "$SQLITE_SHADOW_GATE_LOG" ;;
  *'PRAGMA integrity_check("fts4_tag_titles_icu_'*) printf 'shadow-integrity\n' >> "$SQLITE_SHADOW_GATE_LOG" ;;
esac

case "$sql_text" in
  *"ANALYZE;"*|*"PRAGMA optimize=0x10002;"*)
    printf '%s\n' "$sql_text" | tr '\n' ' ' | sed 's/[[:space:]][[:space:]]*/ /g; s/[[:space:]]$//' >> "$SQLITE_MAINTENANCE_LOG"
    printf '\n' >> "$SQLITE_MAINTENANCE_LOG"
    ;;
esac

case "$sql_text" in
  *"PRAGMA wal_checkpoint(TRUNCATE);"*)
    if [ "${BUSY_WAL_CHECKPOINT:-0}" = "1" ]; then
      printf '1|0|0\n'
      exit 0
    fi
    ;;
esac

if [ "${FAIL_REBUILD:-0}" = "1" ]; then
  for arg in "$@"; do
    case "$arg" in
      *"VALUES('rebuild')"*) exit 1 ;;
    esac
  done
fi

if [ -n "${TRIM_BAD_INTEGRITY_MARKER:-}" ] && [ -e "$TRIM_BAD_INTEGRITY_MARKER" ]; then
  case "$sql_text" in
    *"PRAGMA integrity_check;"*)
      printf 'not ok\n'
      exit 0
      ;;
  esac
fi

for arg in "$@"; do
  case "$arg" in
    *"VACUUM INTO "*)
      "$REAL_SQLITE" "$@"
      rc=$?
      if [ "$rc" -eq 0 ] && [ "${CORRUPT_STAGED_AFTER_VACUUM:-0}" = "1" ]; then
        "$REAL_SQLITE" "$CORRUPT_STAGED_DB" "UPDATE docs SET body='stage-mismatch' WHERE id=1;"
      fi
      if [ "$rc" -eq 0 ] && [ "${ALTER_STAGED_PRAGMAS_AFTER_VACUUM:-0}" = "1" ]; then
        "$REAL_SQLITE" "$PRAGMA_MISMATCH_STAGED_DB" "PRAGMA user_version=999; PRAGMA application_id=123456;"
      fi
      exit "$rc"
      ;;
  esac
done

if [ -n "$stdin_sql" ]; then
  printf '%s\n' "$stdin_sql" | "$REAL_SQLITE" "$@"
  rc=$?
  if [ "$rc" -eq 0 ] && [ -n "${TRIM_BAD_INTEGRITY_MARKER:-}" ]; then
    case "$stdin_sql" in
      *"DELETE FROM blobs"*) : > "$TRIM_BAD_INTEGRITY_MARKER" ;;
    esac
  fi
  exit "$rc"
fi

exec "$REAL_SQLITE" "$@"
EOF_SQLITE
chmod +x "$tmp/bin/sqlite3"
cat > "$tmp/bin/docker" <<'EOF_DOCKER'
#!/usr/bin/env bash
printf 'SKIP: Docker stopped-container gate is not exercised by local helper smoke tests\n' >&2
exit 75
EOF_DOCKER
chmod +x "$tmp/bin/docker"
PATH="$tmp/bin:$PATH"
export PATH REAL_SQLITE="$real_sqlite"
export SQLITE_CALL_LOG="$tmp/sqlite-calls.log"
export SQLITE_INTEGRITY_LOG="$tmp/sqlite-integrity.log"
export SQLITE_MAINTENANCE_LOG="$tmp/sqlite-maintenance.log"
export SQLITE_PIPELINE_ORDER_LOG="$tmp/sqlite-pipeline-order.log"
export SQLITE_SHADOW_GATE_LOG="$tmp/sqlite-shadow-gate.log"
export SQLITE_VACUUM_LOG="$tmp/sqlite-vacuum.log"
: > "$SQLITE_CALL_LOG"
: > "$SQLITE_INTEGRITY_LOG"
: > "$SQLITE_MAINTENANCE_LOG"
: > "$SQLITE_PIPELINE_ORDER_LOG"
: > "$SQLITE_SHADOW_GATE_LOG"
: > "$SQLITE_VACUUM_LOG"

backup_dir="$tmp/backups"
mkdir -p "$backup_dir"

run_rebuild_capture() {
  local name binary db page_size auto_vacuum sanity post_sql hook final_hook
  name="$1"
  binary="$2"
  db="$3"
  page_size="$4"
  auto_vacuum="$5"
  sanity="$6"
  post_sql="$7"
  hook="$8"
  final_hook="${9:-}"
  set +e
  (
    cd "$repo_root"
    . ./scripts/optimize_media_servers.sh
    rebuild_db_vacuum_into "$binary" "$db" "$backup_dir" "$page_size" "$auto_vacuum" "$sanity" "$post_sql" "$hook" "" "" "$final_hook"
  ) >"$tmp/${name}.out" 2>"$tmp/${name}.err"
  rc=$?
  set -e
  printf '%s' "$rc" > "$tmp/${name}.rc"
}

create_normal_fts_db() {
  local db
  db="$1"
  "$real_sqlite" "$db" <<'EOF_SQL'
PRAGMA user_version=4101;
PRAGMA application_id=123456789;
CREATE TABLE media(id INTEGER PRIMARY KEY, title TEXT);
INSERT INTO media(id, title) VALUES(1, 'alpha');
CREATE VIRTUAL TABLE fts_search9 USING fts5(title);
INSERT INTO fts_search9(rowid, title) VALUES(1, 'alpha');
EOF_SQL
}

add_mediaitems_fixture() {
  local db
  db="$1"
  "$real_sqlite" "$db" <<'EOF_SQL'
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
EOF_SQL
}

create_external_fts_db() {
  local db
  db="$1"
  "$real_sqlite" "$db" <<'EOF_SQL'
CREATE TABLE docs(id INTEGER PRIMARY KEY, body TEXT);
INSERT INTO docs(id, body) VALUES(1, 'alpha');
CREATE VIRTUAL TABLE fts5_external USING fts5(body, content='docs', content_rowid='id');
INSERT INTO fts5_external(rowid, body) VALUES(1, 'alpha');
EOF_SQL
}

create_fk_orphan_db() {
  local db
  db="$1"
  "$real_sqlite" "$db" <<'EOF_SQL'
PRAGMA foreign_keys=OFF;
CREATE TABLE parent(id INTEGER PRIMARY KEY);
CREATE TABLE child(id INTEGER PRIMARY KEY, parent_id INTEGER NOT NULL REFERENCES parent(id));
INSERT INTO child(id, parent_id) VALUES(1, 99);
EOF_SQL
}

create_stats_db() {
  local db
  db="$1"
  "$real_sqlite" "$db" <<'EOF_SQL'
PRAGMA user_version=4101;
PRAGMA application_id=123456789;
CREATE TABLE media(id INTEGER PRIMARY KEY, title TEXT);
INSERT INTO media(id, title) VALUES(1, 'plex');
CREATE TABLE statistics_bandwidth(
  id INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL,
  account_id integer,
  device_id integer,
  timespan integer,
  at dt_integer(8),
  lan boolean,
  bytes integer(8)
);
CREATE INDEX index_statistics_bandwidth_on_at ON statistics_bandwidth(at);
CREATE INDEX index_statistics_bandwidth_on_account_id_and_timespan_and_at ON statistics_bandwidth(account_id,timespan,at);
INSERT INTO statistics_bandwidth(id, account_id, device_id, timespan, at, lan, bytes)
VALUES
  (1, 10, 100, 86400, CAST(STRFTIME('%s','now','-120 days') AS INTEGER), 0, 1000),
  (2, 10, 100, 86400, CAST(STRFTIME('%s','now','-10 days') AS INTEGER), 0, 2000),
  (3, NULL, 100, 86400, CAST(STRFTIME('%s','now','-10 days') AS INTEGER), 0, 3000);
EOF_SQL
}

create_trim_main_db() {
  local db
  db="$1"
  "$real_sqlite" "$db" <<'EOF_SQL'
CREATE TABLE metadata_items(id INTEGER PRIMARY KEY,parent_id INTEGER,metadata_type INTEGER,originally_available_at,added_at);
CREATE TABLE media_items(id INTEGER PRIMARY KEY,metadata_item_id INTEGER);
CREATE TABLE media_parts(id INTEGER PRIMARY KEY,media_item_id INTEGER);
INSERT INTO metadata_items VALUES
  (10,NULL,3,NULL,NULL),
  (11,10,4,CAST(strftime('%s','now','-25 months') AS INTEGER),NULL);
INSERT INTO media_items VALUES(110,11);
INSERT INTO media_parts VALUES(100,110);
EOF_SQL
}

create_trim_blob_db() {
  local db
  db="$1"
  "$real_sqlite" "$db" <<'EOF_SQL'
PRAGMA user_version=4101;
PRAGMA application_id=123456789;
CREATE TABLE blobs(
  id INTEGER PRIMARY KEY,
  blob BLOB,
  linked_type TEXT,
  linked_id INTEGER,
  linked_guid TEXT,
  created_at INTEGER,
  blob_type INTEGER,
  UNIQUE(linked_type,linked_id,blob_type),
  UNIQUE(linked_type,linked_guid,blob_type)
);
INSERT INTO blobs VALUES(1,X'01','media_part',100,'g100',1,5);
EOF_SQL
}

add_plex_tag_titles_icu_fixture() {
  local db
  db="$1"
  "$real_sqlite" "$db" <<'EOF_SQL'
CREATE TABLE tags(id INTEGER PRIMARY KEY, tag TEXT);
INSERT INTO tags(id, tag) VALUES
  (1, 'Music'),
  (2, 'Movies'),
  (3, 'imdb://tt0133093'),
  (4, 'tmdb://603'),
  (5, ''),
  (6, NULL);
CREATE VIRTUAL TABLE fts4_tag_titles_icu USING fts4(tag, content='tags');
INSERT INTO fts4_tag_titles_icu(fts4_tag_titles_icu) VALUES('rebuild');
EOF_SQL
}

bad_plex_tag_doc_count() {
  local db
  db="$1"
  "$real_sqlite" "$db" "SELECT count(*) FROM fts4_tag_titles_icu_docsize d JOIN tags t ON t.id=d.docid WHERE t.tag GLOB '*://*' OR t.tag IS NULL OR trim(t.tag)='';"
}

name_plex_tag_doc_count() {
  local db
  db="$1"
  "$real_sqlite" "$db" "SELECT count(*) FROM fts4_tag_titles_icu_docsize d JOIN tags t ON t.id=d.docid WHERE t.tag IN ('Music','Movies');"
}

healthy="$tmp/healthy.db"
create_normal_fts_db "$healthy"
: > "$SQLITE_INTEGRITY_LOG"
: > "$SQLITE_MAINTENANCE_LOG"
run_rebuild_capture healthy sqlite3 "$healthy" 4096 NONE "" "PRAGMA temp_store=2; PRAGMA threads=8; REINDEX; PRAGMA analysis_limit=0; ANALYZE; PRAGMA optimize=0x10002;" ""
assert_eq 0 "$(cat "$tmp/healthy.rc")" "healthy pipeline rc"
assert_eq "1" "$("$real_sqlite" "$healthy" "SELECT COUNT(*) FROM media WHERE title='alpha';")" "healthy pipeline live row"
assert_eq "4101" "$("$real_sqlite" "$healthy" "PRAGMA user_version;")" "healthy pipeline user_version"
assert_eq "123456789" "$("$real_sqlite" "$healthy" "PRAGMA application_id;")" "healthy pipeline application_id"
healthy_backup_count="$(find "$backup_dir" -name 'healthy.db-*.original' -print | wc -l | tr -d ' ')"
assert_eq "1" "$healthy_backup_count" "healthy pipeline backup count"
[ ! -e "$healthy.new" ] || fail "healthy pipeline staged cleanup" "no staged file" "$healthy.new exists"
assert_contains "$(cat "$SQLITE_INTEGRITY_LOG")" "PRAGMA integrity_check;" "healthy full integrity_check"
assert_not_contains "$(cat "$SQLITE_INTEGRITY_LOG")" "PRAGMA integrity_check(1);" "healthy no bounded integrity_check"
assert_contains "$(cat "$SQLITE_MAINTENANCE_LOG")" "ANALYZE; PRAGMA optimize=0x10002;" "healthy staged ANALYZE before optimize"

source_corrupt="$tmp/source-corrupt.db"
create_external_fts_db "$source_corrupt"
"$real_sqlite" "$source_corrupt" "UPDATE docs SET body='source-mismatch' WHERE id=1;"
source_hash_before="$(sha256_file "$source_corrupt")"
run_rebuild_capture source-corrupt sqlite3 "$source_corrupt" 4096 NONE "" "" ""
assert_eq 0 "$(cat "$tmp/source-corrupt.rc")" "source FTS corruption pipeline rc"
source_corrupt_backup_count="$(find "$backup_dir" -name 'source-corrupt.db-*.original' -print | wc -l | tr -d ' ')"
assert_eq "1" "$source_corrupt_backup_count" "source FTS corruption backup count"
[ ! -e "$source_corrupt.new" ] || fail "source FTS corruption staged cleanup" "no staged file" "$source_corrupt.new exists"
source_hash_after="$(sha256_file "$source_corrupt")"
[ "$source_hash_after" != "$source_hash_before" ] || fail "source FTS corruption live hash" "changed hash" "$source_hash_after"

staged_corrupt="$tmp/staged-corrupt.db"
create_external_fts_db "$staged_corrupt"
staged_hash_before="$(sha256_file "$staged_corrupt")"
export CORRUPT_STAGED_AFTER_VACUUM=1
export CORRUPT_STAGED_DB="$staged_corrupt.new"
run_rebuild_capture staged-corrupt sqlite3 "$staged_corrupt" 4096 NONE "" "" ""
unset CORRUPT_STAGED_AFTER_VACUUM CORRUPT_STAGED_DB
assert_eq 0 "$(cat "$tmp/staged-corrupt.rc")" "staged FTS corruption pipeline rc"
[ ! -e "$staged_corrupt.new" ] || fail "staged FTS corruption staged cleanup" "no staged file" "$staged_corrupt.new exists"
staged_hash_after="$(sha256_file "$staged_corrupt")"
[ "$staged_hash_after" != "$staged_hash_before" ] || fail "staged FTS corruption live hash" "changed hash" "$staged_hash_after"

staged_rebuild_fail="$tmp/staged-rebuild-fail.db"
create_external_fts_db "$staged_rebuild_fail"
staged_rebuild_fail_hash_before="$(sha256_file "$staged_rebuild_fail")"
export FAIL_REBUILD=1
run_rebuild_capture staged-rebuild-fail sqlite3 "$staged_rebuild_fail" 4096 NONE "" "" ""
unset FAIL_REBUILD
assert_eq 1 "$(cat "$tmp/staged-rebuild-fail.rc")" "staged FTS rebuild failure pipeline rc"
assert_eq "$staged_rebuild_fail_hash_before" "$(sha256_file "$staged_rebuild_fail")" "staged FTS rebuild failure live hash"
[ ! -e "$staged_rebuild_fail.new" ] || fail "staged FTS rebuild failure staged cleanup" "no staged file" "$staged_rebuild_fail.new exists"
assert_contains "$(cat "$tmp/staged-rebuild-fail.err")" "staged FTS rebuild failed" "staged FTS rebuild failure diagnostic"

fk_db="$tmp/fk-warning.db"
create_fk_orphan_db "$fk_db"
run_rebuild_capture fk-warning sqlite3 "$fk_db" 4096 NONE "" "" ""
assert_eq 0 "$(cat "$tmp/fk-warning.rc")" "FK warning pipeline rc"
assert_contains "$(cat "$tmp/fk-warning.err")" "WARNING: foreign_key_check returned rows" "FK warning pipeline diagnostic"
assert_eq "1" "$("$real_sqlite" "$fk_db" "SELECT COUNT(*) FROM child;")" "FK warning pipeline swapped row"

wal_busy="$tmp/wal-busy.db"
create_normal_fts_db "$wal_busy"
export BUSY_WAL_CHECKPOINT=1
run_rebuild_capture wal-busy sqlite3 "$wal_busy" 4096 NONE "" "PRAGMA temp_store=2; PRAGMA threads=8; REINDEX; PRAGMA analysis_limit=0; ANALYZE; PRAGMA optimize=0x10002;" ""
unset BUSY_WAL_CHECKPOINT
assert_eq 0 "$(cat "$tmp/wal-busy.rc")" "busy WAL checkpoint pipeline rc"
assert_contains "$(cat "$tmp/wal-busy.err")" "WARNING: wal_checkpoint(TRUNCATE) reported busy=1" "busy WAL checkpoint warning"
assert_eq "1" "$("$real_sqlite" "$wal_busy" "SELECT COUNT(*) FROM media WHERE title='alpha';")" "busy WAL checkpoint live row"

pragma_mismatch="$tmp/pragma-mismatch.db"
create_normal_fts_db "$pragma_mismatch"
pragma_mismatch_hash_before="$(sha256_file "$pragma_mismatch")"
export ALTER_STAGED_PRAGMAS_AFTER_VACUUM=1
export PRAGMA_MISMATCH_STAGED_DB="$pragma_mismatch.new"
run_rebuild_capture pragma-mismatch sqlite3 "$pragma_mismatch" 4096 NONE "" "PRAGMA temp_store=2; PRAGMA threads=8; REINDEX; PRAGMA analysis_limit=0; ANALYZE; PRAGMA optimize=0x10002;" ""
unset ALTER_STAGED_PRAGMAS_AFTER_VACUUM PRAGMA_MISMATCH_STAGED_DB
assert_eq 1 "$(cat "$tmp/pragma-mismatch.rc")" "PRAGMA mismatch pipeline rc"
assert_eq "$pragma_mismatch_hash_before" "$(sha256_file "$pragma_mismatch")" "PRAGMA mismatch live hash"
[ ! -e "$pragma_mismatch.new" ] || fail "PRAGMA mismatch staged cleanup" "no staged file" "$pragma_mismatch.new exists"
assert_contains "$(cat "$tmp/pragma-mismatch.err")" "staged user_version mismatch" "PRAGMA mismatch diagnostic"

plex_dir="$tmp/plex-dbs"
mkdir -p "$plex_dir"
PLEX_BINARY="sqlite3"
BACKUP_PATH="$tmp/no-global-backup-root"
STATS_BANDWIDTH_RETAIN_DAYS=90

run_pipeline_plex_optimize_capture() {
  local name db_file pre_swap_hook rc
  local plex_databases_path="$plex_dir"
  local plex_instance="plex-fixture"
  name="$1"
  db_file="$2"
  pre_swap_hook="${3:-}"

  set +e
  ( optimize_plex_db "$db_file" "" "$pre_swap_hook" ) >"$tmp/${name}.out" 2>"$tmp/${name}.err"
  rc=$?
  set -e
  printf '%s' "$rc" > "$tmp/${name}.rc"
}

plex_main="$plex_dir/$_PLEX_DB"
create_stats_db "$plex_main"
: > "$SQLITE_CALL_LOG"
: > "$SQLITE_PIPELINE_ORDER_LOG"
: > "$SQLITE_SHADOW_GATE_LOG"
add_plex_tag_titles_icu_fixture "$plex_main"
run_pipeline_plex_optimize_capture plex-main "$_PLEX_DB" "try_deflate_plex_statistics_bandwidth"
rc="$(cat "$tmp/plex-main.rc")"
[ "$rc" = "0" ] || fail "Plex main optimize rc" "0" "rc=$rc stderr=$(cat "$tmp/plex-main.err")"
final_integrity_line="$(awk '$0 == "integrity" { line=NR } END { if (line) print line }' "$SQLITE_PIPELINE_ORDER_LOG")"
recurate_line="$(awk '$0 == "recurate" { print NR; exit }' "$SQLITE_PIPELINE_ORDER_LOG")"
shadow_discovery_line="$(awk '$0 == "shadow-discovery" { print NR; exit }' "$SQLITE_PIPELINE_ORDER_LOG")"
shadow_integrity_line="$(awk '$0 == "shadow-integrity" { print NR; exit }' "$SQLITE_PIPELINE_ORDER_LOG")"
assert_line_lt "$final_integrity_line" "$recurate_line" "Plex final staged integrity before FTS re-curation"
assert_line_lt "$recurate_line" "$shadow_discovery_line" "Plex FTS re-curation before shadow discovery"
assert_line_lt "$shadow_discovery_line" "$shadow_integrity_line" "Plex shadow discovery before table-scoped integrity checks"
assert_contains "$(cat "$tmp/plex-main.out")" "Deflating Plex statistics_bandwidth" "Plex main deflate log"
assert_eq "2" "$("$real_sqlite" "$plex_main" "SELECT group_concat(id, ',') FROM (SELECT id FROM statistics_bandwidth ORDER BY id);")" "Plex main deflated ids"
assert_eq "0" "$(bad_plex_tag_doc_count "$plex_main")" "Plex main recurate bad tag docs"
assert_eq "2" "$(name_plex_tag_doc_count "$plex_main")" "Plex main recurate name docs"
assert_eq "1" "$(count_log_occurrences 'INSERT INTO "fts4_tag_titles_icu"("fts4_tag_titles_icu") VALUES('\''rebuild'\'');' "$SQLITE_CALL_LOG")" "Plex main staged-only FTS rebuild count"
assert_eq "1" "$(count_log_occurrences 'INSERT INTO "fts4_tag_titles_icu"("fts4_tag_titles_icu") VALUES('\''optimize'\'');' "$SQLITE_CALL_LOG")" "Plex main post-swap FTS optimize count"
plex_shadow_table_count="$("$real_sqlite" "$plex_main" "SELECT count(*) FROM sqlite_master WHERE type='table' AND name GLOB 'fts4_tag_titles_icu_*';")"
assert_eq "1" "$(count_log_occurrences 'shadow-discovery' "$SQLITE_SHADOW_GATE_LOG")" "Plex main shadow discovery invocation count"
assert_eq "$plex_shadow_table_count" "$(count_log_occurrences 'shadow-integrity' "$SQLITE_SHADOW_GATE_LOG")" "Plex main table-scoped shadow integrity invocation count"

plex_blob="$plex_dir/$_PLEX_BLOB_DB"
create_stats_db "$plex_blob"
: > "$SQLITE_SHADOW_GATE_LOG"
run_pipeline_plex_optimize_capture plex-blob "$_PLEX_BLOB_DB" ""
rc="$(cat "$tmp/plex-blob.rc")"
[ "$rc" = "0" ] || fail "Plex blob optimize rc" "0" "rc=$rc stderr=$(cat "$tmp/plex-blob.err")"
assert_not_contains "$(cat "$tmp/plex-blob.out")" "Deflating Plex statistics_bandwidth" "Plex blob no deflate log"
assert_eq "1,2,3" "$("$real_sqlite" "$plex_blob" "SELECT group_concat(id, ',') FROM (SELECT id FROM statistics_bandwidth ORDER BY id);")" "Plex blob retained stats ids"
assert_eq "0" "$(wc -l < "$SQLITE_SHADOW_GATE_LOG" | tr -d ' ')" "Plex blob no post-recuration shadow-gate SQLite invocations"

final_hook_plex_dir="$tmp/final-hook-plex"
mkdir -p "$final_hook_plex_dir"
plex_databases_path="$final_hook_plex_dir"
create_trim_main_db "$plex_databases_path/$_PLEX_DB"
final_hook_blob="$plex_databases_path/$_PLEX_BLOB_DB"
create_trim_blob_db "$final_hook_blob"
final_hook_live_hash_before="$(sha256_file "$final_hook_blob")"
export TRIM_BAD_INTEGRITY_MARKER="$tmp/trim-bad-integrity.marker"
run_rebuild_capture final-trim-integrity sqlite3 "$final_hook_blob" 4096 NONE "" "" "try_trim_plex_finished_season_blobs" ""
unset TRIM_BAD_INTEGRITY_MARKER
assert_eq 1 "$(cat "$tmp/final-trim-integrity.rc")" "final trim integrity pipeline rc"
assert_contains "$(cat "$tmp/final-trim-integrity.err")" "final staged DB failed integrity_check" "final trim integrity diagnostic"
assert_eq "$final_hook_live_hash_before" "$(sha256_file "$final_hook_blob")" "final trim integrity live hash"
[ ! -e "$final_hook_blob.new" ] || fail "final trim integrity staged cleanup" "no staged file" "$final_hook_blob.new exists"

emby="$tmp/emby-library.db"
create_normal_fts_db "$emby"
add_mediaitems_fixture "$emby"
emby_vendor_rows_before="$(movies_rows "$emby" "$(movies_vendor_sql)")"
emby_vendor_eqp_before="$(movies_vendor_eqp "$emby")"
: > "$SQLITE_CALL_LOG"
: > "$SQLITE_INTEGRITY_LOG"
: > "$SQLITE_MAINTENANCE_LOG"
: > "$SQLITE_SHADOW_GATE_LOG"
run_rebuild_capture emby sqlite3 "$emby" 4096 NONE "SELECT 1 FROM media LIMIT 1;" "$(build_emby_optimize_sql)" ""
assert_eq 0 "$(cat "$tmp/emby.rc")" "Emby fts_search9 pipeline rc"
emby_log="$(cat "$SQLITE_CALL_LOG")"
assert_contains "$emby_log" 'INSERT INTO "fts_search9"("fts_search9") VALUES('\''rebuild'\'');' "Emby fts_search9 rebuild"
assert_contains "$emby_log" 'INSERT INTO "fts_search9"("fts_search9") VALUES('\''optimize'\'');' "Emby fts_search9 optimize"
assert_eq "1" "$(count_log_occurrences 'INSERT INTO "fts_search9"("fts_search9") VALUES('\''rebuild'\'');' "$SQLITE_CALL_LOG")" "Emby staged-only FTS rebuild count"
assert_eq "1" "$(count_log_occurrences 'INSERT INTO "fts_search9"("fts_search9") VALUES('\''optimize'\'');' "$SQLITE_CALL_LOG")" "Emby post-swap FTS optimize count"
assert_contains "$(cat "$SQLITE_INTEGRITY_LOG")" "PRAGMA integrity_check;" "Emby full integrity_check"
assert_not_contains "$(cat "$SQLITE_INTEGRITY_LOG")" "PRAGMA integrity_check(1);" "Emby no bounded integrity_check"
assert_contains "$(cat "$SQLITE_MAINTENANCE_LOG")" "ANALYZE; PRAGMA optimize=0x10002;" "Emby staged ANALYZE before optimize"
assert_eq "0" "$(wc -l < "$SQLITE_SHADOW_GATE_LOG" | tr -d ' ')" "Emby no post-recuration shadow-gate SQLite invocations"
assert_eq "1" "$("$real_sqlite" "$emby" "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name='idx_dshadow_mediaitems_parent_type';")" "Emby pipeline index count"
assert_eq "1" "$("$real_sqlite" "$emby" "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name='idx_dshadow_emby_latest_gk_dc';")" "Emby pipeline latest index count"
assert_eq "1" "$("$real_sqlite" "$emby" "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name='idx_dshadow_emby_latest_movies_dcn_puk';")" "Emby pipeline movies outer index count"
assert_eq "1" "$("$real_sqlite" "$emby" "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name='idx_dshadow_emby_latest_movies_puk_dc_cover';")" "Emby pipeline movies inner index count"
assert_eq "0" "$("$real_sqlite" "$emby" "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name='idx_MediaItems47b2';")" "Emby pipeline native movies index absent"
assert_eqp_uses_emby_index "$(mediaitems_eqp "$emby")" "Emby pipeline"
assert_eqp_uses_latest_index "$(latest_eqp "$emby")" "Emby pipeline"
assert_movies_c2_eqp "$(movies_c2_eqp "$emby")" "Emby pipeline"
assert_movies_vendor_eqp "$emby_vendor_eqp_before" "Emby pipeline before indexes"
assert_movies_vendor_eqp "$(movies_vendor_eqp "$emby")" "Emby pipeline after indexes"
assert_eq "$emby_vendor_rows_before" "$(movies_rows "$emby" "$(movies_vendor_sql)")" "Emby pipeline vendor rows stable"
assert_eq "$emby_vendor_rows_before" "$(movies_rows "$emby" "$(movies_c2_sql)")" "Emby pipeline C2/vendor full-row identity"

[ -s "$SQLITE_VACUUM_LOG" ] || fail "pipeline VACUUM coverage" "non-empty log" "empty log"
vacuum_policy_violations="$(awk '
  index($0, "VACUUM") &&
  (!index($0, "PRAGMA threads=8;") ||
   index($0, "PRAGMA temp_store=MEMORY") ||
   index($0, "PRAGMA temp_store=2")) { print }
' "$SQLITE_VACUUM_LOG")"
assert_eq "" "$vacuum_policy_violations" "all pipeline VACUUM calls retain threads and reject MEMORY temp_store"

printf 'SKIP: Docker stopped-container gate cases require a live Docker daemon and are intentionally not run by this local helper smoke test\n'
printf 'optimize_media_servers pipeline smoke tests passed\n'
