#include "rewrite_smoke_harness.h"
#include "sqlite3.h"

#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum emby_smoke_rewrite_mode {
    EMBY_SMOKE_MODE_EPISODES_LATEST = 13,
    EMBY_SMOKE_MODE_MOVIES_LATEST = 14,
    EMBY_SMOKE_MODE_MIXED_LATEST = 15
};

#ifdef EMBY_FTS_REWRITE_DIRECT_TEST
#include "emby_fts_rewrite.h"
#include "observability.h"

_Static_assert(
    EMBY_SMOKE_MODE_EPISODES_LATEST == OBS_MODE_EMBY_EPISODES_LATEST,
    "Emby Episodes Latest rewrite mode ABI drift"
);
_Static_assert(
    EMBY_SMOKE_MODE_MOVIES_LATEST == OBS_MODE_EMBY_MOVIES_LATEST,
    "Emby movies Latest rewrite mode ABI drift"
);
_Static_assert(
    EMBY_SMOKE_MODE_MIXED_LATEST == OBS_MODE_EMBY_MIXED_LATEST,
    "Emby mixed Latest rewrite mode ABI drift"
);

enum direct_fault_mode {
    DIRECT_FAULT_NONE = 0,
    DIRECT_FAULT_CANDIDATE_ERROR,
    DIRECT_FAULT_CANDIDATE_ERROR_WITH_STMT,
    DIRECT_FAULT_CANDIDATE_WRONG_TAIL,
    DIRECT_FAULT_MIXED_BIND_MISMATCH,
    DIRECT_FAULT_MIXED_COLUMN_MISMATCH,
    DIRECT_FAULT_MIXED_INDEX_MISMATCH
};

static const char *direct_original_sql;
static enum direct_fault_mode direct_fault;
static int direct_candidate_calls;
static int direct_original_calls;
static size_t direct_candidate_input_len;
static int direct_candidate_had_stmt;
static int direct_candidate_bind_count;
static int direct_candidate_column_count;
static int direct_candidate_outer_index_count;
static int direct_candidate_inner_index_count;
static int direct_skipped_calls;
static const char *direct_skipped_reason;
static obs_rewrite_mode direct_skipped_mode;

static int direct_count_bytes(const char *text, const char *needle) {
    int count = 0;
    size_t needle_len = strlen(needle);
    const char *p = text;

    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += needle_len;
    }
    return count;
}

__attribute__((visibility("hidden"))) SQLITE_API int sqlite3_prepare_v2_real(
    sqlite3 *db,
    const char *zSql,
    int nByte,
    sqlite3_stmt **ppStmt,
    const char **pzTail
) {
    return sqlite3_prepare_v2(db, zSql, nByte, ppStmt, pzTail);
}

__attribute__((visibility("hidden"))) int plex_fts_rewrite_prepare(
    sqlite3 *db,
    const char *zSql,
    int nByte,
    unsigned int prepFlags,
    sqlite3_stmt **ppStmt,
    const char **pzTail,
    fts_rewrite_prepare_kind kind
) {
    int candidate = direct_original_sql && zSql != direct_original_sql;
    const char *prepare_sql = zSql;
    int prepare_nbyte = nByte;
    int rc;
    if (candidate) {
        direct_candidate_calls++;
        direct_candidate_input_len = strlen(zSql);
        direct_candidate_outer_index_count = direct_count_bytes(
            zSql, "idx_dshadow_emby_latest_mixed_dcn_gk"
        );
        direct_candidate_inner_index_count = direct_count_bytes(
            zSql, "idx_dshadow_emby_latest_mixed_gk_dc"
        );
        if (direct_fault == DIRECT_FAULT_CANDIDATE_ERROR) {
            *ppStmt = NULL;
            if (pzTail) *pzTail = zSql;
            return SQLITE_ERROR;
        }
        if (direct_fault == DIRECT_FAULT_MIXED_BIND_MISMATCH) {
            prepare_sql = "SELECT ?";
            prepare_nbyte = -1;
        } else if (direct_fault == DIRECT_FAULT_MIXED_COLUMN_MISMATCH) {
            prepare_sql = "SELECT 1";
            prepare_nbyte = -1;
        }
    } else if (direct_original_sql && zSql == direct_original_sql) {
        direct_original_calls++;
    }
    if (kind == FTS_REWRITE_PREPARE_V3) {
        rc = sqlite3_prepare_v3(db, prepare_sql, prepare_nbyte, prepFlags, ppStmt, pzTail);
    } else if (kind == FTS_REWRITE_PREPARE_V2) {
        rc = sqlite3_prepare_v2(db, prepare_sql, prepare_nbyte, ppStmt, pzTail);
    } else {
        (void)prepFlags;
        rc = sqlite3_prepare(db, prepare_sql, prepare_nbyte, ppStmt, pzTail);
    }
    if (candidate && prepare_sql != zSql && rc == SQLITE_OK && pzTail) {
        *pzTail = zSql + strlen(zSql);
    }
    if (candidate && direct_fault == DIRECT_FAULT_MIXED_INDEX_MISMATCH &&
        rc == SQLITE_OK) {
        char *index_name = strstr(
            (char *)zSql, "idx_dshadow_emby_latest_mixed_dcn_gk"
        );
        if (index_name) index_name[0] = 'X';
        direct_candidate_outer_index_count = direct_count_bytes(
            zSql, "idx_dshadow_emby_latest_mixed_dcn_gk"
        );
        direct_candidate_inner_index_count = direct_count_bytes(
            zSql, "idx_dshadow_emby_latest_mixed_gk_dc"
        );
    }
    if (candidate) {
        direct_candidate_had_stmt = ppStmt && *ppStmt != NULL;
        if (direct_candidate_had_stmt) {
            direct_candidate_bind_count = sqlite3_bind_parameter_count(*ppStmt);
            direct_candidate_column_count = sqlite3_column_count(*ppStmt);
        }
    }
    if (candidate && direct_fault == DIRECT_FAULT_CANDIDATE_ERROR_WITH_STMT &&
        rc == SQLITE_OK) {
        return SQLITE_ERROR;
    }
    if (candidate && direct_fault == DIRECT_FAULT_CANDIDATE_WRONG_TAIL &&
        rc == SQLITE_OK && pzTail) {
        *pzTail = zSql;
    }
    return rc;
}

__attribute__((visibility("hidden"))) SQLITE_API void obs_logf(const char *fn, const char *fmt, ...) {
    (void)fn;
    (void)fmt;
}

__attribute__((visibility("hidden"))) SQLITE_API int obs_is_disabled(void) {
    return 0;
}

__attribute__((visibility("hidden"))) SQLITE_API void obs_escape_sql(
    const char *src,
    char *dst,
    size_t dst_n
) {
    size_t i;
    if (dst_n == 0) return;
    for (i = 0; i + 1 < dst_n && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

__attribute__((visibility("hidden"))) SQLITE_API void obs_log_rewrite_miss(
    obs_rewrite_mode mode,
    obs_miss_reason reason,
    const char *sub_reason,
    sqlite3 *db,
    const char *sql,
    size_t len,
    uint64_t shape
) {
    (void)mode;
    (void)reason;
    (void)sub_reason;
    (void)db;
    (void)sql;
    (void)len;
    (void)shape;
}

__attribute__((visibility("hidden"))) SQLITE_API void obs_log_index_missing(
    sqlite3 *db,
    obs_rewrite_mode mode
) {
    (void)db;
    (void)mode;
}

__attribute__((visibility("hidden"))) SQLITE_API void obs_log_rewrite_applied(
    obs_rewrite_mode mode,
    sqlite3 *db,
    const char *source,
    size_t source_len,
    const char *rewritten,
    size_t rewritten_len
) {
    (void)mode;
    (void)db;
    (void)source;
    (void)source_len;
    (void)rewritten;
    (void)rewritten_len;
}

__attribute__((visibility("hidden"))) SQLITE_API void obs_log_rewrite_skipped(
    sqlite3 *db,
    const char *reason,
    obs_rewrite_mode mode
) {
    (void)db;
    direct_skipped_calls++;
    direct_skipped_reason = reason;
    direct_skipped_mode = mode;
}
#endif

#ifdef EMBY_FTS_REWRITE_TEST_API
int emby_fts_rewrite_test_scalar_calls(void);
void emby_fts_rewrite_test_reset_scalar_calls(void);
int emby_fts_rewrite_test_duplicate_anchor_guard(void);
int emby_fts_rewrite_test_string_anchor_immunity(void);
#endif

#define RW_FLAGS (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)

typedef struct emby_auth_probe {
    int scalar_calls;
    int deny_scalar;
} emby_auth_probe;

typedef struct sqlite_master_auth_probe {
    int reads;
} sqlite_master_auth_probe;

static void seed_movies_latest_rows(sqlite3 *db);
static void seed_movies_latest_expanded_rows(sqlite3 *db);

static int scalar_authorizer_cb(
    void *ctx,
    int action,
    const char *p1,
    const char *p2,
    const char *db,
    const char *trigger
);

static const rsh_suite_spec emby_suite_spec;

static void failf(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
    exit(1);
}

#include "contract_parity.h"

static int rsh_public_prepare(
    sqlite3 *db,
    const char *sql,
    int nbyte,
    rsh_prepare_kind kind,
    sqlite3_stmt **stmt_out,
    const char **tail_out
) {
    if (kind == RSH_PREPARE_V3) {
        return sqlite3_prepare_v3(
            db, sql, nbyte, SQLITE_PREPARE_PERSISTENT, stmt_out, tail_out
        );
    }
    if (kind == RSH_PREPARE_V2) {
        return sqlite3_prepare_v2(db, sql, nbyte, stmt_out, tail_out);
    }
    return sqlite3_prepare(db, sql, nbyte, stmt_out, tail_out);
}

#ifdef EMBY_FTS_REWRITE_DIRECT_TEST
static int rsh_direct_prepare(
    sqlite3 *db,
    const char *sql,
    int nbyte,
    rsh_prepare_kind kind,
    sqlite3_stmt **stmt_out,
    const char **tail_out
) {
    fts_rewrite_prepare_kind engine_kind = FTS_REWRITE_PREPARE_LEGACY;
    unsigned int flags = 0;

    if (kind == RSH_PREPARE_V3) {
        engine_kind = FTS_REWRITE_PREPARE_V3;
        flags = SQLITE_PREPARE_PERSISTENT;
    } else if (kind == RSH_PREPARE_V2) {
        engine_kind = FTS_REWRITE_PREPARE_V2;
    }
    return emby_fts_rewrite_prepare(
        db, sql, nbyte, flags, stmt_out, tail_out, engine_kind
    );
}
#endif

static char *xasprintf(const char *fmt, ...) {
    va_list ap;
    va_list ap2;
    int n;
    char *buf;

    va_start(ap, fmt);
    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) failf("FATAL: vsnprintf sizing failed");
    buf = (char *)malloc((size_t)n + 1);
    if (!buf) failf("FATAL: malloc failed");
    if (vsnprintf(buf, (size_t)n + 1, fmt, ap2) != n) failf("FATAL: vsnprintf write failed");
    va_end(ap2);
    return buf;
}

static void require_int(const char *label, int got, int want) {
    if (got != want) failf("FAIL [%s]: got=%d want=%d", label, got, want);
}

static void require_str_eq(const char *label, const char *got, const char *want) {
    if (!got || strcmp(got, want) != 0) {
        failf("FAIL [%s]: got=\"%s\" want=\"%s\"", label, got ? got : "(null)", want);
    }
}

static void require_contains(const char *label, const char *got, const char *needle) {
    if (!got || !strstr(got, needle)) {
        failf("FAIL [%s]: got=\"%s\" missing=\"%s\"", label, got ? got : "(null)", needle);
    }
}

static void require_absent(const char *label, const char *got, const char *needle) {
    if (got && strstr(got, needle)) {
        failf("FAIL [%s]: got=\"%s\" unexpected=\"%s\"", label, got, needle);
    }
}

static void require_same_line(
    const char *label,
    const char *text,
    const char *first,
    const char *second
) {
    const char *line = text;

    if (!text) failf("FAIL [%s]: text=(null)", label);
    while (*line) {
        const char *end = strchr(line, '\n');
        const char *first_match = strstr(line, first);
        const char *second_match = strstr(line, second);
        if (first_match && (!end || first_match < end) &&
            second_match && (!end || second_match < end)) {
            return;
        }
        if (!end) break;
        line = end + 1;
    }
    failf(
        "FAIL [%s]: no line contains \"%s\" and \"%s\" in \"%s\"",
        label, first, second, text
    );
}

static int count_occurrences(const char *haystack, const char *needle) {
    int count = 0;
    const char *p = haystack;

    if (!haystack || !needle || !*needle) return 0;
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += strlen(needle);
    }
    return count;
}

static uint64_t sql_corr_key(const char *sql, size_t len) {
    uint64_t hash = UINT64_C(14695981039346656037);
    size_t i;

    for (i = 0; i < len; i++) {
        hash ^= (unsigned char)sql[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void exec_sql(sqlite3 *db, const char *label, const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        failf("FAIL [%s]: sqlite3_exec rc=%d err=%s", label, rc, err ? err : sqlite3_errmsg(db));
    }
    sqlite3_free(err);
}

static sqlite3 *open_db_path(const char *path) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(path, &db, RW_FLAGS, NULL);
    if (rc != SQLITE_OK) {
        failf("FAIL [open]: path=%s rc=%d err=%s", path, rc, db ? sqlite3_errmsg(db) : "(null)");
    }
    return db;
}

static int naturalsort_cmp(void *ctx, int left_n, const void *left, int right_n, const void *right) {
    int min_n = left_n < right_n ? left_n : right_n;
    int cmp;
    (void)ctx;
    cmp = memcmp(left, right, (size_t)min_n);
    if (cmp != 0) return cmp;
    return (left_n > right_n) - (left_n < right_n);
}

static int rsh_base_seed(sqlite3 *db, void *suite_ctx) {
    (void)suite_ctx;
    require_int("schema/collation",
                sqlite3_create_collation(db, "NATURALSORT", SQLITE_UTF8, NULL, naturalsort_cmp),
                SQLITE_OK);
    exec_sql(db, "schema",
        "CREATE TABLE MediaItems("
        "Id INTEGER PRIMARY KEY, Type INTEGER, EndDate TEXT, IndexNumber INTEGER, Name TEXT,"
        "SortName TEXT, DateCreated INTEGER, EpisodeAbsoluteIndexNumber INTEGER,"
        "Path TEXT, ParentIndexNumber INTEGER, ProductionYear INTEGER, RunTimeTicks INTEGER,"
        "ParentId INTEGER, SeriesName TEXT, AlbumId INTEGER, SeriesId INTEGER,"
        "SeriesPresentationUniqueKey TEXT, PresentationUniqueKey TEXT, Images TEXT, Status TEXT, SortIndexNumber INTEGER,"
        "SortParentIndexNumber INTEGER, IndexNumberEnd INTEGER, UserDataKeyId INTEGER,"
        "IsPublic INTEGER, ExtraType INTEGER, CommunityRating REAL, PremiereDate INTEGER,"
        "OfficialRating TEXT, guid TEXT, CriticRating REAL);"
        "CREATE TABLE AncestorIds2(itemid INTEGER, AncestorId INTEGER, Distance INTEGER);"
        "CREATE TABLE ItemLinks2(ItemId INTEGER, LinkedId INTEGER, Type INTEGER);"
        "CREATE TABLE itemPeople2(ItemId INTEGER, PersonId INTEGER);"
        "CREATE TABLE ListItems(ListId INTEGER, ListItemId INTEGER);"
        "CREATE TABLE UserItemShares(UserId INTEGER, ItemId INTEGER, ShareLevel INTEGER);"
        "CREATE TABLE UserDatas(UserDataKeyId INTEGER, UserId INTEGER, IsFavorite INTEGER,"
        "Played INTEGER, PlaybackPositionTicks INTEGER, AudioStreamIndex INTEGER,"
        "SubtitleStreamIndex INTEGER, HideFromResume INTEGER, LastPlayedDateInt INTEGER,"
        "PRIMARY KEY (UserDataKeyId, UserId)) WITHOUT ROWID;"
        "CREATE VIRTUAL TABLE fts_search9 USING fts5(title);"
        "INSERT INTO mediaitems("
        "Id, Type, Name, SortName, DateCreated, EpisodeAbsoluteIndexNumber,"
        "SeriesName, SeriesPresentationUniqueKey, PresentationUniqueKey,"
        "UserDataKeyId, IsPublic, ExtraType"
        ") VALUES"
        "(1,1,'direct','direct',100,1,NULL,NULL,'p1',1,1,NULL),"
        "(2,2,'list','list',101,2,NULL,NULL,'p2',2,1,NULL),"
        "(3,5,'person','person',102,3,NULL,NULL,'p3',3,1,NULL),"
        "(4,6,'link1','link1',103,4,NULL,NULL,'p4',4,1,NULL),"
        "(5,8,'link2','link2',104,5,'series z','latest-g1','p5',5,1,NULL),"
        "(6,8,'latest old','latest old',200,6,'series a','latest-g1','p6',6,1,NULL),"
        "(7,8,'latest new','latest new',300,7,'series a','latest-g1','p7',7,1,NULL),"
        "(8,8,'latest other','latest other',250,8,'series b',NULL,'latest-g2',8,1,NULL),"
        "(9,9,'browse','browse',120,9,NULL,NULL,'p9',9,1,NULL),"
        "(10,8,'latest null','latest null',350,10,'series c',NULL,NULL,10,1,NULL);"
        "INSERT INTO fts_search9(rowid,title) VALUES"
        "(1,'alpha direct'),(2,'alpha list'),(3,'alpha person'),"
        "(4,'alpha link'),(5,'alpha two');"
        "INSERT INTO AncestorIds2 VALUES(1,100,0),(6,100,0),(7,100,0),(8,100,0),(9,100,0),(10,100,0),(200,100,0),(300,100,0),(400,100,0),(502,100,0);"
        "INSERT INTO ListItems VALUES(2,200);"
        "INSERT INTO itemPeople2 VALUES(300,3);"
        "INSERT INTO ItemLinks2 VALUES(400,4,6),(501,5,0),(502,501,7);"
        "INSERT INTO UserItemShares VALUES(1,1,1),(1,2,1),(1,3,1),(1,4,1),(1,5,1);"
        "INSERT INTO UserDatas("
        "UserDataKeyId, UserId, IsFavorite, Played, PlaybackPositionTicks,"
        "AudioStreamIndex, SubtitleStreamIndex, HideFromResume, LastPlayedDateInt"
        ") VALUES"
        "(1,1,0,0,0,0,0,0,10),(2,1,0,0,0,0,0,0,20),(3,1,0,0,0,0,0,0,30),"
        "(4,1,0,0,0,0,0,0,40),(5,1,0,0,0,0,0,0,50),"
        "(6,42,0,0,0,0,0,0,60),(7,42,0,0,0,0,0,0,70),(8,42,0,0,0,0,0,0,80),"
        "(9,1,1,0,0,0,0,0,90),(10,42,0,0,0,0,0,0,100);"
    );
    return SQLITE_OK;
}

static int rsh_open(const char *path, sqlite3 **db_out, void *suite_ctx) {
    (void)suite_ctx;
    *db_out = open_db_path(path);
    return SQLITE_OK;
}

static rsh_apply_profile_fn rsh_resolve_setup_profile(
    int profile,
    void *suite_ctx
) {
    (void)profile;
    (void)suite_ctx;
    return NULL;
}

static void seed_mixed_latest_identity_rows(sqlite3 *db) {
    exec_sql(db, "mixed-identity-seed",
        "INSERT INTO MediaItems(Id,Type,Name,DateCreated,SeriesPresentationUniqueKey,PresentationUniqueKey,UserDataKeyId) VALUES"
        "(2000,8,'cross-old',800,'g-cross','p-cross-8',2000),"
        "(2001,5,'cross-new',900,'g-cross','p-cross-5',2001),"
        "(2002,8,'null-gk-new',850,NULL,NULL,2002),"
        "(2003,5,'null-gk-null-date',NULL,NULL,NULL,2003),"
        "(2004,8,'null-played',700,'g-null-played','p-null-played',2004),"
        "(2005,5,'absent-userdata',600,'g-absent','p-absent',2005),"
        "(2010,8,'all-null-low-id',NULL,'g-all-null','p-all-null-1',2010),"
        "(2011,5,'all-null-high-id',NULL,'g-all-null','p-all-null-2',2011),"
        "(2020,8,'played-newer',950,'g-played','p-played-new',2020),"
        "(2021,5,'unplayed-older',500,'g-played','p-played-old',2021),"
        "(2030,5,'invisible-newer',990,'g-hidden','p-hidden-new',2030),"
        "(2031,8,'visible-older',400,'g-hidden','p-hidden-old',2031),"
        "(2040,8,'same-date-high-id',300,'g-same-id','p-same-1',2040),"
        "(2039,5,'same-date-low-id',300,'g-same-id','p-same-2',2039),"
        "(2050,8,'boundary-d',250,'g-d','p-d',2050),"
        "(2051,5,'boundary-a',250,'g-a','p-a',2051),"
        "(2052,8,'boundary-c',250,'g-c','p-c',2052),"
        "(2053,5,'boundary-b',250,'g-b','p-b',2053);"
        "UPDATE MediaItems SET ProductionYear=2026 WHERE Id=2001;"
        "INSERT INTO AncestorIds2(itemid,AncestorId,Distance) VALUES"
        "(2000,100,0),(2001,100,0),(2002,100,0),(2003,100,0),(2004,100,0),(2005,100,0),"
        "(2010,101,0),(2011,101,0),"
        "(2020,102,0),(2021,102,0),"
        "(2031,103,0),"
        "(2040,105,0),(2039,105,0),"
        "(2050,104,0),(2051,104,0),(2052,104,0),(2053,104,0);"
        "INSERT INTO UserDatas(UserDataKeyId,UserId,Played) VALUES"
        "(2000,42,0),(2001,42,0),(2002,42,0),(2003,42,0),"
        "(2004,42,NULL),(2010,42,0),(2011,42,0),"
        "(2020,42,1),(2021,42,0),(2030,42,0),(2031,42,0),"
        "(2040,42,0),(2039,42,0),(2050,42,0),(2051,42,0),(2052,42,0),(2053,42,0);"
    );
}

static char *replace_once(const char *sql, const char *old, const char *new_text) {
    const char *p = strstr(sql, old);
    size_t prefix;
    size_t out_len;
    char *out;

    if (!p) failf("FATAL: replace_once missing old=\"%s\"", old);
    if (strstr(p + strlen(old), old)) failf("FATAL: replace_once ambiguous old=\"%s\"", old);
    prefix = (size_t)(p - sql);
    out_len = strlen(sql) - strlen(old) + strlen(new_text);
    out = (char *)malloc(out_len + 1);
    if (!out) failf("FATAL: malloc failed");
    memcpy(out, sql, prefix);
    memcpy(out + prefix, new_text, strlen(new_text));
    strcpy(out + prefix + strlen(new_text), p + strlen(old));
    return out;
}

static char *make_ancestor_exists_splice(const char *l1) {
    return xasprintf(
        "EXISTS (SELECT 1 FROM AncestorIds2 WHERE AncestorIds2.itemid = A.Id AND AncestorIds2.AncestorId in (%s))",
        l1
    );
}

static char *make_complex_resume_watched_progress_conjunct(const char *user_id) {
    return xasprintf(
        " AND ((A.Type=5 AND A.UserDataKeyId IN (SELECT UserDataKeyId FROM UserDatas WHERE UserId=%s AND playbackPositionTicks>0)) OR (A.Type=8 AND A.SeriesPresentationUniqueKey IN (SELECT N2.SeriesPresentationUniqueKey FROM MediaItems N2 JOIN UserDatas UN2 ON N2.UserDataKeyId=UN2.UserDataKeyId AND UN2.UserId=%s WHERE N2.Type=8 AND Coalesce(N2.SortParentIndexNumber,N2.ParentIndexNumber,-1) <> 0 AND (UN2.Played=1 OR UN2.playbackPositionTicks>0))))",
        user_id, user_id
    );
}

static char *make_exists_people(const char *l1) {
    return xasprintf(
        "EXISTS (SELECT 1 FROM itemPeople2 JOIN AncestorIds2 ON AncestorIds2.itemid = itemPeople2.ItemId WHERE itemPeople2.PersonId = A.Id and AncestorIds2.AncestorId in (%s))",
        l1
    );
}

static char *make_exists_links_one(const char *l1, const char *t1) {
    return xasprintf(
        "Exists (select 1 From ItemLinks2 join ancestorids2 on ancestorids2.itemid=itemlinks2.itemid and ancestorids2.ancestorid in (%s)  where ItemLinks2.LinkedId = A.Id AND ItemLinks2.Type in (%s))",
        l1, t1
    );
}

static char *make_exists_links_two(const char *l1, const char *t2) {
    return xasprintf(
        "Exists (select 1 from ItemLinks2 ItemLinks2TwoLevel where exists (select 1 From ItemLinks2 join ancestorids2 on ancestorids2.itemid=itemlinks2.itemid and ancestorids2.ancestorid in (%s)  where itemlinks2.linkedid = itemlinks2twolevel.itemid AND ItemLinks2.Type in (%s)) and ItemLinks2TwoLevel.LinkedId=A.Id)",
        l1, t2
    );
}

static char *make_resume_sql(void) {
    return xasprintf(
        "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) )select count(*) OVER() AS TotalRecordCount,A.type,A.Id,A.SeriesPresentationUniqueKey,UserDatas.PlaybackPositionTicks,"
        "((Coalesce(A.SortParentIndexNumber,A.ParentIndexNumber, 1) * 1000000) + Coalesce(A.SortIndexNumber, A.IndexNumber, 0) + (Select Case When Coalesce(A.ParentIndexNumber,1)=0 Then 0 Else 0.5 End) + (Select Case When Coalesce(A.ParentIndexNumber,1)=0 Then (Cast(Coalesce(A.IndexNumber, 0) as REAL) / 100000) Else 0 End)) EpisodeAbsoluteIndexNumber "
        "from mediaitems A left join (Select N.SeriesPresentationUniqueKey,((Coalesce(N.SortParentIndexNumber,N.ParentIndexNumber, 1) * 1000000) + Coalesce(N.SortIndexNumber, N.IndexNumber, 0) + (Select Case When Coalesce(N.ParentIndexNumber,1)=0 Then 0 Else 0.5 End) + (Select Case When Coalesce(N.ParentIndexNumber,1)=0 Then (Cast(Coalesce(N.IndexNumber, 0) as REAL) / 100000) Else 0 End)) AbsoluteIndexNumber,max(UserDatas_N.LastPlayedDateInt) LastPlayedDateInt,UserDatas_N.playbackPositionTicks from MediaItems N join UserDatas UserDatas_N on N.UserDataKeyId=UserDatas_N.UserDataKeyId And UserDatas_N.UserId=1 where N.Type=8 and Coalesce(N.SortParentIndexNumber,N.ParentIndexNumber,-1) <> 0 and (UserDatas_N.Played=1 or UserDatas_N.playbackPositionTicks > 0) Group By N.SeriesPresentationUniqueKey ORDER BY UserDatas_N.LastPlayedDateInt desc, AbsoluteIndexNumber desc) LastWatchedEpisodes on LastWatchedEpisodes.SeriesPresentationUniqueKey=A.SeriesPresentationUniqueKey "
        "left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=1 where ((A.Type=5 and UserDatas.playbackPositionTicks > 0) OR (A.Type=8 AND (UserDatas.playbackPositionTicks > 0 or Coalesce(UserDatas.played,0) = 0) AND (select case when LastWatchedEpisodes.playbackPositionTicks > 0 then EpisodeAbsoluteIndexNumber >= Coalesce(LastWatchedEpisodes.AbsoluteIndexNumber,EpisodeAbsoluteIndexNumber) else EpisodeAbsoluteIndexNumber > Coalesce(LastWatchedEpisodes.AbsoluteIndexNumber,EpisodeAbsoluteIndexNumber) end) AND LastWatchedEpisodes.LastPlayedDateInt not null)) "
        "AND (A.Type=5 OR Coalesce(A.SortParentIndexNumber,A.ParentIndexNumber, -1) <> 0) AND A.Type in (5,8) AND Coalesce(UserDatas.HideFromResume,0)=0 AND COALESCE((select hidefromresume from userdatas where userdatas.userid=1 and userdatas.userdatakeyid=(select userdatakeyid from mediaitems where mediaitems.id=A.seriesid)),0)=0 AND A.Id in WithAncestors Group by coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey) ORDER BY COALESCE(lastwatchedepisodes.lastplayeddateint, userdatas.lastplayeddateint, 0) DESC,Min(EpisodeAbsoluteIndexNumber) ASC LIMIT 12"
    );
}

static char *make_resume_0065_sql(void) {
    return xasprintf(
        "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) )select A.Id,A.IndexNumber,A.Name,A.ParentIndexNumber,A.RunTimeTicks,A.DateCreated,A.ParentId,A.SeriesName,A.SeriesId,A.Images,A.SortIndexNumber,A.SortParentIndexNumber,A.IndexNumberEnd,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,"
        "((Coalesce(A.SortParentIndexNumber,A.ParentIndexNumber, 1) * 1000000) + Coalesce(A.SortIndexNumber, A.IndexNumber, 0) + (Select Case When Coalesce(A.ParentIndexNumber,1)=0 Then 0 Else 0.5 End) + (Select Case When Coalesce(A.ParentIndexNumber,1)=0 Then (Cast(Coalesce(A.IndexNumber, 0) as REAL) / 100000) Else 0 End)) EpisodeAbsoluteIndexNumber "
        "from mediaitems A left join (Select N.SeriesPresentationUniqueKey,((Coalesce(N.SortParentIndexNumber,N.ParentIndexNumber, 1) * 1000000) + Coalesce(N.SortIndexNumber, N.IndexNumber, 0) + (Select Case When Coalesce(N.ParentIndexNumber,1)=0 Then 0 Else 0.5 End) + (Select Case When Coalesce(N.ParentIndexNumber,1)=0 Then (Cast(Coalesce(N.IndexNumber, 0) as REAL) / 100000) Else 0 End)) AbsoluteIndexNumber,max(UserDatas_N.LastPlayedDateInt) LastPlayedDateInt,UserDatas_N.playbackPositionTicks from MediaItems N join UserDatas UserDatas_N on N.UserDataKeyId=UserDatas_N.UserDataKeyId And UserDatas_N.UserId=13 where N.Type=8 and Coalesce(N.SortParentIndexNumber,N.ParentIndexNumber,-1) <> 0 and (UserDatas_N.Played=1 or UserDatas_N.playbackPositionTicks > 0) Group By N.SeriesPresentationUniqueKey ORDER BY UserDatas_N.LastPlayedDateInt desc, AbsoluteIndexNumber desc) LastWatchedEpisodes on LastWatchedEpisodes.SeriesPresentationUniqueKey=A.SeriesPresentationUniqueKey "
        "left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=13 where ((UserDatas.playbackPositionTicks > 0 or Coalesce(UserDatas.played,0) = 0) AND (select case when LastWatchedEpisodes.playbackPositionTicks > 0 then EpisodeAbsoluteIndexNumber >= Coalesce(LastWatchedEpisodes.AbsoluteIndexNumber,EpisodeAbsoluteIndexNumber) else EpisodeAbsoluteIndexNumber > Coalesce(LastWatchedEpisodes.AbsoluteIndexNumber,EpisodeAbsoluteIndexNumber) end) AND LastWatchedEpisodes.LastPlayedDateInt not null) "
        "AND Coalesce(A.SortParentIndexNumber,A.ParentIndexNumber, -1) <> 0 AND A.Type=8 AND Coalesce(UserDatas.played, 0)=0 AND Coalesce(UserDatas.HideFromResume,0)=0 AND COALESCE((select hidefromresume from userdatas where userdatas.userid=13 and userdatas.userdatakeyid=(select userdatakeyid from mediaitems where mediaitems.id=A.seriesid)),0)=0 AND A.Id in WithAncestors Group by coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey) ORDER BY COALESCE(lastwatchedepisodes.lastplayeddateint, userdatas.lastplayeddateint, 0) DESC,Min(EpisodeAbsoluteIndexNumber) ASC LIMIT 30"
    );
}

static char *make_resume_expected(const char *sql, const char *l1, const char *user_id) {
    char *ancestor_exists_splice = make_ancestor_exists_splice(l1);
    char *with_ancestor_exists_splice =
        replace_once(sql, "A.Id in WithAncestors", ancestor_exists_splice);
    char *watched_progress_conjunct =
        make_complex_resume_watched_progress_conjunct(user_id);
    char *group_with_conjunct =
        xasprintf("%s Group by coalesce(", watched_progress_conjunct);
    char *expected = replace_once(
        with_ancestor_exists_splice, " Group by coalesce(", group_with_conjunct
    );

    free(ancestor_exists_splice);
    free(with_ancestor_exists_splice);
    free(watched_progress_conjunct);
    free(group_with_conjunct);
    return expected;
}

static char *make_resume_simple_sql(int count_projection, const char *limit) {
    return xasprintf(
        "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) )%s%s "
        "from mediaitems A join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=1 "
        "where A.Type in (5,8) AND UserDatas.playbackPositionTicks > 0 AND Coalesce(UserDatas.HideFromResume,0)=0 AND COALESCE((select hidefromresume from userdatas where userdatas.userid=1 and userdatas.userdatakeyid=(select userdatakeyid from mediaitems where mediaitems.id=A.seriesid)),0)=0 AND A.Id in WithAncestors Group by A.PresentationUniqueKey ORDER BY UserDatas.LastPlayedDateInt DESC LIMIT %s",
        count_projection ? "select count(*) OVER() AS TotalRecordCount," : "select ",
        count_projection ? "A.type,A.Id,A.SeriesPresentationUniqueKey,UserDatas.PlaybackPositionTicks" : "A.Id,A.SeriesPresentationUniqueKey,UserDatas.PlaybackPositionTicks",
        limit
    );
}

static char *make_resume_simple_expected(const char *sql) {
    char *ancestor = make_ancestor_exists_splice("100");
    char *expected = replace_once(sql, "A.Id in WithAncestors", ancestor);

    free(ancestor);
    return expected;
}

static const char *EMBY_RESUME_SIMPLE_LEFT_JOIN_UNPLAYED_SHAPE_07_SQL =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (9,10,11,12,13,14,101,102,103) )select A.Id,A.Name,A.Path,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.Images,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex from mediaitems A left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=1 where A.Type=5 AND Coalesce(UserDatas.played, 0)=0 AND A.Id in WithAncestors Group by A.PresentationUniqueKey ORDER BY A.DateCreated DESC LIMIT 12;";

static char *make_similar_sql(void) {
    return xasprintf(
        "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) ),"
        "SimB_Ids AS (SELECT DISTINCT ItemLinks2SimB.LinkedId FROM ItemLinks2 ItemLinks2SimB WHERE ItemLinks2SimB.Type in (2,7) AND ItemLinks2SimB.ItemId=1),"
        "LinkedCounts AS (SELECT A.Id AS AId, COUNT(ItemLinks2SimA.LinkedId) AS LinkedCount FROM MediaItems A JOIN ItemLinks2 ItemLinks2SimA ON ItemLinks2SimA.ItemId = A.Id JOIN SimB_Ids ON SimB_Ids.LinkedId = ItemLinks2SimA.LinkedId WHERE ItemLinks2SimA.Type in (2,7) GROUP BY A.Id)"
        "select A.type,A.Id,A.PresentationUniqueKey,(LinkedCounts_Joined.LinkedCount * 15) as SimilarityScore from mediaitems A join LinkedCounts LinkedCounts_Joined ON A.Id = LinkedCounts_Joined.AId left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=1 where SimilarityScore > 19 AND A.Type in (5,6) AND A.Id<>1 AND A.Id in WithAncestors Group by A.PresentationUniqueKey ORDER BY SimilarityScore DESC,RANDOM() ASC LIMIT 16"
    );
}

static char *make_similar_expected(const char *sql) {
    char *ancestor = make_ancestor_exists_splice("100");
    char *expected = replace_once(sql, "A.Id in WithAncestors", ancestor);

    free(ancestor);
    return expected;
}

static const char *EMBY_LINK_TYPE_COUNT_SHAPE_05_SQL =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (15,16,17,18,19,20,107) ),WithItemLinkItemIds AS (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (6,4,3,2) union select ItemLinks2TwoLevel.LinkedId from ItemLinks2 ItemLinks2TwoLevel where itemid in (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (7,0,1,5,6,2)))select A.Type from mediaitems A where A.Type in (34,9,29,21) AND A.Id in WithItemLinkItemIds Group by A.Type";

static char *make_link_type_count_shape_05_expected(void) {
    char *one = make_exists_links_one("15,16,17,18,19,20,107", "6,4,3,2");
    char *two = make_exists_links_two("15,16,17,18,19,20,107", "7,0,1,5,6,2");
    char *replacement = xasprintf("(%s OR %s)", one, two);
    char *expected = replace_once(
        EMBY_LINK_TYPE_COUNT_SHAPE_05_SQL,
        "A.Id in WithItemLinkItemIds",
        replacement
    );

    free(one);
    free(two);
    free(replacement);
    return expected;
}

static void seed_link_type_count_shape_05_rows(sqlite3 *db) {
    exec_sql(db, "links-two-level-seed",
        "INSERT INTO MediaItems(Id,Type,Name) VALUES"
        "(1001,34,'l1-only'),(1002,9,'l2-only'),"
        "(1003,29,'both-and-duplicates'),(1004,21,'no-hit');"
        "INSERT INTO AncestorIds2 VALUES"
        "(2001,15,0),(2002,15,0),(2003,15,0),(2004,15,0);"
        "INSERT INTO ItemLinks2 VALUES"
        "(2001,1001,6),"
        "(2002,2102,7),(2102,1002,99),"
        "(2003,1003,6),(2003,1003,6),"
        "(2003,2103,7),(2103,1003,99),(2103,1003,99),"
        "(2004,NULL,6);"
    );
}

static char *make_favorites_sql(void) {
    return xasprintf(
        "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) ),"
        "WithItemLinkItemIds AS (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (6))"
        "select A.Id,A.PresentationUniqueKey from mediaitems A left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=1 where (A.Id in WithAncestors OR A.Id in WithItemLinkItemIds) Group by A.PresentationUniqueKey ORDER BY Coalesce(UserDatas.IsFavorite, 0) DESC,RANDOM() ASC LIMIT 12"
    );
}

static char *make_browse_sql(void) {
    return xasprintf(
        "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) ),"
        "WithItemLinkItemIds AS (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type=6 union select ItemLinks2TwoLevel.LinkedId from ItemLinks2 ItemLinks2TwoLevel where itemid in (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (7)))"
        "select A.Id,A.SortName from mediaitems A where A.Id in WithItemLinkItemIds ORDER BY A.SortName collate NATURALSORT ASC LIMIT 12"
    );
}

static char *make_people_sql(void) {
    return xasprintf(
        "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) )select A.Id,A.Name from mediaitems A join fts_search9 on A.Id=fts_search9.RowId and fts_search9 match @SearchTerm where A.Id in (select PersonId from itemPeople2 where ItemId in WithAncestors) ORDER BY Rank ASC LIMIT 12"
    );
}

static char *make_links_search_sql(const char *types) {
    return xasprintf(
        "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) ),"
        "WithItemLinkItemIds AS (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (%s))"
        "select A.Id,A.Name from mediaitems A join fts_search9 on A.Id=fts_search9.RowId and fts_search9 match @SearchTerm where A.Id in WithItemLinkItemIds ORDER BY Rank ASC LIMIT 12",
        types
    );
}

static const char EMBY_MOVIES_LATEST_COMPACT_PROJECTION[] =
    "A.Id,A.Name,A.Path,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.Images,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex ";
static const char EMBY_MOVIES_LATEST_WIDE_PROJECTION[] =
    "A.Id,A.EndDate,A.CommunityRating,A.Name,A.Path,A.PremiereDate,A.ProductionYear,A.OfficialRating,A.RunTimeTicks,A.guid,A.ParentId,A.CriticRating,A.Images,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex ";
static const char EMBY_EPISODES_LATEST_WIDE_PROJECTION[] =
    "A.Id,A.EndDate,A.IndexNumber,A.Name,A.Path,A.ParentIndexNumber,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.SeriesName,A.AlbumId,A.SeriesId,A.Images,A.SortIndexNumber,A.SortParentIndexNumber,A.IndexNumberEnd,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex ";
static const char EMBY_EPISODES_LATEST_SEMANTIC_PROJECTION[] =
    "A.Id,A.Name,A.DateCreated,A.SeriesPresentationUniqueKey,A.PresentationUniqueKey,UserDatas.IsFavorite,UserDatas.Played ";
static const char EMBY_MIXED_LATEST_PROJECTION[] =
    "A.type,A.Id,A.IndexNumber,A.Name,A.ParentIndexNumber,A.RunTimeTicks,A.ParentId,A.SeriesName,A.AlbumId,A.SeriesId,A.Images,A.SortIndexNumber,A.SortParentIndexNumber,A.IndexNumberEnd,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex ";
static const char EMBY_MIXED_LATEST_PRODUCTION_YEAR_PROJECTION[] =
    "A.type,A.Id,A.IndexNumber,A.Name,A.ParentIndexNumber,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.SeriesName,A.AlbumId,A.SeriesId,A.Images,A.SortIndexNumber,A.SortParentIndexNumber,A.IndexNumberEnd,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex ";
static const char EMBY_MIXED_LATEST_MB1_PROJECTION[] =
    "A.type,A.Id,A.EndDate,A.IndexNumber,A.Name,A.Path,A.ParentIndexNumber,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.SeriesName,A.AlbumId,A.SeriesId,A.Images,A.SortIndexNumber,A.SortParentIndexNumber,A.IndexNumberEnd,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex ";

static char *make_movies_latest_sql_form(
    int played_guard,
    const char *ancestor_ids,
    int scalar_ancestor,
    const char *projection,
    const char *user_id,
    const char *limit
) {
    return xasprintf(
        "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId%s%s%s )select "
        "%s"
        "from mediaitems A left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=%s "
        "where A.Type=5 %sA.Id in WithAncestors Group by A.PresentationUniqueKey ORDER BY A.DateCreated DESC LIMIT %s",
        scalar_ancestor ? "=" : " in (", ancestor_ids, scalar_ancestor ? "" : ")",
        projection, user_id,
        played_guard ? "AND Coalesce(UserDatas.played, 0)=0 AND " : "AND ", limit
    );
}

static char *make_movies_latest_sql(int played_guard, const char *ancestor_ids,
                                    const char *user_id, const char *limit) {
    return make_movies_latest_sql_form(
        played_guard, ancestor_ids, 0, EMBY_MOVIES_LATEST_COMPACT_PROJECTION,
        user_id, limit
    );
}

static char *make_movies_latest_expected_projection(
    const char *ancestor_ids,
    const char *projection,
    const char *user_id,
    const char *limit
) {
    return xasprintf(
        "WITH ranked(id, dc, puk) AS MATERIALIZED ("
        "  SELECT A.Id, A.DateCreated, A.PresentationUniqueKey "
        "  FROM MediaItems AS A INDEXED BY idx_dshadow_emby_latest_movies_dcn_puk "
        "  WHERE A.Type = 5 "
        "AND EXISTS ( SELECT 1 FROM AncestorIds2 AS X WHERE X.ItemId = A.Id AND X.AncestorId IN (%s) ) "
        "AND NOT EXISTS ( SELECT 1 FROM UserDatas AS U0 WHERE U0.UserDataKeyId = A.UserDataKeyId AND U0.UserId = %s AND U0.played <> 0 ) "
        "AND NOT EXISTS (      SELECT 1 "
        "      FROM MediaItems AS B INDEXED BY idx_dshadow_emby_latest_movies_puk_dc_cover "
        "      WHERE B.Type = 5 AND B.PresentationUniqueKey IS A.PresentationUniqueKey "
        "AND ( (B.DateCreated IS NOT NULL AND A.DateCreated IS NULL) OR B.DateCreated > A.DateCreated OR (B.DateCreated IS A.DateCreated AND B.Id < A.Id) ) "
        "AND EXISTS ( SELECT 1 FROM AncestorIds2 AS XB WHERE XB.ItemId = B.Id AND XB.AncestorId IN (%s) ) "
        "AND NOT EXISTS ( SELECT 1 FROM UserDatas AS UB WHERE UB.UserDataKeyId = B.UserDataKeyId AND UB.UserId = %s AND UB.played <> 0 ) ) "
        "ORDER BY (A.DateCreated IS NULL) ASC, A.DateCreated DESC, A.PresentationUniqueKey ASC LIMIT %s ) "
        "SELECT %s"
        "FROM ranked AS R JOIN MediaItems AS A ON A.Id = R.id LEFT JOIN UserDatas "
        "ON A.UserDataKeyId = UserDatas.UserDataKeyId AND UserDatas.UserId = %s "
        "ORDER BY (R.dc IS NULL) ASC, R.dc DESC, R.puk ASC LIMIT %s",
        ancestor_ids, user_id, ancestor_ids, user_id, limit, projection, user_id, limit
    );
}

static char *make_movies_latest_expected(const char *ancestor_ids,
                                         const char *user_id, const char *limit) {
    return make_movies_latest_expected_projection(
        ancestor_ids, EMBY_MOVIES_LATEST_COMPACT_PROJECTION, user_id, limit
    );
}

static char *make_latest_sql_form(
    const char *projection,
    const char *ancestor_ids,
    int scalar_ancestor,
    const char *limit
) {
    return xasprintf(
        "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId%s%s%s )select %sfrom mediaitems A left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=42 "
        "where A.Type=8 AND Coalesce(UserDatas.played, 0)=0 AND A.Id in WithAncestors Group by coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey) ORDER BY MAX(A.DateCreated) DESC LIMIT %s",
        scalar_ancestor ? "=" : " in (", ancestor_ids, scalar_ancestor ? "" : ")",
        projection, limit
    );
}

static char *make_latest_sql(const char *projection, const char *limit) {
    return make_latest_sql_form(projection, "100", 0, limit);
}

static char *make_latest_expected_form(
    const char *projection,
    const char *ancestor_ids,
    const char *limit
) {
    return xasprintf(
        "WITH ranked(id, dc, gk) AS MATERIALIZED ("
        "  SELECT A.Id, A.DateCreated, coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey) "
        "  FROM MediaItems AS A INDEXED BY idx_dshadow_emby_latest_episodes_dcn_gk "
        "  WHERE A.Type = 8 "
        "AND EXISTS ( SELECT 1 FROM AncestorIds2 AS X WHERE X.ItemId = A.Id AND X.AncestorId IN (%s) ) "
        "AND NOT EXISTS ( SELECT 1 FROM UserDatas AS U0 WHERE U0.UserDataKeyId = A.UserDataKeyId AND U0.UserId = 42 AND U0.played <> 0 ) "
        "AND NOT EXISTS (      SELECT 1 "
        "      FROM MediaItems AS B INDEXED BY idx_dshadow_emby_latest_gk_dc "
        "      WHERE B.Type = 8 AND coalesce(B.SeriesPresentationUniqueKey, B.PresentationUniqueKey) IS coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey) "
        "AND ( (B.DateCreated IS NOT NULL AND A.DateCreated IS NULL) OR B.DateCreated > A.DateCreated OR (B.DateCreated IS A.DateCreated AND B.Id < A.Id) ) "
        "AND EXISTS ( SELECT 1 FROM AncestorIds2 AS XB WHERE XB.ItemId = B.Id AND XB.AncestorId IN (%s) ) "
        "AND NOT EXISTS ( SELECT 1 FROM UserDatas AS UB WHERE UB.UserDataKeyId = B.UserDataKeyId AND UB.UserId = 42 AND UB.played <> 0 ) ) "
        "ORDER BY (A.DateCreated IS NULL) ASC, A.DateCreated DESC, coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey) ASC LIMIT %s ) "
        "SELECT %s"
        "FROM ranked AS R JOIN MediaItems AS A ON A.Id = R.id LEFT JOIN UserDatas "
        "ON A.UserDataKeyId = UserDatas.UserDataKeyId AND UserDatas.UserId = 42 "
        "ORDER BY (R.dc IS NULL) ASC, R.dc DESC, R.gk ASC LIMIT %s",
        ancestor_ids, ancestor_ids, limit, projection, limit
    );
}

static char *make_latest_expected(const char *projection, const char *limit) {
    return make_latest_expected_form(projection, "100", limit);
}

static char *make_mixed_latest_sql_projection_form(
    const char *ancestors,
    const char *projection,
    const char *user_id,
    const char *limit
) {
    return xasprintf(
        "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (%s) )select %s"
        "from mediaitems A left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=%s "
        "where A.Type in (8,5) AND Coalesce(UserDatas.played, 0)=0 AND A.Id in WithAncestors Group by coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey) ORDER BY MAX(A.DateCreated) DESC LIMIT %s",
        ancestors, projection, user_id, limit
    );
}

static char *make_mixed_latest_sql_form(
    const char *ancestors,
    const char *user_id,
    const char *limit
) {
    return make_mixed_latest_sql_projection_form(
        ancestors, EMBY_MIXED_LATEST_PROJECTION, user_id, limit
    );
}

static char *make_mixed_latest_sql(const char *user_id, const char *limit) {
    return make_mixed_latest_sql_form("100", user_id, limit);
}

static char *make_mixed_latest_expected_projection_form(
    const char *ancestors,
    const char *projection,
    const char *user_id,
    const char *limit
) {
    return xasprintf(
        "WITH mixed_latest_args(user_id, row_limit) AS MATERIALIZED ( VALUES (%s, %s) ), "
        "ranked(id, dc, gk) AS MATERIALIZED ("
        "  SELECT A.Id, A.DateCreated, coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey) "
        "  FROM MediaItems AS A INDEXED BY idx_dshadow_emby_latest_mixed_dcn_gk "
        "  CROSS JOIN mixed_latest_args AS Args "
        "  WHERE A.Type IN (8,5) AND EXISTS ( SELECT 1 FROM AncestorIds2 AS X WHERE X.ItemId = A.Id AND X.AncestorId IN (%s) ) "
        "AND NOT EXISTS ( SELECT 1 FROM UserDatas AS U0 WHERE U0.UserDataKeyId = A.UserDataKeyId AND U0.UserId = Args.user_id AND U0.played <> 0 ) "
        "AND NOT EXISTS (      SELECT 1 "
        "      FROM MediaItems AS B INDEXED BY idx_dshadow_emby_latest_mixed_gk_dc "
        "      WHERE B.Type IN (8,5) AND coalesce(B.SeriesPresentationUniqueKey, B.PresentationUniqueKey) IS coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey) "
        "AND ( (B.DateCreated IS NOT NULL AND A.DateCreated IS NULL) OR B.DateCreated > A.DateCreated OR (B.DateCreated IS A.DateCreated AND B.Id < A.Id) ) "
        "AND EXISTS ( SELECT 1 FROM AncestorIds2 AS XB WHERE XB.ItemId = B.Id AND XB.AncestorId IN (%s) ) "
        "AND NOT EXISTS ( SELECT 1 FROM UserDatas AS UB WHERE UB.UserDataKeyId = B.UserDataKeyId AND UB.UserId = Args.user_id AND UB.played <> 0 ) ) "
        "ORDER BY (A.DateCreated IS NULL) ASC, A.DateCreated DESC, coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey) DESC LIMIT (SELECT row_limit FROM mixed_latest_args) ) "
        "SELECT %s"
        "FROM ranked AS R JOIN MediaItems AS A ON A.Id = R.id CROSS JOIN mixed_latest_args AS Args LEFT JOIN UserDatas "
        "ON A.UserDataKeyId = UserDatas.UserDataKeyId AND UserDatas.UserId = Args.user_id "
        "ORDER BY (R.dc IS NULL) ASC, R.dc DESC, R.gk DESC LIMIT (SELECT row_limit FROM mixed_latest_args)",
        user_id, limit, ancestors, ancestors, projection
    );
}

static char *make_mixed_latest_expected_form(
    const char *ancestors,
    const char *user_id,
    const char *limit
) {
    return make_mixed_latest_expected_projection_form(
        ancestors, EMBY_MIXED_LATEST_PROJECTION, user_id, limit
    );
}

static char *make_mixed_latest_expected(const char *user_id, const char *limit) {
    return make_mixed_latest_expected_form("100", user_id, limit);
}

static sqlite3_stmt *contract_prepare_v2(
    sqlite3 *db,
    const char *label,
    const char *sql,
    const char **tail
) {
    sqlite3_stmt *stmt = NULL;
    int rc = emby_suite_spec.prepare(
        db, sql, -1, RSH_PREPARE_V2, &stmt, tail
    );

    if (rc != SQLITE_OK) {
        failf(
            "FAIL [%s]: prepare entry=2 rc=%d err=%s",
            label, rc, sqlite3_errmsg(db)
        );
    }
    return stmt;
}

static sqlite3_stmt *contract_prepare_legacy(
    sqlite3 *db, const char *label, const char *sql, const char **tail
) {
    sqlite3_stmt *stmt = NULL;
    int rc = emby_suite_spec.prepare(
        db, sql, -1, RSH_PREPARE_LEGACY, &stmt, tail
    );
    if (rc != SQLITE_OK) {
        failf("FAIL [%s]: prepare entry=legacy rc=%d err=%s",
              label, rc, sqlite3_errmsg(db));
    }
    return stmt;
}

static sqlite3_stmt *contract_prepare_v3(
    sqlite3 *db, const char *label, const char *sql, const char **tail
) {
    sqlite3_stmt *stmt = NULL;
    int rc = emby_suite_spec.prepare(db, sql, -1, RSH_PREPARE_V3, &stmt, tail);
    if (rc != SQLITE_OK) {
        failf("FAIL [%s]: prepare entry=3 rc=%d err=%s",
              label, rc, sqlite3_errmsg(db));
    }
    return stmt;
}

static void bind_search_term(
    sqlite3_stmt *stmt,
    const char *side,
    const char *label,
    void *ctx
) {
    const char *search_term = (const char *)ctx;
    int index = sqlite3_bind_parameter_index(stmt, "@SearchTerm");
    int rc;
    if (index == 0) {
        failf("FAIL [%s/%s-bind-index]: @SearchTerm missing", label, side);
    }
    rc = sqlite3_bind_text(stmt, index, search_term, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) {
        failf("FAIL [%s/%s-bind]: rc=%d", label, side, rc);
    }
}

static void expect_sql(sqlite3 *db, const char *label, const char *sql, int nbyte,
                       int entry, const char *want_sql, int want_rewrite) {
    const char *tail = NULL;
    sqlite3_stmt *stmt = NULL;
    rsh_prepare_kind kind = entry == 3
        ? RSH_PREPARE_V3
        : entry == 2 ? RSH_PREPARE_V2 : RSH_PREPARE_LEGACY;
    int rc = emby_suite_spec.prepare(
        db, sql, nbyte, kind, &stmt, &tail
    );

    if (rc != SQLITE_OK) {
        failf(
            "FAIL [%s]: prepare entry=%d rc=%d err=%s",
            label, entry, rc, sqlite3_errmsg(db)
        );
    }
    require_str_eq(label, sqlite3_sql(stmt), want_sql);
    if (tail != sql + strlen(sql)) {
        failf("FAIL [%s]: tail_offset=%ld want=%ld",
              label, (long)(tail - sql), (long)strlen(sql));
    }
    if (want_rewrite) {
        if (strstr(want_sql, "@SearchTerm")) {
            require_int("bind-index", sqlite3_bind_parameter_index(stmt, "@SearchTerm") > 0, 1);
        }
        require_contains(label, sqlite3_sql(stmt), "dshadow_emby_fts_rewrite(");
    } else {
        require_absent(label, sqlite3_sql(stmt), "dshadow_emby_fts_rewrite(");
    }
    require_int(label, sqlite3_finalize(stmt), SQLITE_OK);
}

static void expect_mixed_latest_sql(
    sqlite3 *db,
    const char *label,
    const char *sql,
    const char *want_sql,
    int user_bound,
    int limit_bound
) {
    const char *tail = NULL;
    sqlite3_stmt *stmt = NULL;
    int expected_binds = user_bound + limit_bound;
    int i;
    int parameter = 1;
    int rc = emby_suite_spec.prepare(
        db, sql, -1, RSH_PREPARE_V2, &stmt, &tail
    );

    if (rc != SQLITE_OK) {
        failf(
            "FAIL [%s]: prepare entry=2 rc=%d err=%s",
            label, rc, sqlite3_errmsg(db)
        );
    }

    require_str_eq(label, sqlite3_sql(stmt), want_sql);
    require_int(label, tail == sql + strlen(sql), 1);
    require_int(label, sqlite3_bind_parameter_count(stmt), expected_binds);
    require_int(label, sqlite3_column_count(stmt), 19);
    require_int(label, count_occurrences(
        sqlite3_sql(stmt), "INDEXED BY idx_dshadow_emby_latest_mixed_dcn_gk"
    ), 1);
    require_int(label, count_occurrences(
        sqlite3_sql(stmt), "INDEXED BY idx_dshadow_emby_latest_mixed_gk_dc"
    ), 1);
    require_absent(label, sqlite3_sql(stmt), " Group by ");
    require_absent(label, sqlite3_sql(stmt), "MAX(A.DateCreated)");
    for (i = 1; i <= expected_binds; i++) {
        if (sqlite3_bind_parameter_name(stmt, i) != NULL) {
            failf("FAIL [%s/bind-name-%d]: expected anonymous parameter actual=%s",
                  label, i, sqlite3_bind_parameter_name(stmt, i));
        }
    }
    if (user_bound) {
        require_int(label, sqlite3_bind_int(stmt, parameter++, 42), SQLITE_OK);
    }
    if (limit_bound) {
        require_int(label, sqlite3_bind_int(stmt, parameter++, 3), SQLITE_OK);
    }
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {}
    require_int(label, rc, SQLITE_DONE);
    require_int(label, sqlite3_finalize(stmt), SQLITE_OK);
}

static void require_mixed_production_year_row(
    sqlite3 *db,
    const char *label,
    const char *sql,
    const char *expected_sql
) {
    const char *tail = NULL;
    sqlite3_stmt *stmt = contract_prepare_v2(db, label, sql, &tail);
    int found = 0;
    int rc;

    require_str_eq(label, sqlite3_sql(stmt), expected_sql);
    require_int(label, tail == sql + strlen(sql), 1);
    require_int(label, sqlite3_column_count(stmt), 20);
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (sqlite3_column_int64(stmt, 1) != 2001) continue;
        require_int(label, sqlite3_column_type(stmt, 5), SQLITE_INTEGER);
        require_int(label, sqlite3_column_int(stmt, 5), 2026);
        found++;
    }
    require_int(label, rc, SQLITE_DONE);
    require_int(label, found, 1);
    require_int(label, sqlite3_finalize(stmt), SQLITE_OK);
}

static int g_collision_scalar_calls;

static void scalar_collision_tripwire(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    (void)argv;
    g_collision_scalar_calls++;
    sqlite3_result_error(ctx, "collision scalar must not execute", -1);
}

static void scalar_owner_spoof(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    (void)argv;
    sqlite3_result_text(ctx, "__dshadow_emby_owner_ok__", -1, SQLITE_STATIC);
}

static int deny_pragma_function_list(
    void *ctx,
    int action,
    const char *p1,
    const char *p2,
    const char *db,
    const char *trigger
) {
    (void)ctx;
    (void)action;
    (void)db;
    (void)trigger;
    if ((p1 && strcmp(p1, "pragma_function_list") == 0) ||
        (p2 && strcmp(p2, "pragma_function_list") == 0)) {
        return SQLITE_DENY;
    }
    return SQLITE_OK;
}

static int deny_sqlite_master_read(
    void *ctx,
    int action,
    const char *p1,
    const char *p2,
    const char *db,
    const char *trigger
) {
    (void)ctx;
    (void)p2;
    (void)db;
    (void)trigger;
    if (action == SQLITE_READ && p1 &&
        (strcmp(p1, "sqlite_master") == 0 || strcmp(p1, "sqlite_schema") == 0)) {
        return SQLITE_DENY;
    }
    return SQLITE_OK;
}

static int count_sqlite_master_read(
    void *ctx,
    int action,
    const char *p1,
    const char *p2,
    const char *db,
    const char *trigger
) {
    sqlite_master_auth_probe *probe = (sqlite_master_auth_probe *)ctx;
    (void)p2;
    (void)db;
    (void)trigger;
    if (action == SQLITE_READ && p1 &&
        (strcmp(p1, "sqlite_master") == 0 || strcmp(p1, "sqlite_schema") == 0)) {
        probe->reads++;
    }
    return SQLITE_OK;
}

static int scalar_authorizer_cb(
    void *ctx,
    int action,
    const char *p1,
    const char *p2,
    const char *db,
    const char *trigger
) {
    emby_auth_probe *probe = (emby_auth_probe *)ctx;
    (void)db;
    (void)trigger;
    if (action == SQLITE_FUNCTION &&
        ((p1 && strcmp(p1, "dshadow_emby_fts_rewrite") == 0) ||
         (p2 && strcmp(p2, "dshadow_emby_fts_rewrite") == 0))) {
        probe->scalar_calls++;
        if (probe->deny_scalar) return SQLITE_DENY;
    }
    return SQLITE_OK;
}

#ifdef EMBY_FTS_REWRITE_TEST_API
static void expect_scalar_counter(sqlite3 *db, const char *sql) {
    const char *tail = NULL;
    sqlite3_stmt *stmt = NULL;
    int index;
    int rc;
    int calls;

    rc = emby_suite_spec.prepare(
        db, sql, -1, RSH_PREPARE_V2, &stmt, &tail
    );
    if (rc != SQLITE_OK || !stmt) {
        if (stmt) (void)sqlite3_finalize(stmt);
        failf(
            "FAIL [scalar-counter/prepare]: got_rc=%d got_stmt=%s "
            "want_rc=%d want_stmt=non-NULL err=%s",
            rc, stmt ? "non-NULL" : "NULL", SQLITE_OK, sqlite3_errmsg(db)
        );
    }
    index = sqlite3_bind_parameter_index(stmt, "@SearchTerm");

    require_int("scalar-counter/bind-index", index > 0, 1);
    require_int("scalar-counter/bind1",
                sqlite3_bind_text(stmt, index, "(\"alpha\"*) OR (\"direct\"*)", -1, SQLITE_STATIC),
                SQLITE_OK);
    emby_fts_rewrite_test_reset_scalar_calls();
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        failf(
            "FAIL [scalar-counter/first-step]: got=%d want=%d err=%s",
            rc, SQLITE_ROW, sqlite3_errmsg(db)
        );
    }
    calls = emby_fts_rewrite_test_scalar_calls();
    if (calls <= 0) failf("FAIL [scalar-counter/first-calls]: got=%d want>0", calls);

    require_int("scalar-counter/reset", sqlite3_reset(stmt), SQLITE_OK);
    require_int("scalar-counter/clear", sqlite3_clear_bindings(stmt), SQLITE_OK);
    require_int("scalar-counter/bind2",
                sqlite3_bind_text(stmt, index, "(\"alpha\"*) OR (\"missing\"*)", -1, SQLITE_STATIC),
                SQLITE_OK);
    emby_fts_rewrite_test_reset_scalar_calls();
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        failf(
            "FAIL [scalar-counter/second-step]: got=%d want=%d err=%s",
            rc, SQLITE_DONE, sqlite3_errmsg(db)
        );
    }
    calls = emby_fts_rewrite_test_scalar_calls();
    if (calls <= 0) failf("FAIL [scalar-counter/second-calls]: got=%d want>0", calls);
    require_int("scalar-counter/finalize", sqlite3_finalize(stmt), SQLITE_OK);
}
#endif

static unsigned int collect_fts_id_mask(
    sqlite3 *db,
    const char *label,
    const char *sql,
    int nbyte,
    int want_rewrite,
    const char *search_term
) {
    const char *tail = NULL;
    sqlite3_stmt *stmt = NULL;
    int index;
    int rc = emby_suite_spec.prepare(
        db, sql, nbyte, RSH_PREPARE_V2, &stmt, &tail
    );
    int rows = 0;
    unsigned int mask = 0;

    if (rc != SQLITE_OK) {
        failf(
            "FAIL [%s]: prepare entry=2 rc=%d err=%s",
            label, rc, sqlite3_errmsg(db)
        );
    }
    index = sqlite3_bind_parameter_index(stmt, "@SearchTerm");

    if (tail != sql + strlen(sql)) {
        failf("FAIL [%s/tail]: tail_offset=%ld want=%ld",
              label, (long)(tail - sql), (long)strlen(sql));
    }
    if (want_rewrite) {
        require_contains(label, sqlite3_sql(stmt), "dshadow_emby_fts_rewrite(");
    } else {
        require_absent(label, sqlite3_sql(stmt), "dshadow_emby_fts_rewrite(");
    }
    require_int("row-parity/bind-index", index > 0, 1);
    require_int("row-parity/bind",
                sqlite3_bind_text(stmt, index, search_term, -1, SQLITE_STATIC),
                SQLITE_OK);
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        sqlite3_int64 id = sqlite3_column_int64(stmt, 2);
        unsigned int bit;
        if (sqlite3_column_type(stmt, 2) != SQLITE_INTEGER || id < 1 || id > 5) {
            failf("FAIL [%s/id]: type=%d got=%lld want=1..5", label,
                  sqlite3_column_type(stmt, 2), (long long)id);
        }
        bit = 1u << (unsigned int)(id - 1);
        if ((mask & bit) != 0) {
            failf("FAIL [%s/duplicate]: id=%lld mask=0x%x", label,
                  (long long)id, mask);
        }
        mask |= bit;
        rows++;
    }
    if (rc != SQLITE_DONE) {
        failf("FAIL [%s/step]: rc=%d err=%s", label, rc, sqlite3_errmsg(db));
    }
    require_int(label, rows, 5);
    require_int("row-parity/finalize", sqlite3_finalize(stmt), SQLITE_OK);
    return mask;
}

static int accept_emby_fts_legal_difference(
    const char *label,
    int row,
    sqlite3_stmt *vendor,
    sqlite3_stmt *candidate,
    void *ctx
) {
    sqlite3_int64 vendor_id;
    sqlite3_int64 candidate_id;

    (void)label;
    (void)row;
    (void)ctx;
    if (sqlite3_column_type(vendor, 2) != SQLITE_INTEGER ||
        sqlite3_column_type(candidate, 2) != SQLITE_INTEGER) {
        return 0;
    }
    vendor_id = sqlite3_column_int64(vendor, 2);
    candidate_id = sqlite3_column_int64(candidate, 2);
    return vendor_id >= 1 && vendor_id <= 5 &&
        candidate_id >= 1 && candidate_id <= 5;
}

static void collect_int_column(sqlite3 *db, const char *label, const char *sql,
                               const char *want_sql, int nbyte, int column,
                               char *out, size_t out_n) {
    const char *tail = NULL;
    sqlite3_stmt *stmt = NULL;
    int rc = emby_suite_spec.prepare(
        db, sql, nbyte, RSH_PREPARE_V2, &stmt, &tail
    );
    size_t used = 0;
    int rows = 0;

    if (rc != SQLITE_OK) {
        failf(
            "FAIL [%s]: prepare entry=2 rc=%d err=%s",
            label, rc, sqlite3_errmsg(db)
        );
    }

    require_str_eq(label, sqlite3_sql(stmt), want_sql);
    if (tail != sql + strlen(sql)) {
        failf("FAIL [%s/tail]: tail_offset=%ld want=%ld",
              label, (long)(tail - sql), (long)strlen(sql));
    }
    if (sqlite3_column_count(stmt) <= column) {
        failf("FAIL [%s]: column=%d count=%d", label, column, sqlite3_column_count(stmt));
    }
    if (out_n == 0) failf("FAIL [%s]: output buffer empty", label);
    out[0] = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        int n = snprintf(out + used, out_n - used, "%d,", sqlite3_column_int(stmt, column));
        if (n < 0 || (size_t)n >= out_n - used) {
            failf("FAIL [%s]: output buffer too small", label);
        }
        used += (size_t)n;
        rows++;
    }
    if (rc != SQLITE_DONE) {
        failf("FAIL [%s/step]: rc=%d err=%s", label, rc, sqlite3_errmsg(db));
    }
    if (rows == 0) failf("FAIL [%s]: expected rows", label);
    require_int("int-column/finalize", sqlite3_finalize(stmt), SQLITE_OK);
}

typedef struct typed_rows {
    unsigned char *bytes;
    size_t len;
    size_t cap;
    int columns;
    int rows;
} typed_rows;

static void typed_rows_append(typed_rows *rows, const void *src, size_t n) {
    if (rows->len + n + 1 > rows->cap) {
        size_t cap = rows->cap ? rows->cap : 256;
        unsigned char *next;
        while (cap < rows->len + n + 1) cap *= 2;
        next = (unsigned char *)realloc(rows->bytes, cap);
        if (!next) failf("FATAL: typed row realloc failed");
        rows->bytes = next;
        rows->cap = cap;
    }
    memcpy(rows->bytes + rows->len, src, n);
    rows->len += n;
    rows->bytes[rows->len] = 0;
}

static void typed_rows_appendf(typed_rows *rows, const char *fmt, ...) {
    va_list ap;
    va_list ap2;
    int n;
    char *buf;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) failf("FATAL: typed row formatting failed");
    buf = (char *)malloc((size_t)n + 1);
    if (!buf) failf("FATAL: typed row formatting alloc failed");
    if (vsnprintf(buf, (size_t)n + 1, fmt, ap2) != n) {
        failf("FATAL: typed row formatting write failed");
    }
    va_end(ap2);
    typed_rows_append(rows, buf, (size_t)n);
    free(buf);
}

static void typed_rows_append_hex(typed_rows *rows, const unsigned char *src, int n) {
    static const char hex[] = "0123456789abcdef";
    int i;
    for (i = 0; i < n; i++) {
        unsigned char out[2];
        out[0] = (unsigned char)hex[src[i] >> 4];
        out[1] = (unsigned char)hex[src[i] & 15];
        typed_rows_append(rows, out, sizeof(out));
    }
}

static typed_rows collect_typed_rows(sqlite3 *db, const char *label,
                                     const char *sql, const char *want_sql) {
    typed_rows body = {0};
    typed_rows out = {0};
    const char *tail = NULL;
    sqlite3_stmt *stmt = contract_prepare_v2(db, label, sql, &tail);
    int rc;
    int col;

    require_str_eq(label, sqlite3_sql(stmt), want_sql);
    require_int(label, tail == sql + strlen(sql), 1);
    if (tail != sql + strlen(sql)) {
        failf("FAIL [%s/tail]: tail_offset=%ld want=%ld",
              label, (long)(tail - sql), (long)strlen(sql));
    }
    body.columns = sqlite3_column_count(stmt);
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        typed_rows_append(&body, "R", 1);
        body.rows++;
        for (col = 0; col < body.columns; col++) {
            int type = sqlite3_column_type(stmt, col);
            if (type == SQLITE_NULL) {
                typed_rows_append(&body, "N;", 2);
            } else if (type == SQLITE_INTEGER) {
                typed_rows_appendf(&body, "I:%lld;", (long long)sqlite3_column_int64(stmt, col));
            } else if (type == SQLITE_FLOAT) {
                typed_rows_appendf(&body, "F:%.17g;", sqlite3_column_double(stmt, col));
            } else {
                const unsigned char *value = type == SQLITE_TEXT
                    ? sqlite3_column_text(stmt, col)
                    : (const unsigned char *)sqlite3_column_blob(stmt, col);
                int n = sqlite3_column_bytes(stmt, col);
                typed_rows_appendf(&body, "%c:%d:", type == SQLITE_TEXT ? 'T' : 'B', n);
                if (n > 0) typed_rows_append_hex(&body, value, n);
                typed_rows_append(&body, ";", 1);
            }
        }
    }
    if (rc != SQLITE_DONE) {
        failf("FAIL [%s/step]: rc=%d err=%s", label, rc, sqlite3_errmsg(db));
    }
    require_int("typed-rows/finalize", sqlite3_finalize(stmt), SQLITE_OK);
    out.columns = body.columns;
    out.rows = body.rows;
    typed_rows_appendf(&out, "C%d;N%d;", out.columns, out.rows);
    typed_rows_append(&out, body.bytes ? body.bytes : (const unsigned char *)"", body.len);
    free(body.bytes);
    return out;
}

typedef enum mixed_limit_bind_case {
    MIXED_LIMIT_ZERO = 0,
    MIXED_LIMIT_NEGATIVE,
    MIXED_LIMIT_NULL,
    MIXED_LIMIT_TEXT_THREE,
    MIXED_LIMIT_TEXT_GARBAGE
} mixed_limit_bind_case;

static typed_rows collect_bound_mixed_rows(
    sqlite3 *db,
    const char *label,
    const char *sql,
    const char *want_sql,
    mixed_limit_bind_case limit_case,
    int *step_result
) {
    typed_rows body = {0};
    typed_rows out = {0};
    const char *tail = NULL;
    sqlite3_stmt *stmt = contract_prepare_v2(db, label, sql, &tail);
    int rc;
    int col;

    require_str_eq(label, sqlite3_sql(stmt), want_sql);
    require_int(label, tail == sql + strlen(sql), 1);
    require_int(label, sqlite3_bind_parameter_count(stmt), 2);
    require_int(label, sqlite3_bind_int(stmt, 1, 42), SQLITE_OK);
    switch (limit_case) {
        case MIXED_LIMIT_ZERO:
            rc = sqlite3_bind_int(stmt, 2, 0);
            break;
        case MIXED_LIMIT_NEGATIVE:
            rc = sqlite3_bind_int(stmt, 2, -1);
            break;
        case MIXED_LIMIT_NULL:
            rc = sqlite3_bind_null(stmt, 2);
            break;
        case MIXED_LIMIT_TEXT_THREE:
            rc = sqlite3_bind_text(stmt, 2, "3", -1, SQLITE_STATIC);
            break;
        default:
            rc = sqlite3_bind_text(stmt, 2, "garbage", -1, SQLITE_STATIC);
            break;
    }
    require_int(label, rc, SQLITE_OK);
    body.columns = sqlite3_column_count(stmt);
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        typed_rows_append(&body, "R", 1);
        body.rows++;
        for (col = 0; col < body.columns; col++) {
            int type = sqlite3_column_type(stmt, col);
            if (type == SQLITE_NULL) {
                typed_rows_append(&body, "N;", 2);
            } else if (type == SQLITE_INTEGER) {
                typed_rows_appendf(&body, "I:%lld;", (long long)sqlite3_column_int64(stmt, col));
            } else if (type == SQLITE_FLOAT) {
                typed_rows_appendf(&body, "F:%.17g;", sqlite3_column_double(stmt, col));
            } else {
                const unsigned char *value = type == SQLITE_TEXT
                    ? sqlite3_column_text(stmt, col)
                    : (const unsigned char *)sqlite3_column_blob(stmt, col);
                int n = sqlite3_column_bytes(stmt, col);
                typed_rows_appendf(&body, "%c:%d:", type == SQLITE_TEXT ? 'T' : 'B', n);
                if (n > 0) typed_rows_append_hex(&body, value, n);
                typed_rows_append(&body, ";", 1);
            }
        }
    }
    *step_result = rc;
    require_int(label, sqlite3_finalize(stmt), rc == SQLITE_DONE ? SQLITE_OK : rc);
    out.columns = body.columns;
    out.rows = body.rows;
    typed_rows_appendf(&out, "C%d;N%d;RC%d;", out.columns, out.rows, rc);
    typed_rows_append(&out, body.bytes ? body.bytes : (const unsigned char *)"", body.len);
    free(body.bytes);
    return out;
}

static void require_ordered_full_row_identity(const char *label,
                                              const typed_rows *vendor,
                                              const typed_rows *candidate) {
    size_t i;
    if (vendor->columns != candidate->columns || vendor->rows != candidate->rows ||
        vendor->len != candidate->len ||
        memcmp(vendor->bytes, candidate->bytes, vendor->len) != 0) {
        size_t max = vendor->len < candidate->len ? vendor->len : candidate->len;
        for (i = 0; i < max && vendor->bytes[i] == candidate->bytes[i]; i++) {}
        failf("FAIL [%s]: first_diff=%lu vendor_columns=%d candidate_columns=%d "
              "vendor_rows=%d candidate_rows=%d vendor_len=%lu candidate_len=%lu "
              "vendor=\"%s\" candidate=\"%s\"",
              label, (unsigned long)i, vendor->columns, candidate->columns,
              vendor->rows, candidate->rows, (unsigned long)vendor->len,
              (unsigned long)candidate->len, (const char *)vendor->bytes,
              (const char *)candidate->bytes);
    }
}

typedef struct movies_latest_contract {
    sqlite3 *vendor_db;
    sqlite3 *candidate_db;
    const char *projection;
} movies_latest_contract;

static void require_dashboard_latest_fixture_row(
    const char *label,
    const char *side,
    sqlite3 *db,
    sqlite3_stmt *actual,
    const char *projection,
    sqlite3_int64 id
) {
    char *sql = xasprintf(
        "SELECT %sFROM MediaItems AS A LEFT JOIN UserDatas "
        "ON A.UserDataKeyId = UserDatas.UserDataKeyId AND UserDatas.UserId = 42 "
        "WHERE A.Id = ?",
        projection
    );
    sqlite3_stmt *expected = NULL;
    int columns;
    int col;
    int rc;

    rc = sqlite3_prepare_v2(db, sql, -1, &expected, NULL);
    if (rc != SQLITE_OK) {
        failf("FAIL [%s/%s-fixture-prepare]: id=%lld rc=%d err=%s", label,
              side, (long long)id, rc, sqlite3_errmsg(db));
    }
    require_int(label, sqlite3_bind_int64(expected, 1, id), SQLITE_OK);
    rc = sqlite3_step(expected);
    if (rc != SQLITE_ROW) {
        failf("FAIL [%s/%s-fixture-row]: id=%lld rc=%d want=%d err=%s", label,
              side, (long long)id, rc, SQLITE_ROW, sqlite3_errmsg(db));
    }
    columns = sqlite3_column_count(actual);
    if (sqlite3_column_count(expected) != columns) {
        failf("FAIL [%s/%s-fixture-columns]: id=%lld actual=%d expected=%d",
              label, side, (long long)id, columns, sqlite3_column_count(expected));
    }
    for (col = 0; col < columns; col++) {
        if (!contract_parity_cell_equal(actual, expected, col)) {
            failf("FAIL [%s/%s-fixture-column-%d]: id=%lld actual_type=%d "
                  "expected_type=%d actual_bytes=%d expected_bytes=%d",
                  label, side, col, (long long)id,
                  sqlite3_column_type(actual, col), sqlite3_column_type(expected, col),
                  sqlite3_column_bytes(actual, col), sqlite3_column_bytes(expected, col));
        }
    }
    require_int(label, sqlite3_step(expected), SQLITE_DONE);
    require_int(label, sqlite3_finalize(expected), SQLITE_OK);
    free(sql);
}

static unsigned int movies_latest_vendor_group_bit(sqlite3_int64 id) {
    if (id >= 9301 && id <= 9303) return 1u;
    if (id >= 9311 && id <= 9312) return 2u;
    if (id >= 9321 && id <= 9322) return 4u;
    return 0;
}

static int accept_movies_latest_legal_difference(
    const char *label,
    int row,
    sqlite3_stmt *vendor,
    sqlite3_stmt *candidate,
    void *ctx
) {
    static const sqlite3_int64 candidate_ids[] = {9303, 9312, 9321};
    movies_latest_contract *contract = (movies_latest_contract *)ctx;
    sqlite3_int64 vendor_id;
    sqlite3_int64 candidate_id;

    if (row < 0 || row >= (int)(sizeof(candidate_ids) / sizeof(candidate_ids[0])) ||
        sqlite3_column_type(vendor, 0) != SQLITE_INTEGER ||
        sqlite3_column_type(candidate, 0) != SQLITE_INTEGER) {
        return 0;
    }
    vendor_id = sqlite3_column_int64(vendor, 0);
    candidate_id = sqlite3_column_int64(candidate, 0);
    if (movies_latest_vendor_group_bit(vendor_id) == 0 ||
        candidate_id != candidate_ids[row]) {
        return 0;
    }
    require_dashboard_latest_fixture_row(
        label, "vendor", contract->vendor_db, vendor,
        contract->projection, vendor_id
    );
    require_dashboard_latest_fixture_row(
        label, "candidate", contract->candidate_db, candidate,
        contract->projection, candidate_id
    );
    return 1;
}

static void require_movies_latest_vendor_rows(
    sqlite3 *db,
    const char *label,
    const char *sql,
    const char *projection
) {
    const char *tail = NULL;
    sqlite3_stmt *stmt = contract_prepare_v2(db, label, sql, &tail);
    unsigned int group_mask = 0;
    int rows = 0;
    int rc;

    require_str_eq(label, sqlite3_sql(stmt), sql);
    if (tail != sql + strlen(sql)) {
        failf("FAIL [%s/vendor-tail]: got=%ld want=%ld", label,
              (long)(tail - sql), (long)strlen(sql));
    }
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        sqlite3_int64 id = sqlite3_column_int64(stmt, 0);
        unsigned int bit;
        if (sqlite3_column_type(stmt, 0) != SQLITE_INTEGER ||
            (bit = movies_latest_vendor_group_bit(id)) == 0) {
            failf("FAIL [%s/vendor-id]: type=%d got=%lld", label,
                  sqlite3_column_type(stmt, 0), (long long)id);
        }
        if ((group_mask & bit) != 0) {
            failf("FAIL [%s/vendor-duplicate-group]: id=%lld mask=0x%x bit=0x%x",
                  label, (long long)id, group_mask, bit);
        }
        require_dashboard_latest_fixture_row(
            label, "vendor", db, stmt, projection, id
        );
        group_mask |= bit;
        rows++;
    }
    if (rc != SQLITE_DONE) {
        failf("FAIL [%s/vendor-step]: rc=%d err=%s", label, rc, sqlite3_errmsg(db));
    }
    require_int(label, rows, 3);
    require_int(label, (int)group_mask, 7);
    require_int(label, sqlite3_finalize(stmt), SQLITE_OK);
}

static void require_movies_latest_candidate_rows(
    sqlite3 *db,
    const char *label,
    const char *source_sql,
    const char *expected_sql,
    const char *projection,
    const sqlite3_int64 *expected_ids,
    int expected_count
) {
    const char *tail = NULL;
    sqlite3_stmt *stmt = contract_prepare_v2(db, label, source_sql, &tail);
    int row;

    require_str_eq(label, sqlite3_sql(stmt), expected_sql);
    if (tail != source_sql + strlen(source_sql)) {
        failf("FAIL [%s/tail]: got=%ld want=%ld", label,
              (long)(tail - source_sql), (long)strlen(source_sql));
    }
    for (row = 0; row < expected_count; row++) {
        int rc = sqlite3_step(stmt);
        if (rc != SQLITE_ROW) {
            failf("FAIL [%s/row-%d]: rc=%d want=%d err=%s", label, row, rc,
                  SQLITE_ROW, sqlite3_errmsg(db));
        }
        if (sqlite3_column_type(stmt, 0) != SQLITE_INTEGER ||
            sqlite3_column_int64(stmt, 0) != expected_ids[row]) {
            failf("FAIL [%s/id-row-%d]: type=%d got=%lld want_type=%d want=%lld",
                  label, row, sqlite3_column_type(stmt, 0),
                  (long long)sqlite3_column_int64(stmt, 0), SQLITE_INTEGER,
                  (long long)expected_ids[row]);
        }
        require_dashboard_latest_fixture_row(
            label, "candidate", db, stmt, projection, expected_ids[row]
        );
    }
    require_int(label, sqlite3_step(stmt), SQLITE_DONE);
    require_int(label, sqlite3_finalize(stmt), SQLITE_OK);
}

static void free_typed_rows(typed_rows *rows) {
    free(rows->bytes);
    memset(rows, 0, sizeof(*rows));
}

static void seed_complex_resume_watched_progress_parity_rows(sqlite3 *db) {
    exec_sql(db, "complex-resume-watched-progress-parity-seed",
        "INSERT INTO MediaItems("
        "Id, Type, Name, SortName, DateCreated, EpisodeAbsoluteIndexNumber,"
        "ParentIndexNumber, SortIndexNumber, SortParentIndexNumber, IndexNumber,"
        "SeriesName, SeriesPresentationUniqueKey, PresentationUniqueKey,"
        "UserDataKeyId, IsPublic, ExtraType"
        ") VALUES"
        "(100,5,'resume movie','resume movie',800,1,NULL,NULL,NULL,NULL,NULL,NULL,'resume-movie',100,1,NULL),"
        "(101,8,'resume watched','resume watched',900,1,1,1,1,1,'resume series','resume-series','resume-episode-1',101,1,NULL),"
        "(102,8,'resume next','resume next',901,1000002,1,2,1,2,'resume series','resume-series','resume-episode-2',102,1,NULL);"
        "INSERT INTO AncestorIds2 VALUES(100,100,0),(102,100,0);"
        "INSERT INTO UserDatas("
        "UserDataKeyId, UserId, IsFavorite, Played, PlaybackPositionTicks,"
        "AudioStreamIndex, SubtitleStreamIndex, HideFromResume, LastPlayedDateInt"
        ") VALUES"
        "(100,1,0,0,500,0,0,0,800),"
        "(101,1,0,1,0,0,0,0,900),"
        "(102,1,0,0,0,0,0,0,0);"
    );
}

#ifdef EMBY_FTS_REWRITE_DIRECT_TEST

typedef struct direct_mixed_fail_open_case {
    const char *name;
    enum direct_fault_mode fault;
    int constrain_sql_limit;
    int candidate_calls;
    const char *skip_reason;
    int candidate_had_stmt;
    int candidate_bind_count;
    int candidate_column_count;
    int candidate_outer_index_count;
    int candidate_inner_index_count;
} direct_mixed_fail_open_case;

static int direct_stmt_is_baseline(
    sqlite3_stmt *stmt,
    sqlite3_stmt **baseline,
    int baseline_n
) {
    int i;
    for (i = 0; i < baseline_n; i++) {
        if (stmt == baseline[i]) return 1;
    }
    return 0;
}

#endif

#define TEST_MOVIES_LIST_PREFIX \
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) )select "
#define TEST_MOVIES_FROM \
    "from mediaitems A left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=42 "
#define TEST_MOVIES_TAIL \
    "where A.Type=5 AND Coalesce(UserDatas.played, 0)=0 AND A.Id in WithAncestors Group by A.PresentationUniqueKey ORDER BY A.DateCreated DESC LIMIT 12"

static const char TEST_E1_FAVORITES[] =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) )select "
    "count(*) OVER() AS TotalRecordCount,A.Id,A.Name,UserDatas.IsFavorite "
    "from mediaitems A join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=42 and UserDatas.IsFavorite=1 "
    "where A.Type=8 AND UserDatas.IsFavorite=1 AND A.Id in WithAncestors Group by A.PresentationUniqueKey ORDER BY A.SeriesName collate NATURALSORT ASC,A.SortName collate NATURALSORT ASC LIMIT 30";
static const char TEST_M1_FAVORITES[] =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) )select "
    "count(*) OVER() AS TotalRecordCount,A.Id,A.Name,UserDatas.IsFavorite "
    "from mediaitems A join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=42 and UserDatas.IsFavorite=1 "
    "where A.Type=5 AND UserDatas.IsFavorite=1 AND A.Id in WithAncestors Group by A.PresentationUniqueKey ORDER BY A.SortName collate NATURALSORT ASC LIMIT 30";
static const char TEST_TYPE5_RESUME[] =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) )select A.Id,A.Name "
    "from mediaitems A left join MediaItems AS LastWatchedEpisodes ON LastWatchedEpisodes.SeriesPresentationUniqueKey=A.SeriesPresentationUniqueKey "
    "left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=42 "
    "where A.Type=5 AND UserDatas.PlaybackPositionTicks>0 AND A.Id in WithAncestors Group by A.PresentationUniqueKey ORDER BY UserDatas.LastPlayedDateInt DESC LIMIT 12";
static const char TEST_M2_PREMIERE_TAIL[] =
    TEST_MOVIES_LIST_PREFIX
    "A.Id,A.EndDate,A.CommunityRating,A.Name,A.Path,A.PremiereDate,A.ProductionYear,A.OfficialRating,A.RunTimeTicks,A.guid,A.ParentId,A.CriticRating,A.Images,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex "
    TEST_MOVIES_FROM
    "where A.Type=5 AND A.PremiereDate>=1752355308 AND Coalesce(UserDatas.played, 0)=0 AND A.Id in WithAncestors Group by A.PresentationUniqueKey ORDER BY A.ProductionYear DESC,A.PremiereDate DESC,A.SortName collate NATURALSORT DESC LIMIT 30";
static const char TEST_EPISODES_DATE_WINDOW_OFFSET[] =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (15,16,17,18,19,20,3923210) )select "
    "A.Id,A.EndDate,A.IndexNumber,A.Name,A.Path,A.ParentIndexNumber,A.RunTimeTicks,A.ParentId,A.SeriesName,A.AlbumId,A.SeriesId,A.Images,A.SortIndexNumber,A.SortParentIndexNumber,A.IndexNumberEnd,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex "
    "from mediaitems A left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=5 "
    "where A.Type=8 AND A.PremiereDate>=1782906237 AND A.PremiereDate <1784121997 AND A.Id in WithAncestors Group by A.PresentationUniqueKey ORDER BY A.ProductionYear DESC,A.PremiereDate DESC,COALESCE(A.SortParentIndexNumber,A.ParentIndexNumber) ASC,COALESCE(A.SortIndexNumber,A.IndexNumber) ASC LIMIT 12 OFFSET 12";
static const char TEST_MOVIES_CORRELATED_RANDOM_IMAGE[] =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId=838031 AND ItemId=A.Id )select "
    "A.Id from mediaitems A where A.Type=5 AND A.Images like '%Primary%' AND A.Id in WithAncestors ORDER BY RANDOM() ASC LIMIT 12";
static const char TEST_MOVIES_AGGREGATE_PROJECTION[] =
    TEST_MOVIES_LIST_PREFIX "MAX(A.DateCreated) AS DateCreated,A.Id " TEST_MOVIES_FROM TEST_MOVIES_TAIL;
static const char TEST_MOVIES_WINDOW_PROJECTION[] =
    TEST_MOVIES_LIST_PREFIX "count(*) OVER() AS TotalRecordCount,A.Id " TEST_MOVIES_FROM TEST_MOVIES_TAIL;
static const char TEST_MOVIES_BARE_STAR_PROJECTION[] =
    TEST_MOVIES_LIST_PREFIX "* " TEST_MOVIES_FROM TEST_MOVIES_TAIL;
static const char TEST_MOVIES_PAREN_PROJECTION[] =
    TEST_MOVIES_LIST_PREFIX "(A.Id) AS Id,A.Name " TEST_MOVIES_FROM TEST_MOVIES_TAIL;
static const char TEST_MOVIES_BAD_LIMIT[] =
    TEST_MOVIES_LIST_PREFIX
    "A.Id,A.Name,A.Path,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.Images,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex "
    TEST_MOVIES_FROM
    "where A.Type=5 AND Coalesce(UserDatas.played, 0)=0 AND A.Id in WithAncestors Group by A.PresentationUniqueKey ORDER BY A.DateCreated DESC LIMIT 11";
static const char TEST_MOVIES_SCALAR_BIND[] =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId=? )select "
    "A.Id,A.Name,A.Path,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.Images,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex "
    TEST_MOVIES_FROM TEST_MOVIES_TAIL;
static const char TEST_MOVIES_SCALAR_NONINTEGER[] =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId=+100 )select "
    "A.Id,A.Name,A.Path,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.Images,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex "
    TEST_MOVIES_FROM TEST_MOVIES_TAIL;
static const char TEST_MOVIES_SCALAR_OPERATOR[] =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId=100 OR AncestorId=200 )select "
    "A.Id,A.Name,A.Path,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.Images,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex "
    TEST_MOVIES_FROM TEST_MOVIES_TAIL;
static const char TEST_MOVIES_MIXED_ANCESTOR[] =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) ),"
    "ScalarCarrier AS (WITH WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId=200 )select itemid FROM WithAncestors) select "
    "A.Id,A.Name,A.Path,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.Images,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex "
    TEST_MOVIES_FROM TEST_MOVIES_TAIL;
static const char TEST_MOVIES_DUPLICATE_LIST_ANCESTOR[] =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) ),"
    "Other AS (WITH WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (200) )select itemid FROM WithAncestors) select "
    "A.Id,A.Name,A.Path,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.Images,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex "
    TEST_MOVIES_FROM TEST_MOVIES_TAIL;
static const char TEST_MOVIES_AMBIGUOUS_SELECT[] =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) ),"
    "Other1 AS (WITH withancestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (200) )select itemid FROM withancestors),"
    "Other2 AS (WITH withancestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (300) )select itemid FROM withancestors) select "
    "A.Id,A.Name,A.Path,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.Images,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex "
    TEST_MOVIES_FROM TEST_MOVIES_TAIL;
static const char TEST_MOVIES_MISSING_INDEX[] =
    TEST_MOVIES_LIST_PREFIX
    "A.Id,A.Name,A.Path,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.Images,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex "
    TEST_MOVIES_FROM TEST_MOVIES_TAIL;

#undef TEST_MOVIES_LIST_PREFIX
#undef TEST_MOVIES_FROM
#undef TEST_MOVIES_TAIL

static void expect_movies_latest_semantics(sqlite3 *db) {
    static const char *keys[] = {
        "contract-decorrelated", "contract-mixed-null", "contract-all-null"
    };
    static const sqlite3_int64 dates[] = {300, 250, 0};
    char *sql = make_movies_latest_sql_form(
        1, "302", 0, "A.PresentationUniqueKey,A.DateCreated ", "42", "12"
    );
    sqlite3_stmt *stmt = contract_prepare_v2(
        db, "movies-latest-semantics", sql, NULL
    );
    const char *saved_sql = sqlite3_sql(stmt);
    int row;

    if (!saved_sql || strcmp(saved_sql, sql) == 0) {
        failf("FAIL [movies-latest-semantics/rewrite-fired]: candidate SQL did not change");
    }
    require_int("movies-latest-semantics/columns", sqlite3_column_count(stmt), 2);
    require_int("movies-latest-semantics/binds", sqlite3_bind_parameter_count(stmt), 0);
    for (row = 0; row < 3; row++) {
        const unsigned char *key;
        int rc = sqlite3_step(stmt);
        if (rc != SQLITE_ROW) {
            failf("FAIL [movies-latest-semantics/row-%d]: rc=%d want=%d err=%s",
                  row, rc, SQLITE_ROW, sqlite3_errmsg(db));
        }
        key = sqlite3_column_text(stmt, 0);
        if (sqlite3_column_type(stmt, 0) != SQLITE_TEXT || !key ||
            strcmp((const char *)key, keys[row]) != 0) {
            failf("FAIL [movies-latest-semantics/key-%d]: type=%d got=%s want=%s",
                  row, sqlite3_column_type(stmt, 0),
                  key ? (const char *)key : "(null)", keys[row]);
        }
        if (row == 2) {
            if (sqlite3_column_type(stmt, 1) != SQLITE_NULL) {
                failf("FAIL [movies-latest-semantics/date-%d]: type=%d want=%d",
                      row, sqlite3_column_type(stmt, 1), SQLITE_NULL);
            }
        } else if (sqlite3_column_type(stmt, 1) != SQLITE_INTEGER ||
                   sqlite3_column_int64(stmt, 1) != dates[row]) {
            failf("FAIL [movies-latest-semantics/date-%d]: type=%d got=%lld "
                  "want_type=%d want=%lld", row, sqlite3_column_type(stmt, 1),
                  (long long)sqlite3_column_int64(stmt, 1), SQLITE_INTEGER,
                  (long long)dates[row]);
        }
    }
    require_int("movies-latest-semantics/done", sqlite3_step(stmt), SQLITE_DONE);
    require_int("movies-latest-semantics/finalize", sqlite3_finalize(stmt), SQLITE_OK);
    free(sql);
}

static void seed_movies_latest_rows(sqlite3 *db) {
    exec_sql(db, "movies-matrix-seed",
        "WITH RECURSIVE n(i) AS (VALUES(1) UNION ALL SELECT i+1 FROM n WHERE i<30) "
        "INSERT INTO MediaItems(Id,Type,Name,Path,ProductionYear,RunTimeTicks,ParentId,Images,DateCreated,PresentationUniqueKey,UserDataKeyId) "
        "SELECT 5000+i,5,'matrix-hot-'||i,'/matrix/hot/'||i,1900+i,1000000+5000+i,9000+i,'img-hot-'||i,300000-i,printf('mx-hot-%02d',i),7000+i FROM n;"
        "WITH RECURSIVE n(i) AS (VALUES(1) UNION ALL SELECT i+1 FROM n WHERE i<24) "
        "INSERT INTO MediaItems(Id,Type,Name,Path,ProductionYear,RunTimeTicks,ParentId,Images,DateCreated,PresentationUniqueKey,UserDataKeyId) "
        "SELECT 6000+i,5,'matrix-typical-'||i,'/matrix/typical/'||i,1900+i,1000000+6000+i,9000+i,'img-typical-'||i,200000-i,printf('mx-typical-%02d',i),8000+i FROM n;"
        "INSERT INTO AncestorIds2 SELECT Id,100,0 FROM MediaItems WHERE Id BETWEEN 5001 AND 5030;"
        "INSERT INTO AncestorIds2 SELECT Id,200,0 FROM MediaItems WHERE Id BETWEEN 6001 AND 6024;"
        "INSERT INTO UserDatas(UserDataKeyId,UserId,IsFavorite,Played,PlaybackPositionTicks,AudioStreamIndex,SubtitleStreamIndex) "
        "SELECT UserDataKeyId,42,(Id%1000)%2,((Id%1000)%5=0),Id*10,(Id%1000)%3,(Id%1000)%4 "
        "FROM MediaItems WHERE Id BETWEEN 5001 AND 5030 OR Id BETWEEN 6001 AND 6024;"
        "INSERT INTO MediaItems(Id,Type,Name,Path,ProductionYear,RunTimeTicks,ParentId,Images,DateCreated,PresentationUniqueKey,UserDataKeyId) VALUES"
        "(9301,5,'contract-middle-id','/contract/9301',2031,9301,1,'i9301',200,'contract-decorrelated',19301),"
        "(9302,5,'contract-oldest','/contract/9302',2032,9302,1,'i9302',100,'contract-decorrelated',19302),"
        "(9303,5,'contract-newest','/contract/9303',2033,9303,1,'i9303',300,'contract-decorrelated',19303),"
        "(9311,5,'contract-null','/contract/9311',2034,9311,1,'i9311',NULL,'contract-mixed-null',19311),"
        "(9312,5,'contract-mixed-newest','/contract/9312',2035,9312,1,'i9312',250,'contract-mixed-null',19312),"
        "(9321,5,'contract-all-null-a','/contract/9321',2036,9321,1,'i9321',NULL,'contract-all-null',19321),"
        "(9322,5,'contract-all-null-b','/contract/9322',2037,9322,1,'i9322',NULL,'contract-all-null',19322);"
        "INSERT INTO AncestorIds2 SELECT Id,302,0 FROM MediaItems WHERE Id BETWEEN 9301 AND 9322;"
        "INSERT INTO UserDatas(UserDataKeyId,UserId,IsFavorite,Played,PlaybackPositionTicks,AudioStreamIndex,SubtitleStreamIndex) "
        "SELECT UserDataKeyId,42,0,0,0,0,0 FROM MediaItems WHERE Id BETWEEN 9301 AND 9322;"
    );
}

static void seed_movies_latest_expanded_rows(sqlite3 *db) {
    exec_sql(db, "movies-expanded-seed",
        "WITH RECURSIVE n(i) AS (VALUES(1) UNION ALL SELECT i+1 FROM n WHERE i<10) "
        "INSERT INTO MediaItems(Id,Type,Name,Path,ProductionYear,RunTimeTicks,ParentId,Images,DateCreated,PresentationUniqueKey,UserDataKeyId) "
        "SELECT 9000+i,5,'boundary-'||i,'/expanded/'||i,2000+i,900000+i,9900+i,'boundary-img-'||i,10000-i,'boundary-'||i,10000+i FROM n;"
        "INSERT INTO MediaItems(Id,Type,Name,Path,ProductionYear,RunTimeTicks,ParentId,Images,DateCreated,PresentationUniqueKey,UserDataKeyId) VALUES"
        "(9101,5,'singleton','/e/9101',2001,9101,'',NULL,900,'p-single',11101),"
        "(9102,5,'dup-middle-id','/e/9102',2002,9102,1,'i9102',800,'p-dup',11102),"
        "(9103,5,'dup-oldest','/e/9103',2003,9103,1,'i9103',600,'p-dup',11103),"
        "(9104,5,'equal-min-id','/e/9104',2004,9104,1,'i9104',600,'p-equal',11104),"
        "(9105,5,'equal-other','/e/9105',2005,9105,1,'i9105',600,'p-equal',11105),"
        "(9106,5,'mixed-null','',NULL,9106,1,NULL,NULL,'p-mixed',11106),"
        "(9107,5,'mixed-value','/e/9107',2007,9107,1,'i9107',500,'p-mixed',11107),"
        "(9108,5,'all-null-min','/e/9108',2008,9108,1,'i9108',NULL,'p-all-null',11108),"
        "(9109,5,'all-null-other','/e/9109',2009,9109,1,'i9109',NULL,'p-all-null',11109),"
        "(9110,5,'null-puk-min','/e/9110',2010,9110,1,'i9110',400,NULL,11110),"
        "(9111,5,'null-puk-other','/e/9111',2011,9111,1,'i9111',450,NULL,11111),"
        "(9112,5,'missing-userdata','/e/9112',2012,9112,1,'i9112',390,'p-missing',11112),"
        "(9113,5,'played-zero','/e/9113',2013,9113,1,'i9113',380,'p-played-zero',11113),"
        "(9114,5,'played-one','/e/9114',2014,9114,1,'i9114',370,'p-played-one',11114),"
        "(9115,5,'xb-invisible-min','/e/9115',2015,9115,1,'i9115',100,'p-xb',11115),"
        "(9116,5,'xb-visible-next','/e/9116',2016,9116,1,'i9116',200,'p-xb',11116),"
        "(9117,5,'ub-played-min','/e/9117',2017,9117,1,'i9117',110,'p-ub',11117),"
        "(9118,5,'ub-unplayed-next','/e/9118',2018,9118,1,'i9118',210,'p-ub',11118),"
        "(9119,5,'dup-newest','/e/9119',2019,9119,1,'i9119',900,'p-dup',11119);"
        "INSERT INTO AncestorIds2 SELECT Id,300,0 FROM MediaItems WHERE Type=5 AND Id>=9001 AND Id<>9115;"
        "INSERT INTO AncestorIds2 VALUES(9115,999,0);"
        "INSERT INTO UserDatas(UserDataKeyId,UserId,IsFavorite,Played,PlaybackPositionTicks,AudioStreamIndex,SubtitleStreamIndex) "
        "SELECT UserDataKeyId,42,Id%2,(Id=9114 OR Id=9117),Id*10,Id%3,Id%4 FROM MediaItems WHERE Type=5 AND Id>=9001 AND Id<>9112;"
    );
}

static void seed_movies_latest_rewrite_order_rows(sqlite3 *db) {
    exec_sql(db, "movies-rewrite-order-seed",
        "INSERT INTO MediaItems(Id,Type,Name,Path,ProductionYear,RunTimeTicks,ParentId,Images,DateCreated,PresentationUniqueKey,UserDataKeyId) VALUES"
        "(9201,5,'tie-b','/e/9201',2021,9201,1,'i9201',100,'p-tie-b',11201),"
        "(9202,5,'tie-a','/e/9202',2022,9202,1,'i9202',100,'p-tie-a',11202),"
        "(9203,5,'null-b','/e/9203',2023,9203,1,'i9203',NULL,'p-null-b',11203),"
        "(9204,5,'null-a','/e/9204',2024,9204,1,'i9204',NULL,'p-null-a',11204);"
        "INSERT INTO AncestorIds2 VALUES(9201,301,0),(9202,301,0),(9203,301,0),(9204,301,0);"
        "INSERT INTO UserDatas(UserDataKeyId,UserId,IsFavorite,Played,PlaybackPositionTicks,AudioStreamIndex,SubtitleStreamIndex) "
        "SELECT UserDataKeyId,42,0,0,0,0,0 FROM MediaItems WHERE Id BETWEEN 9201 AND 9204;"
    );
}

static void seed_episodes_latest_semantic_rows(sqlite3 *db) {
    exec_sql(db, "episodes-semantic-seed",
        "INSERT INTO MediaItems(Id,Type,Name,Path,ProductionYear,RunTimeTicks,ParentId,Images,DateCreated,SeriesPresentationUniqueKey,PresentationUniqueKey,UserDataKeyId) VALUES"
        "(9401,8,'ep-singleton','/ep/9401',2001,9401,1,'ep9401',900,'ep-single',NULL,19401),"
        "(9402,8,'ep-dup-old','/ep/9402',2002,9402,1,'ep9402',700,'ep-dup',NULL,19402),"
        "(9403,8,'ep-dup-new','/ep/9403',2003,9403,1,'ep9403',800,'ep-dup',NULL,19403),"
        "(9404,8,'ep-equal-min-id','/ep/9404',2004,9404,1,'ep9404',600,'ep-equal',NULL,19404),"
        "(9405,8,'ep-equal-other','/ep/9405',2005,9405,1,'ep9405',600,'ep-equal',NULL,19405),"
        "(9406,8,'ep-mixed-null','/ep/9406',2006,9406,1,'ep9406',NULL,'ep-mixed',NULL,19406),"
        "(9407,8,'ep-mixed-value','/ep/9407',2007,9407,1,'ep9407',500,'ep-mixed',NULL,19407),"
        "(9408,8,'ep-all-null-min','/ep/9408',2008,9408,1,'ep9408',NULL,'ep-all-null',NULL,19408),"
        "(9409,8,'ep-all-null-other','/ep/9409',2009,9409,1,'ep9409',NULL,'ep-all-null',NULL,19409),"
        "(9410,8,'ep-null-gk-old','/ep/9410',2010,9410,1,'ep9410',400,NULL,NULL,19410),"
        "(9411,8,'ep-null-gk-new','/ep/9411',2011,9411,1,'ep9411',450,NULL,NULL,19411),"
        "(9412,8,'ep-missing-userdata','/ep/9412',2012,9412,1,'ep9412',390,'ep-missing',NULL,19412),"
        "(9413,8,'ep-played-zero','/ep/9413',2013,9413,1,'ep9413',390,'ep-played-zero',NULL,19413),"
        "(9414,8,'ep-played-one','/ep/9414',2014,9414,1,'ep9414',370,'ep-played-one',NULL,19414),"
        "(9415,8,'ep-xb-invisible-new','/ep/9415',2015,9415,1,'ep9415',300,'ep-xb',NULL,19415),"
        "(9416,8,'ep-xb-visible','/ep/9416',2016,9416,1,'ep9416',200,'ep-xb',NULL,19416),"
        "(9417,8,'ep-ub-played-new','/ep/9417',2017,9417,1,'ep9417',310,'ep-ub',NULL,19417),"
        "(9418,8,'ep-ub-unplayed','/ep/9418',2018,9418,1,'ep9418',210,'ep-ub',NULL,19418);"
        "INSERT INTO AncestorIds2 SELECT Id,303,0 FROM MediaItems WHERE Id BETWEEN 9401 AND 9418 AND Id<>9415;"
        "INSERT INTO AncestorIds2 VALUES(9415,999,0);"
        "INSERT INTO UserDatas(UserDataKeyId,UserId,IsFavorite,Played,PlaybackPositionTicks,AudioStreamIndex,SubtitleStreamIndex) "
        "SELECT UserDataKeyId,42,Id%2,(Id=9414 OR Id=9417),Id*10,Id%3,Id%4 "
        "FROM MediaItems WHERE Id BETWEEN 9401 AND 9418 AND Id<>9412;"
    );
}

static unsigned int episodes_latest_fixture_group_bit(sqlite3_int64 id) {
    if (id == 9401) return 1u;
    if (id >= 9402 && id <= 9403) return 2u;
    if (id >= 9404 && id <= 9405) return 4u;
    if (id >= 9406 && id <= 9407) return 8u;
    if (id >= 9408 && id <= 9409) return 16u;
    if (id >= 9410 && id <= 9411) return 32u;
    if (id == 9412) return 64u;
    if (id == 9413) return 128u;
    if (id >= 9415 && id <= 9416) return 256u;
    if (id >= 9417 && id <= 9418) return 512u;
    return 0;
}

static int episodes_latest_row_is_eligible_max(sqlite3 *db, sqlite3_int64 id) {
    static const char sql[] =
        "SELECT EXISTS(SELECT 1 FROM MediaItems AS A WHERE A.Id=?1 AND A.Type=8 "
        "AND EXISTS (SELECT 1 FROM AncestorIds2 AS X WHERE X.ItemId=A.Id AND X.AncestorId=303) "
        "AND NOT EXISTS (SELECT 1 FROM UserDatas AS U WHERE U.UserDataKeyId=A.UserDataKeyId AND U.UserId=42 AND U.played<>0) "
        "AND NOT EXISTS (SELECT 1 FROM MediaItems AS B WHERE B.Type=8 "
        "AND coalesce(B.SeriesPresentationUniqueKey,B.PresentationUniqueKey) IS coalesce(A.SeriesPresentationUniqueKey,A.PresentationUniqueKey) "
        "AND ((B.DateCreated IS NOT NULL AND A.DateCreated IS NULL) OR B.DateCreated>A.DateCreated) "
        "AND EXISTS (SELECT 1 FROM AncestorIds2 AS XB WHERE XB.ItemId=B.Id AND XB.AncestorId=303) "
        "AND NOT EXISTS (SELECT 1 FROM UserDatas AS UB WHERE UB.UserDataKeyId=B.UserDataKeyId AND UB.UserId=42 AND UB.played<>0)))";
    sqlite3_stmt *stmt = NULL;
    int value;

    require_int("episodes-semantic/oracle-prepare",
                sqlite3_prepare_v2(db, sql, -1, &stmt, NULL), SQLITE_OK);
    require_int("episodes-semantic/oracle-bind",
                sqlite3_bind_int64(stmt, 1, id), SQLITE_OK);
    require_int("episodes-semantic/oracle-row", sqlite3_step(stmt), SQLITE_ROW);
    value = sqlite3_column_int(stmt, 0);
    require_int("episodes-semantic/oracle-done", sqlite3_step(stmt), SQLITE_DONE);
    require_int("episodes-semantic/oracle-finalize", sqlite3_finalize(stmt), SQLITE_OK);
    return value;
}

static void require_episodes_latest_semantics(sqlite3 *vendor_db, sqlite3 *candidate_db) {
    static const sqlite3_int64 expected_candidate_ids[] = {
        9401, 9403, 9404, 9407, 9411, 9412, 9413, 9418, 9416, 9408
    };
    char *raw = make_latest_sql_form(
        EMBY_EPISODES_LATEST_SEMANTIC_PROJECTION, "303", 0, "20"
    );
    char *expected = make_latest_expected_form(
        EMBY_EPISODES_LATEST_SEMANTIC_PROJECTION, "303", "20"
    );
    const char *tail = NULL;
    sqlite3_stmt *stmt;
    unsigned int group_mask = 0;
    int row;
    int rc;

    stmt = contract_prepare_v2(
        candidate_db, "episodes-semantic/candidate", raw, &tail
    );
    require_str_eq("episodes-semantic/candidate-sql", sqlite3_sql(stmt), expected);
    if (tail != raw + strlen(raw)) {
        failf("FAIL [episodes-semantic/candidate-tail]: got=%ld want=%ld",
              (long)(tail - raw), (long)strlen(raw));
    }
    for (row = 0; row < (int)(sizeof(expected_candidate_ids) / sizeof(expected_candidate_ids[0])); row++) {
        sqlite3_int64 id;
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_ROW) {
            failf("FAIL [episodes-semantic/candidate-row-%d]: rc=%d want=%d err=%s",
                  row, rc, SQLITE_ROW, sqlite3_errmsg(candidate_db));
        }
        id = sqlite3_column_int64(stmt, 0);
        if (sqlite3_column_type(stmt, 0) != SQLITE_INTEGER ||
            id != expected_candidate_ids[row]) {
            failf("FAIL [episodes-semantic/candidate-id-%d]: type=%d got=%lld want=%lld",
                  row, sqlite3_column_type(stmt, 0), (long long)id,
                  (long long)expected_candidate_ids[row]);
        }
        require_dashboard_latest_fixture_row(
            "episodes-semantic", "candidate", candidate_db, stmt,
            EMBY_EPISODES_LATEST_SEMANTIC_PROJECTION, id
        );
    }
    require_int("episodes-semantic/candidate-done", sqlite3_step(stmt), SQLITE_DONE);
    require_int("episodes-semantic/candidate-finalize", sqlite3_finalize(stmt), SQLITE_OK);

    tail = NULL;
    stmt = contract_prepare_v2(
        vendor_db, "episodes-semantic/vendor", raw, &tail
    );
    require_str_eq("episodes-semantic/vendor-sql", sqlite3_sql(stmt), raw);
    if (tail != raw + strlen(raw)) {
        failf("FAIL [episodes-semantic/vendor-tail]: got=%ld want=%ld",
              (long)(tail - raw), (long)strlen(raw));
    }
    row = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        sqlite3_int64 id = sqlite3_column_int64(stmt, 0);
        unsigned int bit = episodes_latest_fixture_group_bit(id);
        if (sqlite3_column_type(stmt, 0) != SQLITE_INTEGER || bit == 0) {
            failf("FAIL [episodes-semantic/vendor-id-%d]: type=%d got=%lld",
                  row, sqlite3_column_type(stmt, 0), (long long)id);
        }
        if ((group_mask & bit) != 0) {
            failf("FAIL [episodes-semantic/vendor-duplicate-group-%d]: id=%lld mask=0x%x bit=0x%x",
                  row, (long long)id, group_mask, bit);
        }
        require_int("episodes-semantic/vendor-eligible-max",
                    episodes_latest_row_is_eligible_max(vendor_db, id), 1);
        require_dashboard_latest_fixture_row(
            "episodes-semantic", "vendor", vendor_db, stmt,
            EMBY_EPISODES_LATEST_SEMANTIC_PROJECTION, id
        );
        group_mask |= bit;
        row++;
    }
    require_int("episodes-semantic/vendor-step", rc, SQLITE_DONE);
    require_int("episodes-semantic/vendor-row-count", row, 10);
    require_int("episodes-semantic/vendor-group-mask", (int)group_mask, 1023);
    require_int("episodes-semantic/vendor-finalize", sqlite3_finalize(stmt), SQLITE_OK);
    free(raw);
    free(expected);
}

static int query_int(sqlite3 *db, const char *label, const char *sql) {
    sqlite3_stmt *stmt = NULL;
    int value;
    require_int(label, sqlite3_prepare_v2(db, sql, -1, &stmt, NULL), SQLITE_OK);
    require_int(label, sqlite3_step(stmt), SQLITE_ROW);
    value = sqlite3_column_int(stmt, 0);
    require_int(label, sqlite3_step(stmt), SQLITE_DONE);
    require_int(label, sqlite3_finalize(stmt), SQLITE_OK);
    return value;
}

static const char *const emby_controlled_env_gates[] = {
    "SQLITE3_DISABLE_AUTOPRAGMA",
    "SQLITE3_DISABLE_RUNTIME_OPTIMIZE",
    "SQLITE3_DISABLE_OBSERVABILITY",
    "SQLITE3_DISABLE_PLEX_FTS_REWRITE",
    "SQLITE3_DISABLE_EMBY_FTS_REWRITE",
    "SQLITE3_DISABLE_EMBY_FANOUT_REWRITE",
    "SQLITE3_DISABLE_EMBY_DASHBOARD_REWRITE",
    "SQLITE3_DISABLE_STMT_TRACE",
    "SQLITE3_DISABLE_REWRITE_APPLIED_SQL"
};

#define EMBY_ENV_UNSET(gate_name) \
    {.name = (gate_name), .value = {.state = RSH_ENV_UNSET}}
#define EMBY_ENV_EMPTY(gate_name) \
    {.name = (gate_name), .value = {.state = RSH_ENV_EMPTY}}
#define EMBY_ENV_SET(gate_name, gate_value) \
    {.name = (gate_name), \
     .value = {.state = RSH_ENV_VALUE, .value = (gate_value)}}
#define EMBY_NATIVE_ENV_COMMON \
    EMBY_ENV_SET("SQLITE3_DISABLE_AUTOPRAGMA", "1"), \
    EMBY_ENV_SET("SQLITE3_DISABLE_RUNTIME_OPTIMIZE", "1"), \
    EMBY_ENV_SET("SQLITE3_DISABLE_OBSERVABILITY", "1"), \
    EMBY_ENV_UNSET("SQLITE3_DISABLE_PLEX_FTS_REWRITE"), \
    EMBY_ENV_UNSET("SQLITE3_DISABLE_STMT_TRACE"), \
    EMBY_ENV_UNSET("SQLITE3_DISABLE_REWRITE_APPLIED_SQL")

static const rsh_env_assignment emby_fts_default_env[] = {
    EMBY_NATIVE_ENV_COMMON,
    EMBY_ENV_UNSET("SQLITE3_DISABLE_EMBY_FTS_REWRITE"),
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_FANOUT_REWRITE", "1"),
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_DASHBOARD_REWRITE", "1")
};

static const rsh_env_assignment emby_fts_zero_env[] = {
    EMBY_NATIVE_ENV_COMMON,
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_FTS_REWRITE", "0"),
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_FANOUT_REWRITE", "1"),
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_DASHBOARD_REWRITE", "1")
};

static const rsh_env_assignment emby_fts_one_env[] = {
    EMBY_NATIVE_ENV_COMMON,
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_FTS_REWRITE", "1"),
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_FANOUT_REWRITE", "1"),
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_DASHBOARD_REWRITE", "1")
};

static const rsh_env_assignment emby_fts_garbage_env[] = {
    EMBY_NATIVE_ENV_COMMON,
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_FTS_REWRITE", "xyz"),
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_FANOUT_REWRITE", "1"),
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_DASHBOARD_REWRITE", "1")
};

static const rsh_env_assignment emby_fanout_default_env[] = {
    EMBY_NATIVE_ENV_COMMON,
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_FTS_REWRITE", "1"),
    EMBY_ENV_UNSET("SQLITE3_DISABLE_EMBY_FANOUT_REWRITE"),
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_DASHBOARD_REWRITE", "1")
};

static const rsh_env_assignment emby_fanout_zero_env[] = {
    EMBY_NATIVE_ENV_COMMON,
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_FTS_REWRITE", "1"),
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_FANOUT_REWRITE", "0"),
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_DASHBOARD_REWRITE", "1")
};

static const rsh_env_assignment emby_d6_fanout_zero_env[] = {
    EMBY_NATIVE_ENV_COMMON,
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_FTS_REWRITE", "1"),
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_FANOUT_REWRITE", "0"),
    EMBY_ENV_UNSET("SQLITE3_DISABLE_EMBY_DASHBOARD_REWRITE")
};

static const rsh_env_assignment emby_fanout_one_env[] = {
    EMBY_NATIVE_ENV_COMMON,
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_FTS_REWRITE", "1"),
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_FANOUT_REWRITE", "1"),
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_DASHBOARD_REWRITE", "1")
};

static const rsh_env_assignment emby_fanout_garbage_env[] = {
    EMBY_NATIVE_ENV_COMMON,
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_FTS_REWRITE", "1"),
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_FANOUT_REWRITE", "false"),
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_DASHBOARD_REWRITE", "1")
};

static const rsh_env_assignment emby_dashboard_default_env[] = {
    EMBY_NATIVE_ENV_COMMON,
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_FTS_REWRITE", "1"),
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_FANOUT_REWRITE", "1"),
    EMBY_ENV_UNSET("SQLITE3_DISABLE_EMBY_DASHBOARD_REWRITE")
};

static const rsh_env_assignment emby_dashboard_one_env[] = {
    EMBY_NATIVE_ENV_COMMON,
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_FTS_REWRITE", "1"),
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_FANOUT_REWRITE", "1"),
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_DASHBOARD_REWRITE", "1")
};

static const rsh_env_assignment emby_dashboard_empty_env[] = {
    EMBY_NATIVE_ENV_COMMON,
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_FTS_REWRITE", "1"),
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_FANOUT_REWRITE", "1"),
    EMBY_ENV_EMPTY("SQLITE3_DISABLE_EMBY_DASHBOARD_REWRITE")
};

static const rsh_env_assignment emby_dashboard_garbage_env[] = {
    EMBY_NATIVE_ENV_COMMON,
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_FTS_REWRITE", "1"),
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_FANOUT_REWRITE", "1"),
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_DASHBOARD_REWRITE", "false")
};

static const rsh_env_assignment emby_fixture_canary_env[] = {
    EMBY_NATIVE_ENV_COMMON,
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_FTS_REWRITE", "0"),
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_FANOUT_REWRITE", "0"),
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_DASHBOARD_REWRITE", "0")
};

static const rsh_env_assignment emby_latest_native_env[] = {
    EMBY_NATIVE_ENV_COMMON,
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_FTS_REWRITE", "1"),
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_FANOUT_REWRITE", "1"),
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_DASHBOARD_REWRITE", "0")
};

static const rsh_env_assignment emby_dashboard_matcher_negatives_env[] = {
    EMBY_ENV_SET("SQLITE3_DISABLE_AUTOPRAGMA", "1"),
    EMBY_ENV_SET("SQLITE3_DISABLE_RUNTIME_OPTIMIZE", "1"),
    EMBY_ENV_UNSET("SQLITE3_DISABLE_OBSERVABILITY"),
    EMBY_ENV_UNSET("SQLITE3_DISABLE_PLEX_FTS_REWRITE"),
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_FTS_REWRITE", "1"),
    EMBY_ENV_UNSET("SQLITE3_DISABLE_EMBY_FANOUT_REWRITE"),
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_DASHBOARD_REWRITE", "0"),
    EMBY_ENV_UNSET("SQLITE3_DISABLE_STMT_TRACE"),
    EMBY_ENV_UNSET("SQLITE3_DISABLE_REWRITE_APPLIED_SQL")
};

static const rsh_env_assignment emby_d8_index_definition_env[] = {
    EMBY_ENV_SET("SQLITE3_DISABLE_AUTOPRAGMA", "1"),
    EMBY_ENV_SET("SQLITE3_DISABLE_RUNTIME_OPTIMIZE", "1"),
    EMBY_ENV_UNSET("SQLITE3_DISABLE_OBSERVABILITY"),
    EMBY_ENV_UNSET("SQLITE3_DISABLE_PLEX_FTS_REWRITE"),
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_FTS_REWRITE", "1"),
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_FANOUT_REWRITE", "1"),
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_DASHBOARD_REWRITE", "0"),
    EMBY_ENV_SET("SQLITE3_DISABLE_STMT_TRACE", "1"),
    EMBY_ENV_UNSET("SQLITE3_DISABLE_REWRITE_APPLIED_SQL")
};

static const rsh_env_assignment emby_d8_observability_env[] = {
    EMBY_ENV_SET("SQLITE3_DISABLE_AUTOPRAGMA", "1"),
    EMBY_ENV_SET("SQLITE3_DISABLE_RUNTIME_OPTIMIZE", "1"),
    EMBY_ENV_UNSET("SQLITE3_DISABLE_OBSERVABILITY"),
    EMBY_ENV_UNSET("SQLITE3_DISABLE_PLEX_FTS_REWRITE"),
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_FTS_REWRITE", "1"),
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_FANOUT_REWRITE", "0"),
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_DASHBOARD_REWRITE", "0"),
    EMBY_ENV_SET("SQLITE3_DISABLE_STMT_TRACE", "1"),
    EMBY_ENV_UNSET("SQLITE3_DISABLE_REWRITE_APPLIED_SQL")
};

static const rsh_env_assignment emby_d8_master_disabled_env[] = {
    EMBY_ENV_SET("SQLITE3_DISABLE_AUTOPRAGMA", "1"),
    EMBY_ENV_SET("SQLITE3_DISABLE_RUNTIME_OPTIMIZE", "1"),
    EMBY_ENV_SET("SQLITE3_DISABLE_OBSERVABILITY", "1"),
    EMBY_ENV_UNSET("SQLITE3_DISABLE_PLEX_FTS_REWRITE"),
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_FTS_REWRITE", "1"),
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_FANOUT_REWRITE", "1"),
    EMBY_ENV_SET("SQLITE3_DISABLE_EMBY_DASHBOARD_REWRITE", "0"),
    EMBY_ENV_SET("SQLITE3_DISABLE_STMT_TRACE", "0"),
    EMBY_ENV_UNSET("SQLITE3_DISABLE_REWRITE_APPLIED_SQL")
};

#undef EMBY_NATIVE_ENV_COMMON
#undef EMBY_ENV_SET
#undef EMBY_ENV_EMPTY
#undef EMBY_ENV_UNSET

enum {
    EMBY_PROFILE_DASHBOARD_OFF = 1,
    EMBY_PROFILE_LATEST_INDEXES,
    EMBY_PROFILE_FIXTURE_CANARY,
    EMBY_PROFILE_MOVIES_SEED_ONLY,
    EMBY_PROFILE_EPISODES_DATE_INDEX_MISSING,
    EMBY_PROFILE_EPISODES_GROUP_INDEX_MISSING,
    EMBY_PROFILE_EPISODES_BOTH_INDEXES_MISSING,
    EMBY_PROFILE_MOVIES_OUTER_INDEX_MISSING,
    EMBY_PROFILE_MOVIES_INNER_INDEX_MISSING,
    EMBY_PROFILE_D5B_FIX_CANDIDATE,
    EMBY_PROFILE_D5B_SCALAR_MINUS_ONE_VENDOR_SEED,
    EMBY_PROFILE_D5B_SCALAR_MINUS_ONE_CANDIDATE_SEED,
    EMBY_PROFILE_D5B_MIXED_ALL_INDEXES,
    EMBY_PROFILE_D5B_MIXED_OUTER_MISSING,
    EMBY_PROFILE_D5B_MIXED_INNER_MISSING,
    EMBY_PROFILE_D5B_MIXED_BOTH_MISSING,
    EMBY_PROFILE_D5B_MIXED_OUTER_MALFORMED,
    EMBY_PROFILE_D5B_MIXED_INNER_MALFORMED,
    EMBY_PROFILE_D6_COMPLEX_RESUME_SEED,
    EMBY_PROFILE_D6_LINKS_TWO_LEVEL_SEED,
    EMBY_PROFILE_D6_MIXED_IDENTITY,
    EMBY_PROFILE_D7_DASHBOARD_INDEXES,
    EMBY_PROFILE_D7_EPISODES_SEMANTIC_SEED,
    EMBY_PROFILE_D7_MOVIES_EXPANDED_SEED,
    EMBY_PROFILE_D7_MOVIES_REWRITE_ORDER_SEED,
    EMBY_PROFILE_D8_EPISODES_GROUP_MALFORMED,
    EMBY_PROFILE_D8_EPISODES_DATE_MALFORMED,
    EMBY_PROFILE_D8_MOVIES_OUTER_MALFORMED,
    EMBY_PROFILE_D8_MOVIES_INNER_MALFORMED,
    EMBY_PROFILE_D8_MOVIES_CANONICAL,
    EMBY_PROFILE_D8_EPISODES_DATE_MISSING,
    EMBY_PROFILE_D8_EPISODES_GROUP_MISSING,
    EMBY_PROFILE_D8_EPISODES_BOTH_MISSING,
    EMBY_PROFILE_D8_MIXED_ALL_INDEXES_WITH_MOVIES_SEED
};

static int rsh_apply_emby_dashboard_off_profile(
    sqlite3 *db,
    int profile,
    void *suite_ctx
) {
    (void)suite_ctx;
    if (profile != EMBY_PROFILE_DASHBOARD_OFF) return SQLITE_MISUSE;
    exec_sql(
        db, "latest-index",
        "CREATE INDEX idx_dshadow_emby_latest_gk_dc "
        "ON MediaItems (coalesce(SeriesPresentationUniqueKey, PresentationUniqueKey), DateCreated DESC, Id, UserDataKeyId) "
        "WHERE Type = 8;"
    );
    exec_sql(
        db, "latest-episodes-date-index",
        "CREATE INDEX idx_dshadow_emby_latest_episodes_dcn_gk "
        "ON MediaItems ((DateCreated IS NULL), DateCreated DESC, coalesce(SeriesPresentationUniqueKey, PresentationUniqueKey), Id, UserDataKeyId) "
        "WHERE Type = 8;"
    );
    exec_sql(
        db, "movies-outer-index",
        "CREATE INDEX idx_dshadow_emby_latest_movies_dcn_puk "
        "ON MediaItems ((DateCreated IS NULL), DateCreated DESC, PresentationUniqueKey, Id, UserDataKeyId) "
        "WHERE Type = 5;"
    );
    exec_sql(
        db, "movies-inner-index",
        "CREATE INDEX idx_dshadow_emby_latest_movies_puk_dc_cover "
        "ON MediaItems (PresentationUniqueKey, DateCreated, Id, UserDataKeyId) "
        "WHERE Type = 5;"
    );
    exec_sql(
        db, "mixed-outer-index",
        "CREATE INDEX idx_dshadow_emby_latest_mixed_dcn_gk "
        "ON MediaItems ((DateCreated IS NULL), DateCreated DESC, coalesce(SeriesPresentationUniqueKey, PresentationUniqueKey), Id, UserDataKeyId) "
        "WHERE Type IN (8,5);"
    );
    exec_sql(
        db, "mixed-inner-index",
        "CREATE INDEX idx_dshadow_emby_latest_mixed_gk_dc "
        "ON MediaItems (coalesce(SeriesPresentationUniqueKey, PresentationUniqueKey), DateCreated DESC, Id, UserDataKeyId) "
        "WHERE Type IN (8,5);"
    );
    seed_movies_latest_rows(db);
    return SQLITE_OK;
}

static int rsh_apply_emby_latest_indexes_profile(
    sqlite3 *db,
    int profile,
    void *suite_ctx
) {
    (void)suite_ctx;
    if (profile != EMBY_PROFILE_LATEST_INDEXES) return SQLITE_MISUSE;
    exec_sql(
        db, "latest-index",
        "CREATE INDEX idx_dshadow_emby_latest_gk_dc "
        "ON MediaItems (coalesce(SeriesPresentationUniqueKey, PresentationUniqueKey), DateCreated DESC, Id, UserDataKeyId) "
        "WHERE Type = 8;"
    );
    exec_sql(
        db, "latest-episodes-date-index",
        "CREATE INDEX idx_dshadow_emby_latest_episodes_dcn_gk "
        "ON MediaItems ((DateCreated IS NULL), DateCreated DESC, coalesce(SeriesPresentationUniqueKey, PresentationUniqueKey), Id, UserDataKeyId) "
        "WHERE Type = 8;"
    );
    return SQLITE_OK;
}

static int rsh_apply_emby_fixture_canary_profile(
    sqlite3 *db,
    int profile,
    void *suite_ctx
) {
    (void)suite_ctx;
    if (profile != EMBY_PROFILE_FIXTURE_CANARY) return SQLITE_MISUSE;
    exec_sql(
        db, "latest-index",
        "CREATE INDEX idx_dshadow_emby_latest_gk_dc "
        "ON MediaItems (coalesce(SeriesPresentationUniqueKey, PresentationUniqueKey), DateCreated DESC, Id, UserDataKeyId) "
        "WHERE Type = 8;"
    );
    exec_sql(
        db, "latest-episodes-date-index",
        "CREATE INDEX idx_dshadow_emby_latest_episodes_dcn_gk "
        "ON MediaItems ((DateCreated IS NULL), DateCreated DESC, coalesce(SeriesPresentationUniqueKey, PresentationUniqueKey), Id, UserDataKeyId) "
        "WHERE Type = 8;"
    );
    exec_sql(
        db, "movies-outer-index",
        "CREATE INDEX idx_dshadow_emby_latest_movies_dcn_puk "
        "ON MediaItems ((DateCreated IS NULL), DateCreated DESC, PresentationUniqueKey, Id, UserDataKeyId) "
        "WHERE Type = 5;"
    );
    exec_sql(
        db, "movies-inner-index",
        "CREATE INDEX idx_dshadow_emby_latest_movies_puk_dc_cover "
        "ON MediaItems (PresentationUniqueKey, DateCreated, Id, UserDataKeyId) "
        "WHERE Type = 5;"
    );
    seed_movies_latest_rows(db);
    return SQLITE_OK;
}

static int rsh_apply_emby_dashboard_matrix_profile(
    sqlite3 *db,
    int profile,
    void *suite_ctx
) {
    int episodes_gk_dc_index;
    int episodes_dcn_gk_index;
    int movies_outer_index;
    int movies_inner_index;

    (void)suite_ctx;
    if (profile == EMBY_PROFILE_MOVIES_SEED_ONLY) {
        episodes_gk_dc_index = 0;
        episodes_dcn_gk_index = 0;
        movies_outer_index = 0;
        movies_inner_index = 0;
    } else if (profile == EMBY_PROFILE_EPISODES_DATE_INDEX_MISSING) {
        episodes_gk_dc_index = 1;
        episodes_dcn_gk_index = 0;
        movies_outer_index = 1;
        movies_inner_index = 1;
    } else if (profile == EMBY_PROFILE_EPISODES_GROUP_INDEX_MISSING) {
        episodes_gk_dc_index = 0;
        episodes_dcn_gk_index = 1;
        movies_outer_index = 1;
        movies_inner_index = 1;
    } else if (profile == EMBY_PROFILE_EPISODES_BOTH_INDEXES_MISSING) {
        episodes_gk_dc_index = 0;
        episodes_dcn_gk_index = 0;
        movies_outer_index = 1;
        movies_inner_index = 1;
    } else if (profile == EMBY_PROFILE_MOVIES_OUTER_INDEX_MISSING) {
        episodes_gk_dc_index = 1;
        episodes_dcn_gk_index = 1;
        movies_outer_index = 0;
        movies_inner_index = 1;
    } else if (profile == EMBY_PROFILE_MOVIES_INNER_INDEX_MISSING) {
        episodes_gk_dc_index = 1;
        episodes_dcn_gk_index = 1;
        movies_outer_index = 1;
        movies_inner_index = 0;
    } else {
        return SQLITE_MISUSE;
    }

    if (episodes_gk_dc_index) {
        exec_sql(
            db, "latest-index",
            "CREATE INDEX idx_dshadow_emby_latest_gk_dc "
            "ON MediaItems (coalesce(SeriesPresentationUniqueKey, PresentationUniqueKey), DateCreated DESC, Id, UserDataKeyId) "
            "WHERE Type = 8;"
        );
    }
    if (episodes_dcn_gk_index) {
        exec_sql(
            db, "latest-episodes-date-index",
            "CREATE INDEX idx_dshadow_emby_latest_episodes_dcn_gk "
            "ON MediaItems ((DateCreated IS NULL), DateCreated DESC, coalesce(SeriesPresentationUniqueKey, PresentationUniqueKey), Id, UserDataKeyId) "
            "WHERE Type = 8;"
        );
    }
    if (movies_outer_index) {
        exec_sql(
            db, "movies-outer-index",
            "CREATE INDEX idx_dshadow_emby_latest_movies_dcn_puk "
            "ON MediaItems ((DateCreated IS NULL), DateCreated DESC, PresentationUniqueKey, Id, UserDataKeyId) "
            "WHERE Type = 5;"
        );
    }
    if (movies_inner_index) {
        exec_sql(
            db, "movies-inner-index",
            "CREATE INDEX idx_dshadow_emby_latest_movies_puk_dc_cover "
            "ON MediaItems (PresentationUniqueKey, DateCreated, Id, UserDataKeyId) "
            "WHERE Type = 5;"
        );
    }
    seed_movies_latest_rows(db);
    return SQLITE_OK;
}

static int rsh_apply_emby_d5b_profile(
    sqlite3 *db,
    int profile,
    void *suite_ctx
) {
    int mixed_outer_index = 1;
    int mixed_inner_index = 1;

    (void)suite_ctx;
    if (profile == EMBY_PROFILE_D5B_FIX_CANDIDATE) {
        return rsh_apply_emby_fixture_canary_profile(
            db, EMBY_PROFILE_FIXTURE_CANARY, suite_ctx
        );
    }
    if (profile == EMBY_PROFILE_D5B_SCALAR_MINUS_ONE_VENDOR_SEED ||
        profile == EMBY_PROFILE_D5B_SCALAR_MINUS_ONE_CANDIDATE_SEED) {
        exec_sql(
            db,
            profile == EMBY_PROFILE_D5B_SCALAR_MINUS_ONE_VENDOR_SEED
                ? "dashboard-scalar-minus-one-vendor-seed"
                : "dashboard-scalar-minus-one-candidate-seed",
            "INSERT INTO AncestorIds2 VALUES(7,-1,0),(3,-1,0);"
        );
        return SQLITE_OK;
    }
    if (profile == EMBY_PROFILE_D5B_MIXED_OUTER_MISSING) {
        mixed_outer_index = 0;
    } else if (profile == EMBY_PROFILE_D5B_MIXED_INNER_MISSING) {
        mixed_inner_index = 0;
    } else if (profile == EMBY_PROFILE_D5B_MIXED_BOTH_MISSING) {
        mixed_outer_index = 0;
        mixed_inner_index = 0;
    } else if (profile != EMBY_PROFILE_D5B_MIXED_ALL_INDEXES &&
               profile != EMBY_PROFILE_D5B_MIXED_OUTER_MALFORMED &&
               profile != EMBY_PROFILE_D5B_MIXED_INNER_MALFORMED) {
        return SQLITE_MISUSE;
    }

    exec_sql(
        db, "latest-index",
        "CREATE INDEX idx_dshadow_emby_latest_gk_dc "
        "ON MediaItems (coalesce(SeriesPresentationUniqueKey, PresentationUniqueKey), DateCreated DESC, Id, UserDataKeyId) "
        "WHERE Type = 8;"
    );
    exec_sql(
        db, "latest-episodes-date-index",
        "CREATE INDEX idx_dshadow_emby_latest_episodes_dcn_gk "
        "ON MediaItems ((DateCreated IS NULL), DateCreated DESC, coalesce(SeriesPresentationUniqueKey, PresentationUniqueKey), Id, UserDataKeyId) "
        "WHERE Type = 8;"
    );
    exec_sql(
        db, "movies-outer-index",
        "CREATE INDEX idx_dshadow_emby_latest_movies_dcn_puk "
        "ON MediaItems ((DateCreated IS NULL), DateCreated DESC, PresentationUniqueKey, Id, UserDataKeyId) "
        "WHERE Type = 5;"
    );
    exec_sql(
        db, "movies-inner-index",
        "CREATE INDEX idx_dshadow_emby_latest_movies_puk_dc_cover "
        "ON MediaItems (PresentationUniqueKey, DateCreated, Id, UserDataKeyId) "
        "WHERE Type = 5;"
    );
    if (mixed_outer_index) {
        exec_sql(
            db, "mixed-outer-index",
            "CREATE INDEX idx_dshadow_emby_latest_mixed_dcn_gk "
            "ON MediaItems ((DateCreated IS NULL), DateCreated DESC, coalesce(SeriesPresentationUniqueKey, PresentationUniqueKey), Id, UserDataKeyId) "
            "WHERE Type IN (8,5);"
        );
    }
    if (mixed_inner_index) {
        exec_sql(
            db, "mixed-inner-index",
            "CREATE INDEX idx_dshadow_emby_latest_mixed_gk_dc "
            "ON MediaItems (coalesce(SeriesPresentationUniqueKey, PresentationUniqueKey), DateCreated DESC, Id, UserDataKeyId) "
            "WHERE Type IN (8,5);"
        );
    }
    if (profile == EMBY_PROFILE_D5B_MIXED_OUTER_MALFORMED) {
        exec_sql(
            db, "dashboard-mixed-outer-mismatch",
            "DROP INDEX idx_dshadow_emby_latest_mixed_dcn_gk;"
            "CREATE INDEX idx_dshadow_emby_latest_mixed_dcn_gk ON MediaItems (DateCreated) WHERE Type IN (8,5);"
        );
    }
    if (profile == EMBY_PROFILE_D5B_MIXED_INNER_MALFORMED) {
        exec_sql(
            db, "dashboard-mixed-inner-mismatch",
            "DROP INDEX idx_dshadow_emby_latest_mixed_gk_dc;"
            "CREATE INDEX idx_dshadow_emby_latest_mixed_gk_dc ON MediaItems (DateCreated) WHERE Type IN (8,5);"
        );
    }
    return SQLITE_OK;
}

static int rsh_apply_emby_d6_profile(
    sqlite3 *db,
    int profile,
    void *suite_ctx
) {
    if (profile == EMBY_PROFILE_D6_COMPLEX_RESUME_SEED) {
        seed_complex_resume_watched_progress_parity_rows(db);
        return SQLITE_OK;
    }
    if (profile == EMBY_PROFILE_D6_LINKS_TWO_LEVEL_SEED) {
        seed_link_type_count_shape_05_rows(db);
        return SQLITE_OK;
    }
    if (profile == EMBY_PROFILE_D6_MIXED_IDENTITY) {
        int rc = rsh_apply_emby_d5b_profile(
            db, EMBY_PROFILE_D5B_MIXED_ALL_INDEXES, suite_ctx
        );
        if (rc != SQLITE_OK) return rc;
        seed_mixed_latest_identity_rows(db);
        return SQLITE_OK;
    }
    return SQLITE_MISUSE;
}

static int rsh_apply_emby_d7_profile(
    sqlite3 *db,
    int profile,
    void *suite_ctx
) {
    (void)suite_ctx;
    if (profile == EMBY_PROFILE_D7_DASHBOARD_INDEXES) {
        exec_sql(
            db, "latest-index",
            "CREATE INDEX idx_dshadow_emby_latest_gk_dc "
            "ON MediaItems (coalesce(SeriesPresentationUniqueKey, PresentationUniqueKey), DateCreated DESC, Id, UserDataKeyId) "
            "WHERE Type = 8;"
        );
        exec_sql(
            db, "latest-episodes-date-index",
            "CREATE INDEX idx_dshadow_emby_latest_episodes_dcn_gk "
            "ON MediaItems ((DateCreated IS NULL), DateCreated DESC, coalesce(SeriesPresentationUniqueKey, PresentationUniqueKey), Id, UserDataKeyId) "
            "WHERE Type = 8;"
        );
        exec_sql(
            db, "movies-outer-index",
            "CREATE INDEX idx_dshadow_emby_latest_movies_dcn_puk "
            "ON MediaItems ((DateCreated IS NULL), DateCreated DESC, PresentationUniqueKey, Id, UserDataKeyId) "
            "WHERE Type = 5;"
        );
        exec_sql(
            db, "movies-inner-index",
            "CREATE INDEX idx_dshadow_emby_latest_movies_puk_dc_cover "
            "ON MediaItems (PresentationUniqueKey, DateCreated, Id, UserDataKeyId) "
            "WHERE Type = 5;"
        );
        return SQLITE_OK;
    }
    if (profile == EMBY_PROFILE_D7_EPISODES_SEMANTIC_SEED) {
        seed_episodes_latest_semantic_rows(db);
        return SQLITE_OK;
    }
    if (profile == EMBY_PROFILE_D7_MOVIES_EXPANDED_SEED) {
        seed_movies_latest_expanded_rows(db);
        return SQLITE_OK;
    }
    if (profile == EMBY_PROFILE_D7_MOVIES_REWRITE_ORDER_SEED) {
        seed_movies_latest_rewrite_order_rows(db);
        return SQLITE_OK;
    }
    return SQLITE_MISUSE;
}

static int rsh_apply_emby_d8_profile(
    sqlite3 *db,
    int profile,
    void *suite_ctx
) {
    int episodes_group = 0;
    int episodes_date = 0;
    int movies_outer = 0;
    int movies_inner = 0;
    int rc;

    if (profile == EMBY_PROFILE_D8_MIXED_ALL_INDEXES_WITH_MOVIES_SEED) {
        rc = rsh_apply_emby_d5b_profile(
            db, EMBY_PROFILE_D5B_MIXED_ALL_INDEXES, suite_ctx
        );
        if (rc != SQLITE_OK) return rc;
        seed_movies_latest_rows(db);
        return SQLITE_OK;
    }
    (void)suite_ctx;
    if (profile == EMBY_PROFILE_D8_EPISODES_GROUP_MALFORMED) {
        episodes_group = 2;
        episodes_date = 1;
    } else if (profile == EMBY_PROFILE_D8_EPISODES_DATE_MALFORMED) {
        episodes_group = 1;
        episodes_date = 2;
    } else if (profile == EMBY_PROFILE_D8_MOVIES_OUTER_MALFORMED) {
        movies_outer = 2;
        movies_inner = 1;
    } else if (profile == EMBY_PROFILE_D8_MOVIES_INNER_MALFORMED) {
        movies_outer = 1;
        movies_inner = 2;
    } else if (profile == EMBY_PROFILE_D8_MOVIES_CANONICAL) {
        movies_outer = 1;
        movies_inner = 1;
    } else if (profile == EMBY_PROFILE_D8_EPISODES_DATE_MISSING) {
        episodes_group = 1;
    } else if (profile == EMBY_PROFILE_D8_EPISODES_GROUP_MISSING) {
        episodes_date = 1;
    } else if (profile != EMBY_PROFILE_D8_EPISODES_BOTH_MISSING) {
        return SQLITE_MISUSE;
    }

    if (episodes_group == 1) {
        exec_sql(
            db, "latest-index",
            "CREATE INDEX idx_dshadow_emby_latest_gk_dc "
            "ON MediaItems (coalesce(SeriesPresentationUniqueKey, PresentationUniqueKey), DateCreated DESC, Id, UserDataKeyId) "
            "WHERE Type = 8;"
        );
    }
    if (episodes_date == 1) {
        exec_sql(
            db, "latest-episodes-date-index",
            "CREATE INDEX idx_dshadow_emby_latest_episodes_dcn_gk "
            "ON MediaItems ((DateCreated IS NULL), DateCreated DESC, coalesce(SeriesPresentationUniqueKey, PresentationUniqueKey), Id, UserDataKeyId) "
            "WHERE Type = 8;"
        );
    } else if (episodes_date == 2) {
        exec_sql(
            db, "malformed-episodes-date-index",
            "CREATE INDEX idx_dshadow_emby_latest_episodes_dcn_gk "
            "ON MediaItems (Id) WHERE Type = 8;"
        );
    }
    if (episodes_group == 2) {
        exec_sql(
            db, "malformed-episodes-group-index",
            "CREATE INDEX idx_dshadow_emby_latest_gk_dc "
            "ON MediaItems (Id) WHERE Type = 8;"
        );
    }
    if (movies_outer == 1) {
        exec_sql(
            db, "movies-outer-index",
            "CREATE INDEX idx_dshadow_emby_latest_movies_dcn_puk "
            "ON MediaItems ((DateCreated IS NULL), DateCreated DESC, PresentationUniqueKey, Id, UserDataKeyId) "
            "WHERE Type = 5;"
        );
    }
    if (movies_inner == 1) {
        exec_sql(
            db, "movies-inner-index",
            "CREATE INDEX idx_dshadow_emby_latest_movies_puk_dc_cover "
            "ON MediaItems (PresentationUniqueKey, DateCreated, Id, UserDataKeyId) "
            "WHERE Type = 5;"
        );
    } else if (movies_inner == 2) {
        exec_sql(
            db, "malformed-movies-inner-index",
            "CREATE INDEX idx_dshadow_emby_latest_movies_puk_dc_cover "
            "ON MediaItems (Id) WHERE Type = 5;"
        );
    }
    if (movies_outer == 2) {
        exec_sql(
            db, "malformed-movies-outer-index",
            "CREATE INDEX idx_dshadow_emby_latest_movies_dcn_puk "
            "ON MediaItems (Id) WHERE Type = 5;"
        );
    }
    return SQLITE_OK;
}

static rsh_apply_profile_fn rsh_resolve_native_setup_profile(
    int profile,
    void *suite_ctx
) {
    if (profile == EMBY_PROFILE_DASHBOARD_OFF) {
        return rsh_apply_emby_dashboard_off_profile;
    }
    if (profile == EMBY_PROFILE_LATEST_INDEXES) {
        return rsh_apply_emby_latest_indexes_profile;
    }
    if (profile == EMBY_PROFILE_FIXTURE_CANARY) {
        return rsh_apply_emby_fixture_canary_profile;
    }
    if (profile >= EMBY_PROFILE_MOVIES_SEED_ONLY &&
        profile <= EMBY_PROFILE_MOVIES_INNER_INDEX_MISSING) {
        return rsh_apply_emby_dashboard_matrix_profile;
    }
    if (profile >= EMBY_PROFILE_D5B_FIX_CANDIDATE &&
        profile <= EMBY_PROFILE_D5B_MIXED_INNER_MALFORMED) {
        return rsh_apply_emby_d5b_profile;
    }
    if (profile >= EMBY_PROFILE_D6_COMPLEX_RESUME_SEED &&
        profile <= EMBY_PROFILE_D6_MIXED_IDENTITY) {
        return rsh_apply_emby_d6_profile;
    }
    if (profile >= EMBY_PROFILE_D7_DASHBOARD_INDEXES &&
        profile <= EMBY_PROFILE_D7_MOVIES_REWRITE_ORDER_SEED) {
        return rsh_apply_emby_d7_profile;
    }
    if (profile >= EMBY_PROFILE_D8_EPISODES_GROUP_MALFORMED &&
        profile <= EMBY_PROFILE_D8_MIXED_ALL_INDEXES_WITH_MOVIES_SEED) {
        return rsh_apply_emby_d8_profile;
    }
    return rsh_resolve_setup_profile(profile, suite_ctx);
}

static const char emby_fts_env_sql[] =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) ),"
    "WithItemLinkItemIds AS (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (6) union select ItemLinks2TwoLevel.LinkedId from ItemLinks2 ItemLinks2TwoLevel where itemid in (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (7)))"
    "select count(*) OVER() AS TotalRecordCount,A.type,"
    "(Select ShareLevel from UserItemShares join AncestorIds2 on AncestorIds2.AncestorId=UserItemShares.ItemId where UserItemShares.UserId=1 and UserItemShares.ShareLevel not null and AncestorIds2.ItemId=A.Id order by Distance limit 1) as ShareLevel from"
    " mediaitems A join fts_search9 on A.Id=fts_search9.RowId and fts_search9 match @SearchTerm where "
    "A.Type in (1,2,5,6,8,9,10,11,12,13,14,15,16,18,19,20,21,22,23,24,25,26,29,34) "
    "AND (Coalesce(ShareLevel, 0) > 0 OR A.Type in (1,2,5,6,8,9,10,11,12,13,14,15,18,19,20,21,22,23,24,25,26,29,34) OR A.IsPublic=1) "
    "AND A.ExtraType is null AND "
    "(A.Id in WithAncestors OR A.Id in (select ListItemsExemptionForPlaylists.ListId from ListItems ListItemsExemptionForPlaylists join ancestorids2 AncestorIdExemptionForPlaylists on ListItemsExemptionForPlaylists.ListItemId=AncestorIdExemptionForPlaylists.itemid and AncestorIdExemptionForPlaylists.AncestorId in (100)) OR A.Id in (select PersonId from itemPeople2 where ItemId in WithAncestors) OR A.Id in WithItemLinkItemIds) "
    "Group by A.Type ORDER BY Rank ASC LIMIT 50";

static const char emby_fts_env_rewritten_sql[] =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) ),"
    "WithItemLinkItemIds AS (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (6) union select ItemLinks2TwoLevel.LinkedId from ItemLinks2 ItemLinks2TwoLevel where itemid in (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (7)))"
    "select count(*) OVER() AS TotalRecordCount,A.type,"
    "(Select ShareLevel from UserItemShares join AncestorIds2 on AncestorIds2.AncestorId=UserItemShares.ItemId where UserItemShares.UserId=1 and UserItemShares.ShareLevel not null and AncestorIds2.ItemId=A.Id order by Distance limit 1) as ShareLevel from"
    " mediaitems A join fts_search9 on A.Id=fts_search9.RowId and fts_search9 match dshadow_emby_fts_rewrite(@SearchTerm) where "
    "A.Type in (1,2,5,6,8,9,10,11,12,13,14,15,16,18,19,20,21,22,23,24,25,26,29,34) "
    "AND (Coalesce(ShareLevel, 0) > 0 OR A.Type in (1,2,5,6,8,9,10,11,12,13,14,15,18,19,20,21,22,23,24,25,26,29,34) OR A.IsPublic=1) "
    "AND A.ExtraType is null AND "
    "(EXISTS (SELECT 1 FROM AncestorIds2 WHERE AncestorIds2.itemid = A.Id AND AncestorIds2.AncestorId in (100))"
    " OR  exists (select 1 from ListItems join ancestorids2 on ListItems.ListItemId=ancestorids2.itemid and ancestorids2.AncestorId in (100) where ListItems.ListId=A.Id)"
    " OR EXISTS (SELECT 1 FROM itemPeople2 JOIN AncestorIds2 ON AncestorIds2.itemid = itemPeople2.ItemId WHERE itemPeople2.PersonId = A.Id and AncestorIds2.AncestorId in (100))"
    " OR Exists (select 1 From ItemLinks2 join ancestorids2 on ancestorids2.itemid=itemlinks2.itemid and ancestorids2.ancestorid in (100)  where ItemLinks2.LinkedId = A.Id AND ItemLinks2.Type in (6))"
    " OR Exists (select 1 from ItemLinks2 ItemLinks2TwoLevel where exists (select 1 From ItemLinks2 join ancestorids2 on ancestorids2.itemid=itemlinks2.itemid and ancestorids2.ancestorid in (100)  where itemlinks2.linkedid = itemlinks2twolevel.itemid AND ItemLinks2.Type in (7)) and ItemLinks2TwoLevel.LinkedId=A.Id)) "
    "Group by A.Type ORDER BY Rank ASC LIMIT 50";

static const char emby_fanout_env_sql[] =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) ),"
    "WithItemLinkItemIds AS (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type=6 union select ItemLinks2TwoLevel.LinkedId from ItemLinks2 ItemLinks2TwoLevel where itemid in (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (7)))"
    "select A.Id,A.SortName from mediaitems A where A.Id in WithItemLinkItemIds ORDER BY A.SortName collate NATURALSORT ASC LIMIT 12";

static const char emby_fanout_env_rewritten_sql[] =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) ),"
    "WithItemLinkItemIds AS (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type=6 union select ItemLinks2TwoLevel.LinkedId from ItemLinks2 ItemLinks2TwoLevel where itemid in (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (7)))"
    "select A.Id,A.SortName from mediaitems A where (Exists (select 1 From ItemLinks2 join ancestorids2 on ancestorids2.itemid=itemlinks2.itemid and ancestorids2.ancestorid in (100)  where ItemLinks2.LinkedId = A.Id AND ItemLinks2.Type in (6)) OR Exists (select 1 from ItemLinks2 ItemLinks2TwoLevel where exists (select 1 From ItemLinks2 join ancestorids2 on ancestorids2.itemid=itemlinks2.itemid and ancestorids2.ancestorid in (100)  where itemlinks2.linkedid = itemlinks2twolevel.itemid AND ItemLinks2.Type in (7)) and ItemLinks2TwoLevel.LinkedId=A.Id)) ORDER BY A.SortName collate NATURALSORT ASC LIMIT 12";

static const char emby_dashboard_episodes_off_sql[] =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) )"
    "select A.Id,A.SeriesName,A.SortName from mediaitems A left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=42 "
    "where A.Type=8 AND Coalesce(UserDatas.played, 0)=0 AND A.Id in WithAncestors Group by coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey) ORDER BY MAX(A.DateCreated) DESC LIMIT 12";

static const char emby_dashboard_movies_off_sql[] =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) )"
    "select A.Id,A.Name,A.Path,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.Images,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex from mediaitems A left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=42 "
    "where A.Type=5 AND Coalesce(UserDatas.played, 0)=0 AND A.Id in WithAncestors Group by A.PresentationUniqueKey ORDER BY A.DateCreated DESC LIMIT 12";

static const char emby_dashboard_mixed_off_sql[] =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) )"
    "select A.type,A.Id,A.IndexNumber,A.Name,A.ParentIndexNumber,A.RunTimeTicks,A.ParentId,A.SeriesName,A.AlbumId,A.SeriesId,A.Images,A.SortIndexNumber,A.SortParentIndexNumber,A.IndexNumberEnd,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex from mediaitems A left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=42 "
    "where A.Type in (8,5) AND Coalesce(UserDatas.played, 0)=0 AND A.Id in WithAncestors Group by coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey) ORDER BY MAX(A.DateCreated) DESC LIMIT 3";

static const char *const emby_fts_env_bind_names[] = {
    "@SearchTerm"
};

#define EMBY_PREPARE_V2 \
    {.kind = RSH_PREPARE_V2, \
     .nbyte_kind = RSH_NBYTE_MINUS_ONE, \
     .tail_kind = RSH_TAIL_FULL}
#define EMBY_SQL_CASE(case_label, source_sql, saved_sql) \
    { \
        .label = (case_label), \
        .kind = RSH_CASE_SQL_EXACT, \
        .build_mask = RSH_BUILD_EMBY_LINKED, \
        .data.sql_exact = { \
            .role = "candidate", \
            .sql = (source_sql), \
            .expected_sql = (saved_sql), \
            .prepare = EMBY_PREPARE_V2 \
        } \
    }
#define EMBY_FTS_SQL_CASE(case_label, saved_sql) \
    { \
        .label = (case_label), \
        .kind = RSH_CASE_SQL_EXACT, \
        .build_mask = RSH_BUILD_EMBY_LINKED, \
        .data.sql_exact = { \
            .role = "candidate", \
            .sql = emby_fts_env_sql, \
            .expected_sql = (saved_sql), \
            .prepare = EMBY_PREPARE_V2, \
            .check_bind_count = 1, \
            .expected_bind_count = 1, \
            .expected_bind_names = emby_fts_env_bind_names, \
            .expected_bind_name_count = sizeof(emby_fts_env_bind_names) / \
                                        sizeof(emby_fts_env_bind_names[0]) \
        } \
    }
#define DEFINE_EMBY_SCALAR_PHASE(name, phase_label, db_rows, case_rows) \
    static const rsh_phase_spec name[] = { \
        { \
            .label = (phase_label), \
            .dbs = (db_rows), \
            .db_count = sizeof(db_rows) / sizeof((db_rows)[0]), \
            .cases = (case_rows), \
            .case_count = sizeof(case_rows) / sizeof((case_rows)[0]) \
        } \
    }

static const rsh_db_spec emby_env_candidate_db[] = {
    {
        .role = "candidate",
        .relative_path = "library.db",
        .kind = RSH_DB_CANDIDATE,
        .storage = RSH_DB_RELATIVE
    }
};

static const rsh_db_spec emby_dashboard_off_candidate_db[] = {
    {
        .role = "candidate",
        .relative_path = "library.db",
        .kind = RSH_DB_CANDIDATE,
        .storage = RSH_DB_RELATIVE,
        .setup_profile = EMBY_PROFILE_DASHBOARD_OFF
    }
};

#define EMBY_POSITIVE_CTE \
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) )," \
    "WithItemLinkItemIds AS (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (6) union select ItemLinks2TwoLevel.LinkedId from ItemLinks2 ItemLinks2TwoLevel where itemid in (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (7)))"
#define EMBY_POSITIVE_TYPE_SELECT \
    "select count(*) OVER() AS TotalRecordCount,A.type," \
    "(Select ShareLevel from UserItemShares join AncestorIds2 on AncestorIds2.AncestorId=UserItemShares.ItemId where UserItemShares.UserId=1 and UserItemShares.ShareLevel not null and AncestorIds2.ItemId=A.Id order by Distance limit 1) as ShareLevel from"
#define EMBY_POSITIVE_PRESENTATION_SELECT \
    "select count(*) OVER() AS TotalRecordCount,A.type,A.Id,A.EndDate,A.IndexNumber,A.Name,A.Path,A.ParentIndexNumber,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.SeriesName,A.AlbumId,A.SeriesId,A.PresentationUniqueKey,A.Images,A.Status,A.SortIndexNumber,A.SortParentIndexNumber,A.IndexNumberEnd,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex," \
    "(Select ShareLevel from UserItemShares join AncestorIds2 on AncestorIds2.AncestorId=UserItemShares.ItemId where UserItemShares.UserId=1 and UserItemShares.ShareLevel not null and AncestorIds2.ItemId=A.Id order by Distance limit 1) as ShareLevel from"
#define EMBY_POSITIVE_FROM \
    " mediaitems A join fts_search9 on A.Id=fts_search9.RowId and fts_search9 match "
#define EMBY_POSITIVE_WHERE \
    " where A.Type in (1,2,5,6,8,9,10,11,12,13,14,15,16,18,19,20,21,22,23,24,25,26,29,34) " \
    "AND (Coalesce(ShareLevel, 0) > 0 OR A.Type in (1,2,5,6,8,9,10,11,12,13,14,15,18,19,20,21,22,23,24,25,26,29,34) OR A.IsPublic=1) " \
    "AND A.ExtraType is null AND "
#define EMBY_POSITIVE_ORIGINAL_MEMBERSHIP \
    "(A.Id in WithAncestors OR A.Id in (select ListItemsExemptionForPlaylists.ListId from ListItems ListItemsExemptionForPlaylists join ancestorids2 AncestorIdExemptionForPlaylists on ListItemsExemptionForPlaylists.ListItemId=AncestorIdExemptionForPlaylists.itemid and AncestorIdExemptionForPlaylists.AncestorId in (100)) OR A.Id in (select PersonId from itemPeople2 where ItemId in WithAncestors) OR A.Id in WithItemLinkItemIds)"
#define EMBY_POSITIVE_REWRITTEN_MEMBERSHIP \
    "(EXISTS (SELECT 1 FROM AncestorIds2 WHERE AncestorIds2.itemid = A.Id AND AncestorIds2.AncestorId in (100))" \
    " OR  exists (select 1 from ListItems join ancestorids2 on ListItems.ListItemId=ancestorids2.itemid and ancestorids2.AncestorId in (100) where ListItems.ListId=A.Id)" \
    " OR EXISTS (SELECT 1 FROM itemPeople2 JOIN AncestorIds2 ON AncestorIds2.itemid = itemPeople2.ItemId WHERE itemPeople2.PersonId = A.Id and AncestorIds2.AncestorId in (100))" \
    " OR Exists (select 1 From ItemLinks2 join ancestorids2 on ancestorids2.itemid=itemlinks2.itemid and ancestorids2.ancestorid in (100)  where ItemLinks2.LinkedId = A.Id AND ItemLinks2.Type in (6))" \
    " OR Exists (select 1 from ItemLinks2 ItemLinks2TwoLevel where exists (select 1 From ItemLinks2 join ancestorids2 on ancestorids2.itemid=itemlinks2.itemid and ancestorids2.ancestorid in (100)  where itemlinks2.linkedid = itemlinks2twolevel.itemid AND ItemLinks2.Type in (7)) and ItemLinks2TwoLevel.LinkedId=A.Id))"

static const char emby_positive_numbered_sql[] =
    EMBY_POSITIVE_CTE EMBY_POSITIVE_TYPE_SELECT EMBY_POSITIVE_FROM "?1"
    EMBY_POSITIVE_WHERE EMBY_POSITIVE_ORIGINAL_MEMBERSHIP
    " Group by A.Type ORDER BY Rank ASC LIMIT 50";

static const char emby_positive_numbered_expected_sql[] =
    EMBY_POSITIVE_CTE EMBY_POSITIVE_TYPE_SELECT EMBY_POSITIVE_FROM
    "dshadow_emby_fts_rewrite(?1)" EMBY_POSITIVE_WHERE
    EMBY_POSITIVE_REWRITTEN_MEMBERSHIP
    " Group by A.Type ORDER BY Rank ASC LIMIT 50";

static const char emby_positive_presentation_sql[] =
    EMBY_POSITIVE_CTE EMBY_POSITIVE_PRESENTATION_SELECT EMBY_POSITIVE_FROM
    "@SearchTerm left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=42"
    EMBY_POSITIVE_WHERE EMBY_POSITIVE_ORIGINAL_MEMBERSHIP
    " Group by A.PresentationUniqueKey ORDER BY Rank ASC LIMIT 50";

static const char emby_positive_presentation_expected_sql[] =
    EMBY_POSITIVE_CTE EMBY_POSITIVE_PRESENTATION_SELECT EMBY_POSITIVE_FROM
    "dshadow_emby_fts_rewrite(@SearchTerm) left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=42"
    EMBY_POSITIVE_WHERE EMBY_POSITIVE_REWRITTEN_MEMBERSHIP
    " Group by A.PresentationUniqueKey ORDER BY Rank ASC LIMIT 50";

#define EMBY_D6_ROW_PARITY_SOURCE \
    EMBY_POSITIVE_CTE EMBY_POSITIVE_PRESENTATION_SELECT EMBY_POSITIVE_FROM \
    "@SearchTerm left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=1" \
    EMBY_POSITIVE_WHERE EMBY_POSITIVE_ORIGINAL_MEMBERSHIP \
    " Group by A.PresentationUniqueKey ORDER BY Rank ASC LIMIT 50"
#define EMBY_D6_ROW_PARITY_EXPECTED \
    EMBY_POSITIVE_CTE EMBY_POSITIVE_PRESENTATION_SELECT EMBY_POSITIVE_FROM \
    "dshadow_emby_fts_rewrite(@SearchTerm) left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=1" \
    EMBY_POSITIVE_WHERE EMBY_POSITIVE_REWRITTEN_MEMBERSHIP \
    " Group by A.PresentationUniqueKey ORDER BY Rank ASC LIMIT 50"

static const char emby_d6_row_parity_sql[] =
    EMBY_D6_ROW_PARITY_SOURCE;
static const char emby_d6_row_parity_expected_sql[] =
    EMBY_D6_ROW_PARITY_EXPECTED;
static const char emby_d6_row_parity_ordered_sql[] =
    "SELECT * FROM (" EMBY_D6_ROW_PARITY_SOURCE ") ORDER BY 3";
static const char emby_d6_row_parity_ordered_expected_sql[] =
    "SELECT * FROM (" EMBY_D6_ROW_PARITY_EXPECTED ") ORDER BY 3";

#undef EMBY_D6_ROW_PARITY_EXPECTED
#undef EMBY_D6_ROW_PARITY_SOURCE

#ifdef EMBY_FTS_REWRITE_TEST_LITERAL_MATCH
static const char emby_positive_literal_sql[] =
    EMBY_POSITIVE_CTE EMBY_POSITIVE_TYPE_SELECT EMBY_POSITIVE_FROM
    "'(\"alpha\"*) OR (\"direct\"*)'" EMBY_POSITIVE_WHERE
    EMBY_POSITIVE_ORIGINAL_MEMBERSHIP
    " Group by A.Type ORDER BY Rank ASC LIMIT 50";

static const char emby_positive_literal_expected_sql[] =
    EMBY_POSITIVE_CTE EMBY_POSITIVE_TYPE_SELECT EMBY_POSITIVE_FROM
    "dshadow_emby_fts_rewrite('(\"alpha\"*) OR (\"direct\"*)')"
    EMBY_POSITIVE_WHERE EMBY_POSITIVE_REWRITTEN_MEMBERSHIP
    " Group by A.Type ORDER BY Rank ASC LIMIT 50";
#endif

#define EMBY_NONMATCH_SQL(match_rhs, membership) \
    EMBY_POSITIVE_CTE EMBY_POSITIVE_TYPE_SELECT EMBY_POSITIVE_FROM \
    match_rhs EMBY_POSITIVE_WHERE membership \
    " Group by A.Type ORDER BY Rank ASC LIMIT 50"
#define EMBY_NONMATCH_FAST_MEMBERSHIP \
    "(EXISTS (SELECT 1 FROM AncestorIds2 WHERE AncestorIds2.itemid=A.Id)" \
    " OR A.Id in (select ListItemsExemptionForPlaylists.ListId from ListItems ListItemsExemptionForPlaylists join ancestorids2 AncestorIdExemptionForPlaylists on ListItemsExemptionForPlaylists.ListItemId=AncestorIdExemptionForPlaylists.itemid and AncestorIdExemptionForPlaylists.AncestorId in (100))" \
    " OR A.Id in (select PersonId from itemPeople2 where ItemId in WithAncestors)" \
    " OR A.Id in WithItemLinkItemIds)"

static const char emby_nonmatch_fast_sql[] =
    EMBY_NONMATCH_SQL("@SearchTerm", EMBY_NONMATCH_FAST_MEMBERSHIP);
static const char emby_nonmatch_duplicate_fts_sql[] =
    EMBY_NONMATCH_SQL(
        "@SearchTerm AND fts_search9 match @SearchTerm2",
        EMBY_POSITIVE_ORIGINAL_MEMBERSHIP
    );
#ifndef EMBY_FTS_REWRITE_TEST_LITERAL_MATCH
static const char emby_nonmatch_literal_rhs_sql[] =
    EMBY_NONMATCH_SQL("'alpha'", EMBY_POSITIVE_ORIGINAL_MEMBERSHIP);
#endif
static const char emby_nonmatch_concat_rhs_sql[] =
    EMBY_NONMATCH_SQL("@SearchTerm || ''", EMBY_POSITIVE_ORIGINAL_MEMBERSHIP);
static const char emby_nonmatch_named_colon_rhs_sql[] =
    EMBY_NONMATCH_SQL(
        "$SearchTerm::suffix", EMBY_POSITIVE_ORIGINAL_MEMBERSHIP
    );
static const char emby_nonmatch_named_func_rhs_sql[] =
    EMBY_NONMATCH_SQL("$SearchTerm(extra)", EMBY_POSITIVE_ORIGINAL_MEMBERSHIP);

#define EMBY_NONMATCH_ONES_1 "1,"
#define EMBY_NONMATCH_ONES_2 \
    EMBY_NONMATCH_ONES_1 EMBY_NONMATCH_ONES_1
#define EMBY_NONMATCH_ONES_4 \
    EMBY_NONMATCH_ONES_2 EMBY_NONMATCH_ONES_2
#define EMBY_NONMATCH_ONES_8 \
    EMBY_NONMATCH_ONES_4 EMBY_NONMATCH_ONES_4
#define EMBY_NONMATCH_ONES_16 \
    EMBY_NONMATCH_ONES_8 EMBY_NONMATCH_ONES_8
#define EMBY_NONMATCH_ONES_32 \
    EMBY_NONMATCH_ONES_16 EMBY_NONMATCH_ONES_16
#define EMBY_NONMATCH_ONES_64 \
    EMBY_NONMATCH_ONES_32 EMBY_NONMATCH_ONES_32
#define EMBY_NONMATCH_ONES_128 \
    EMBY_NONMATCH_ONES_64 EMBY_NONMATCH_ONES_64
#define EMBY_NONMATCH_ONES_256 \
    EMBY_NONMATCH_ONES_128 EMBY_NONMATCH_ONES_128
#define EMBY_NONMATCH_ONES_512 \
    EMBY_NONMATCH_ONES_256 EMBY_NONMATCH_ONES_256
#define EMBY_NONMATCH_ONES_1024 \
    EMBY_NONMATCH_ONES_512 EMBY_NONMATCH_ONES_512
#define EMBY_NONMATCH_ONES_2048 \
    EMBY_NONMATCH_ONES_1024 EMBY_NONMATCH_ONES_1024
#define EMBY_NONMATCH_ONES_4096 \
    EMBY_NONMATCH_ONES_2048 EMBY_NONMATCH_ONES_2048
#define EMBY_NONMATCH_ONES_8192 \
    EMBY_NONMATCH_ONES_4096 EMBY_NONMATCH_ONES_4096
#define EMBY_NONMATCH_ONES_16384 \
    EMBY_NONMATCH_ONES_8192 EMBY_NONMATCH_ONES_8192
#define EMBY_NONMATCH_ONES_32768 \
    EMBY_NONMATCH_ONES_16384 EMBY_NONMATCH_ONES_16384
#define EMBY_NONMATCH_OVER_CAP_LIST \
    EMBY_NONMATCH_ONES_32768 EMBY_NONMATCH_ONES_128 \
    EMBY_NONMATCH_ONES_64 EMBY_NONMATCH_ONES_32 EMBY_NONMATCH_ONES_4 \
    EMBY_NONMATCH_ONES_2 EMBY_NONMATCH_ONES_1 "1"

static const char emby_nonmatch_over_cap_sql[] =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in ("
    EMBY_NONMATCH_OVER_CAP_LIST
    ") ),"
    "WithItemLinkItemIds AS (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (6) union select ItemLinks2TwoLevel.LinkedId from ItemLinks2 ItemLinks2TwoLevel where itemid in (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (7)))"
    EMBY_POSITIVE_TYPE_SELECT EMBY_POSITIVE_FROM "@SearchTerm"
    EMBY_POSITIVE_WHERE
    "(A.Id in WithAncestors OR A.Id in (select ListItemsExemptionForPlaylists.ListId from ListItems ListItemsExemptionForPlaylists join ancestorids2 AncestorIdExemptionForPlaylists on ListItemsExemptionForPlaylists.ListItemId=AncestorIdExemptionForPlaylists.itemid and AncestorIdExemptionForPlaylists.AncestorId in ("
    EMBY_NONMATCH_OVER_CAP_LIST
    ")) OR A.Id in (select PersonId from itemPeople2 where ItemId in WithAncestors) OR A.Id in WithItemLinkItemIds) "
    "Group by A.Type ORDER BY Rank ASC LIMIT 50";
static const char emby_nonmatch_over_cap_discriminator[] =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (1,1,";

#undef EMBY_NONMATCH_OVER_CAP_LIST
#undef EMBY_NONMATCH_ONES_32768
#undef EMBY_NONMATCH_ONES_16384
#undef EMBY_NONMATCH_ONES_8192
#undef EMBY_NONMATCH_ONES_4096
#undef EMBY_NONMATCH_ONES_2048
#undef EMBY_NONMATCH_ONES_1024
#undef EMBY_NONMATCH_ONES_512
#undef EMBY_NONMATCH_ONES_256
#undef EMBY_NONMATCH_ONES_128
#undef EMBY_NONMATCH_ONES_64
#undef EMBY_NONMATCH_ONES_32
#undef EMBY_NONMATCH_ONES_16
#undef EMBY_NONMATCH_ONES_8
#undef EMBY_NONMATCH_ONES_4
#undef EMBY_NONMATCH_ONES_2
#undef EMBY_NONMATCH_ONES_1

#define EMBY_NONMATCH_AFTER_L1 \
    ") ),WithItemLinkItemIds AS (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in ("
#define EMBY_NONMATCH_AFTER_T1 \
    ") union select ItemLinks2TwoLevel.LinkedId from ItemLinks2 ItemLinks2TwoLevel where itemid in (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in ("
#define EMBY_NONMATCH_AFTER_T2 ")))select"
#define EMBY_NONMATCH_AFTER_L2 \
    ")) OR A.Id in (select PersonId from itemPeople2 where ItemId in WithAncestors) OR A.Id in WithItemLinkItemIds)"
#define EMBY_NONMATCH_AMBIGUOUS_SQL(match_rhs, membership, anchor) \
    EMBY_POSITIVE_CTE EMBY_POSITIVE_TYPE_SELECT EMBY_POSITIVE_FROM \
    match_rhs EMBY_POSITIVE_WHERE membership \
    " AND '" anchor "'='" anchor "' Group by A.Type ORDER BY Rank ASC LIMIT 50"

static const char emby_nonmatch_ambiguous_after_l1_sql[] =
    EMBY_NONMATCH_AMBIGUOUS_SQL(
        "@SearchTerm", EMBY_POSITIVE_ORIGINAL_MEMBERSHIP,
        EMBY_NONMATCH_AFTER_L1
    );
static const char emby_nonmatch_ambiguous_after_l1_expected_sql[] =
    EMBY_NONMATCH_AMBIGUOUS_SQL(
        "dshadow_emby_fts_rewrite(@SearchTerm)",
        EMBY_POSITIVE_REWRITTEN_MEMBERSHIP, EMBY_NONMATCH_AFTER_L1
    );
static const char emby_nonmatch_ambiguous_after_t1_sql[] =
    EMBY_NONMATCH_AMBIGUOUS_SQL(
        "@SearchTerm", EMBY_POSITIVE_ORIGINAL_MEMBERSHIP,
        EMBY_NONMATCH_AFTER_T1
    );
static const char emby_nonmatch_ambiguous_after_t1_expected_sql[] =
    EMBY_NONMATCH_AMBIGUOUS_SQL(
        "dshadow_emby_fts_rewrite(@SearchTerm)",
        EMBY_POSITIVE_REWRITTEN_MEMBERSHIP, EMBY_NONMATCH_AFTER_T1
    );
static const char emby_nonmatch_ambiguous_after_t2_sql[] =
    EMBY_NONMATCH_AMBIGUOUS_SQL(
        "@SearchTerm", EMBY_POSITIVE_ORIGINAL_MEMBERSHIP,
        EMBY_NONMATCH_AFTER_T2
    );
static const char emby_nonmatch_ambiguous_after_t2_expected_sql[] =
    EMBY_NONMATCH_AMBIGUOUS_SQL(
        "dshadow_emby_fts_rewrite(@SearchTerm)",
        EMBY_POSITIVE_REWRITTEN_MEMBERSHIP, EMBY_NONMATCH_AFTER_T2
    );
static const char emby_nonmatch_ambiguous_after_l2_sql[] =
    EMBY_NONMATCH_AMBIGUOUS_SQL(
        "@SearchTerm", EMBY_POSITIVE_ORIGINAL_MEMBERSHIP,
        EMBY_NONMATCH_AFTER_L2
    );
static const char emby_nonmatch_ambiguous_after_l2_expected_sql[] =
    EMBY_NONMATCH_AMBIGUOUS_SQL(
        "dshadow_emby_fts_rewrite(@SearchTerm)",
        EMBY_POSITIVE_REWRITTEN_MEMBERSHIP, EMBY_NONMATCH_AFTER_L2
    );

#undef EMBY_NONMATCH_AMBIGUOUS_SQL
#undef EMBY_NONMATCH_AFTER_L2
#undef EMBY_NONMATCH_AFTER_T2
#undef EMBY_NONMATCH_AFTER_T1
#undef EMBY_NONMATCH_AFTER_L1
#undef EMBY_NONMATCH_FAST_MEMBERSHIP
#undef EMBY_NONMATCH_SQL

static const char emby_nonmatch_semicolon_sql[] = "SELECT 1; SELECT 2";
static const char emby_nonmatch_semicolon_saved_sql[] = "SELECT 1;";
static const char emby_nonmatch_embedded_nul_sql[] = "SELECT 1\0 SELECT 2";
static const char emby_nonmatch_embedded_nul_saved_sql[] = "SELECT 1";

#undef EMBY_POSITIVE_REWRITTEN_MEMBERSHIP
#undef EMBY_POSITIVE_ORIGINAL_MEMBERSHIP
#undef EMBY_POSITIVE_WHERE
#undef EMBY_POSITIVE_FROM
#undef EMBY_POSITIVE_PRESENTATION_SELECT
#undef EMBY_POSITIVE_TYPE_SELECT
#undef EMBY_POSITIVE_CTE

static const char emby_positive_scalar_sql[] =
    "SELECT dshadow_emby_fts_rewrite(?)";
static const unsigned char emby_positive_scalar_blob[] = {0x01, 0x02, 0x03};

static int rsh_bind_emby_positive_scalar_text(
    sqlite3_stmt *stmt,
    const char *label,
    void *ctx
) {
    (void)label;
    return sqlite3_bind_text(stmt, 1, (const char *)ctx, -1, SQLITE_STATIC);
}

static int rsh_bind_emby_positive_scalar_null(
    sqlite3_stmt *stmt,
    const char *label,
    void *ctx
) {
    (void)label;
    (void)ctx;
    return sqlite3_bind_null(stmt, 1);
}

static int rsh_bind_emby_positive_scalar_integer(
    sqlite3_stmt *stmt,
    const char *label,
    void *ctx
) {
    (void)label;
    (void)ctx;
    return sqlite3_bind_int(stmt, 1, 42);
}

static int rsh_bind_emby_positive_scalar_blob(
    sqlite3_stmt *stmt,
    const char *label,
    void *ctx
) {
    (void)label;
    (void)ctx;
    return sqlite3_bind_blob(
        stmt, 1, emby_positive_scalar_blob,
        (int)sizeof(emby_positive_scalar_blob), SQLITE_STATIC
    );
}

#ifdef EMBY_FTS_REWRITE_TEST_API
static void rsh_reset_emby_positive_scalar_counter(void *ctx) {
    (void)ctx;
    emby_fts_rewrite_test_reset_scalar_calls();
}

static int rsh_read_emby_positive_scalar_counter(void *ctx) {
    (void)ctx;
    return emby_fts_rewrite_test_scalar_calls() > 0 ? 1 : 0;
}
#endif

static const char *const emby_positive_numbered_bind_names[] = {
    "?1"
};

#define EMBY_POSITIVE_SQL_CASE( \
    case_label, source_sql, saved_sql, prepare_kind, nbyte_form, tail_form, \
    bind_names, column_count) \
    { \
        .label = (case_label), \
        .kind = RSH_CASE_SQL_EXACT, \
        .build_mask = RSH_BUILD_EMBY_LINKED, \
        .data.sql_exact = { \
            .role = "candidate", \
            .sql = (source_sql), \
            .expected_sql = (saved_sql), \
            .prepare = { \
                .kind = (prepare_kind), \
                .nbyte_kind = (nbyte_form), \
                .tail_kind = (tail_form) \
            }, \
            .check_bind_count = 1, \
            .expected_bind_count = 1, \
            .expected_bind_names = (bind_names), \
            .expected_bind_name_count = sizeof(bind_names) / \
                                        sizeof((bind_names)[0]), \
            .check_column_count = 1, \
            .expected_column_count = (column_count) \
        } \
    }

#define EMBY_POSITIVE_SCALAR_PREPARE \
    {.kind = RSH_PREPARE_V2, \
     .nbyte_kind = RSH_NBYTE_MINUS_ONE, \
     .tail_kind = RSH_TAIL_NULL_OUT}

#define EMBY_POSITIVE_SCALAR_TEXT_CASE(case_label, input_text, expected_text) \
    { \
        .label = (case_label), \
        .kind = RSH_CASE_SCALAR, \
        .build_mask = RSH_BUILD_EMBY_LINKED, \
        .data.scalar = { \
            .role = "candidate", \
            .sql = emby_positive_scalar_sql, \
            .expected_sql = emby_positive_scalar_sql, \
            .prepare = EMBY_POSITIVE_SCALAR_PREPARE, \
            .bind = rsh_bind_emby_positive_scalar_text, \
            .bind_ctx = (void *)(input_text), \
            .value_kind = RSH_SCALAR_TEXT, \
            .bytes_value = (expected_text), \
            .bytes_count = (int)sizeof(expected_text) - 1 \
        } \
    }

static const rsh_case_spec emby_positive_type_legacy_assertion =
    EMBY_POSITIVE_SQL_CASE(
        "type-legacy", emby_fts_env_sql, emby_fts_env_rewritten_sql,
        RSH_PREPARE_LEGACY, RSH_NBYTE_MINUS_ONE, RSH_TAIL_FULL,
        emby_fts_env_bind_names, 3
    );

static int rsh_custom_adapter_emby_positive_type_legacy_authorizer(
    const rsh_case_context *context,
    const void *immutable_data
) {
    const rsh_case_spec *assertion =
        (const rsh_case_spec *)immutable_data;
    const rsh_db_handle *handle = rsh_require_case_db(
        &emby_suite_spec, assertion, context, assertion->data.sql_exact.role
    );
    emby_auth_probe probe = {0, 0};

    require_int(
        "type-legacy/authorizer-set",
        sqlite3_set_authorizer(handle->db, scalar_authorizer_cb, &probe),
        SQLITE_OK
    );
    rsh_run_sql_exact(
        &emby_suite_spec, assertion, context, &assertion->data.sql_exact
    );
    require_int(
        "type-legacy/authorizer-clear",
        sqlite3_set_authorizer(handle->db, NULL, NULL), SQLITE_OK
    );
    if (probe.scalar_calls < 1) {
        failf(
            "FAIL [type-legacy/authorizer-count]: got=%d want=>=1",
            probe.scalar_calls
        );
    }
    return SQLITE_OK;
}

static const rsh_case_spec emby_positive_cases[] = {
    EMBY_POSITIVE_SQL_CASE(
        "type-v2", emby_fts_env_sql, emby_fts_env_rewritten_sql,
        RSH_PREPARE_V2, RSH_NBYTE_MINUS_ONE, RSH_TAIL_FULL,
        emby_fts_env_bind_names, 3
    ),
    EMBY_POSITIVE_SQL_CASE(
        "type-v3", emby_fts_env_sql, emby_fts_env_rewritten_sql,
        RSH_PREPARE_V3, RSH_NBYTE_MINUS_ONE, RSH_TAIL_FULL,
        emby_fts_env_bind_names, 3
    ),
    {
        .label = "type-legacy",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_IDENTITY,
            .assert_custom =
                rsh_custom_adapter_emby_positive_type_legacy_authorizer,
            .immutable_data = &emby_positive_type_legacy_assertion
        }
    },
    EMBY_POSITIVE_SQL_CASE(
        "type-numbered-param", emby_positive_numbered_sql,
        emby_positive_numbered_expected_sql,
        RSH_PREPARE_V2, RSH_NBYTE_MINUS_ONE, RSH_TAIL_FULL,
        emby_positive_numbered_bind_names, 3
    ),
    EMBY_POSITIVE_SQL_CASE(
        "type-nbyte-no-nul", emby_fts_env_sql, emby_fts_env_rewritten_sql,
        RSH_PREPARE_V2, RSH_NBYTE_EXACT_LENGTH, RSH_TAIL_FULL,
        emby_fts_env_bind_names, 3
    ),
    EMBY_POSITIVE_SQL_CASE(
        "type-nbyte-with-nul", emby_fts_env_sql, emby_fts_env_rewritten_sql,
        RSH_PREPARE_V2, RSH_NBYTE_LENGTH_WITH_NUL, RSH_TAIL_FULL,
        emby_fts_env_bind_names, 3
    ),
    EMBY_POSITIVE_SQL_CASE(
        "pztail-null", emby_fts_env_sql, emby_fts_env_rewritten_sql,
        RSH_PREPARE_V2, RSH_NBYTE_MINUS_ONE, RSH_TAIL_NULL_OUT,
        emby_fts_env_bind_names, 3
    ),
    EMBY_POSITIVE_SQL_CASE(
        "presentation-user42", emby_positive_presentation_sql,
        emby_positive_presentation_expected_sql,
        RSH_PREPARE_V2, RSH_NBYTE_MINUS_ONE, RSH_TAIL_FULL,
        emby_fts_env_bind_names, 26
    ),
    EMBY_POSITIVE_SCALAR_TEXT_CASE(
        "scalar-star-wars", "(\"Star\"*) OR (\"Wars\"*)",
        "(\"Star\"*) AND (\"Wars\"*)"
    ),
    EMBY_POSITIVE_SCALAR_TEXT_CASE(
        "scalar-three-way-or", "(\"a\"*) OR (\"b\"*) OR (\"c\"*)",
        "(\"a\"*) AND (\"b\"*) AND (\"c\"*)"
    ),
    EMBY_POSITIVE_SCALAR_TEXT_CASE(
        "scalar-existing-and", "(\"a\"*) AND (\"b\"*)",
        "(\"a\"*) AND (\"b\"*)"
    ),
    EMBY_POSITIVE_SCALAR_TEXT_CASE(
        "scalar-quoted-phrase", "\"star wars\"", "\"star wars\""
    ),
    EMBY_POSITIVE_SCALAR_TEXT_CASE(
        "scalar-mixed-form", "(\"a\"*) OR \"b\"*", "(\"a\"*) OR \"b\"*"
    ),
    EMBY_POSITIVE_SCALAR_TEXT_CASE(
        "scalar-lowercase-or", "(\"a\"*) or (\"b\"*)",
        "(\"a\"*) or (\"b\"*)"
    ),
    EMBY_POSITIVE_SCALAR_TEXT_CASE(
        "scalar-non-ascii", "(\"caf\303\251\"*) OR (\"b\"*)",
        "(\"caf\303\251\"*) OR (\"b\"*)"
    ),
    EMBY_POSITIVE_SCALAR_TEXT_CASE("scalar-empty", "", ""),
    {
        .label = "scalar-null",
        .kind = RSH_CASE_SCALAR,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.scalar = {
            .role = "candidate",
            .sql = emby_positive_scalar_sql,
            .expected_sql = emby_positive_scalar_sql,
            .prepare = EMBY_POSITIVE_SCALAR_PREPARE,
            .bind = rsh_bind_emby_positive_scalar_null,
            .value_kind = RSH_SCALAR_NULL
        }
    },
    {
        .label = "scalar-integer",
        .kind = RSH_CASE_SCALAR,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.scalar = {
            .role = "candidate",
            .sql = emby_positive_scalar_sql,
            .expected_sql = emby_positive_scalar_sql,
            .prepare = EMBY_POSITIVE_SCALAR_PREPARE,
            .bind = rsh_bind_emby_positive_scalar_integer,
            .value_kind = RSH_SCALAR_INTEGER,
            .integer_value = 42
        }
    },
    {
        .label = "scalar-blob",
        .kind = RSH_CASE_SCALAR,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.scalar = {
            .role = "candidate",
            .sql = emby_positive_scalar_sql,
            .expected_sql = emby_positive_scalar_sql,
            .prepare = EMBY_POSITIVE_SCALAR_PREPARE,
            .bind = rsh_bind_emby_positive_scalar_blob,
            .value_kind = RSH_SCALAR_BLOB,
            .bytes_value = emby_positive_scalar_blob,
            .bytes_count = (int)sizeof(emby_positive_scalar_blob)
        }
    },
#ifdef EMBY_FTS_REWRITE_TEST_LITERAL_MATCH
    {
        .label = "literal-rhs-positive",
        .kind = RSH_CASE_SQL_EXACT,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.sql_exact = {
            .role = "candidate",
            .sql = emby_positive_literal_sql,
            .expected_sql = emby_positive_literal_expected_sql,
            .prepare = EMBY_PREPARE_V2,
            .check_bind_count = 1,
            .expected_bind_count = 0,
            .check_column_count = 1,
            .expected_column_count = 3
        }
    },
#endif
#ifdef EMBY_FTS_REWRITE_TEST_API
    {
        .label = "scalar-counter",
        .kind = RSH_CASE_SCALAR,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.scalar = {
            .role = "candidate",
            .sql = emby_positive_scalar_sql,
            .expected_sql = emby_positive_scalar_sql,
            .prepare = EMBY_PREPARE_V2,
            .bind = rsh_bind_emby_positive_scalar_text,
            .bind_ctx = (void *)"(\"alpha\"*) OR (\"direct\"*)",
            .value_kind = RSH_SCALAR_TEXT,
            .bytes_value = "(\"alpha\"*) AND (\"direct\"*)",
            .bytes_count = (int)sizeof("(\"alpha\"*) AND (\"direct\"*)") - 1,
            .reset_counter = rsh_reset_emby_positive_scalar_counter,
            .read_counter = rsh_read_emby_positive_scalar_counter,
            .expected_counter = 1
        }
    },
#endif
};
DEFINE_EMBY_SCALAR_PHASE(
    emby_positive_phases, "positive", emby_env_candidate_db,
    emby_positive_cases
);

#undef EMBY_POSITIVE_SCALAR_TEXT_CASE
#undef EMBY_POSITIVE_SCALAR_PREPARE
#undef EMBY_POSITIVE_SQL_CASE

static const rsh_case_spec emby_fts_default_cases[] = {
    EMBY_FTS_SQL_CASE("fts-default-on", emby_fts_env_rewritten_sql)
};
DEFINE_EMBY_SCALAR_PHASE(
    emby_fts_default_phases, "fts-default-on", emby_env_candidate_db,
    emby_fts_default_cases
);

static const rsh_case_spec emby_fts_zero_cases[] = {
    EMBY_FTS_SQL_CASE("fts-zero-enables", emby_fts_env_rewritten_sql)
};
DEFINE_EMBY_SCALAR_PHASE(
    emby_fts_zero_phases, "fts-zero-enables", emby_env_candidate_db,
    emby_fts_zero_cases
);

static const rsh_case_spec emby_fts_one_cases[] = {
    EMBY_SQL_CASE("fts-one-disables", emby_fts_env_sql, emby_fts_env_sql)
};
DEFINE_EMBY_SCALAR_PHASE(
    emby_fts_one_phases, "fts-one-disables", emby_env_candidate_db,
    emby_fts_one_cases
);

static const rsh_case_spec emby_fts_garbage_cases[] = {
    EMBY_FTS_SQL_CASE("fts-garbage-enables", emby_fts_env_rewritten_sql)
};
DEFINE_EMBY_SCALAR_PHASE(
    emby_fts_garbage_phases, "fts-garbage-enables", emby_env_candidate_db,
    emby_fts_garbage_cases
);

static const rsh_case_spec emby_fanout_default_cases[] = {
    EMBY_SQL_CASE(
        "fanout-default-on", emby_fanout_env_sql,
        emby_fanout_env_rewritten_sql
    )
};
DEFINE_EMBY_SCALAR_PHASE(
    emby_fanout_default_phases, "fanout-default-on", emby_env_candidate_db,
    emby_fanout_default_cases
);

static const rsh_case_spec emby_fanout_zero_cases[] = {
    EMBY_SQL_CASE(
        "fanout-zero-enables", emby_fanout_env_sql,
        emby_fanout_env_rewritten_sql
    )
};
DEFINE_EMBY_SCALAR_PHASE(
    emby_fanout_zero_phases, "fanout-zero-enables", emby_env_candidate_db,
    emby_fanout_zero_cases
);

static const rsh_case_spec emby_fanout_one_cases[] = {
    EMBY_SQL_CASE(
        "fanout-one-disables", emby_fanout_env_sql, emby_fanout_env_sql
    )
};
DEFINE_EMBY_SCALAR_PHASE(
    emby_fanout_one_phases, "fanout-one-disables", emby_env_candidate_db,
    emby_fanout_one_cases
);

static const rsh_case_spec emby_fanout_garbage_cases[] = {
    EMBY_SQL_CASE(
        "fanout-garbage-enables", emby_fanout_env_sql,
        emby_fanout_env_rewritten_sql
    )
};
DEFINE_EMBY_SCALAR_PHASE(
    emby_fanout_garbage_phases, "fanout-garbage-enables",
    emby_env_candidate_db, emby_fanout_garbage_cases
);

static const rsh_case_spec emby_dashboard_default_cases[] = {
    EMBY_SQL_CASE(
        "dashboard-default-off", emby_dashboard_episodes_off_sql,
        emby_dashboard_episodes_off_sql
    ),
    EMBY_SQL_CASE(
        "dashboard-default-off-movies", emby_dashboard_movies_off_sql,
        emby_dashboard_movies_off_sql
    ),
    EMBY_SQL_CASE(
        "dashboard-default-off-mixed", emby_dashboard_mixed_off_sql,
        emby_dashboard_mixed_off_sql
    )
};
DEFINE_EMBY_SCALAR_PHASE(
    emby_dashboard_default_phases, "dashboard-default-off",
    emby_dashboard_off_candidate_db, emby_dashboard_default_cases
);

#define DEFINE_EMBY_DASHBOARD_OFF_ROW(name, case_label) \
    static const rsh_case_spec name##_cases[] = { \
        EMBY_SQL_CASE( \
            (case_label), emby_dashboard_episodes_off_sql, \
            emby_dashboard_episodes_off_sql \
        ), \
        EMBY_SQL_CASE( \
            (case_label), emby_dashboard_movies_off_sql, \
            emby_dashboard_movies_off_sql \
        ), \
        EMBY_SQL_CASE( \
            (case_label), emby_dashboard_mixed_off_sql, \
            emby_dashboard_mixed_off_sql \
        ) \
    }; \
    DEFINE_EMBY_SCALAR_PHASE( \
        name##_phases, (case_label), emby_dashboard_off_candidate_db, \
        name##_cases \
    )

DEFINE_EMBY_DASHBOARD_OFF_ROW(emby_dashboard_one, "dashboard-one-off");
DEFINE_EMBY_DASHBOARD_OFF_ROW(emby_dashboard_empty, "dashboard-empty-off");
DEFINE_EMBY_DASHBOARD_OFF_ROW(emby_dashboard_garbage, "dashboard-garbage-off");

static const char emby_select_one_sql[] = "SELECT 1";

static const char emby_latest_limit20_sql[] =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) )"
    "select A.Id from mediaitems A left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=42 "
    "where A.Type=8 AND Coalesce(UserDatas.played, 0)=0 AND A.Id in WithAncestors Group by coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey) ORDER BY MAX(A.DateCreated) DESC LIMIT 20";

static const char emby_latest_limit20_expected_sql[] =
    "WITH ranked(id, dc, gk) AS MATERIALIZED ("
    "  SELECT A.Id, A.DateCreated, coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey) "
    "  FROM MediaItems AS A INDEXED BY idx_dshadow_emby_latest_episodes_dcn_gk "
    "  WHERE A.Type = 8 "
    "AND EXISTS ( SELECT 1 FROM AncestorIds2 AS X WHERE X.ItemId = A.Id AND X.AncestorId IN (100) ) "
    "AND NOT EXISTS ( SELECT 1 FROM UserDatas AS U0 WHERE U0.UserDataKeyId = A.UserDataKeyId AND U0.UserId = 42 AND U0.played <> 0 ) "
    "AND NOT EXISTS (      SELECT 1 "
    "      FROM MediaItems AS B INDEXED BY idx_dshadow_emby_latest_gk_dc "
    "      WHERE B.Type = 8 AND coalesce(B.SeriesPresentationUniqueKey, B.PresentationUniqueKey) IS coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey) "
    "AND ( (B.DateCreated IS NOT NULL AND A.DateCreated IS NULL) OR B.DateCreated > A.DateCreated OR (B.DateCreated IS A.DateCreated AND B.Id < A.Id) ) "
    "AND EXISTS ( SELECT 1 FROM AncestorIds2 AS XB WHERE XB.ItemId = B.Id AND XB.AncestorId IN (100) ) "
    "AND NOT EXISTS ( SELECT 1 FROM UserDatas AS UB WHERE UB.UserDataKeyId = B.UserDataKeyId AND UB.UserId = 42 AND UB.played <> 0 ) ) "
    "ORDER BY (A.DateCreated IS NULL) ASC, A.DateCreated DESC, coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey) ASC LIMIT 20 ) "
    "SELECT A.Id FROM ranked AS R JOIN MediaItems AS A ON A.Id = R.id LEFT JOIN UserDatas "
    "ON A.UserDataKeyId = UserDatas.UserDataKeyId AND UserDatas.UserId = 42 "
    "ORDER BY (R.dc IS NULL) ASC, R.dc DESC, R.gk ASC LIMIT 20";

static const char emby_latest_resume_sql[] =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) )select count(*) OVER() AS TotalRecordCount,A.type,A.Id,A.SeriesPresentationUniqueKey,UserDatas.PlaybackPositionTicks,"
    "((Coalesce(A.SortParentIndexNumber,A.ParentIndexNumber, 1) * 1000000) + Coalesce(A.SortIndexNumber, A.IndexNumber, 0) + (Select Case When Coalesce(A.ParentIndexNumber,1)=0 Then 0 Else 0.5 End) + (Select Case When Coalesce(A.ParentIndexNumber,1)=0 Then (Cast(Coalesce(A.IndexNumber, 0) as REAL) / 100000) Else 0 End)) EpisodeAbsoluteIndexNumber "
    "from mediaitems A left join (Select N.SeriesPresentationUniqueKey,((Coalesce(N.SortParentIndexNumber,N.ParentIndexNumber, 1) * 1000000) + Coalesce(N.SortIndexNumber, N.IndexNumber, 0) + (Select Case When Coalesce(N.ParentIndexNumber,1)=0 Then 0 Else 0.5 End) + (Select Case When Coalesce(N.ParentIndexNumber,1)=0 Then (Cast(Coalesce(N.IndexNumber, 0) as REAL) / 100000) Else 0 End)) AbsoluteIndexNumber,max(UserDatas_N.LastPlayedDateInt) LastPlayedDateInt,UserDatas_N.playbackPositionTicks from MediaItems N join UserDatas UserDatas_N on N.UserDataKeyId=UserDatas_N.UserDataKeyId And UserDatas_N.UserId=1 where N.Type=8 and Coalesce(N.SortParentIndexNumber,N.ParentIndexNumber,-1) <> 0 and (UserDatas_N.Played=1 or UserDatas_N.playbackPositionTicks > 0) Group By N.SeriesPresentationUniqueKey ORDER BY UserDatas_N.LastPlayedDateInt desc, AbsoluteIndexNumber desc) LastWatchedEpisodes on LastWatchedEpisodes.SeriesPresentationUniqueKey=A.SeriesPresentationUniqueKey "
    "left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=1 where ((A.Type=5 and UserDatas.playbackPositionTicks > 0) OR (A.Type=8 AND (UserDatas.playbackPositionTicks > 0 or Coalesce(UserDatas.played,0) = 0) AND (select case when LastWatchedEpisodes.playbackPositionTicks > 0 then EpisodeAbsoluteIndexNumber >= Coalesce(LastWatchedEpisodes.AbsoluteIndexNumber,EpisodeAbsoluteIndexNumber) else EpisodeAbsoluteIndexNumber > Coalesce(LastWatchedEpisodes.AbsoluteIndexNumber,EpisodeAbsoluteIndexNumber) end) AND LastWatchedEpisodes.LastPlayedDateInt not null)) "
    "AND (A.Type=5 OR Coalesce(A.SortParentIndexNumber,A.ParentIndexNumber, -1) <> 0) AND A.Type in (5,8) AND Coalesce(UserDatas.HideFromResume,0)=0 AND COALESCE((select hidefromresume from userdatas where userdatas.userid=1 and userdatas.userdatakeyid=(select userdatakeyid from mediaitems where mediaitems.id=A.seriesid)),0)=0 AND A.Id in WithAncestors Group by coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey) ORDER BY COALESCE(lastwatchedepisodes.lastplayeddateint, userdatas.lastplayeddateint, 0) DESC,Min(EpisodeAbsoluteIndexNumber) ASC LIMIT 12";

static const rsh_occurrence_expectation emby_no_scalar_rewrite_occurrences[] = {
    {
        .needle = "dshadow_emby_fts_rewrite(",
        .expected_count = 0
    }
};

static const rsh_db_spec emby_nonmatch_dbs[] = {
    {
        .role = "vendor",
        .relative_path = "not-target.db",
        .kind = RSH_DB_VENDOR,
        .storage = RSH_DB_RELATIVE
    },
    {
        .role = "candidate",
        .relative_path = "library.db",
        .kind = RSH_DB_CANDIDATE,
        .storage = RSH_DB_RELATIVE
    }
};

#define EMBY_NONMATCH_NEGATIVE_CASE(case_label, source_sql, unique_needle) \
    { \
        .label = (case_label), \
        .kind = RSH_CASE_NEGATIVE, \
        .build_mask = RSH_BUILD_EMBY_LINKED, \
        .data.negative = { \
            .source_kind = RSH_NEGATIVE_STATIC, \
            .sql = (source_sql), \
            .discriminating_needle = (unique_needle), \
            .prepare = EMBY_PREPARE_V2, \
            .vendor_role = "vendor", \
            .candidate_role = "candidate", \
            .followup_occurrences = emby_no_scalar_rewrite_occurrences, \
            .followup_occurrence_count = \
                sizeof(emby_no_scalar_rewrite_occurrences) / \
                sizeof(emby_no_scalar_rewrite_occurrences[0]) \
        } \
    }

#define EMBY_NONMATCH_POSITIVE_CASE(case_label, source_sql, saved_sql) \
    { \
        .label = (case_label), \
        .kind = RSH_CASE_SQL_EXACT, \
        .build_mask = RSH_BUILD_EMBY_LINKED, \
        .data.sql_exact = { \
            .role = "candidate", \
            .sql = (source_sql), \
            .expected_sql = (saved_sql), \
            .prepare = EMBY_PREPARE_V2, \
            .check_bind_count = 1, \
            .expected_bind_count = 1, \
            .expected_bind_names = emby_fts_env_bind_names, \
            .expected_bind_name_count = sizeof(emby_fts_env_bind_names) / \
                                        sizeof(emby_fts_env_bind_names[0]) \
        } \
    }

static const rsh_case_spec emby_nonmatch_cases[] = {
    EMBY_NONMATCH_NEGATIVE_CASE(
        "fast-form", emby_nonmatch_fast_sql,
        "EXISTS (SELECT 1 FROM AncestorIds2 WHERE AncestorIds2.itemid=A.Id)"
    ),
    EMBY_NONMATCH_NEGATIVE_CASE(
        "duplicate-fts", emby_nonmatch_duplicate_fts_sql,
        "@SearchTerm AND fts_search9 match @SearchTerm2"
    ),
#ifndef EMBY_FTS_REWRITE_TEST_LITERAL_MATCH
    EMBY_NONMATCH_NEGATIVE_CASE(
        "literal-rhs", emby_nonmatch_literal_rhs_sql,
        "fts_search9 match 'alpha'"
    ),
#endif
    EMBY_NONMATCH_NEGATIVE_CASE(
        "match-rhs-concat", emby_nonmatch_concat_rhs_sql,
        "fts_search9 match @SearchTerm || ''"
    ),
    EMBY_NONMATCH_NEGATIVE_CASE(
        "match-rhs-named-colon", emby_nonmatch_named_colon_rhs_sql,
        "$SearchTerm::suffix"
    ),
    EMBY_NONMATCH_NEGATIVE_CASE(
        "match-rhs-named-func", emby_nonmatch_named_func_rhs_sql,
        "$SearchTerm(extra)"
    ),
    EMBY_NONMATCH_NEGATIVE_CASE(
        "over-cap-slot", emby_nonmatch_over_cap_sql,
        emby_nonmatch_over_cap_discriminator
    ),
    EMBY_NONMATCH_POSITIVE_CASE(
        "ambiguous-after-l1", emby_nonmatch_ambiguous_after_l1_sql,
        emby_nonmatch_ambiguous_after_l1_expected_sql
    ),
    EMBY_NONMATCH_POSITIVE_CASE(
        "ambiguous-after-t1", emby_nonmatch_ambiguous_after_t1_sql,
        emby_nonmatch_ambiguous_after_t1_expected_sql
    ),
    EMBY_NONMATCH_POSITIVE_CASE(
        "ambiguous-after-t2", emby_nonmatch_ambiguous_after_t2_sql,
        emby_nonmatch_ambiguous_after_t2_expected_sql
    ),
    EMBY_NONMATCH_POSITIVE_CASE(
        "ambiguous-after-l2", emby_nonmatch_ambiguous_after_l2_sql,
        emby_nonmatch_ambiguous_after_l2_expected_sql
    ),
    {
        .label = "semicolon",
        .kind = RSH_CASE_SQL_EXACT,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.sql_exact = {
            .role = "candidate",
            .sql = emby_nonmatch_semicolon_sql,
            .expected_sql = emby_nonmatch_semicolon_saved_sql,
            .prepare = {
                .kind = RSH_PREPARE_V2,
                .nbyte_kind = RSH_NBYTE_MINUS_ONE,
                .tail_kind = RSH_TAIL_OFFSET,
                .tail_offset = sizeof(emby_nonmatch_semicolon_saved_sql) - 1
            }
        }
    },
    {
        .label = "embedded-nul",
        .kind = RSH_CASE_SQL_EXACT,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.sql_exact = {
            .role = "candidate",
            .sql = emby_nonmatch_embedded_nul_sql,
            .expected_sql = emby_nonmatch_embedded_nul_saved_sql,
            .prepare = {
                .kind = RSH_PREPARE_V2,
                .nbyte_kind = RSH_NBYTE_LITERAL,
                .literal_nbyte =
                    (int)sizeof(emby_nonmatch_embedded_nul_sql) - 1,
                .tail_kind = RSH_TAIL_IGNORE
            }
        }
    }
};
DEFINE_EMBY_SCALAR_PHASE(
    emby_nonmatch_phases, "nonmatch", emby_nonmatch_dbs,
    emby_nonmatch_cases
);

#undef EMBY_NONMATCH_POSITIVE_CASE
#undef EMBY_NONMATCH_NEGATIVE_CASE

static sqlite3 *rsh_context_db(
    const rsh_case_context *context,
    const char *role
) {
    size_t i;

    for (i = 0; i < context->db_count; i++) {
        if (context->dbs[i].spec &&
            strcmp(context->dbs[i].spec->role, role) == 0) {
            return context->dbs[i].db;
        }
    }
    failf("FAIL [%s/%s/role]: missing=%s", context->run_name,
          context->phase_label, role);
    return NULL;
}

static int rsh_custom_adapter_emby_collision(
    const rsh_case_context *context,
    const void *immutable_data
) {
    sqlite3 *db = rsh_context_db(context, "candidate");

    (void)immutable_data;
    g_collision_scalar_calls = 0;
    require_int(
        "collision/register",
        sqlite3_create_function_v2(
            db, "dshadow_emby_fts_rewrite", 1, SQLITE_UTF8, NULL,
            scalar_collision_tripwire, NULL, NULL, NULL
        ),
        SQLITE_OK
    );
    expect_sql(
        db, "collision", emby_fts_env_sql, -1, 2,
        emby_fts_env_sql, 0
    );
    require_int("collision/scalar-calls", g_collision_scalar_calls, 0);
    return SQLITE_OK;
}

static const rsh_case_spec emby_collision_cases[] = {
    {
        .label = "collision",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_FAULT_MATRIX,
            .assert_custom = rsh_custom_adapter_emby_collision,
        }
    }
};
DEFINE_EMBY_SCALAR_PHASE(
    emby_collision_phases, "collision", emby_env_candidate_db,
    emby_collision_cases
);

static int rsh_custom_adapter_emby_authorizer_and_ownership(
    const rsh_case_context *context,
    const void *immutable_data
) {
    sqlite3 *db = rsh_context_db(context, "candidate");

    (void)immutable_data;
    require_int(
        "authorizer/set",
        sqlite3_set_authorizer(db, deny_pragma_function_list, NULL),
        SQLITE_OK
    );
    expect_sql(
        db, "authorizer-deny-probe", emby_fts_env_sql, -1, 2,
        emby_fts_env_sql, 0
    );
    require_int(
        "authorizer/clear", sqlite3_set_authorizer(db, NULL, NULL), SQLITE_OK
    );

    expect_sql(
        db, "ownership-ready", emby_fts_env_sql, -1, 2,
        emby_fts_env_rewritten_sql, 1
    );
    require_int(
        "ownership/replace",
        sqlite3_create_function_v2(
            db, "dshadow_emby_fts_rewrite", 1, SQLITE_UTF8, NULL,
            scalar_owner_spoof, NULL, NULL, NULL
        ),
        SQLITE_OK
    );
    expect_sql(
        db, "ownership-replaced", emby_fts_env_sql, -1, 2,
        emby_fts_env_sql, 0
    );
    return SQLITE_OK;
}

static const rsh_case_spec emby_authorizer_ownership_cases[] = {
    {
        .label = "authorizer-ownership",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_FAULT_MATRIX,
            .assert_custom =
                rsh_custom_adapter_emby_authorizer_and_ownership,
        }
    }
};
DEFINE_EMBY_SCALAR_PHASE(
    emby_authorizer_ownership_phases, "authorizer-ownership",
    emby_env_candidate_db, emby_authorizer_ownership_cases
);

static const rsh_db_spec emby_path_negative_dbs[] = {
    {
        .role = "plex",
        .relative_path = "com.plexapp.plugins.library.db",
        .kind = RSH_DB_AUXILIARY,
        .storage = RSH_DB_RELATIVE
    },
    {
        .role = "jellyfin",
        .relative_path = "jellyfin.db",
        .kind = RSH_DB_AUXILIARY,
        .storage = RSH_DB_RELATIVE
    },
    {
        .role = "vendor",
        .relative_path = "not-target.db",
        .kind = RSH_DB_VENDOR,
        .storage = RSH_DB_RELATIVE
    },
    {
        .role = "memory",
        .relative_path = ":memory:",
        .kind = RSH_DB_AUXILIARY,
        .storage = RSH_DB_MEMORY
    },
    {
        .role = "non-ascii",
        .relative_path = "emb\303\251/library.db",
        .kind = RSH_DB_CANDIDATE,
        .storage = RSH_DB_RELATIVE
    }
};

#define EMBY_PATH_CASE(case_label, role_name, source_sql) \
    { \
        .label = (case_label), \
        .kind = RSH_CASE_PATH, \
        .build_mask = RSH_BUILD_EMBY_LINKED, \
        .data.path.assertion = { \
            .role = (role_name), \
            .sql = (source_sql), \
            .expected_sql = (source_sql), \
            .prepare = EMBY_PREPARE_V2 \
        } \
    }

static const rsh_case_spec emby_path_negative_cases[] = {
    EMBY_PATH_CASE(
        "com.plexapp.plugins.library.db", "plex", emby_fts_env_sql
    ),
    EMBY_PATH_CASE("jellyfin.db", "jellyfin", emby_fts_env_sql),
    EMBY_PATH_CASE("not-target.db", "vendor", emby_fts_env_sql),
    EMBY_PATH_CASE("memory", "memory", emby_select_one_sql),
    EMBY_PATH_CASE("non-ascii-path", "non-ascii", emby_fts_env_sql)
};
DEFINE_EMBY_SCALAR_PHASE(
    emby_path_negative_phases, "path-negative", emby_path_negative_dbs,
    emby_path_negative_cases
);

#undef EMBY_PATH_CASE

static const rsh_db_spec emby_fixture_canary_dbs[] = {
    {
        .role = "vendor",
        .relative_path = "not-target.db",
        .kind = RSH_DB_VENDOR,
        .storage = RSH_DB_RELATIVE,
        .setup_profile = EMBY_PROFILE_FIXTURE_CANARY
    },
    {
        .role = "candidate",
        .relative_path = "library.db",
        .kind = RSH_DB_CANDIDATE,
        .storage = RSH_DB_RELATIVE,
        .setup_profile = EMBY_PROFILE_FIXTURE_CANARY
    }
};

static const rsh_db_spec emby_latest_candidate_db[] = {
    {
        .role = "candidate",
        .relative_path = "library.db",
        .kind = RSH_DB_CANDIDATE,
        .storage = RSH_DB_RELATIVE,
        .setup_profile = EMBY_PROFILE_LATEST_INDEXES
    }
};

static const rsh_db_spec emby_latest_negative_dbs[] = {
    {
        .role = "vendor",
        .relative_path = "not-target.db",
        .kind = RSH_DB_VENDOR,
        .storage = RSH_DB_RELATIVE,
        .setup_profile = EMBY_PROFILE_LATEST_INDEXES
    },
    {
        .role = "candidate",
        .relative_path = "library.db",
        .kind = RSH_DB_CANDIDATE,
        .storage = RSH_DB_RELATIVE,
        .setup_profile = EMBY_PROFILE_LATEST_INDEXES
    }
};

#define EMBY_FIXTURE_SQL_CASE(case_label) \
    { \
        .label = (case_label), \
        .kind = RSH_CASE_FIXTURE, \
        .build_mask = RSH_BUILD_EMBY_LINKED, \
        .data.fixture = { \
            .source_path = \
                "tests/fixtures/emby-fts-rewrite/" case_label ".sql", \
            .expected_path = \
                "tests/fixtures/emby-fts-rewrite/" case_label ".expected.sql", \
            .strip_final_lf = 1, \
            .assertion_kind = RSH_FIXTURE_SQL_EXACT, \
            .sql_exact = { \
                .role = "candidate", \
                .prepare = EMBY_PREPARE_V2 \
            } \
        } \
    }

#define EMBY_FTS_FIXTURE_SQL_CASE(case_label) \
    { \
        .label = (case_label), \
        .kind = RSH_CASE_FIXTURE, \
        .build_mask = RSH_BUILD_EMBY_LINKED, \
        .data.fixture = { \
            .source_path = \
                "tests/fixtures/emby-fts-rewrite/" case_label ".sql", \
            .expected_path = \
                "tests/fixtures/emby-fts-rewrite/" case_label ".expected.sql", \
            .strip_final_lf = 1, \
            .assertion_kind = RSH_FIXTURE_SQL_EXACT, \
            .sql_exact = { \
                .role = "candidate", \
                .prepare = EMBY_PREPARE_V2, \
                .check_bind_count = 1, \
                .expected_bind_count = 1, \
                .expected_bind_names = emby_fts_env_bind_names, \
                .expected_bind_name_count = \
                    sizeof(emby_fts_env_bind_names) / \
                    sizeof(emby_fts_env_bind_names[0]) \
            } \
        } \
    }

#define EMBY_FIXTURE_NEGATIVE_CASE(case_label, unique_needle) \
    { \
        .label = (case_label), \
        .kind = RSH_CASE_FIXTURE, \
        .build_mask = RSH_BUILD_EMBY_LINKED, \
        .data.fixture = { \
            .source_path = \
                "tests/fixtures/emby-fts-rewrite/" case_label ".sql", \
            .expected_path = \
                "tests/fixtures/emby-fts-rewrite/" case_label ".expected.sql", \
            .strip_final_lf = 1, \
            .assertion_kind = RSH_FIXTURE_NEGATIVE, \
            .negative = { \
                .source_kind = RSH_NEGATIVE_STATIC, \
                .discriminating_needle = (unique_needle), \
                .prepare = EMBY_PREPARE_V2, \
                .vendor_role = "vendor", \
                .candidate_role = "candidate", \
                .followup_occurrences = emby_no_scalar_rewrite_occurrences, \
                .followup_occurrence_count = \
                    sizeof(emby_no_scalar_rewrite_occurrences) / \
                    sizeof(emby_no_scalar_rewrite_occurrences[0]) \
            } \
        } \
    }

static const rsh_case_spec emby_fixture_canary_cases[] = {
    EMBY_FTS_FIXTURE_SQL_CASE("slow-type"),
    EMBY_FTS_FIXTURE_SQL_CASE("slow-presentation"),
    EMBY_FTS_FIXTURE_SQL_CASE("presentation-user42"),
    EMBY_FTS_FIXTURE_SQL_CASE("l1-l2-mismatch"),
    EMBY_FTS_FIXTURE_SQL_CASE("mutated-type-slots"),
    EMBY_FIXTURE_NEGATIVE_CASE(
        "fast-exists", "AncestorIds2.itemid=A.Id"
    ),
    EMBY_FIXTURE_NEGATIVE_CASE(
        "string-anchor-passthrough", "'WithAncestors AS"
    ),
    EMBY_FIXTURE_NEGATIVE_CASE(
        "comment-anchor-passthrough", "-- WithAncestors AS"
    ),
    EMBY_FIXTURE_SQL_CASE("fanout-people"),
    EMBY_FIXTURE_SQL_CASE("fanout-links-search"),
    EMBY_FIXTURE_SQL_CASE("fanout-browse"),
    EMBY_FIXTURE_SQL_CASE("fanout-favorites"),
    EMBY_FIXTURE_SQL_CASE("latest-limit12"),
    EMBY_FIXTURE_SQL_CASE("latest-limit16"),
    EMBY_FIXTURE_SQL_CASE("latest-movies-played-limit12"),
    EMBY_FIXTURE_NEGATIVE_CASE(
        "latest-star-projection-negative", "select * from mediaitems A"
    ),
    EMBY_FIXTURE_NEGATIVE_CASE(
        "latest-capture-miss-negative",
        "select A.Id,(A.DateCreated) from"
    ),
    EMBY_FIXTURE_NEGATIVE_CASE(
        "latest-aggregate-projection-negative",
        "select MAX(A.DateCreated) from"
    ),
    EMBY_FIXTURE_NEGATIVE_CASE(
        "latest-over-negative", "count(*) OVER() AS TotalRecordCount"
    ),
    EMBY_FIXTURE_NEGATIVE_CASE(
        "latest-series-browse-negative", "ORDER BY SeriesName LIMIT 12"
    ),
    EMBY_FIXTURE_NEGATIVE_CASE(
        "latest-explain-prefix-negative", "EXPLAIN QUERY PLAN with"
    )
};
DEFINE_EMBY_SCALAR_PHASE(
    emby_fixture_canary_phases, "fixture-canary", emby_fixture_canary_dbs,
    emby_fixture_canary_cases
);

static const rsh_case_spec emby_latest_limit20_cases[] = {
    EMBY_SQL_CASE(
        "latest-limit20", emby_latest_limit20_sql,
        emby_latest_limit20_expected_sql
    )
};
DEFINE_EMBY_SCALAR_PHASE(
    emby_latest_limit20_phases, "latest-limit20", emby_latest_candidate_db,
    emby_latest_limit20_cases
);

static const rsh_case_spec emby_latest_capture_miss_negative_cases[] = {
    EMBY_FIXTURE_NEGATIVE_CASE(
        "latest-capture-miss-negative",
        "select A.Id,(A.DateCreated) from"
    )
};
DEFINE_EMBY_SCALAR_PHASE(
    emby_latest_capture_miss_negative_phases,
    "latest-capture-miss-negative", emby_latest_negative_dbs,
    emby_latest_capture_miss_negative_cases
);

static const rsh_case_spec emby_latest_series_browse_negative_cases[] = {
    EMBY_FIXTURE_NEGATIVE_CASE(
        "latest-series-browse-negative", "ORDER BY SeriesName LIMIT 12"
    )
};
DEFINE_EMBY_SCALAR_PHASE(
    emby_latest_series_browse_negative_phases,
    "latest-series-browse-negative", emby_latest_negative_dbs,
    emby_latest_series_browse_negative_cases
);

static const rsh_case_spec emby_latest_resume_no_misfire_cases[] = {
    {
        .label = "latest-resume-no-misfire",
        .kind = RSH_CASE_NEGATIVE,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.negative = {
            .source_kind = RSH_NEGATIVE_STATIC,
            .sql = emby_latest_resume_sql,
            .discriminating_needle =
                "LastWatchedEpisodes.LastPlayedDateInt not null",
            .prepare = EMBY_PREPARE_V2,
            .vendor_role = "vendor",
            .candidate_role = "candidate",
            .followup_occurrences = emby_no_scalar_rewrite_occurrences,
            .followup_occurrence_count =
                sizeof(emby_no_scalar_rewrite_occurrences) /
                sizeof(emby_no_scalar_rewrite_occurrences[0])
        }
    }
};
DEFINE_EMBY_SCALAR_PHASE(
    emby_latest_resume_no_misfire_phases, "latest-resume-no-misfire",
    emby_latest_negative_dbs, emby_latest_resume_no_misfire_cases
);

typedef struct emby_matrix_case {
    const char *label;
    int case_id;
} emby_matrix_case;

static const emby_matrix_case *rsh_emby_matrix_case(
    const rsh_case_context *context
) {
    const rsh_matrix_cell *cell = context->matrix_cell;

    if (!cell || cell->axis_count != 1 || !cell->axis_values[0] ||
        !cell->axis_values[0]->immutable_data) {
        failf("FAIL [%s/%s/matrix-case]: invalid one-axis cell",
              context->run_name, context->phase_label);
    }
    return (const emby_matrix_case *)cell->axis_values[0]->immutable_data;
}

static void rsh_run_emby_matrix_sql_exact(
    const rsh_case_context *context,
    const char *label,
    const char *sql,
    const char *expected_sql,
    rsh_prepare_kind prepare_kind,
    rsh_nbyte_kind nbyte_kind
) {
    rsh_case_spec test_case;

    memset(&test_case, 0, sizeof(test_case));
    test_case.label = label;
    test_case.kind = RSH_CASE_SQL_EXACT;
    test_case.build_mask = RSH_BUILD_EMBY_LINKED;
    test_case.data.sql_exact.role = "candidate";
    test_case.data.sql_exact.sql = sql;
    test_case.data.sql_exact.expected_sql = expected_sql;
    test_case.data.sql_exact.prepare.kind = prepare_kind;
    test_case.data.sql_exact.prepare.nbyte_kind = nbyte_kind;
    test_case.data.sql_exact.prepare.tail_kind = RSH_TAIL_FULL;
    rsh_run_sql_exact(
        &emby_suite_spec, &test_case, context, &test_case.data.sql_exact
    );
}

static void rsh_run_emby_matrix_negative(
    const rsh_case_context *context,
    const char *label,
    rsh_negative_source_kind source_kind,
    const char *sql,
    const char *base_sql,
    const char *replacement_needle,
    const char *replacement,
    const char *discriminating_needle
) {
    rsh_case_spec test_case;

    memset(&test_case, 0, sizeof(test_case));
    test_case.label = label;
    test_case.kind = RSH_CASE_NEGATIVE;
    test_case.build_mask = RSH_BUILD_EMBY_LINKED;
    test_case.data.negative.source_kind = source_kind;
    test_case.data.negative.sql = sql;
    test_case.data.negative.base_sql = base_sql;
    test_case.data.negative.replacement_needle = replacement_needle;
    test_case.data.negative.replacement = replacement;
    test_case.data.negative.discriminating_needle = discriminating_needle;
    test_case.data.negative.prepare = (rsh_prepare_spec)EMBY_PREPARE_V2;
    test_case.data.negative.vendor_role = "vendor";
    test_case.data.negative.candidate_role = "candidate";
    test_case.data.negative.followup_occurrences =
        emby_no_scalar_rewrite_occurrences;
    test_case.data.negative.followup_occurrence_count =
        sizeof(emby_no_scalar_rewrite_occurrences) /
        sizeof(emby_no_scalar_rewrite_occurrences[0]);
    rsh_run_negative(
        &emby_suite_spec, &test_case, context, &test_case.data.negative
    );
}

static void rsh_run_emby_matrix_static_negative(
    const rsh_case_context *context,
    const char *label,
    const char *sql,
    const char *discriminating_needle
) {
    rsh_run_emby_matrix_negative(
        context, label, RSH_NEGATIVE_STATIC, sql, NULL, NULL, NULL,
        discriminating_needle
    );
}

static void rsh_run_emby_matrix_generated_negative(
    const rsh_case_context *context,
    const char *label,
    const char *base_sql,
    const char *replacement_needle,
    const char *replacement,
    const char *discriminating_needle
) {
    rsh_run_emby_matrix_negative(
        context, label, RSH_NEGATIVE_GENERATED, NULL, base_sql,
        replacement_needle, replacement, discriminating_needle
    );
}

enum emby_fanout_matrix_case_id {
    EMBY_FANOUT_MATRIX_BROWSE_V2 = 1,
    EMBY_FANOUT_MATRIX_BROWSE_V3,
    EMBY_FANOUT_MATRIX_FAVORITES,
    EMBY_FANOUT_MATRIX_RESUME,
    EMBY_FANOUT_MATRIX_PEOPLE,
    EMBY_FANOUT_MATRIX_LINKS_SEARCH,
    EMBY_FANOUT_MATRIX_SEARCH_SHAPE_NOMISFIRE
};

static int rsh_matrix_assert_emby_fanout(
    const rsh_case_context *context,
    const rsh_matrix_cell_step *step,
    void *ctx
) {
    const emby_matrix_case *matrix_case = rsh_emby_matrix_case(context);
    char *sql = NULL;
    char *expected = NULL;
    char *one = NULL;
    char *two = NULL;
    char *replacement = NULL;

    (void)step;
    (void)ctx;
    if (matrix_case->case_id == EMBY_FANOUT_MATRIX_BROWSE_V2 ||
        matrix_case->case_id == EMBY_FANOUT_MATRIX_BROWSE_V3) {
        sql = make_browse_sql();
        one = make_exists_links_one("100", "6");
        two = make_exists_links_two("100", "7");
        replacement = xasprintf("(%s OR %s)", one, two);
        expected = replace_once(sql, "A.Id in WithItemLinkItemIds", replacement);
        rsh_run_emby_matrix_sql_exact(
            context, matrix_case->label, sql, expected,
            matrix_case->case_id == EMBY_FANOUT_MATRIX_BROWSE_V3
                ? RSH_PREPARE_V3 : RSH_PREPARE_V2,
            RSH_NBYTE_MINUS_ONE
        );
    } else if (matrix_case->case_id == EMBY_FANOUT_MATRIX_FAVORITES) {
        sql = make_favorites_sql();
        one = make_ancestor_exists_splice("100");
        two = make_exists_links_one("100", "6");
        replacement = xasprintf("(%s OR %s)", one, two);
        expected = replace_once(
            sql, "(A.Id in WithAncestors OR A.Id in WithItemLinkItemIds)",
            replacement
        );
        rsh_run_emby_matrix_sql_exact(
            context, matrix_case->label, sql, expected,
            RSH_PREPARE_V2, RSH_NBYTE_MINUS_ONE
        );
    } else if (matrix_case->case_id == EMBY_FANOUT_MATRIX_RESUME) {
        sql = make_resume_sql();
        expected = make_resume_expected(sql, "100", "1");
        rsh_run_emby_matrix_sql_exact(
            context, matrix_case->label, sql, expected,
            RSH_PREPARE_V2, RSH_NBYTE_MINUS_ONE
        );
    } else if (matrix_case->case_id == EMBY_FANOUT_MATRIX_PEOPLE) {
        sql = make_people_sql();
        replacement = make_exists_people("100");
        expected = replace_once(
            sql,
            "A.Id in (select PersonId from itemPeople2 where ItemId in WithAncestors)",
            replacement
        );
        rsh_run_emby_matrix_sql_exact(
            context, matrix_case->label, sql, expected,
            RSH_PREPARE_V2, RSH_NBYTE_MINUS_ONE
        );
    } else if (matrix_case->case_id == EMBY_FANOUT_MATRIX_LINKS_SEARCH) {
        sql = make_links_search_sql("2,3");
        replacement = make_exists_links_one("100", "2,3");
        expected = replace_once(sql, "A.Id in WithItemLinkItemIds", replacement);
        rsh_run_emby_matrix_sql_exact(
            context, matrix_case->label, sql, expected,
            RSH_PREPARE_V2, RSH_NBYTE_MINUS_ONE
        );
    } else if (matrix_case->case_id ==
               EMBY_FANOUT_MATRIX_SEARCH_SHAPE_NOMISFIRE) {
        rsh_run_emby_matrix_static_negative(
            context, matrix_case->label, emby_fts_env_sql,
            "fts_search9 match @SearchTerm"
        );
    } else {
        failf("FAIL [fanout-matrix/case-id]: got=%d", matrix_case->case_id);
    }

    free(replacement);
    free(two);
    free(one);
    free(expected);
    free(sql);
    return SQLITE_OK;
}

static const rsh_db_spec emby_matrix_base_dbs[] = {
    {
        .role = "vendor",
        .relative_path = "not-target.db",
        .kind = RSH_DB_VENDOR,
        .storage = RSH_DB_RELATIVE
    },
    {
        .role = "candidate",
        .relative_path = "library.db",
        .kind = RSH_DB_CANDIDATE,
        .storage = RSH_DB_RELATIVE
    }
};

static const rsh_matrix_cell_step emby_matrix_assert_steps[] = {
    {.kind = RSH_CELL_ASSERT}
};

#define EMBY_MATRIX_AXIS_VALUE(case_rows, row_index, case_label, case_number) \
    { \
        .label = (case_label), \
        .integer = (case_number), \
        .immutable_data = &(case_rows)[row_index] \
    }

static const emby_matrix_case emby_fanout_matrix_first_cases[] = {
    {"fanout-browse", EMBY_FANOUT_MATRIX_BROWSE_V2},
    {"fanout-browse-v3", EMBY_FANOUT_MATRIX_BROWSE_V3},
    {"fanout-favorites", EMBY_FANOUT_MATRIX_FAVORITES}
};
static const rsh_matrix_axis_value emby_fanout_matrix_first_values[] = {
    EMBY_MATRIX_AXIS_VALUE(
        emby_fanout_matrix_first_cases, 0, "fanout-browse",
        EMBY_FANOUT_MATRIX_BROWSE_V2
    ),
    EMBY_MATRIX_AXIS_VALUE(
        emby_fanout_matrix_first_cases, 1, "fanout-browse-v3",
        EMBY_FANOUT_MATRIX_BROWSE_V3
    ),
    EMBY_MATRIX_AXIS_VALUE(
        emby_fanout_matrix_first_cases, 2, "fanout-favorites",
        EMBY_FANOUT_MATRIX_FAVORITES
    )
};
static const rsh_matrix_axis emby_fanout_matrix_first_axes[] = {
    {
        .name = "case",
        .values = emby_fanout_matrix_first_values,
        .value_count = sizeof(emby_fanout_matrix_first_values) /
                       sizeof(emby_fanout_matrix_first_values[0])
    }
};

static const emby_matrix_case emby_fanout_matrix_second_cases[] = {
    {"fanout-resume", EMBY_FANOUT_MATRIX_RESUME},
    {"fanout-people", EMBY_FANOUT_MATRIX_PEOPLE},
    {"fanout-links-search", EMBY_FANOUT_MATRIX_LINKS_SEARCH}
};
static const rsh_matrix_axis_value emby_fanout_matrix_second_values[] = {
    EMBY_MATRIX_AXIS_VALUE(
        emby_fanout_matrix_second_cases, 0, "fanout-resume",
        EMBY_FANOUT_MATRIX_RESUME
    ),
    EMBY_MATRIX_AXIS_VALUE(
        emby_fanout_matrix_second_cases, 1, "fanout-people",
        EMBY_FANOUT_MATRIX_PEOPLE
    ),
    EMBY_MATRIX_AXIS_VALUE(
        emby_fanout_matrix_second_cases, 2, "fanout-links-search",
        EMBY_FANOUT_MATRIX_LINKS_SEARCH
    )
};
static const rsh_matrix_axis emby_fanout_matrix_second_axes[] = {
    {
        .name = "case",
        .values = emby_fanout_matrix_second_values,
        .value_count = sizeof(emby_fanout_matrix_second_values) /
                       sizeof(emby_fanout_matrix_second_values[0])
    }
};

static const emby_matrix_case emby_fanout_matrix_third_cases[] = {
    {"fanout-search-shape-nomisfire",
     EMBY_FANOUT_MATRIX_SEARCH_SHAPE_NOMISFIRE}
};
static const rsh_matrix_axis_value emby_fanout_matrix_third_values[] = {
    EMBY_MATRIX_AXIS_VALUE(
        emby_fanout_matrix_third_cases, 0,
        "fanout-search-shape-nomisfire",
        EMBY_FANOUT_MATRIX_SEARCH_SHAPE_NOMISFIRE
    )
};
static const rsh_matrix_axis emby_fanout_matrix_third_axes[] = {
    {
        .name = "case",
        .values = emby_fanout_matrix_third_values,
        .value_count = sizeof(emby_fanout_matrix_third_values) /
                       sizeof(emby_fanout_matrix_third_values[0])
    }
};

#define EMBY_DYNAMIC_MATRIX_PHASE( \
    phase_label, prefix, axis_rows, db_rows, adapter, cell_count) \
    { \
        .label = (phase_label), \
        .cell_dir_prefix = (prefix), \
        .axes = (axis_rows), \
        .axis_count = sizeof(axis_rows) / sizeof((axis_rows)[0]), \
        .dbs = (db_rows), \
        .db_count = sizeof(db_rows) / sizeof((db_rows)[0]), \
        .steps = emby_matrix_assert_steps, \
        .step_count = sizeof(emby_matrix_assert_steps) / \
                      sizeof(emby_matrix_assert_steps[0]), \
        .assertion_adapter = (adapter), \
        .expected_cells = (cell_count) \
    }

static const rsh_matrix_phase_spec emby_fanout_matrix_phases[] = {
    EMBY_DYNAMIC_MATRIX_PHASE(
        "fanout-first", "fanout-first", emby_fanout_matrix_first_axes,
        emby_matrix_base_dbs, rsh_matrix_assert_emby_fanout, 3
    ),
    EMBY_DYNAMIC_MATRIX_PHASE(
        "fanout-second", "fanout-second", emby_fanout_matrix_second_axes,
        emby_matrix_base_dbs, rsh_matrix_assert_emby_fanout, 3
    ),
    EMBY_DYNAMIC_MATRIX_PHASE(
        "fanout-third", "fanout-third", emby_fanout_matrix_third_axes,
        emby_matrix_base_dbs, rsh_matrix_assert_emby_fanout, 1
    )
};

static const char emby_slow_resume_complex_missing_not_null_sql[] =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) )select count(*) OVER() AS TotalRecordCount,A.type,A.Id,A.SeriesPresentationUniqueKey,UserDatas.PlaybackPositionTicks,"
    "((Coalesce(A.SortParentIndexNumber,A.ParentIndexNumber, 1) * 1000000) + Coalesce(A.SortIndexNumber, A.IndexNumber, 0) + (Select Case When Coalesce(A.ParentIndexNumber,1)=0 Then 0 Else 0.5 End) + (Select Case When Coalesce(A.ParentIndexNumber,1)=0 Then (Cast(Coalesce(A.IndexNumber, 0) as REAL) / 100000) Else 0 End)) EpisodeAbsoluteIndexNumber "
    "from mediaitems A left join (Select N.SeriesPresentationUniqueKey,((Coalesce(N.SortParentIndexNumber,N.ParentIndexNumber, 1) * 1000000) + Coalesce(N.SortIndexNumber, N.IndexNumber, 0) + (Select Case When Coalesce(N.ParentIndexNumber,1)=0 Then 0 Else 0.5 End) + (Select Case When Coalesce(N.ParentIndexNumber,1)=0 Then (Cast(Coalesce(N.IndexNumber, 0) as REAL) / 100000) Else 0 End)) AbsoluteIndexNumber,max(UserDatas_N.LastPlayedDateInt) LastPlayedDateInt,UserDatas_N.playbackPositionTicks from MediaItems N join UserDatas UserDatas_N on N.UserDataKeyId=UserDatas_N.UserDataKeyId And UserDatas_N.UserId=1 where N.Type=8 and Coalesce(N.SortParentIndexNumber,N.ParentIndexNumber,-1) <> 0 and (UserDatas_N.Played=1 or UserDatas_N.playbackPositionTicks > 0) Group By N.SeriesPresentationUniqueKey ORDER BY UserDatas_N.LastPlayedDateInt desc, AbsoluteIndexNumber desc) LastWatchedEpisodes on LastWatchedEpisodes.SeriesPresentationUniqueKey=A.SeriesPresentationUniqueKey "
    "left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=1 where ((A.Type=5 and UserDatas.playbackPositionTicks > 0) OR (A.Type=8 AND (UserDatas.playbackPositionTicks > 0 or Coalesce(UserDatas.played,0) = 0) AND (select case when LastWatchedEpisodes.playbackPositionTicks > 0 then EpisodeAbsoluteIndexNumber >= Coalesce(LastWatchedEpisodes.AbsoluteIndexNumber,EpisodeAbsoluteIndexNumber) else EpisodeAbsoluteIndexNumber > Coalesce(LastWatchedEpisodes.AbsoluteIndexNumber,EpisodeAbsoluteIndexNumber) end))) "
    "AND (A.Type=5 OR Coalesce(A.SortParentIndexNumber,A.ParentIndexNumber, -1) <> 0) AND A.Type in (5,8) AND Coalesce(UserDatas.HideFromResume,0)=0 AND COALESCE((select hidefromresume from userdatas where userdatas.userid=1 and userdatas.userdatakeyid=(select userdatakeyid from mediaitems where mediaitems.id=A.seriesid)),0)=0 AND A.Id in WithAncestors Group by coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey) ORDER BY COALESCE(lastwatchedepisodes.lastplayeddateint, userdatas.lastplayeddateint, 0) DESC,Min(EpisodeAbsoluteIndexNumber) ASC LIMIT 12";

enum emby_slow_matrix_case_id {
    EMBY_SLOW_RESUME_SIMPLE_COUNT = 1,
    EMBY_SLOW_RESUME_SIMPLE_BARE,
    EMBY_SLOW_RESUME_SIMPLE_UNPLAYED_NEGATIVE,
    EMBY_SLOW_RESUME_SIMPLE_SHAPE_07_NEGATIVE,
    EMBY_SLOW_RESUME_SIMPLE_BAD_L1_NEGATIVE,
    EMBY_SLOW_RESUME_SIMPLE_BAD_LIMIT_NEGATIVE,
    EMBY_SLOW_RESUME_SIMPLE_STRING_EMBEDDED,
    EMBY_SLOW_RESUME_SIMPLE_STRING_ONLY_NEGATIVE,
    EMBY_SLOW_RESUME_SIMPLE_DUPLICATE_NEGATIVE,
    EMBY_SLOW_RESUME_COMPLEX_COUNT,
    EMBY_SLOW_RESUME_COMPLEX_MISSING_NOT_NULL_NEGATIVE,
    EMBY_SLOW_RESUME_COMPLEX_COMMENT_EMBEDDED,
    EMBY_SLOW_RESUME_COMPLEX_0065,
    EMBY_SLOW_RESUME_COMPLEX_BOUND_USER_NEGATIVE,
    EMBY_SLOW_RESUME_COMPLEX_NAMED_USER_NEGATIVE,
    EMBY_SLOW_RESUME_COMPLEX_DERIVED_PARAM_NEGATIVE,
    EMBY_SLOW_RESUME_COMPLEX_CONFLICTING_USER_NEGATIVE,
    EMBY_SLOW_RESUME_COMPLEX_DUPLICATE_NEGATIVE,
    EMBY_SLOW_SIMILAR,
    EMBY_SLOW_SIMILAR_COMMENT_EMBEDDED,
    EMBY_SLOW_SIMILAR_COMMENT_ONLY_NEGATIVE,
    EMBY_SLOW_PEOPLE,
    EMBY_SLOW_LINKS_SEARCH,
    EMBY_SLOW_LINK_TYPE_COUNT,
    EMBY_SLOW_LINK_TYPE_COUNT_BAD_T1,
    EMBY_SLOW_LINK_TYPE_COUNT_BAD_T2,
    EMBY_SLOW_LINK_TYPE_COUNT_DUPLICATE,
    EMBY_SLOW_FTS_NEGATIVE,
    EMBY_SLOW_LATEST_NEGATIVE
};

static int rsh_matrix_assert_emby_slow_search(
    const rsh_case_context *context,
    const rsh_matrix_cell_step *step,
    void *ctx
) {
    const emby_matrix_case *matrix_case = rsh_emby_matrix_case(context);
    char *base = NULL;
    char *expected_base = NULL;
    char *sql = NULL;
    char *expected = NULL;
    char *replacement = NULL;

    (void)step;
    (void)ctx;
    switch (matrix_case->case_id) {
    case EMBY_SLOW_RESUME_SIMPLE_COUNT:
        sql = make_resume_simple_sql(1, "12");
        expected = make_resume_simple_expected(sql);
        break;
    case EMBY_SLOW_RESUME_SIMPLE_BARE:
        sql = make_resume_simple_sql(0, "24");
        expected = make_resume_simple_expected(sql);
        break;
    case EMBY_SLOW_RESUME_SIMPLE_UNPLAYED_NEGATIVE:
        base = make_resume_simple_sql(1, "12");
        rsh_run_emby_matrix_generated_negative(
            context, matrix_case->label, base,
            "UserDatas.playbackPositionTicks > 0",
            "Coalesce(UserDatas.played, 0)=0",
            "Coalesce(UserDatas.played, 0)=0"
        );
        break;
    case EMBY_SLOW_RESUME_SIMPLE_SHAPE_07_NEGATIVE:
        rsh_run_emby_matrix_static_negative(
            context, matrix_case->label,
            EMBY_RESUME_SIMPLE_LEFT_JOIN_UNPLAYED_SHAPE_07_SQL,
            "AncestorId in (9,10,11,12,13,14,101,102,103)"
        );
        break;
    case EMBY_SLOW_RESUME_SIMPLE_BAD_L1_NEGATIVE:
        base = make_resume_simple_sql(1, "12");
        rsh_run_emby_matrix_generated_negative(
            context, matrix_case->label, base,
            "AncestorId in (100)", "AncestorId in (@AncestorId)",
            "AncestorId in (@AncestorId)"
        );
        break;
    case EMBY_SLOW_RESUME_SIMPLE_BAD_LIMIT_NEGATIVE:
        base = make_resume_simple_sql(1, "12");
        rsh_run_emby_matrix_generated_negative(
            context, matrix_case->label, base,
            "LIMIT 12", "LIMIT @Limit", "LIMIT @Limit"
        );
        break;
    case EMBY_SLOW_RESUME_SIMPLE_STRING_EMBEDDED:
        base = make_resume_simple_sql(1, "12");
        expected_base = make_resume_simple_expected(base);
        sql = replace_once(
            base, "AND A.Id in WithAncestors",
            "AND 'A.Id in WithAncestors' IS NOT NULL AND A.Id in WithAncestors"
        );
        expected = replace_once(
            expected_base, "AND EXISTS (SELECT 1 FROM AncestorIds2",
            "AND 'A.Id in WithAncestors' IS NOT NULL AND EXISTS (SELECT 1 FROM AncestorIds2"
        );
        break;
    case EMBY_SLOW_RESUME_SIMPLE_STRING_ONLY_NEGATIVE:
        base = make_resume_simple_sql(1, "12");
        rsh_run_emby_matrix_generated_negative(
            context, matrix_case->label, base,
            "AND A.Id in WithAncestors",
            "AND 'A.Id in WithAncestors' IS NOT NULL",
            "AND 'A.Id in WithAncestors' IS NOT NULL Group by"
        );
        break;
    case EMBY_SLOW_RESUME_SIMPLE_DUPLICATE_NEGATIVE:
        base = make_resume_simple_sql(1, "12");
        rsh_run_emby_matrix_generated_negative(
            context, matrix_case->label, base,
            "AND A.Id in WithAncestors",
            "AND A.Id in WithAncestors AND A.Id in WithAncestors",
            "AND A.Id in WithAncestors AND A.Id in WithAncestors"
        );
        break;
    case EMBY_SLOW_RESUME_COMPLEX_COUNT:
        sql = make_resume_sql();
        expected = make_resume_expected(sql, "100", "1");
        break;
    case EMBY_SLOW_RESUME_COMPLEX_MISSING_NOT_NULL_NEGATIVE:
        rsh_run_emby_matrix_static_negative(
            context, matrix_case->label,
            emby_slow_resume_complex_missing_not_null_sql,
            "end))) AND (A.Type=5 OR"
        );
        break;
    case EMBY_SLOW_RESUME_COMPLEX_COMMENT_EMBEDDED:
        base = make_resume_sql();
        expected_base = make_resume_expected(base, "100", "1");
        sql = replace_once(
            base, "AND A.Id in WithAncestors Group by",
            "AND /* A.Id in WithAncestors */ A.Id in WithAncestors Group by"
        );
        expected = replace_once(
            expected_base, "AND EXISTS (SELECT 1 FROM AncestorIds2",
            "AND /* A.Id in WithAncestors */ EXISTS (SELECT 1 FROM AncestorIds2"
        );
        break;
    case EMBY_SLOW_RESUME_COMPLEX_0065:
        sql = make_resume_0065_sql();
        expected = make_resume_expected(sql, "100", "13");
        break;
    case EMBY_SLOW_RESUME_COMPLEX_BOUND_USER_NEGATIVE:
        base = make_resume_sql();
        rsh_run_emby_matrix_generated_negative(
            context, matrix_case->label, base,
            "UserDatas.UserId=1", "UserDatas.UserId=?1",
            "UserDatas.UserId=?1"
        );
        break;
    case EMBY_SLOW_RESUME_COMPLEX_NAMED_USER_NEGATIVE:
        base = make_resume_sql();
        rsh_run_emby_matrix_generated_negative(
            context, matrix_case->label, base,
            "userdatas.userid=1", "userdatas.userid=@UserId",
            "userdatas.userid=@UserId"
        );
        break;
    case EMBY_SLOW_RESUME_COMPLEX_DERIVED_PARAM_NEGATIVE:
        base = make_resume_sql();
        rsh_run_emby_matrix_generated_negative(
            context, matrix_case->label, base,
            "UserDatas_N.UserId=1", "UserDatas_N.UserId=:UserId",
            "UserDatas_N.UserId=:UserId"
        );
        break;
    case EMBY_SLOW_RESUME_COMPLEX_CONFLICTING_USER_NEGATIVE:
        base = make_resume_sql();
        rsh_run_emby_matrix_generated_negative(
            context, matrix_case->label, base,
            "userdatas.userid=1", "userdatas.userid=2",
            "userdatas.userid=2"
        );
        break;
    case EMBY_SLOW_RESUME_COMPLEX_DUPLICATE_NEGATIVE:
        base = make_resume_sql();
        rsh_run_emby_matrix_generated_negative(
            context, matrix_case->label, base,
            "AND A.Id in WithAncestors",
            "AND A.Id in WithAncestors AND A.Id in WithAncestors",
            "AND A.Id in WithAncestors AND A.Id in WithAncestors"
        );
        break;
    case EMBY_SLOW_SIMILAR:
        sql = make_similar_sql();
        expected = make_similar_expected(sql);
        break;
    case EMBY_SLOW_SIMILAR_COMMENT_EMBEDDED:
        base = make_similar_sql();
        expected_base = make_similar_expected(base);
        sql = replace_once(
            base, "AND A.Id in WithAncestors",
            "AND /* A.Id in WithAncestors */ A.Id in WithAncestors"
        );
        expected = replace_once(
            expected_base, "AND EXISTS (SELECT 1 FROM AncestorIds2",
            "AND /* A.Id in WithAncestors */ EXISTS (SELECT 1 FROM AncestorIds2"
        );
        break;
    case EMBY_SLOW_SIMILAR_COMMENT_ONLY_NEGATIVE:
        base = make_similar_sql();
        rsh_run_emby_matrix_generated_negative(
            context, matrix_case->label, base,
            "AND A.Id in WithAncestors",
            "AND /* A.Id in WithAncestors */ 1=1",
            "AND /* A.Id in WithAncestors */ 1=1"
        );
        break;
    case EMBY_SLOW_PEOPLE:
        sql = make_people_sql();
        replacement = make_exists_people("100");
        expected = replace_once(
            sql,
            "A.Id in (select PersonId from itemPeople2 where ItemId in WithAncestors)",
            replacement
        );
        break;
    case EMBY_SLOW_LINKS_SEARCH:
        sql = make_links_search_sql("2,3");
        replacement = make_exists_links_one("100", "2,3");
        expected = replace_once(sql, "A.Id in WithItemLinkItemIds", replacement);
        break;
    case EMBY_SLOW_LINK_TYPE_COUNT:
        sql = xasprintf("%s", EMBY_LINK_TYPE_COUNT_SHAPE_05_SQL);
        expected = make_link_type_count_shape_05_expected();
        break;
    case EMBY_SLOW_LINK_TYPE_COUNT_BAD_T1:
        rsh_run_emby_matrix_generated_negative(
            context, matrix_case->label, EMBY_LINK_TYPE_COUNT_SHAPE_05_SQL,
            "Type in (6,4,3,2) union", "Type in (6,'bad') union",
            "Type in (6,'bad') union"
        );
        break;
    case EMBY_SLOW_LINK_TYPE_COUNT_BAD_T2:
        rsh_run_emby_matrix_generated_negative(
            context, matrix_case->label, EMBY_LINK_TYPE_COUNT_SHAPE_05_SQL,
            "Type in (7,0,1,5,6,2)))select", "Type in (7,'bad')))select",
            "Type in (7,'bad')))select"
        );
        break;
    case EMBY_SLOW_LINK_TYPE_COUNT_DUPLICATE:
        rsh_run_emby_matrix_generated_negative(
            context, matrix_case->label, EMBY_LINK_TYPE_COUNT_SHAPE_05_SQL,
            "AND A.Id in WithItemLinkItemIds Group by",
            "AND A.Id in WithItemLinkItemIds AND A.Id in WithItemLinkItemIds Group by",
            "AND A.Id in WithItemLinkItemIds AND A.Id in WithItemLinkItemIds"
        );
        break;
    case EMBY_SLOW_FTS_NEGATIVE:
        rsh_run_emby_matrix_static_negative(
            context, matrix_case->label, emby_fts_env_sql,
            "fts_search9 match @SearchTerm"
        );
        break;
    case EMBY_SLOW_LATEST_NEGATIVE:
        require_absent(
            "fanout-latest-negative/indexed-by",
            emby_dashboard_episodes_off_sql,
            "INDEXED BY idx_dshadow_emby_latest_gk_dc"
        );
        rsh_run_emby_matrix_static_negative(
            context, matrix_case->label, emby_dashboard_episodes_off_sql,
            "ORDER BY MAX(A.DateCreated) DESC LIMIT 12"
        );
        break;
    default:
        failf("FAIL [emby-slow-search-matrix/case-id]: got=%d",
              matrix_case->case_id);
    }

    if (sql) {
        rsh_run_emby_matrix_sql_exact(
            context, matrix_case->label, sql, expected,
            RSH_PREPARE_V2, RSH_NBYTE_MINUS_ONE
        );
    }
    free(replacement);
    free(expected);
    free(sql);
    free(expected_base);
    free(base);
    return SQLITE_OK;
}

#define EMBY_DEFINE_MATRIX_AXIS_1(axis_id, label_1, id_1) \
    static const emby_matrix_case axis_id##_cases[] = { \
        {(label_1), (id_1)} \
    }; \
    static const rsh_matrix_axis_value axis_id##_values[] = { \
        EMBY_MATRIX_AXIS_VALUE(axis_id##_cases, 0, (label_1), (id_1)) \
    }; \
    static const rsh_matrix_axis axis_id##_axes[] = { \
        { \
            .name = "case", \
            .values = axis_id##_values, \
            .value_count = sizeof(axis_id##_values) / \
                           sizeof(axis_id##_values[0]) \
        } \
    }

#define EMBY_DEFINE_MATRIX_AXIS_2( \
    axis_id, label_1, id_1, label_2, id_2) \
    static const emby_matrix_case axis_id##_cases[] = { \
        {(label_1), (id_1)}, \
        {(label_2), (id_2)} \
    }; \
    static const rsh_matrix_axis_value axis_id##_values[] = { \
        EMBY_MATRIX_AXIS_VALUE(axis_id##_cases, 0, (label_1), (id_1)), \
        EMBY_MATRIX_AXIS_VALUE(axis_id##_cases, 1, (label_2), (id_2)) \
    }; \
    static const rsh_matrix_axis axis_id##_axes[] = { \
        { \
            .name = "case", \
            .values = axis_id##_values, \
            .value_count = sizeof(axis_id##_values) / \
                           sizeof(axis_id##_values[0]) \
        } \
    }

#define EMBY_DEFINE_MATRIX_AXIS_3( \
    axis_id, label_1, id_1, label_2, id_2, label_3, id_3) \
    static const emby_matrix_case axis_id##_cases[] = { \
        {(label_1), (id_1)}, \
        {(label_2), (id_2)}, \
        {(label_3), (id_3)} \
    }; \
    static const rsh_matrix_axis_value axis_id##_values[] = { \
        EMBY_MATRIX_AXIS_VALUE(axis_id##_cases, 0, (label_1), (id_1)), \
        EMBY_MATRIX_AXIS_VALUE(axis_id##_cases, 1, (label_2), (id_2)), \
        EMBY_MATRIX_AXIS_VALUE(axis_id##_cases, 2, (label_3), (id_3)) \
    }; \
    static const rsh_matrix_axis axis_id##_axes[] = { \
        { \
            .name = "case", \
            .values = axis_id##_values, \
            .value_count = sizeof(axis_id##_values) / \
                           sizeof(axis_id##_values[0]) \
        } \
    }

EMBY_DEFINE_MATRIX_AXIS_3(
    emby_slow_group_1,
    "resume-simple-count", EMBY_SLOW_RESUME_SIMPLE_COUNT,
    "resume-simple-bare", EMBY_SLOW_RESUME_SIMPLE_BARE,
    "resume-simple-unplayed-negative",
    EMBY_SLOW_RESUME_SIMPLE_UNPLAYED_NEGATIVE
);
EMBY_DEFINE_MATRIX_AXIS_3(
    emby_slow_group_2,
    "resume-simple-shape-07-547bab3e-negative",
    EMBY_SLOW_RESUME_SIMPLE_SHAPE_07_NEGATIVE,
    "resume-simple-bad-l1-negative", EMBY_SLOW_RESUME_SIMPLE_BAD_L1_NEGATIVE,
    "resume-simple-bad-limit-negative",
    EMBY_SLOW_RESUME_SIMPLE_BAD_LIMIT_NEGATIVE
);
EMBY_DEFINE_MATRIX_AXIS_3(
    emby_slow_group_3,
    "resume-simple-string-embedded-immunity",
    EMBY_SLOW_RESUME_SIMPLE_STRING_EMBEDDED,
    "resume-simple-string-only-negative",
    EMBY_SLOW_RESUME_SIMPLE_STRING_ONLY_NEGATIVE,
    "resume-simple-duplicate-negative",
    EMBY_SLOW_RESUME_SIMPLE_DUPLICATE_NEGATIVE
);
EMBY_DEFINE_MATRIX_AXIS_3(
    emby_slow_group_4,
    "resume-complex-count", EMBY_SLOW_RESUME_COMPLEX_COUNT,
    "resume-complex-missing-lastwatched-not-null-negative",
    EMBY_SLOW_RESUME_COMPLEX_MISSING_NOT_NULL_NEGATIVE,
    "resume-complex-comment-embedded-immunity",
    EMBY_SLOW_RESUME_COMPLEX_COMMENT_EMBEDDED
);
EMBY_DEFINE_MATRIX_AXIS_3(
    emby_slow_group_5,
    "resume-complex-0065", EMBY_SLOW_RESUME_COMPLEX_0065,
    "resume-complex-bound-user-negative",
    EMBY_SLOW_RESUME_COMPLEX_BOUND_USER_NEGATIVE,
    "resume-complex-named-user-negative",
    EMBY_SLOW_RESUME_COMPLEX_NAMED_USER_NEGATIVE
);
EMBY_DEFINE_MATRIX_AXIS_3(
    emby_slow_group_6,
    "resume-complex-derived-param-negative",
    EMBY_SLOW_RESUME_COMPLEX_DERIVED_PARAM_NEGATIVE,
    "resume-complex-conflicting-user-negative",
    EMBY_SLOW_RESUME_COMPLEX_CONFLICTING_USER_NEGATIVE,
    "resume-complex-duplicate-negative",
    EMBY_SLOW_RESUME_COMPLEX_DUPLICATE_NEGATIVE
);
EMBY_DEFINE_MATRIX_AXIS_3(
    emby_slow_group_7,
    "fanout-similar", EMBY_SLOW_SIMILAR,
    "similar-comment-embedded-immunity", EMBY_SLOW_SIMILAR_COMMENT_EMBEDDED,
    "similar-comment-only-negative", EMBY_SLOW_SIMILAR_COMMENT_ONLY_NEGATIVE
);
EMBY_DEFINE_MATRIX_AXIS_3(
    emby_slow_group_8,
    "similar-people-negative", EMBY_SLOW_PEOPLE,
    "similar-links-search-negative", EMBY_SLOW_LINKS_SEARCH,
    "links-type-count-shape-05-two-level", EMBY_SLOW_LINK_TYPE_COUNT
);
EMBY_DEFINE_MATRIX_AXIS_3(
    emby_slow_group_9,
    "links-type-count-shape-05-bad-t1", EMBY_SLOW_LINK_TYPE_COUNT_BAD_T1,
    "links-type-count-shape-05-bad-t2", EMBY_SLOW_LINK_TYPE_COUNT_BAD_T2,
    "links-type-count-shape-05-duplicate-membership",
    EMBY_SLOW_LINK_TYPE_COUNT_DUPLICATE
);
EMBY_DEFINE_MATRIX_AXIS_2(
    emby_slow_group_10,
    "fanout-search-fts-negative", EMBY_SLOW_FTS_NEGATIVE,
    "fanout-latest-negative", EMBY_SLOW_LATEST_NEGATIVE
);

static const rsh_matrix_phase_spec emby_slow_search_matrix_phases[] = {
    EMBY_DYNAMIC_MATRIX_PHASE(
        "slow-group-1", "slow-group-1", emby_slow_group_1_axes,
        emby_matrix_base_dbs, rsh_matrix_assert_emby_slow_search, 3
    ),
    EMBY_DYNAMIC_MATRIX_PHASE(
        "slow-group-2", "slow-group-2", emby_slow_group_2_axes,
        emby_matrix_base_dbs, rsh_matrix_assert_emby_slow_search, 3
    ),
    EMBY_DYNAMIC_MATRIX_PHASE(
        "slow-group-3", "slow-group-3", emby_slow_group_3_axes,
        emby_matrix_base_dbs, rsh_matrix_assert_emby_slow_search, 3
    ),
    EMBY_DYNAMIC_MATRIX_PHASE(
        "slow-group-4", "slow-group-4", emby_slow_group_4_axes,
        emby_matrix_base_dbs, rsh_matrix_assert_emby_slow_search, 3
    ),
    EMBY_DYNAMIC_MATRIX_PHASE(
        "slow-group-5", "slow-group-5", emby_slow_group_5_axes,
        emby_matrix_base_dbs, rsh_matrix_assert_emby_slow_search, 3
    ),
    EMBY_DYNAMIC_MATRIX_PHASE(
        "slow-group-6", "slow-group-6", emby_slow_group_6_axes,
        emby_matrix_base_dbs, rsh_matrix_assert_emby_slow_search, 3
    ),
    EMBY_DYNAMIC_MATRIX_PHASE(
        "slow-group-7", "slow-group-7", emby_slow_group_7_axes,
        emby_matrix_base_dbs, rsh_matrix_assert_emby_slow_search, 3
    ),
    EMBY_DYNAMIC_MATRIX_PHASE(
        "slow-group-8", "slow-group-8", emby_slow_group_8_axes,
        emby_matrix_base_dbs, rsh_matrix_assert_emby_slow_search, 3
    ),
    EMBY_DYNAMIC_MATRIX_PHASE(
        "slow-group-9", "slow-group-9", emby_slow_group_9_axes,
        emby_matrix_base_dbs, rsh_matrix_assert_emby_slow_search, 3
    ),
    EMBY_DYNAMIC_MATRIX_PHASE(
        "slow-group-10", "slow-group-10", emby_slow_group_10_axes,
        emby_matrix_base_dbs, rsh_matrix_assert_emby_slow_search, 2
    )
};

enum emby_dashboard_matrix_case_id {
    EMBY_DASHBOARD_MATRIX_LATEST_12 = 1,
    EMBY_DASHBOARD_MATRIX_LATEST_12_NUL,
    EMBY_DASHBOARD_MATRIX_LATEST_16,
    EMBY_DASHBOARD_MATRIX_MIXED_CONTROL,
    EMBY_DASHBOARD_MATRIX_DISTINCT_NEGATIVE,
    EMBY_DASHBOARD_MATRIX_AGGREGATE_NEGATIVE,
    EMBY_DASHBOARD_MATRIX_OVER_NEGATIVE,
    EMBY_DASHBOARD_MATRIX_MOVIES_12,
    EMBY_DASHBOARD_MATRIX_MOVIES_16,
    EMBY_DASHBOARD_MATRIX_MOVIES_20,
    EMBY_DASHBOARD_MATRIX_EPISODES_DATE_INDEX_MISSING,
    EMBY_DASHBOARD_MATRIX_EPISODES_GROUP_INDEX_MISSING,
    EMBY_DASHBOARD_MATRIX_EPISODES_BOTH_INDEXES_MISSING,
    EMBY_DASHBOARD_MATRIX_MOVIES_OUTER_INDEX_MISSING,
    EMBY_DASHBOARD_MATRIX_MOVIES_INNER_INDEX_MISSING
};

static int rsh_matrix_assert_emby_dashboard(
    const rsh_case_context *context,
    const rsh_matrix_cell_step *step,
    void *ctx
) {
    const emby_matrix_case *matrix_case = rsh_emby_matrix_case(context);
    sqlite3 *candidate = rsh_context_db(context, "candidate");
    char *sql = NULL;
    char *expected = NULL;

    (void)step;
    (void)ctx;
    switch (matrix_case->case_id) {
    case EMBY_DASHBOARD_MATRIX_LATEST_12:
        sql = make_latest_sql("A.Id,A.SeriesName,A.SortName ", "12");
        expected = make_latest_expected("A.Id,A.SeriesName,A.SortName ", "12");
        require_absent(
            "dashboard-latest-original/indexed-by", sql,
            "INDEXED BY idx_dshadow_emby_latest_gk_dc"
        );
        require_int(
            "dashboard-latest-expected/outer-index-count",
            count_occurrences(
                expected,
                "INDEXED BY idx_dshadow_emby_latest_episodes_dcn_gk"
            ),
            1
        );
        require_int(
            "dashboard-latest-expected/inner-index-count",
            count_occurrences(
                expected, "INDEXED BY idx_dshadow_emby_latest_gk_dc"
            ),
            1
        );
        require_contains(
            "dashboard-latest-expected/ranked", expected,
            "WITH ranked(id, dc, gk) AS MATERIALIZED ("
        );
        require_absent(
            "dashboard-latest-expected/keys", expected, "WITH keys(gk)"
        );
        require_absent(
            "dashboard-latest-expected/picked", expected,
            "picked AS MATERIALIZED"
        );
        require_absent(
            "dashboard-latest-expected/exact-groups", expected,
            "exact_groups AS MATERIALIZED"
        );
        rsh_run_emby_matrix_sql_exact(
            context, matrix_case->label, sql, expected,
            RSH_PREPARE_V2, RSH_NBYTE_MINUS_ONE
        );
        break;
    case EMBY_DASHBOARD_MATRIX_LATEST_12_NUL:
        sql = make_latest_sql("A.Id,A.SeriesName,A.SortName ", "12");
        expected = make_latest_expected("A.Id,A.SeriesName,A.SortName ", "12");
        rsh_run_emby_matrix_sql_exact(
            context, matrix_case->label, sql, expected,
            RSH_PREPARE_V2, RSH_NBYTE_LENGTH_WITH_NUL
        );
        break;
    case EMBY_DASHBOARD_MATRIX_LATEST_16:
        sql = make_latest_sql("A.Id ", "16");
        expected = make_latest_expected("A.Id ", "16");
        require_int(
            "dashboard-latest-limit16/outer-index-count",
            count_occurrences(
                expected,
                "INDEXED BY idx_dshadow_emby_latest_episodes_dcn_gk"
            ),
            1
        );
        require_int(
            "dashboard-latest-limit16/inner-index-count",
            count_occurrences(
                expected, "INDEXED BY idx_dshadow_emby_latest_gk_dc"
            ),
            1
        );
        require_contains(
            "dashboard-latest-limit16/ranked", expected,
            "WITH ranked(id, dc, gk) AS MATERIALIZED ("
        );
        require_absent(
            "dashboard-latest-limit16/keys", expected, "WITH keys(gk)"
        );
        require_absent(
            "dashboard-latest-limit16/picked", expected,
            "picked AS MATERIALIZED"
        );
        require_absent(
            "dashboard-latest-limit16/exact-groups", expected,
            "exact_groups AS MATERIALIZED"
        );
        rsh_run_emby_matrix_sql_exact(
            context, matrix_case->label, sql, expected,
            RSH_PREPARE_V2, RSH_NBYTE_MINUS_ONE
        );
        break;
    case EMBY_DASHBOARD_MATRIX_MIXED_CONTROL:
        sql = make_mixed_latest_sql("42", "3");
        expected = make_mixed_latest_expected("42", "3");
        expect_mixed_latest_sql(
            candidate, matrix_case->label, sql, expected, 0, 0
        );
        break;
    case EMBY_DASHBOARD_MATRIX_DISTINCT_NEGATIVE:
        sql = make_latest_sql("DISTINCT A.Id ", "12");
        rsh_run_emby_matrix_static_negative(
            context, matrix_case->label, sql, "select DISTINCT A.Id from"
        );
        break;
    case EMBY_DASHBOARD_MATRIX_AGGREGATE_NEGATIVE:
        sql = make_latest_sql("MAX(A.DateCreated) ", "12");
        rsh_run_emby_matrix_static_negative(
            context, matrix_case->label, sql,
            "select MAX(A.DateCreated) from"
        );
        break;
    case EMBY_DASHBOARD_MATRIX_OVER_NEGATIVE:
        sql = make_latest_sql(
            "count(*) OVER() AS TotalRecordCount,A.Id ", "12"
        );
        rsh_run_emby_matrix_static_negative(
            context, matrix_case->label, sql,
            "select count(*) OVER() AS TotalRecordCount,A.Id from"
        );
        break;
    case EMBY_DASHBOARD_MATRIX_MOVIES_12:
    case EMBY_DASHBOARD_MATRIX_MOVIES_16:
    case EMBY_DASHBOARD_MATRIX_MOVIES_20:
        {
            const char *limit =
                matrix_case->case_id == EMBY_DASHBOARD_MATRIX_MOVIES_12
                    ? "12"
                    : matrix_case->case_id == EMBY_DASHBOARD_MATRIX_MOVIES_16
                        ? "16" : "20";
            sql = make_movies_latest_sql(1, "100", "42", limit);
            expected = make_movies_latest_expected("100", "42", limit);
            rsh_run_emby_matrix_sql_exact(
                context, matrix_case->label, sql, expected,
                RSH_PREPARE_V2, RSH_NBYTE_MINUS_ONE
            );
            if (matrix_case->case_id == EMBY_DASHBOARD_MATRIX_MOVIES_20) {
                sqlite3 *vendor = rsh_context_db(context, "vendor");
                typed_rows vendor_rows = collect_typed_rows(
                    vendor, "movies-row-original", sql, sql
                );
                typed_rows candidate_rows = collect_typed_rows(
                    candidate, "movies-row-rewritten", sql, expected
                );
                require_ordered_full_row_identity(
                    "movies-row-identity", &vendor_rows, &candidate_rows
                );
                free_typed_rows(&vendor_rows);
                free_typed_rows(&candidate_rows);
            }
        }
        break;
    case EMBY_DASHBOARD_MATRIX_EPISODES_DATE_INDEX_MISSING:
    case EMBY_DASHBOARD_MATRIX_EPISODES_GROUP_INDEX_MISSING:
    case EMBY_DASHBOARD_MATRIX_EPISODES_BOTH_INDEXES_MISSING:
        sql = make_latest_sql("A.Id,A.SeriesName,A.SortName ", "12");
        rsh_run_emby_matrix_static_negative(
            context, matrix_case->label, sql,
            "ORDER BY MAX(A.DateCreated) DESC LIMIT 12"
        );
        break;
    case EMBY_DASHBOARD_MATRIX_MOVIES_OUTER_INDEX_MISSING:
    case EMBY_DASHBOARD_MATRIX_MOVIES_INNER_INDEX_MISSING:
        sql = make_movies_latest_sql(1, "100", "42", "12");
        rsh_run_emby_matrix_static_negative(
            context, matrix_case->label, sql,
            "Group by A.PresentationUniqueKey ORDER BY A.DateCreated DESC LIMIT 12"
        );
        break;
    default:
        failf("FAIL [dashboard-matrix/case-id]: got=%d", matrix_case->case_id);
    }

    free(expected);
    free(sql);
    return SQLITE_OK;
}

static int rsh_custom_adapter_dashboard_probe_authorizer_set(
    const rsh_case_context *context,
    const void *immutable_data
) {
    sqlite3 *candidate = rsh_context_db(context, "candidate");

    (void)immutable_data;
    require_int(
        "dashboard/probe-authorizer-set",
        sqlite3_set_authorizer(candidate, deny_sqlite_master_read, NULL),
        SQLITE_OK
    );
    return SQLITE_OK;
}

static int rsh_custom_adapter_dashboard_probe_authorizer_clear(
    const rsh_case_context *context,
    const void *immutable_data
) {
    sqlite3 *candidate = rsh_context_db(context, "candidate");

    (void)immutable_data;
    require_int(
        "dashboard/probe-authorizer-clear",
        sqlite3_set_authorizer(candidate, NULL, NULL), SQLITE_OK
    );
    return SQLITE_OK;
}

static int rsh_assert_emby_dashboard_probe_error(
    const rsh_case_context *context,
    int mode_id,
    int expected_delta,
    void *ctx
) {
    char *sql;

    (void)ctx;
    if (expected_delta != 0) {
        failf("FAIL [dashboard/probe-expected-delta]: got=%d want=0",
              expected_delta);
    }
    if (mode_id == EMBY_SMOKE_MODE_EPISODES_LATEST) {
        sql = make_latest_sql("A.Id,A.SeriesName,A.SortName ", "12");
        rsh_run_emby_matrix_static_negative(
            context, "dashboard-episodes-probe-error", sql,
            "ORDER BY MAX(A.DateCreated) DESC LIMIT 12"
        );
    } else if (mode_id == EMBY_SMOKE_MODE_MOVIES_LATEST) {
        sql = make_movies_latest_sql(1, "100", "42", "12");
        rsh_run_emby_matrix_static_negative(
            context, "dashboard-movies-probe-error", sql,
            "Group by A.PresentationUniqueKey ORDER BY A.DateCreated DESC LIMIT 12"
        );
    } else {
        failf("FAIL [dashboard/probe-mode]: got=%d", mode_id);
        return SQLITE_MISUSE;
    }
    free(sql);
    return SQLITE_OK;
}

static const rsh_case_spec emby_dashboard_probe_error_cases[] = {
    {
        .label = "dashboard-probe-authorizer-set",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_FAULT_MATRIX,
            .assert_custom =
                rsh_custom_adapter_dashboard_probe_authorizer_set,
        }
    },
    {
        .label = "dashboard-episodes-probe-error",
        .kind = RSH_CASE_INDEX_PROBE,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.index_probe = {
            .role = "candidate",
            .mode_id = EMBY_SMOKE_MODE_EPISODES_LATEST,
            .expected_delta = 0,
            .assert_probe = rsh_assert_emby_dashboard_probe_error
        }
    },
    {
        .label = "dashboard-movies-probe-error",
        .kind = RSH_CASE_INDEX_PROBE,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.index_probe = {
            .role = "candidate",
            .mode_id = EMBY_SMOKE_MODE_MOVIES_LATEST,
            .expected_delta = 0,
            .assert_probe = rsh_assert_emby_dashboard_probe_error
        }
    },
    {
        .label = "dashboard-probe-authorizer-clear",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_FAULT_MATRIX,
            .assert_custom =
                rsh_custom_adapter_dashboard_probe_authorizer_clear,
        }
    }
};

static const rsh_matrix_cell_step emby_dashboard_probe_error_steps[] = {
    {
        .kind = RSH_CELL_ASSERT,
        .cases = emby_dashboard_probe_error_cases,
        .case_count = sizeof(emby_dashboard_probe_error_cases) /
                      sizeof(emby_dashboard_probe_error_cases[0])
    }
};

static const rsh_db_spec emby_dashboard_matrix_all_dbs[] = {
    {
        .role = "vendor",
        .relative_path = "not-target.db",
        .kind = RSH_DB_VENDOR,
        .storage = RSH_DB_RELATIVE,
        .setup_profile = EMBY_PROFILE_MOVIES_SEED_ONLY
    },
    {
        .role = "candidate",
        .relative_path = "library.db",
        .kind = RSH_DB_CANDIDATE,
        .storage = RSH_DB_RELATIVE,
        .setup_profile = EMBY_PROFILE_DASHBOARD_OFF
    }
};

#define EMBY_DASHBOARD_MATRIX_DBS(name, candidate_profile) \
    static const rsh_db_spec name[] = { \
        { \
            .role = "vendor", \
            .relative_path = "not-target.db", \
            .kind = RSH_DB_VENDOR, \
            .storage = RSH_DB_RELATIVE, \
            .setup_profile = EMBY_PROFILE_MOVIES_SEED_ONLY \
        }, \
        { \
            .role = "candidate", \
            .relative_path = "library.db", \
            .kind = RSH_DB_CANDIDATE, \
            .storage = RSH_DB_RELATIVE, \
            .setup_profile = (candidate_profile) \
        } \
    }

EMBY_DASHBOARD_MATRIX_DBS(
    emby_dashboard_date_missing_dbs,
    EMBY_PROFILE_EPISODES_DATE_INDEX_MISSING
);
EMBY_DASHBOARD_MATRIX_DBS(
    emby_dashboard_group_missing_dbs,
    EMBY_PROFILE_EPISODES_GROUP_INDEX_MISSING
);
EMBY_DASHBOARD_MATRIX_DBS(
    emby_dashboard_both_missing_dbs,
    EMBY_PROFILE_EPISODES_BOTH_INDEXES_MISSING
);
EMBY_DASHBOARD_MATRIX_DBS(
    emby_dashboard_movies_outer_missing_dbs,
    EMBY_PROFILE_MOVIES_OUTER_INDEX_MISSING
);
EMBY_DASHBOARD_MATRIX_DBS(
    emby_dashboard_movies_inner_missing_dbs,
    EMBY_PROFILE_MOVIES_INNER_INDEX_MISSING
);
EMBY_DASHBOARD_MATRIX_DBS(
    emby_dashboard_probe_error_dbs,
    EMBY_PROFILE_FIXTURE_CANARY
);

EMBY_DEFINE_MATRIX_AXIS_3(
    emby_dashboard_group_1,
    "dashboard-latest", EMBY_DASHBOARD_MATRIX_LATEST_12,
    "dashboard-latest-nbyte-with-nul", EMBY_DASHBOARD_MATRIX_LATEST_12_NUL,
    "dashboard-latest-limit16", EMBY_DASHBOARD_MATRIX_LATEST_16
);
EMBY_DEFINE_MATRIX_AXIS_3(
    emby_dashboard_group_2,
    "dashboard-mixed-same-control", EMBY_DASHBOARD_MATRIX_MIXED_CONTROL,
    "dashboard-distinct-negative", EMBY_DASHBOARD_MATRIX_DISTINCT_NEGATIVE,
    "dashboard-aggregate-negative", EMBY_DASHBOARD_MATRIX_AGGREGATE_NEGATIVE
);
EMBY_DEFINE_MATRIX_AXIS_1(
    emby_dashboard_group_3,
    "dashboard-over-negative", EMBY_DASHBOARD_MATRIX_OVER_NEGATIVE
);
static const emby_matrix_case emby_dashboard_movies_cases[] = {
    {"dashboard-movies-limits", EMBY_DASHBOARD_MATRIX_MOVIES_12},
    {"dashboard-movies-limits", EMBY_DASHBOARD_MATRIX_MOVIES_16},
    {"dashboard-movies-limits", EMBY_DASHBOARD_MATRIX_MOVIES_20}
};
static const rsh_matrix_axis_value emby_dashboard_movies_values[] = {
    EMBY_MATRIX_AXIS_VALUE(
        emby_dashboard_movies_cases, 0, "limit-12",
        EMBY_DASHBOARD_MATRIX_MOVIES_12
    ),
    EMBY_MATRIX_AXIS_VALUE(
        emby_dashboard_movies_cases, 1, "limit-16",
        EMBY_DASHBOARD_MATRIX_MOVIES_16
    ),
    EMBY_MATRIX_AXIS_VALUE(
        emby_dashboard_movies_cases, 2, "limit-20",
        EMBY_DASHBOARD_MATRIX_MOVIES_20
    )
};
static const rsh_matrix_axis emby_dashboard_movies_axes[] = {
    {
        .name = "limit",
        .values = emby_dashboard_movies_values,
        .value_count = sizeof(emby_dashboard_movies_values) /
                       sizeof(emby_dashboard_movies_values[0])
    }
};
EMBY_DEFINE_MATRIX_AXIS_1(
    emby_dashboard_date_missing,
    "dashboard-episodes-date-index-missing",
    EMBY_DASHBOARD_MATRIX_EPISODES_DATE_INDEX_MISSING
);
EMBY_DEFINE_MATRIX_AXIS_1(
    emby_dashboard_group_missing,
    "dashboard-episodes-group-index-missing",
    EMBY_DASHBOARD_MATRIX_EPISODES_GROUP_INDEX_MISSING
);
EMBY_DEFINE_MATRIX_AXIS_1(
    emby_dashboard_both_missing,
    "dashboard-episodes-both-indexes-missing",
    EMBY_DASHBOARD_MATRIX_EPISODES_BOTH_INDEXES_MISSING
);
EMBY_DEFINE_MATRIX_AXIS_1(
    emby_dashboard_movies_outer_missing,
    "dashboard-movies-outer-missing",
    EMBY_DASHBOARD_MATRIX_MOVIES_OUTER_INDEX_MISSING
);
EMBY_DEFINE_MATRIX_AXIS_1(
    emby_dashboard_movies_inner_missing,
    "dashboard-movies-inner-missing",
    EMBY_DASHBOARD_MATRIX_MOVIES_INNER_INDEX_MISSING
);
static const rsh_matrix_axis_value emby_dashboard_probe_error_values[] = {
    {.label = "probe-error", .integer = 0}
};
static const rsh_matrix_axis emby_dashboard_probe_error_axes[] = {
    {
        .name = "probe-state",
        .values = emby_dashboard_probe_error_values,
        .value_count = sizeof(emby_dashboard_probe_error_values) /
                       sizeof(emby_dashboard_probe_error_values[0])
    }
};

static const rsh_matrix_phase_spec emby_dashboard_matrix_phases[] = {
    EMBY_DYNAMIC_MATRIX_PHASE(
        "dashboard-group-1", "dashboard-group-1", emby_dashboard_group_1_axes,
        emby_dashboard_matrix_all_dbs, rsh_matrix_assert_emby_dashboard, 3
    ),
    EMBY_DYNAMIC_MATRIX_PHASE(
        "dashboard-group-2", "dashboard-group-2", emby_dashboard_group_2_axes,
        emby_dashboard_matrix_all_dbs, rsh_matrix_assert_emby_dashboard, 3
    ),
    EMBY_DYNAMIC_MATRIX_PHASE(
        "dashboard-group-3", "dashboard-group-3", emby_dashboard_group_3_axes,
        emby_dashboard_matrix_all_dbs, rsh_matrix_assert_emby_dashboard, 1
    ),
    EMBY_DYNAMIC_MATRIX_PHASE(
        "dashboard-movies", "dashboard-movies", emby_dashboard_movies_axes,
        emby_dashboard_matrix_all_dbs, rsh_matrix_assert_emby_dashboard, 3
    ),
    EMBY_DYNAMIC_MATRIX_PHASE(
        "dashboard-date-missing", "dashboard-date-missing",
        emby_dashboard_date_missing_axes, emby_dashboard_date_missing_dbs,
        rsh_matrix_assert_emby_dashboard, 1
    ),
    EMBY_DYNAMIC_MATRIX_PHASE(
        "dashboard-group-missing", "dashboard-group-missing",
        emby_dashboard_group_missing_axes, emby_dashboard_group_missing_dbs,
        rsh_matrix_assert_emby_dashboard, 1
    ),
    EMBY_DYNAMIC_MATRIX_PHASE(
        "dashboard-both-missing", "dashboard-both-missing",
        emby_dashboard_both_missing_axes, emby_dashboard_both_missing_dbs,
        rsh_matrix_assert_emby_dashboard, 1
    ),
    EMBY_DYNAMIC_MATRIX_PHASE(
        "dashboard-movies-outer-missing", "dashboard-movies-outer-missing",
        emby_dashboard_movies_outer_missing_axes,
        emby_dashboard_movies_outer_missing_dbs,
        rsh_matrix_assert_emby_dashboard, 1
    ),
    EMBY_DYNAMIC_MATRIX_PHASE(
        "dashboard-movies-inner-missing", "dashboard-movies-inner-missing",
        emby_dashboard_movies_inner_missing_axes,
        emby_dashboard_movies_inner_missing_dbs,
        rsh_matrix_assert_emby_dashboard, 1
    ),
    {
        .label = "dashboard-probe-error",
        .cell_dir_prefix = "dashboard-probe-error",
        .axes = emby_dashboard_probe_error_axes,
        .axis_count = sizeof(emby_dashboard_probe_error_axes) /
                      sizeof(emby_dashboard_probe_error_axes[0]),
        .dbs = emby_dashboard_probe_error_dbs,
        .db_count = sizeof(emby_dashboard_probe_error_dbs) /
                    sizeof(emby_dashboard_probe_error_dbs[0]),
        .steps = emby_dashboard_probe_error_steps,
        .step_count = sizeof(emby_dashboard_probe_error_steps) /
                      sizeof(emby_dashboard_probe_error_steps[0]),
        .expected_cells = 1
    }
};

enum emby_d5b_case_id {
    EMBY_D5B_FIX_WIDE_M3 = 1,
    EMBY_D5B_FIX_WIDE_M4,
    EMBY_D5B_FIX_COMPACT_M5,
    EMBY_D5B_FIX_COMPACT_M6,
    EMBY_D5B_FIX_EPISODES_E2,
    EMBY_D5B_FIX_EPISODES_MINUS_ONE,
    EMBY_D5B_FIX_MOVIES_M7,
    EMBY_D5B_FIX_MOVIES_MINUS_ONE,
    EMBY_D5B_FIX_NEG_E1_FAVORITES,
    EMBY_D5B_FIX_NEG_M1_FAVORITES,
    EMBY_D5B_FIX_NEG_TYPE5_RESUME,
    EMBY_D5B_FIX_NEG_M2_TAIL,
    EMBY_D5B_FIX_NEG_AGGREGATE,
    EMBY_D5B_FIX_NEG_WINDOW,
    EMBY_D5B_FIX_NEG_STAR,
    EMBY_D5B_FIX_NEG_PAREN,
    EMBY_D5B_FIX_NEG_SCALAR_MINUS_TWO,
    EMBY_D5B_FIX_NEG_SCALAR_LEADING_SPACE,
    EMBY_D5B_FIX_NEG_SCALAR_SPLIT_SIGN,
    EMBY_D5B_FIX_NEG_SCALAR_EXPRESSION,
    EMBY_D5B_FIX_NEG_SCALAR_OR,
    EMBY_D5B_FIX_NEG_SCALAR_CORRELATED,
    EMBY_D5B_FIX_NEG_SCALAR_BIND,
    EMBY_D5B_FIX_NEG_BAD_LIMIT,
    EMBY_D5B_FIX_NEG_SCALAR_NONINTEGER,
    EMBY_D5B_FIX_NEG_SCALAR_OPERATOR,
    EMBY_D5B_FIX_NEG_MIXED_ANCESTOR,
    EMBY_D5B_FIX_NEG_DUPLICATE_LIST,
    EMBY_D5B_FIX_NEG_AMBIGUOUS_SELECT,
    EMBY_D5B_FIX_NEG_EPISODES_OFFSET,
    EMBY_D5B_FIX_NEG_RANDOM_IMAGE,
    EMBY_D5B_FIX_NEG_MISSING_INDEX,
    EMBY_D5B_MIXED_POS_LITERAL_LITERAL,
    EMBY_D5B_MIXED_POS_BIND_LITERAL,
    EMBY_D5B_MIXED_POS_LITERAL_BIND,
    EMBY_D5B_MIXED_POS_BIND_BIND,
    EMBY_D5B_MIXED_POS_REVERSED_TYPES,
    EMBY_D5B_MIXED_NEG_TYPE8,
    EMBY_D5B_MIXED_NEG_TYPE55,
    EMBY_D5B_MIXED_NEG_TYPE856,
    EMBY_D5B_MIXED_NEG_TYPE_CASE,
    EMBY_D5B_MIXED_NEG_FROM_CASE,
    EMBY_D5B_MIXED_NEG_COALESCE,
    EMBY_D5B_MIXED_NEG_GROUP_CASE,
    EMBY_D5B_MIXED_NEG_ORDER_DESC,
    EMBY_D5B_MIXED_NEG_LIMIT4,
    EMBY_D5B_MIXED_NEG_USER_Q1,
    EMBY_D5B_MIXED_NEG_USER_COLON,
    EMBY_D5B_MIXED_NEG_USER_AT,
    EMBY_D5B_MIXED_NEG_USER_DOLLAR,
    EMBY_D5B_MIXED_NEG_OUTSIDE_SLOTS,
    EMBY_D5B_MIXED_NEG_THIRD_BIND,
    EMBY_D5B_MIXED_OUTER_MISSING,
    EMBY_D5B_MIXED_INNER_MISSING,
    EMBY_D5B_MIXED_BOTH_MISSING,
    EMBY_D5B_MIXED_OUTER_MALFORMED,
    EMBY_D5B_MIXED_INNER_MALFORMED,
    EMBY_D5B_MIXED_DISABLED
};

static void rsh_run_emby_d5b_dashboard_negative(
    const rsh_case_context *context,
    const char *label,
    const char *sql,
    const char *boundary_needle,
    int boundary_count,
    const char *discriminating_needle
) {
    require_int(
        label, count_occurrences(sql, boundary_needle), boundary_count
    );
    rsh_run_emby_matrix_static_negative(
        context, label, sql, discriminating_needle
    );
}

static int rsh_custom_adapter_dashboard_fix_b_c_identity(
    const rsh_case_context *context,
    const void *immutable_data
) {
    sqlite3 *vendor_db = rsh_context_db(context, "vendor");
    sqlite3 *candidate_db = rsh_context_db(context, "candidate");
    const char *const wide_ancestors[] = {"100", "100,200"};
    size_t i;

    (void)immutable_data;
    expect_movies_latest_semantics(candidate_db);
    {
        static const sqlite3_int64 candidate_ids[] = {9303, 9312, 9321};
        movies_latest_contract contract = {
            vendor_db,
            candidate_db,
            EMBY_MOVIES_LATEST_COMPACT_PROJECTION
        };
        char *raw = make_movies_latest_sql(1, "302", "42", "12");
        char *expected = make_movies_latest_expected("302", "42", "12");

        contract_parity_require(
            vendor_db, candidate_db, contract_prepare_v2,
            "emby-dashboard-movies-representative-contract",
            raw, raw, expected, NULL, NULL,
            accept_movies_latest_legal_difference, &contract
        );
        require_movies_latest_vendor_rows(
            vendor_db, "emby-dashboard-movies-vendor-contract", raw,
            EMBY_MOVIES_LATEST_COMPACT_PROJECTION
        );
        require_movies_latest_candidate_rows(
            candidate_db, "emby-dashboard-movies-candidate-contract", raw,
            expected, EMBY_MOVIES_LATEST_COMPACT_PROJECTION, candidate_ids,
            (int)(sizeof(candidate_ids) / sizeof(candidate_ids[0]))
        );
        free(raw);
        free(expected);
    }
    for (i = 0; i < sizeof(wide_ancestors) / sizeof(wide_ancestors[0]); i++) {
        char label[64];
        char *raw = make_movies_latest_sql_form(
            1, wide_ancestors[i], 0, EMBY_MOVIES_LATEST_WIDE_PROJECTION,
            "42", "20"
        );
        char *expected = make_movies_latest_expected_projection(
            wide_ancestors[i], EMBY_MOVIES_LATEST_WIDE_PROJECTION, "42", "20"
        );

        snprintf(
            label, sizeof(label), "dashboard-movies-wide-m%lu",
            (unsigned long)i + 3
        );
        contract_parity_require(
            vendor_db, candidate_db, contract_prepare_v2, label,
            raw, raw, expected, NULL, NULL, NULL, NULL
        );
        free(raw);
        free(expected);
    }
    {
        char *oracle = make_latest_sql_form(
            EMBY_EPISODES_LATEST_WIDE_PROJECTION, "100", 0, "12"
        );
        char *scalar = make_latest_sql_form(
            EMBY_EPISODES_LATEST_WIDE_PROJECTION, "100", 1, "12"
        );
        contract_parity_require_fail_open(
            vendor_db, candidate_db, contract_prepare_v2,
            "emby-dashboard-episodes-scalar-fail-open-contract",
            oracle, scalar, NULL, NULL
        );
        free(oracle);
        free(scalar);
    }
    {
        const char *ancestors =
            "100,101,102,103,104,105,106,107,108,109,110,111,112,113,"
            "114,115,116,117,118,119,120,121,122,123,124,125,126,127,"
            "128,129";
        char *wide = make_latest_sql_form(
            EMBY_EPISODES_LATEST_WIDE_PROJECTION, ancestors, 0, "12"
        );

        contract_parity_require_fail_open(
            vendor_db, candidate_db, contract_prepare_v2,
            "emby-dashboard-episodes-30-ancestor-fail-open-contract",
            wide, wide, NULL, NULL
        );
        free(wide);
    }
    {
        char ids[32];
        char *scalar = make_latest_sql_form(
            EMBY_EPISODES_LATEST_WIDE_PROJECTION, "-1", 1, "12"
        );
        contract_parity_require_fail_open(
            vendor_db, candidate_db, contract_prepare_v2,
            "emby-dashboard-episodes-scalar-minus-one-fail-open-contract",
            scalar, scalar, NULL, NULL
        );
        collect_int_column(
            candidate_db, "dashboard-episodes-scalar-minus-one-ids",
            scalar, scalar, -1, 0, ids, sizeof(ids)
        );
        require_str_eq(
            "dashboard-episodes-scalar-minus-one-exact-id", ids, "7,"
        );
        free(scalar);
    }
    {
        char *oracle = make_movies_latest_sql(1, "100", "42", "12");
        char *scalar = make_movies_latest_sql_form(
            1, "100", 1, EMBY_MOVIES_LATEST_COMPACT_PROJECTION, "42", "12"
        );
        char *expected = make_movies_latest_expected("100", "42", "12");

        contract_parity_require(
            vendor_db, candidate_db, contract_prepare_v2,
            "emby-dashboard-movies-singleton-contract", oracle, scalar,
            expected, NULL, NULL, NULL, NULL
        );
        free(oracle);
        free(scalar);
        free(expected);
    }
    {
        char ids[32];
        char *scalar = make_movies_latest_sql_form(
            1, "-1", 1, EMBY_MOVIES_LATEST_COMPACT_PROJECTION, "42", "12"
        );
        char *expected = make_movies_latest_expected("-1", "42", "12");

        contract_parity_require(
            vendor_db, candidate_db, contract_prepare_v2,
            "emby-dashboard-movies-scalar-minus-one-contract",
            scalar, scalar, expected, NULL, NULL, NULL, NULL
        );
        collect_int_column(
            candidate_db, "dashboard-movies-scalar-minus-one-ids",
            scalar, expected, -1, 0, ids, sizeof(ids)
        );
        require_str_eq(
            "dashboard-movies-scalar-minus-one-exact-id", ids, "3,"
        );
        free(scalar);
        free(expected);
    }
    return SQLITE_OK;
}

static int rsh_matrix_assert_emby_d5b_fix(
    const rsh_case_context *context,
    const rsh_matrix_cell_step *step,
    void *ctx
) {
    const emby_matrix_case *matrix_case = rsh_emby_matrix_case(context);
    char *sql = NULL;
    char *expected = NULL;
    const char *boundary = NULL;
    const char *discriminator = NULL;
    int boundary_count = 1;
    int sql_owned = 0;

    (void)step;
    (void)ctx;
    switch (matrix_case->case_id) {
    case EMBY_D5B_FIX_WIDE_M3:
    case EMBY_D5B_FIX_WIDE_M4:
        {
            const char *ancestors =
                matrix_case->case_id == EMBY_D5B_FIX_WIDE_M3
                    ? "100" : "100,200";
            char *outer_select;

            sql = make_movies_latest_sql_form(
                1, ancestors, 0, EMBY_MOVIES_LATEST_WIDE_PROJECTION,
                "42", "20"
            );
            sql_owned = 1;
            expected = make_movies_latest_expected_projection(
                ancestors, EMBY_MOVIES_LATEST_WIDE_PROJECTION, "42", "20"
            );
            outer_select = xasprintf(
                " ) SELECT %sFROM ranked AS R",
                EMBY_MOVIES_LATEST_WIDE_PROJECTION
            );
            require_contains(matrix_case->label, expected, outer_select);
            free(outer_select);
            rsh_run_emby_matrix_sql_exact(
                context, matrix_case->label, sql, expected,
                RSH_PREPARE_V2, RSH_NBYTE_MINUS_ONE
            );
        }
        break;
    case EMBY_D5B_FIX_COMPACT_M5:
    case EMBY_D5B_FIX_COMPACT_M6:
        {
            const char *ancestors =
                matrix_case->case_id == EMBY_D5B_FIX_COMPACT_M5
                    ? "201,202,203,204" : "9,10,11,12,13,14";
            sql = make_movies_latest_sql(1, ancestors, "42", "12");
            sql_owned = 1;
            expected = make_movies_latest_expected(ancestors, "42", "12");
            rsh_run_emby_matrix_sql_exact(
                context, matrix_case->label, sql, expected,
                RSH_PREPARE_V2, RSH_NBYTE_MINUS_ONE
            );
        }
        break;
    case EMBY_D5B_FIX_EPISODES_E2:
        sql = make_latest_sql_form(
            EMBY_EPISODES_LATEST_WIDE_PROJECTION, "100", 1, "12"
        );
        sql_owned = 1;
        expected = make_latest_sql_form(
            EMBY_EPISODES_LATEST_WIDE_PROJECTION, "100", 1, "12"
        );
        rsh_run_emby_matrix_sql_exact(
            context, matrix_case->label, sql, expected,
            RSH_PREPARE_V2, RSH_NBYTE_MINUS_ONE
        );
        break;
    case EMBY_D5B_FIX_EPISODES_MINUS_ONE:
        sql = make_latest_sql_form(
            EMBY_EPISODES_LATEST_WIDE_PROJECTION, "-1", 1, "12"
        );
        sql_owned = 1;
        expected = make_latest_sql_form(
            EMBY_EPISODES_LATEST_WIDE_PROJECTION, "-1", 1, "12"
        );
        rsh_run_emby_matrix_sql_exact(
            context, matrix_case->label, sql, expected,
            RSH_PREPARE_V2, RSH_NBYTE_MINUS_ONE
        );
        break;
    case EMBY_D5B_FIX_MOVIES_M7:
        sql = make_movies_latest_sql_form(
            1, "100", 1, EMBY_MOVIES_LATEST_COMPACT_PROJECTION, "42", "12"
        );
        sql_owned = 1;
        expected = make_movies_latest_expected("100", "42", "12");
        rsh_run_emby_matrix_sql_exact(
            context, matrix_case->label, sql, expected,
            RSH_PREPARE_V2, RSH_NBYTE_MINUS_ONE
        );
        break;
    case EMBY_D5B_FIX_MOVIES_MINUS_ONE:
        sql = make_movies_latest_sql_form(
            1, "-1", 1, EMBY_MOVIES_LATEST_COMPACT_PROJECTION, "42", "12"
        );
        sql_owned = 1;
        expected = make_movies_latest_expected("-1", "42", "12");
        rsh_run_emby_matrix_sql_exact(
            context, matrix_case->label, sql, expected,
            RSH_PREPARE_V2, RSH_NBYTE_MINUS_ONE
        );
        break;
    case EMBY_D5B_FIX_NEG_E1_FAVORITES:
        sql = (char *)TEST_E1_FAVORITES;
        boundary = "where A.Type=8 ";
        discriminator = boundary;
        break;
    case EMBY_D5B_FIX_NEG_M1_FAVORITES:
        sql = (char *)TEST_M1_FAVORITES;
        boundary = "where A.Type=5 ";
        discriminator = boundary;
        break;
    case EMBY_D5B_FIX_NEG_TYPE5_RESUME:
        sql = (char *)TEST_TYPE5_RESUME;
        boundary = "LastWatchedEpisodes";
        boundary_count = 2;
        discriminator =
            "left join MediaItems AS LastWatchedEpisodes ON "
            "LastWatchedEpisodes.SeriesPresentationUniqueKey="
            "A.SeriesPresentationUniqueKey";
        break;
    case EMBY_D5B_FIX_NEG_M2_TAIL:
        sql = (char *)TEST_M2_PREMIERE_TAIL;
        boundary = "A.PremiereDate>=1752355308";
        discriminator = boundary;
        break;
    case EMBY_D5B_FIX_NEG_AGGREGATE:
        sql = (char *)TEST_MOVIES_AGGREGATE_PROJECTION;
        boundary = "MAX(A.DateCreated)";
        discriminator = boundary;
        break;
    case EMBY_D5B_FIX_NEG_WINDOW:
        sql = (char *)TEST_MOVIES_WINDOW_PROJECTION;
        boundary = "count(*) OVER()";
        discriminator = boundary;
        break;
    case EMBY_D5B_FIX_NEG_STAR:
        sql = (char *)TEST_MOVIES_BARE_STAR_PROJECTION;
        boundary = "select * from";
        discriminator = boundary;
        break;
    case EMBY_D5B_FIX_NEG_PAREN:
        sql = (char *)TEST_MOVIES_PAREN_PROJECTION;
        boundary = "select (A.Id)";
        discriminator = boundary;
        break;
    case EMBY_D5B_FIX_NEG_SCALAR_MINUS_TWO:
        sql = make_movies_latest_sql_form(
            1, "-2", 1, EMBY_MOVIES_LATEST_COMPACT_PROJECTION, "42", "12"
        );
        sql_owned = 1;
        boundary = "AncestorId=-2 )select";
        discriminator = boundary;
        break;
    case EMBY_D5B_FIX_NEG_SCALAR_LEADING_SPACE:
        sql = make_movies_latest_sql_form(
            1, " -1", 1, EMBY_MOVIES_LATEST_COMPACT_PROJECTION, "42", "12"
        );
        sql_owned = 1;
        boundary = "AncestorId= -1 )select";
        discriminator = boundary;
        break;
    case EMBY_D5B_FIX_NEG_SCALAR_SPLIT_SIGN:
        sql = make_movies_latest_sql_form(
            1, "- 1", 1, EMBY_MOVIES_LATEST_COMPACT_PROJECTION, "42", "12"
        );
        sql_owned = 1;
        boundary = "AncestorId=- 1 )select";
        discriminator = boundary;
        break;
    case EMBY_D5B_FIX_NEG_SCALAR_EXPRESSION:
        sql = make_movies_latest_sql_form(
            1, "-1+0", 1, EMBY_MOVIES_LATEST_COMPACT_PROJECTION, "42", "12"
        );
        sql_owned = 1;
        boundary = "AncestorId=-1+0 )select";
        discriminator = boundary;
        break;
    case EMBY_D5B_FIX_NEG_SCALAR_OR:
        sql = make_movies_latest_sql_form(
            1, "-1 OR AncestorId=200", 1,
            EMBY_MOVIES_LATEST_COMPACT_PROJECTION, "42", "12"
        );
        sql_owned = 1;
        boundary = "AncestorId=-1 OR AncestorId=200 )select";
        discriminator = boundary;
        break;
    case EMBY_D5B_FIX_NEG_SCALAR_CORRELATED:
        sql = make_movies_latest_sql_form(
            1, "-1 AND ItemId=A.Id", 1,
            EMBY_MOVIES_LATEST_COMPACT_PROJECTION, "42", "12"
        );
        sql_owned = 1;
        boundary = "AncestorId=-1 AND ItemId=A.Id )select";
        discriminator = boundary;
        break;
    case EMBY_D5B_FIX_NEG_SCALAR_BIND:
        sql = (char *)TEST_MOVIES_SCALAR_BIND;
        boundary = "AncestorId=?";
        discriminator = boundary;
        break;
    case EMBY_D5B_FIX_NEG_BAD_LIMIT:
        sql = (char *)TEST_MOVIES_BAD_LIMIT;
        boundary = "LIMIT 11";
        discriminator = boundary;
        break;
    case EMBY_D5B_FIX_NEG_SCALAR_NONINTEGER:
        sql = (char *)TEST_MOVIES_SCALAR_NONINTEGER;
        boundary = "AncestorId=+100";
        discriminator = boundary;
        break;
    case EMBY_D5B_FIX_NEG_SCALAR_OPERATOR:
        sql = (char *)TEST_MOVIES_SCALAR_OPERATOR;
        boundary = "AncestorId=100 OR AncestorId=200";
        discriminator = boundary;
        break;
    case EMBY_D5B_FIX_NEG_MIXED_ANCESTOR:
        sql = (char *)TEST_MOVIES_MIXED_ANCESTOR;
        boundary =
            "WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE "
            "AncestorId=";
        discriminator = "AncestorId=200 )select itemid FROM WithAncestors";
        break;
    case EMBY_D5B_FIX_NEG_DUPLICATE_LIST:
        sql = (char *)TEST_MOVIES_DUPLICATE_LIST_ANCESTOR;
        boundary =
            "WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE "
            "AncestorId in (";
        boundary_count = 2;
        discriminator =
            "Other AS (WITH WithAncestors AS (SELECT itemid FROM "
            "AncestorIds2 WHERE AncestorId in (200) )";
        break;
    case EMBY_D5B_FIX_NEG_AMBIGUOUS_SELECT:
        sql = (char *)TEST_MOVIES_AMBIGUOUS_SELECT;
        boundary = ") )select ";
        boundary_count = 2;
        discriminator =
            "Other2 AS (WITH withancestors AS (SELECT itemid FROM "
            "AncestorIds2 WHERE AncestorId in (300) )select itemid FROM "
            "withancestors) select ";
        break;
    case EMBY_D5B_FIX_NEG_EPISODES_OFFSET:
        sql = (char *)TEST_EPISODES_DATE_WINDOW_OFFSET;
        boundary = "OFFSET 12";
        discriminator = boundary;
        break;
    case EMBY_D5B_FIX_NEG_RANDOM_IMAGE:
        sql = (char *)TEST_MOVIES_CORRELATED_RANDOM_IMAGE;
        require_int(
            "dashboard-movies-correlated-random-image/images-boundary",
            count_occurrences(sql, "A.Images like '%Primary%'"), 1
        );
        require_int(
            "dashboard-movies-correlated-random-image/order-boundary",
            count_occurrences(sql, "ORDER BY RANDOM()"), 1
        );
        boundary = "AncestorId=838031 AND ItemId=A.Id";
        discriminator = boundary;
        break;
    default:
        failf("FAIL [dashboard-fix-b-c/case-id]: got=%d", matrix_case->case_id);
    }
    if (boundary) {
        rsh_run_emby_d5b_dashboard_negative(
            context, matrix_case->label, sql, boundary, boundary_count,
            discriminator
        );
    }
    if (sql_owned) free(sql);
    free(expected);
    return SQLITE_OK;
}

static sqlite_master_auth_probe emby_d5b_mixed_probe;

static int rsh_custom_adapter_d5b_mixed_probe_set(
    const rsh_case_context *context,
    const void *immutable_data
) {
    sqlite3 *candidate = rsh_context_db(context, "candidate");
    const char *label = strcmp(context->run_name, "dashboard-mixed-disabled") == 0
        ? "dashboard-mixed-disabled/authorizer-set"
        : "dashboard-mixed/authorizer-set";

    (void)immutable_data;
    memset(&emby_d5b_mixed_probe, 0, sizeof(emby_d5b_mixed_probe));
    require_int(
        label,
        sqlite3_set_authorizer(
            candidate, count_sqlite_master_read, &emby_d5b_mixed_probe
        ),
        SQLITE_OK
    );
    return SQLITE_OK;
}

static int rsh_custom_adapter_d5b_mixed_probe_clear(
    const rsh_case_context *context,
    const void *immutable_data
) {
    sqlite3 *candidate = rsh_context_db(context, "candidate");
    const char *label = strcmp(context->run_name, "dashboard-mixed-disabled") == 0
        ? "dashboard-mixed-disabled/authorizer-clear"
        : "dashboard-mixed/authorizer-clear";

    (void)immutable_data;
    require_int(
        label, sqlite3_set_authorizer(candidate, NULL, NULL), SQLITE_OK
    );
    return SQLITE_OK;
}

static void rsh_run_emby_d5b_mixed_negative(
    const rsh_case_context *context,
    const emby_matrix_case *matrix_case
) {
    char *sql = NULL;
    char *base = NULL;

    switch (matrix_case->case_id) {
    case EMBY_D5B_MIXED_NEG_TYPE8:
        base = make_mixed_latest_sql("42", "3");
        rsh_run_emby_matrix_generated_negative(
            context, matrix_case->label, base, "where A.Type in (8,5) ",
            "where A.Type=8 ", "where A.Type=8 "
        );
        break;
    case EMBY_D5B_MIXED_NEG_TYPE55:
        base = make_mixed_latest_sql("42", "3");
        rsh_run_emby_matrix_generated_negative(
            context, matrix_case->label, base, "where A.Type in (8,5) ",
            "where A.Type in (5,5) ", "where A.Type in (5,5) "
        );
        break;
    case EMBY_D5B_MIXED_NEG_TYPE856:
        base = make_mixed_latest_sql("42", "3");
        rsh_run_emby_matrix_generated_negative(
            context, matrix_case->label, base, "where A.Type in (8,5) ",
            "where A.Type in (8,5,6) ", "where A.Type in (8,5,6) "
        );
        break;
    case EMBY_D5B_MIXED_NEG_TYPE_CASE:
        base = make_mixed_latest_sql("42", "3");
        rsh_run_emby_matrix_generated_negative(
            context, matrix_case->label, base, "A.type,A.Id", "A.Type,A.Id",
            "A.Type,A.Id"
        );
        break;
    case EMBY_D5B_MIXED_NEG_FROM_CASE:
        base = make_mixed_latest_sql("42", "3");
        rsh_run_emby_matrix_generated_negative(
            context, matrix_case->label, base, "from mediaitems A left join",
            "FROM mediaitems AS A left join",
            "FROM mediaitems AS A left join"
        );
        break;
    case EMBY_D5B_MIXED_NEG_COALESCE:
        base = make_mixed_latest_sql("42", "3");
        rsh_run_emby_matrix_generated_negative(
            context, matrix_case->label, base,
            "Coalesce(UserDatas.played, 0)=0",
            "Coalesce(UserDatas.played,0)=0",
            "Coalesce(UserDatas.played,0)=0"
        );
        break;
    case EMBY_D5B_MIXED_NEG_GROUP_CASE:
        base = make_mixed_latest_sql("42", "3");
        rsh_run_emby_matrix_generated_negative(
            context, matrix_case->label, base,
            "Group by coalesce(A.SeriesPresentationUniqueKey, "
            "A.PresentationUniqueKey)",
            "Group By coalesce(A.SeriesPresentationUniqueKey, "
            "A.PresentationUniqueKey)",
            "Group By coalesce(A.SeriesPresentationUniqueKey, "
            "A.PresentationUniqueKey)"
        );
        break;
    case EMBY_D5B_MIXED_NEG_ORDER_DESC:
        base = make_mixed_latest_sql("42", "3");
        rsh_run_emby_matrix_generated_negative(
            context, matrix_case->label, base,
            "ORDER BY MAX(A.DateCreated) DESC",
            "ORDER BY MAX(A.DateCreated) desc",
            "ORDER BY MAX(A.DateCreated) desc"
        );
        break;
    case EMBY_D5B_MIXED_NEG_LIMIT4:
        base = make_mixed_latest_sql("42", "3");
        rsh_run_emby_matrix_generated_negative(
            context, matrix_case->label, base, "LIMIT 3", "LIMIT 4", "LIMIT 4"
        );
        break;
    case EMBY_D5B_MIXED_NEG_USER_Q1:
        sql = make_mixed_latest_sql("?1", "3");
        rsh_run_emby_matrix_static_negative(
            context, matrix_case->label, sql, "UserDatas.UserId=?1 "
        );
        break;
    case EMBY_D5B_MIXED_NEG_USER_COLON:
        sql = make_mixed_latest_sql(":name", "3");
        rsh_run_emby_matrix_static_negative(
            context, matrix_case->label, sql, "UserDatas.UserId=:name "
        );
        break;
    case EMBY_D5B_MIXED_NEG_USER_AT:
        sql = make_mixed_latest_sql("@name", "3");
        rsh_run_emby_matrix_static_negative(
            context, matrix_case->label, sql, "UserDatas.UserId=@name "
        );
        break;
    case EMBY_D5B_MIXED_NEG_USER_DOLLAR:
        sql = make_mixed_latest_sql("$name", "3");
        rsh_run_emby_matrix_static_negative(
            context, matrix_case->label, sql, "UserDatas.UserId=$name "
        );
        break;
    case EMBY_D5B_MIXED_NEG_OUTSIDE_SLOTS:
        base = make_mixed_latest_sql("42", "3");
        rsh_run_emby_matrix_generated_negative(
            context, matrix_case->label, base, "A.type,A.Id",
            "? AS extra,A.type,A.Id", "? AS extra,A.type,A.Id"
        );
        break;
    case EMBY_D5B_MIXED_NEG_THIRD_BIND:
        base = make_mixed_latest_sql("?", "?");
        rsh_run_emby_matrix_generated_negative(
            context, matrix_case->label, base, "A.type,A.Id",
            "? AS third,A.type,A.Id", "? AS third,A.type,A.Id"
        );
        break;
    case EMBY_D5B_MIXED_DISABLED:
        sql = make_mixed_latest_sql("42", "3");
        rsh_run_emby_matrix_static_negative(
            context, matrix_case->label, sql,
            "where A.Type in (8,5) AND Coalesce(UserDatas.played, 0)=0"
        );
        break;
    default:
        failf("FAIL [dashboard-mixed/case-id]: got=%d", matrix_case->case_id);
    }
    free(base);
    free(sql);
}

static int rsh_assert_emby_d5b_mixed_probe(
    const rsh_case_context *context,
    int mode_id,
    int expected_delta,
    void *ctx
) {
    const emby_matrix_case *matrix_case = rsh_emby_matrix_case(context);
    int before = emby_d5b_mixed_probe.reads;

    (void)ctx;
    if (mode_id != EMBY_SMOKE_MODE_MIXED_LATEST || expected_delta != 0) {
        failf(
            "FAIL [dashboard-mixed/probe-contract]: mode=%d delta=%d",
            mode_id, expected_delta
        );
    }
    rsh_run_emby_d5b_mixed_negative(context, matrix_case);
    if (matrix_case->case_id == EMBY_D5B_MIXED_DISABLED) {
        require_int(
            "dashboard-mixed-disabled/probes",
            emby_d5b_mixed_probe.reads, 0
        );
    } else if (matrix_case->case_id >= EMBY_D5B_MIXED_NEG_TYPE8 &&
               matrix_case->case_id <= EMBY_D5B_MIXED_NEG_TYPE856 &&
               emby_d5b_mixed_probe.reads - before != expected_delta) {
        failf(
            "FAIL [%s/probe]: expected=%d actual=%d",
            matrix_case->label, before, emby_d5b_mixed_probe.reads
        );
    }
    return SQLITE_OK;
}

static int rsh_matrix_assert_emby_d5b_mixed_positive(
    const rsh_case_context *context,
    const rsh_matrix_cell_step *step,
    void *ctx
) {
    const emby_matrix_case *matrix_case = rsh_emby_matrix_case(context);
    sqlite3 *candidate = rsh_context_db(context, "candidate");
    const char *user = "42";
    const char *limit = "3";
    int user_bound = 0;
    int limit_bound = 0;
    char *sql;
    char *expected;

    (void)step;
    (void)ctx;
    if (matrix_case->case_id == EMBY_D5B_MIXED_POS_BIND_LITERAL ||
        matrix_case->case_id == EMBY_D5B_MIXED_POS_BIND_BIND) {
        user = "?";
        user_bound = 1;
    }
    if (matrix_case->case_id == EMBY_D5B_MIXED_POS_LITERAL_BIND ||
        matrix_case->case_id == EMBY_D5B_MIXED_POS_BIND_BIND) {
        limit = "?";
        limit_bound = 1;
    }
    sql = make_mixed_latest_sql(user, limit);
    if (matrix_case->case_id == EMBY_D5B_MIXED_POS_REVERSED_TYPES) {
        char *reversed = replace_once(
            sql, "where A.Type in (8,5) ", "where A.Type in (5,8) "
        );
        free(sql);
        sql = reversed;
    }
    expected = make_mixed_latest_expected(user, limit);
    expect_mixed_latest_sql(
        candidate, matrix_case->label, sql, expected,
        user_bound, limit_bound
    );
    free(sql);
    free(expected);
    return SQLITE_OK;
}

static int rsh_assert_emby_d5b_index_negative(
    const rsh_case_context *context,
    int mode_id,
    int expected_delta,
    void *ctx
) {
    const emby_matrix_case *matrix_case = rsh_emby_matrix_case(context);
    char *sql;

    (void)ctx;
    if (expected_delta != 0) {
        failf("FAIL [d5b/index-delta]: got=%d want=0", expected_delta);
    }
    if (matrix_case->case_id == EMBY_D5B_FIX_NEG_MISSING_INDEX) {
        if (mode_id != EMBY_SMOKE_MODE_MOVIES_LATEST) {
            failf("FAIL [d5b/index-mode]: got=%d want=movies", mode_id);
        }
        rsh_run_emby_d5b_dashboard_negative(
            context, matrix_case->label, TEST_MOVIES_MISSING_INDEX,
            "where A.Type=5 AND Coalesce(UserDatas.played, 0)=0", 1,
            "where A.Type=5 AND Coalesce(UserDatas.played, 0)=0"
        );
        return SQLITE_OK;
    }
    if (mode_id != EMBY_SMOKE_MODE_MIXED_LATEST) {
        failf("FAIL [d5b/index-mode]: got=%d want=mixed", mode_id);
    }
    sql = make_mixed_latest_sql("42", "3");
    rsh_run_emby_matrix_static_negative(
        context, matrix_case->label, sql,
        "where A.Type in (8,5) AND Coalesce(UserDatas.played, 0)=0"
    );
    free(sql);
    return SQLITE_OK;
}

static int rsh_custom_adapter_dashboard_fix_b_c_log_assert(
    const rsh_case_context *context,
    const void *immutable_data
) {
    const char *line = context->captured_stderr;
    int episodes_misses = 0;
    int movies_misses = 0;

    (void)immutable_data;
    while (line && *line) {
        const char *end = strchr(line, '\n');
        const char *e1 = strstr(line, "where A.Type=8 AND UserDatas.IsFavorite=1");
        const char *m1 = strstr(line, "where A.Type=5 AND UserDatas.IsFavorite=1");
        const char *type5 = strstr(line, "LastWatchedEpisodes ON");
        const char *m2 = strstr(line, "A.PremiereDate>=1752355308");
        int legacy_capture_source =
            (e1 && (!end || e1 < end)) || (m1 && (!end || m1 < end)) ||
            (type5 && (!end || type5 < end)) || (m2 && (!end || m2 < end));

        if (legacy_capture_source) {
            const char *episodes = strstr(
                line, "reason=capture_miss mode=dashboard+episodes_latest"
            );
            const char *movies = strstr(
                line, "reason=capture_miss mode=dashboard+movies_latest"
            );
            if (episodes && (!end || episodes < end)) episodes_misses++;
            if (movies && (!end || movies < end)) movies_misses++;
        }
        if (!end) break;
        line = end + 1;
    }
    require_int(
        "dashboard-fix-b-c/episodes-silent",
        episodes_misses, 0
    );
    require_int(
        "dashboard-fix-b-c/movies-only-m2-capture",
        movies_misses, 1
    );
    return SQLITE_OK;
}

static const rsh_db_spec emby_d5b_fix_dbs[] = {
    {
        .role = "vendor",
        .relative_path = "not-target.db",
        .kind = RSH_DB_VENDOR,
        .storage = RSH_DB_RELATIVE,
        .seed_profile = EMBY_PROFILE_MOVIES_SEED_ONLY,
        .setup_profile = EMBY_PROFILE_D5B_SCALAR_MINUS_ONE_VENDOR_SEED
    },
    {
        .role = "candidate",
        .relative_path = "library.db",
        .kind = RSH_DB_CANDIDATE,
        .storage = RSH_DB_RELATIVE,
        .seed_profile = EMBY_PROFILE_D5B_FIX_CANDIDATE,
        .setup_profile = EMBY_PROFILE_D5B_SCALAR_MINUS_ONE_CANDIDATE_SEED
    }
};

static const rsh_case_spec emby_d5b_fix_identity_cases[] = {
    {
        .label = "dashboard-fix-b-c-identity",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_IDENTITY,
            .assert_custom = rsh_custom_adapter_dashboard_fix_b_c_identity,
        }
    }
};

static const rsh_phase_spec emby_dashboard_fix_b_c_identity_phases[] = {
    {
        .label = "dashboard-fix-b-c-identity",
        .dbs = emby_d5b_fix_dbs,
        .db_count = sizeof(emby_d5b_fix_dbs) / sizeof(emby_d5b_fix_dbs[0]),
        .cases = emby_d5b_fix_identity_cases,
        .case_count = sizeof(emby_d5b_fix_identity_cases) /
                      sizeof(emby_d5b_fix_identity_cases[0])
    }
};

#define EMBY_D5B_PROFILE_DBS(name, profile_value) \
    static const rsh_db_spec name[] = { \
        { \
            .role = "vendor", \
            .relative_path = "not-target.db", \
            .kind = RSH_DB_VENDOR, \
            .storage = RSH_DB_RELATIVE, \
            .setup_profile = (profile_value) \
        }, \
        { \
            .role = "candidate", \
            .relative_path = "library.db", \
            .kind = RSH_DB_CANDIDATE, \
            .storage = RSH_DB_RELATIVE, \
            .setup_profile = (profile_value) \
        } \
    }

EMBY_D5B_PROFILE_DBS(
    emby_d5b_fix_missing_index_dbs,
    EMBY_PROFILE_MOVIES_OUTER_INDEX_MISSING
);
EMBY_D5B_PROFILE_DBS(
    emby_d5b_mixed_all_dbs, EMBY_PROFILE_D5B_MIXED_ALL_INDEXES
);
EMBY_D5B_PROFILE_DBS(
    emby_d5b_mixed_outer_missing_dbs,
    EMBY_PROFILE_D5B_MIXED_OUTER_MISSING
);
EMBY_D5B_PROFILE_DBS(
    emby_d5b_mixed_inner_missing_dbs,
    EMBY_PROFILE_D5B_MIXED_INNER_MISSING
);
EMBY_D5B_PROFILE_DBS(
    emby_d5b_mixed_both_missing_dbs,
    EMBY_PROFILE_D5B_MIXED_BOTH_MISSING
);
EMBY_D5B_PROFILE_DBS(
    emby_d5b_mixed_outer_malformed_dbs,
    EMBY_PROFILE_D5B_MIXED_OUTER_MALFORMED
);
EMBY_D5B_PROFILE_DBS(
    emby_d5b_mixed_inner_malformed_dbs,
    EMBY_PROFILE_D5B_MIXED_INNER_MALFORMED
);

static const rsh_case_spec emby_d5b_mixed_probe_cases[] = {
    {
        .label = "dashboard-mixed-probe-set",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_FAULT_MATRIX,
            .assert_custom = rsh_custom_adapter_d5b_mixed_probe_set,
        }
    },
    {
        .label = "dashboard-mixed-probe-negative",
        .kind = RSH_CASE_INDEX_PROBE,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.index_probe = {
            .role = "candidate",
            .mode_id = EMBY_SMOKE_MODE_MIXED_LATEST,
            .expected_delta = 0,
            .assert_probe = rsh_assert_emby_d5b_mixed_probe
        }
    },
    {
        .label = "dashboard-mixed-probe-clear",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_FAULT_MATRIX,
            .assert_custom = rsh_custom_adapter_d5b_mixed_probe_clear,
        }
    }
};

static const rsh_matrix_cell_step emby_d5b_mixed_probe_steps[] = {
    {
        .kind = RSH_CELL_ASSERT,
        .cases = emby_d5b_mixed_probe_cases,
        .case_count = sizeof(emby_d5b_mixed_probe_cases) /
                      sizeof(emby_d5b_mixed_probe_cases[0])
    }
};

static const rsh_case_spec emby_d5b_fix_index_cases[] = {
    {
        .label = "dashboard-movies-missing-index",
        .kind = RSH_CASE_INDEX_PROBE,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.index_probe = {
            .role = "candidate",
            .mode_id = EMBY_SMOKE_MODE_MOVIES_LATEST,
            .expected_delta = 0,
            .assert_probe = rsh_assert_emby_d5b_index_negative
        }
    }
};

static const rsh_matrix_cell_step emby_d5b_fix_index_steps[] = {
    {
        .kind = RSH_CELL_ASSERT,
        .cases = emby_d5b_fix_index_cases,
        .case_count = sizeof(emby_d5b_fix_index_cases) /
                      sizeof(emby_d5b_fix_index_cases[0])
    }
};

static const rsh_case_spec emby_d5b_mixed_index_cases[] = {
    {
        .label = "dashboard-mixed-index-negative",
        .kind = RSH_CASE_INDEX_PROBE,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.index_probe = {
            .role = "candidate",
            .mode_id = EMBY_SMOKE_MODE_MIXED_LATEST,
            .expected_delta = 0,
            .assert_probe = rsh_assert_emby_d5b_index_negative
        }
    }
};

static const rsh_matrix_cell_step emby_d5b_mixed_index_steps[] = {
    {
        .kind = RSH_CELL_ASSERT,
        .cases = emby_d5b_mixed_index_cases,
        .case_count = sizeof(emby_d5b_mixed_index_cases) /
                      sizeof(emby_d5b_mixed_index_cases[0])
    }
};

#define EMBY_D5B_CASE_MATRIX_PHASE( \
    phase_label, prefix, axis_rows, db_rows, step_rows, cell_count) \
    { \
        .label = (phase_label), \
        .cell_dir_prefix = (prefix), \
        .axes = (axis_rows), \
        .axis_count = sizeof(axis_rows) / sizeof((axis_rows)[0]), \
        .dbs = (db_rows), \
        .db_count = sizeof(db_rows) / sizeof((db_rows)[0]), \
        .steps = (step_rows), \
        .step_count = sizeof(step_rows) / sizeof((step_rows)[0]), \
        .expected_cells = (cell_count) \
    }

EMBY_DEFINE_MATRIX_AXIS_3(
    emby_d5b_fix_positive_1,
    "dashboard-movies-wide-m3", EMBY_D5B_FIX_WIDE_M3,
    "dashboard-movies-wide-m4", EMBY_D5B_FIX_WIDE_M4,
    "dashboard-movies-m5-byte-identical", EMBY_D5B_FIX_COMPACT_M5
);
EMBY_DEFINE_MATRIX_AXIS_3(
    emby_d5b_fix_positive_2,
    "dashboard-movies-m6-byte-identical", EMBY_D5B_FIX_COMPACT_M6,
    "dashboard-episodes-e2-scalar-fail-open", EMBY_D5B_FIX_EPISODES_E2,
    "dashboard-episodes-scalar-minus-one-fail-open", EMBY_D5B_FIX_EPISODES_MINUS_ONE
);
EMBY_DEFINE_MATRIX_AXIS_2(
    emby_d5b_fix_positive_3,
    "dashboard-movies-m7-scalar", EMBY_D5B_FIX_MOVIES_M7,
    "dashboard-movies-scalar-minus-one", EMBY_D5B_FIX_MOVIES_MINUS_ONE
);
EMBY_DEFINE_MATRIX_AXIS_3(
    emby_d5b_fix_negative_1,
    "dashboard-e1-favorites-silent", EMBY_D5B_FIX_NEG_E1_FAVORITES,
    "dashboard-m1-favorites-silent", EMBY_D5B_FIX_NEG_M1_FAVORITES,
    "dashboard-type5-resume-silent", EMBY_D5B_FIX_NEG_TYPE5_RESUME
);
EMBY_DEFINE_MATRIX_AXIS_3(
    emby_d5b_fix_negative_2,
    "dashboard-m2-tail-miss", EMBY_D5B_FIX_NEG_M2_TAIL,
    "dashboard-movies-aggregate-projection", EMBY_D5B_FIX_NEG_AGGREGATE,
    "dashboard-movies-window-projection", EMBY_D5B_FIX_NEG_WINDOW
);
EMBY_DEFINE_MATRIX_AXIS_3(
    emby_d5b_fix_negative_3,
    "dashboard-movies-bare-star-projection", EMBY_D5B_FIX_NEG_STAR,
    "dashboard-movies-parenthesized-projection", EMBY_D5B_FIX_NEG_PAREN,
    "dashboard-movies-scalar-minus-two", EMBY_D5B_FIX_NEG_SCALAR_MINUS_TWO
);
EMBY_DEFINE_MATRIX_AXIS_3(
    emby_d5b_fix_negative_4,
    "dashboard-movies-scalar-leading-space",
    EMBY_D5B_FIX_NEG_SCALAR_LEADING_SPACE,
    "dashboard-movies-scalar-split-sign",
    EMBY_D5B_FIX_NEG_SCALAR_SPLIT_SIGN,
    "dashboard-movies-scalar-expression",
    EMBY_D5B_FIX_NEG_SCALAR_EXPRESSION
);
EMBY_DEFINE_MATRIX_AXIS_3(
    emby_d5b_fix_negative_5,
    "dashboard-movies-scalar-minus-one-or", EMBY_D5B_FIX_NEG_SCALAR_OR,
    "dashboard-movies-scalar-minus-one-correlated",
    EMBY_D5B_FIX_NEG_SCALAR_CORRELATED,
    "dashboard-movies-scalar-bind", EMBY_D5B_FIX_NEG_SCALAR_BIND
);
EMBY_DEFINE_MATRIX_AXIS_3(
    emby_d5b_fix_negative_6,
    "dashboard-movies-bad-limit", EMBY_D5B_FIX_NEG_BAD_LIMIT,
    "dashboard-movies-scalar-noninteger",
    EMBY_D5B_FIX_NEG_SCALAR_NONINTEGER,
    "dashboard-movies-scalar-operator", EMBY_D5B_FIX_NEG_SCALAR_OPERATOR
);
EMBY_DEFINE_MATRIX_AXIS_3(
    emby_d5b_fix_negative_7,
    "dashboard-movies-mixed-ancestor", EMBY_D5B_FIX_NEG_MIXED_ANCESTOR,
    "dashboard-movies-duplicate-list-ancestor",
    EMBY_D5B_FIX_NEG_DUPLICATE_LIST,
    "dashboard-movies-ambiguous-select", EMBY_D5B_FIX_NEG_AMBIGUOUS_SELECT
);
EMBY_DEFINE_MATRIX_AXIS_2(
    emby_d5b_fix_negative_8,
    "dashboard-episodes-date-window-offset",
    EMBY_D5B_FIX_NEG_EPISODES_OFFSET,
    "dashboard-movies-correlated-random-image",
    EMBY_D5B_FIX_NEG_RANDOM_IMAGE
);
EMBY_DEFINE_MATRIX_AXIS_1(
    emby_d5b_fix_missing_index,
    "dashboard-movies-missing-index", EMBY_D5B_FIX_NEG_MISSING_INDEX
);

static const rsh_matrix_phase_spec emby_dashboard_fix_b_c_matrix_phases[] = {
    EMBY_DYNAMIC_MATRIX_PHASE(
        "fix-positive-1", "fix-positive-1", emby_d5b_fix_positive_1_axes,
        emby_d5b_fix_dbs, rsh_matrix_assert_emby_d5b_fix, 3
    ),
    EMBY_DYNAMIC_MATRIX_PHASE(
        "fix-positive-2", "fix-positive-2", emby_d5b_fix_positive_2_axes,
        emby_d5b_fix_dbs, rsh_matrix_assert_emby_d5b_fix, 3
    ),
    EMBY_DYNAMIC_MATRIX_PHASE(
        "fix-positive-3", "fix-positive-3", emby_d5b_fix_positive_3_axes,
        emby_d5b_fix_dbs, rsh_matrix_assert_emby_d5b_fix, 2
    ),
    EMBY_DYNAMIC_MATRIX_PHASE(
        "fix-negative-1", "fix-negative-1", emby_d5b_fix_negative_1_axes,
        emby_d5b_fix_dbs, rsh_matrix_assert_emby_d5b_fix, 3
    ),
    EMBY_DYNAMIC_MATRIX_PHASE(
        "fix-negative-2", "fix-negative-2", emby_d5b_fix_negative_2_axes,
        emby_d5b_fix_dbs, rsh_matrix_assert_emby_d5b_fix, 3
    ),
    EMBY_DYNAMIC_MATRIX_PHASE(
        "fix-negative-3", "fix-negative-3", emby_d5b_fix_negative_3_axes,
        emby_d5b_fix_dbs, rsh_matrix_assert_emby_d5b_fix, 3
    ),
    EMBY_DYNAMIC_MATRIX_PHASE(
        "fix-negative-4", "fix-negative-4", emby_d5b_fix_negative_4_axes,
        emby_d5b_fix_dbs, rsh_matrix_assert_emby_d5b_fix, 3
    ),
    EMBY_DYNAMIC_MATRIX_PHASE(
        "fix-negative-5", "fix-negative-5", emby_d5b_fix_negative_5_axes,
        emby_d5b_fix_dbs, rsh_matrix_assert_emby_d5b_fix, 3
    ),
    EMBY_DYNAMIC_MATRIX_PHASE(
        "fix-negative-6", "fix-negative-6", emby_d5b_fix_negative_6_axes,
        emby_d5b_fix_dbs, rsh_matrix_assert_emby_d5b_fix, 3
    ),
    EMBY_DYNAMIC_MATRIX_PHASE(
        "fix-negative-7", "fix-negative-7", emby_d5b_fix_negative_7_axes,
        emby_d5b_fix_dbs, rsh_matrix_assert_emby_d5b_fix, 3
    ),
    EMBY_DYNAMIC_MATRIX_PHASE(
        "fix-negative-8", "fix-negative-8", emby_d5b_fix_negative_8_axes,
        emby_d5b_fix_dbs, rsh_matrix_assert_emby_d5b_fix, 2
    ),
    EMBY_D5B_CASE_MATRIX_PHASE(
        "fix-missing-index", "fix-missing-index",
        emby_d5b_fix_missing_index_axes, emby_d5b_fix_missing_index_dbs,
        emby_d5b_fix_index_steps, 1
    )
};

static const rsh_case_spec emby_dashboard_fix_b_c_post_close_cases[] = {
    {
        .label = "dashboard-fix-b-c-log-assert",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_LOG_CAPTURE,
            .assert_custom = rsh_custom_adapter_dashboard_fix_b_c_log_assert,
        }
    }
};

EMBY_DEFINE_MATRIX_AXIS_3(
    emby_d5b_mixed_positive_1,
    "dashboard-mixed-latest-bind-0", EMBY_D5B_MIXED_POS_LITERAL_LITERAL,
    "dashboard-mixed-latest-bind-1", EMBY_D5B_MIXED_POS_BIND_LITERAL,
    "dashboard-mixed-latest-bind-2", EMBY_D5B_MIXED_POS_LITERAL_BIND
);
EMBY_DEFINE_MATRIX_AXIS_1(
    emby_d5b_mixed_positive_2,
    "dashboard-mixed-latest-bind-3", EMBY_D5B_MIXED_POS_BIND_BIND
);
EMBY_DEFINE_MATRIX_AXIS_1(
    emby_d5b_mixed_positive_reversed,
    "dashboard-mixed-latest-reversed-types", EMBY_D5B_MIXED_POS_REVERSED_TYPES
);
EMBY_DEFINE_MATRIX_AXIS_3(
    emby_d5b_mixed_probe_negatives,
    "dashboard-mixed-negative-0", EMBY_D5B_MIXED_NEG_TYPE8,
    "dashboard-mixed-negative-1", EMBY_D5B_MIXED_NEG_TYPE55,
    "dashboard-mixed-negative-2", EMBY_D5B_MIXED_NEG_TYPE856
);
EMBY_DEFINE_MATRIX_AXIS_3(
    emby_d5b_mixed_negatives_1,
    "dashboard-mixed-negative-3", EMBY_D5B_MIXED_NEG_TYPE_CASE,
    "dashboard-mixed-negative-4", EMBY_D5B_MIXED_NEG_FROM_CASE,
    "dashboard-mixed-negative-5", EMBY_D5B_MIXED_NEG_COALESCE
);
EMBY_DEFINE_MATRIX_AXIS_3(
    emby_d5b_mixed_negatives_2,
    "dashboard-mixed-negative-6", EMBY_D5B_MIXED_NEG_GROUP_CASE,
    "dashboard-mixed-negative-7", EMBY_D5B_MIXED_NEG_ORDER_DESC,
    "dashboard-mixed-negative-8", EMBY_D5B_MIXED_NEG_LIMIT4
);
EMBY_DEFINE_MATRIX_AXIS_3(
    emby_d5b_mixed_negatives_3,
    "dashboard-mixed-bind-reject-0", EMBY_D5B_MIXED_NEG_USER_Q1,
    "dashboard-mixed-bind-reject-1", EMBY_D5B_MIXED_NEG_USER_COLON,
    "dashboard-mixed-bind-reject-2", EMBY_D5B_MIXED_NEG_USER_AT
);
EMBY_DEFINE_MATRIX_AXIS_3(
    emby_d5b_mixed_negatives_4,
    "dashboard-mixed-bind-reject-3", EMBY_D5B_MIXED_NEG_USER_DOLLAR,
    "dashboard-mixed-bind-outside-slots", EMBY_D5B_MIXED_NEG_OUTSIDE_SLOTS,
    "dashboard-mixed-third-bind", EMBY_D5B_MIXED_NEG_THIRD_BIND
);
EMBY_DEFINE_MATRIX_AXIS_1(
    emby_d5b_mixed_outer_missing,
    "dashboard-mixed-outer-missing", EMBY_D5B_MIXED_OUTER_MISSING
);
EMBY_DEFINE_MATRIX_AXIS_1(
    emby_d5b_mixed_inner_missing,
    "dashboard-mixed-inner-missing", EMBY_D5B_MIXED_INNER_MISSING
);
EMBY_DEFINE_MATRIX_AXIS_1(
    emby_d5b_mixed_both_missing,
    "dashboard-mixed-both-missing", EMBY_D5B_MIXED_BOTH_MISSING
);
EMBY_DEFINE_MATRIX_AXIS_1(
    emby_d5b_mixed_outer_malformed,
    "dashboard-mixed-outer-mismatch", EMBY_D5B_MIXED_OUTER_MALFORMED
);
EMBY_DEFINE_MATRIX_AXIS_1(
    emby_d5b_mixed_inner_malformed,
    "dashboard-mixed-inner-mismatch", EMBY_D5B_MIXED_INNER_MALFORMED
);
EMBY_DEFINE_MATRIX_AXIS_1(
    emby_d5b_mixed_disabled,
    "dashboard-mixed-disabled", EMBY_D5B_MIXED_DISABLED
);

static const rsh_matrix_phase_spec emby_dashboard_mixed_latest_matrix_phases[] = {
    EMBY_DYNAMIC_MATRIX_PHASE(
        "mixed-positive-1", "mixed-positive-1",
        emby_d5b_mixed_positive_1_axes, emby_d5b_mixed_all_dbs,
        rsh_matrix_assert_emby_d5b_mixed_positive, 3
    ),
    EMBY_DYNAMIC_MATRIX_PHASE(
        "mixed-positive-2", "mixed-positive-2",
        emby_d5b_mixed_positive_2_axes, emby_d5b_mixed_all_dbs,
        rsh_matrix_assert_emby_d5b_mixed_positive, 1
    ),
    EMBY_DYNAMIC_MATRIX_PHASE(
        "mixed-positive-reversed", "mixed-positive-reversed",
        emby_d5b_mixed_positive_reversed_axes, emby_d5b_mixed_all_dbs,
        rsh_matrix_assert_emby_d5b_mixed_positive, 1
    ),
    EMBY_D5B_CASE_MATRIX_PHASE(
        "mixed-probe-negatives", "mixed-probe-negatives",
        emby_d5b_mixed_probe_negatives_axes, emby_d5b_mixed_all_dbs,
        emby_d5b_mixed_probe_steps, 3
    ),
    EMBY_D5B_CASE_MATRIX_PHASE(
        "mixed-negatives-1", "mixed-negatives-1",
        emby_d5b_mixed_negatives_1_axes, emby_d5b_mixed_all_dbs,
        emby_d5b_mixed_probe_steps, 3
    ),
    EMBY_D5B_CASE_MATRIX_PHASE(
        "mixed-negatives-2", "mixed-negatives-2",
        emby_d5b_mixed_negatives_2_axes, emby_d5b_mixed_all_dbs,
        emby_d5b_mixed_probe_steps, 3
    ),
    EMBY_D5B_CASE_MATRIX_PHASE(
        "mixed-negatives-3", "mixed-negatives-3",
        emby_d5b_mixed_negatives_3_axes, emby_d5b_mixed_all_dbs,
        emby_d5b_mixed_probe_steps, 3
    ),
    EMBY_D5B_CASE_MATRIX_PHASE(
        "mixed-negatives-4", "mixed-negatives-4",
        emby_d5b_mixed_negatives_4_axes, emby_d5b_mixed_all_dbs,
        emby_d5b_mixed_probe_steps, 3
    ),
    EMBY_D5B_CASE_MATRIX_PHASE(
        "mixed-outer-missing", "mixed-outer-missing",
        emby_d5b_mixed_outer_missing_axes,
        emby_d5b_mixed_outer_missing_dbs, emby_d5b_mixed_index_steps, 1
    ),
    EMBY_D5B_CASE_MATRIX_PHASE(
        "mixed-inner-missing", "mixed-inner-missing",
        emby_d5b_mixed_inner_missing_axes,
        emby_d5b_mixed_inner_missing_dbs, emby_d5b_mixed_index_steps, 1
    ),
    EMBY_D5B_CASE_MATRIX_PHASE(
        "mixed-both-missing", "mixed-both-missing",
        emby_d5b_mixed_both_missing_axes,
        emby_d5b_mixed_both_missing_dbs, emby_d5b_mixed_index_steps, 1
    ),
    EMBY_D5B_CASE_MATRIX_PHASE(
        "mixed-outer-malformed", "mixed-outer-malformed",
        emby_d5b_mixed_outer_malformed_axes,
        emby_d5b_mixed_outer_malformed_dbs, emby_d5b_mixed_index_steps, 1
    ),
    EMBY_D5B_CASE_MATRIX_PHASE(
        "mixed-inner-malformed", "mixed-inner-malformed",
        emby_d5b_mixed_inner_malformed_axes,
        emby_d5b_mixed_inner_malformed_dbs, emby_d5b_mixed_index_steps, 1
    )
};

static const rsh_matrix_phase_spec emby_dashboard_mixed_disabled_matrix_phases[] = {
    EMBY_D5B_CASE_MATRIX_PHASE(
        "mixed-disabled", "mixed-disabled", emby_d5b_mixed_disabled_axes,
        emby_d5b_mixed_all_dbs, emby_d5b_mixed_probe_steps, 1
    )
};

static const char emby_d5c_bind_qmark[] = "?";
static const char emby_d5c_bind_qmark_1[] = "?1";
static const char emby_d5c_bind_colon_name[] = ":name";
static const char emby_d5c_bind_at_name[] = "@name";
static const char emby_d5c_bind_dollar_name[] = "$name";
static const char emby_d5c_bind_colon_1[] = ":1";
static const char emby_d5c_bind_at_1[] = "@1";
static const char emby_d5c_bind_dollar_1[] = "$1";
static const char emby_d5c_bind_colon_e_acute[] = ":\xC3\xA9";

static const char *const emby_d5c_binds[] = {
    emby_d5c_bind_qmark,
    emby_d5c_bind_qmark_1,
    emby_d5c_bind_colon_name,
    emby_d5c_bind_at_name,
    emby_d5c_bind_dollar_name,
    emby_d5c_bind_colon_1,
    emby_d5c_bind_at_1,
    emby_d5c_bind_dollar_1,
    emby_d5c_bind_colon_e_acute
};

static const char emby_d5c_bind_episode_projection_needle[] =
    "select A.Id from mediaitems A";
static const char emby_d5c_bind_ancestor_needle[] = "in (100)";
static const char emby_d5c_bind_user_needle[] = "UserDatas.UserId=42";
static const char emby_d5c_bind_limit_needle[] = "LIMIT 12";
static const char emby_d5c_bind_movie_projection_needle[] = "A.Id,A.Name";

static const char *const emby_d5c_bind_needles[] = {
    emby_d5c_bind_episode_projection_needle,
    emby_d5c_bind_ancestor_needle,
    emby_d5c_bind_user_needle,
    emby_d5c_bind_limit_needle,
    emby_d5c_bind_movie_projection_needle,
    emby_d5c_bind_ancestor_needle,
    emby_d5c_bind_user_needle,
    emby_d5c_bind_limit_needle
};

static const char emby_d5c_bind_episode_projection_format[] =
    "select %s AS bound,A.Id from mediaitems A";
static const char emby_d5c_bind_ancestor_format[] = "in (%s)";
static const char emby_d5c_bind_user_format[] = "UserDatas.UserId=%s";
static const char emby_d5c_bind_limit_format[] = "LIMIT %s";
static const char emby_d5c_bind_movie_projection_format[] =
    "%s AS bound,A.Id,A.Name";

static const char *const emby_d5c_bind_replacement_formats[] = {
    emby_d5c_bind_episode_projection_format,
    emby_d5c_bind_ancestor_format,
    emby_d5c_bind_user_format,
    emby_d5c_bind_limit_format,
    emby_d5c_bind_movie_projection_format,
    emby_d5c_bind_ancestor_format,
    emby_d5c_bind_user_format,
    emby_d5c_bind_limit_format
};

static const char emby_d5c_episode_projection_needle[] =
    "select A.Id from mediaitems A";
static const char emby_d5c_episode_projection_replacement[] =
    "select * from mediaitems A";
static const char emby_d5c_episode_group_needle[] =
    " Group by coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey)";
static const char emby_d5c_episode_group_replacement[] =
    " Group by A.PresentationUniqueKey";
static const char emby_d5c_episode_order_needle[] =
    " ORDER BY MAX(A.DateCreated) DESC";
static const char emby_d5c_episode_order_replacement[] =
    " ORDER BY A.DateCreated DESC";
static const char emby_d5c_episode_limit_needle[] = " LIMIT 12";
static const char emby_d5c_episode_limit_replacement[] = " LIMIT 11";

static const char *const emby_d5c_episode_structural_needles[] = {
    emby_d5c_episode_projection_needle,
    emby_d5c_episode_group_needle,
    emby_d5c_episode_order_needle,
    emby_d5c_episode_limit_needle
};

static const char *const emby_d5c_episode_structural_replacements[] = {
    emby_d5c_episode_projection_replacement,
    emby_d5c_episode_group_replacement,
    emby_d5c_episode_order_replacement,
    emby_d5c_episode_limit_replacement
};

static const char emby_d5c_movie_projection_needle[] = "A.Id,A.Name";
static const char emby_d5c_movie_projection_reorder[] = "A.Name,A.Id";
static const char emby_d5c_movie_group_needle[] =
    " Group by A.PresentationUniqueKey";
static const char emby_d5c_movie_group_replacement[] = " Group by A.Id";
static const char emby_d5c_movie_order_needle[] =
    " ORDER BY A.DateCreated DESC";
static const char emby_d5c_movie_order_replacement[] =
    " ORDER BY A.Name DESC";
static const char emby_d5c_movie_limit_needle[] = " LIMIT 12";
static const char emby_d5c_movie_limit_replacement[] = " LIMIT 21";
static const char emby_d5c_movie_anchor_needle[] =
    "AND A.Id in WithAncestors";
static const char emby_d5c_movie_anchor_replacement[] =
    "AND A.Id in WithAncestors AND A.IsPublic=1";
static const char emby_d5c_movie_aggregate_replacement[] =
    "MAX(A.Id),A.Name";

static const char *const emby_d5c_movie_structural_needles[] = {
    emby_d5c_movie_projection_needle,
    emby_d5c_movie_group_needle,
    emby_d5c_movie_order_needle,
    emby_d5c_movie_limit_needle,
    emby_d5c_movie_anchor_needle,
    emby_d5c_movie_projection_needle
};

static const char *const emby_d5c_movie_structural_replacements[] = {
    emby_d5c_movie_projection_reorder,
    emby_d5c_movie_group_replacement,
    emby_d5c_movie_order_replacement,
    emby_d5c_movie_limit_replacement,
    emby_d5c_movie_anchor_replacement,
    emby_d5c_movie_aggregate_replacement
};

static const char emby_d5c_movie_no_guard_discriminator[] =
    "where A.Type=5 AND A.Id in WithAncestors";
static const char emby_d5c_movie_no_guard_sql[] =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) )select "
    "A.Id,A.Name,A.Path,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.Images,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex "
    "from mediaitems A left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=42 "
    "where A.Type=5 AND A.Id in WithAncestors Group by A.PresentationUniqueKey ORDER BY A.DateCreated DESC LIMIT 12";
static const char emby_d5c_movie_empty_ancestor_discriminator[] =
    "AncestorId in ()";
static const char emby_d5c_movie_empty_ancestor_sql[] =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in () )select "
    "A.Id,A.Name,A.Path,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.Images,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex "
    "from mediaitems A left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=42 "
    "where A.Type=5 AND Coalesce(UserDatas.played, 0)=0 AND A.Id in WithAncestors Group by A.PresentationUniqueKey ORDER BY A.DateCreated DESC LIMIT 12";
static const char emby_d5c_movie_comment_needle[] =
    "where A.Type=5 AND Coalesce(UserDatas.played, 0)=0";
static const char emby_d5c_movie_comment_replacement[] =
    "where A.Type=5 /* guarded tail text: where A.Type=5 AND Coalesce(UserDatas.played, 0)=0 */ AND Coalesce(UserDatas.played, 0)=0";
static const char emby_d5c_movie_string_projection_replacement[] =
    "'where A.Type=5 ' AS marker,A.Id,A.Name";
static const char emby_d5c_movie_type_gate_needle[] = "where A.Type=5 ";
static const char emby_d5c_episode_explain_discriminator[] =
    "EXPLAIN with WithAncestors AS";
static const char emby_d5c_episode_explain_sql[] =
    "EXPLAIN with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) )select A.Id from mediaitems A left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=42 "
    "where A.Type=8 AND Coalesce(UserDatas.played, 0)=0 AND A.Id in WithAncestors Group by coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey) ORDER BY MAX(A.DateCreated) DESC LIMIT 12";
static const char emby_d5c_applied_log_needle[] =
    "event=rewrite_applied target=emby mode=dashboard+";
static const char emby_d5c_index_missing_log_needle[] =
    "reason=index_missing mode=dashboard+";
static const char emby_d5c_index_probe_error_log_needle[] =
    "reason=index_probe_error mode=dashboard+";
static const char emby_d5c_bind_capture_start_marker[] =
    "rsh-window=dashboard-bind start";
static const char emby_d5c_bind_capture_end_marker[] =
    "rsh-window=dashboard-bind end";

static sqlite_master_auth_probe emby_d5c_matcher_probe;

static int rsh_matrix_assert_emby_d5c_matcher_negatives(
    const rsh_case_context *context,
    const rsh_matrix_cell_step *step,
    void *ctx
) {
    const emby_matrix_case *matrix_case = rsh_emby_matrix_case(context);
    sqlite3 *candidate = rsh_context_db(context, "candidate");
    char *episodes = make_latest_sql("A.Id ", "12");
    char *movies = make_movies_latest_sql(1, "100", "42", "12");
    size_t i;

    (void)step;
    (void)ctx;
    if (matrix_case->case_id != 1) {
        failf(
            "FAIL [dashboard-matcher-negatives/case-id]: got=%d want=1",
            matrix_case->case_id
        );
    }

    memset(&emby_d5c_matcher_probe, 0, sizeof(emby_d5c_matcher_probe));
    fprintf(stderr, "%s\n", emby_d5c_bind_capture_start_marker);
    fflush(stderr);
    require_int(
        "dashboard-bind/authorizer-set",
        sqlite3_set_authorizer(
            candidate, count_sqlite_master_read, &emby_d5c_matcher_probe
        ),
        SQLITE_OK
    );
    for (i = 0; i < sizeof(emby_d5c_binds) / sizeof(emby_d5c_binds[0]); i++) {
        size_t c;
        for (c = 0; c < sizeof(emby_d5c_bind_needles) /
                            sizeof(emby_d5c_bind_needles[0]); c++) {
            const char *base = c < 4 ? episodes : movies;
            char label[128];
            char *replacement = xasprintf(
                emby_d5c_bind_replacement_formats[c], emby_d5c_binds[i]
            );

            snprintf(
                label, sizeof(label), "dashboard-bind-%lu-%lu",
                (unsigned long)i, (unsigned long)c
            );
            rsh_run_emby_matrix_generated_negative(
                context, label, base, emby_d5c_bind_needles[c], replacement,
                replacement
            );
            free(replacement);
        }
    }
    require_int(
        "dashboard-bind/authorizer-clear",
        sqlite3_set_authorizer(candidate, NULL, NULL), SQLITE_OK
    );
    fprintf(stderr, "%s\n", emby_d5c_bind_capture_end_marker);
    fflush(stderr);

    for (i = 0; i < sizeof(emby_d5c_episode_structural_needles) /
                        sizeof(emby_d5c_episode_structural_needles[0]); i++) {
        char label[128];
        snprintf(
            label, sizeof(label), "episodes-structural-negative[%lu]",
            (unsigned long)i
        );
        rsh_run_emby_matrix_generated_negative(
            context, label, episodes,
            emby_d5c_episode_structural_needles[i],
            emby_d5c_episode_structural_replacements[i],
            emby_d5c_episode_structural_replacements[i]
        );
    }
    for (i = 0; i < sizeof(emby_d5c_movie_structural_needles) /
                        sizeof(emby_d5c_movie_structural_needles[0]); i++) {
        char label[128];
        if (i == 0) {
            char *source = replace_once(
                movies, emby_d5c_movie_structural_needles[i],
                emby_d5c_movie_structural_replacements[i]
            );
            char *projection = replace_once(
                EMBY_MOVIES_LATEST_COMPACT_PROJECTION,
                emby_d5c_movie_structural_needles[i],
                emby_d5c_movie_structural_replacements[i]
            );
            char *expected = make_movies_latest_expected_projection(
                "100", projection, "42", "12"
            );

            snprintf(
                label, sizeof(label), "movies-structural-positive[%lu]",
                (unsigned long)i
            );
            rsh_run_emby_matrix_sql_exact(
                context, label, source, expected,
                RSH_PREPARE_V2, RSH_NBYTE_MINUS_ONE
            );
            free(expected);
            free(projection);
            free(source);
        } else {
            snprintf(
                label, sizeof(label), "movies-structural-negative[%lu]",
                (unsigned long)i
            );
            rsh_run_emby_matrix_generated_negative(
                context, label, movies,
                emby_d5c_movie_structural_needles[i],
                emby_d5c_movie_structural_replacements[i],
                emby_d5c_movie_structural_replacements[i]
            );
        }
    }

    {
        char *movies_string_projection = replace_once(
            EMBY_MOVIES_LATEST_COMPACT_PROJECTION,
            emby_d5c_movie_projection_needle,
            emby_d5c_movie_string_projection_replacement
        );
        char *movies_string = make_movies_latest_sql_form(
            1, "100", 0, movies_string_projection, "42", "12"
        );
        char *movies_string_expected = make_movies_latest_expected_projection(
            "100", movies_string_projection, "42", "12"
        );

        rsh_run_emby_matrix_static_negative(
            context, "movies-no-guard-negative", emby_d5c_movie_no_guard_sql,
            emby_d5c_movie_no_guard_discriminator
        );
        rsh_run_emby_matrix_static_negative(
            context, "movies-empty-ancestor-negative",
            emby_d5c_movie_empty_ancestor_sql,
            emby_d5c_movie_empty_ancestor_discriminator
        );
        rsh_run_emby_matrix_generated_negative(
            context, "movies-comment-tail-negative", movies,
            emby_d5c_movie_comment_needle,
            emby_d5c_movie_comment_replacement,
            emby_d5c_movie_comment_replacement
        );
        require_int(
            "movies-string-type-gate-immunity/type-gate-count",
            count_occurrences(movies_string, emby_d5c_movie_type_gate_needle),
            2
        );
        rsh_run_emby_matrix_sql_exact(
            context, "movies-string-type-gate-immunity", movies_string,
            movies_string_expected, RSH_PREPARE_V2, RSH_NBYTE_MINUS_ONE
        );
        rsh_run_emby_matrix_static_negative(
            context, "episodes-explain-negative", emby_d5c_episode_explain_sql,
            emby_d5c_episode_explain_discriminator
        );

        free(movies_string_expected);
        free(movies_string);
        free(movies_string_projection);
    }

    free(movies);
    free(episodes);
    return SQLITE_OK;
}

static int rsh_custom_adapter_emby_d5c_matcher_log_assert(
    const rsh_case_context *context,
    const void *immutable_data
) {
    const char *capture_start;
    const char *capture_end;
    char *bind_capture;
    size_t bind_capture_len;

    (void)immutable_data;
    require_int(
        "dashboard-bind/readiness-probes", emby_d5c_matcher_probe.reads, 0
    );
    require_int(
        "dashboard-bind/capture-start-markers",
        count_occurrences(
            context->captured_stderr, emby_d5c_bind_capture_start_marker
        ),
        1
    );
    require_int(
        "dashboard-bind/capture-end-markers",
        count_occurrences(
            context->captured_stderr, emby_d5c_bind_capture_end_marker
        ),
        1
    );
    capture_start = strstr(
        context->captured_stderr, emby_d5c_bind_capture_start_marker
    );
    capture_start += strlen(emby_d5c_bind_capture_start_marker);
    capture_end = strstr(capture_start, emby_d5c_bind_capture_end_marker);
    if (!capture_end) {
        failf("FAIL [dashboard-bind/capture-window]: end marker precedes start");
    }
    bind_capture_len = (size_t)(capture_end - capture_start);
    bind_capture = (char *)malloc(bind_capture_len + 1);
    if (!bind_capture) {
        failf(
            "FAIL [dashboard-bind/capture-window-allocation]: bytes=%lu",
            (unsigned long)(bind_capture_len + 1)
        );
    }
    memcpy(bind_capture, capture_start, bind_capture_len);
    bind_capture[bind_capture_len] = '\0';
    require_absent(
        "dashboard-bind/applied-log", bind_capture,
        emby_d5c_applied_log_needle
    );
    require_absent(
        "dashboard-bind/index-missing-log", bind_capture,
        emby_d5c_index_missing_log_needle
    );
    require_absent(
        "dashboard-bind/index-probe-error-log", bind_capture,
        emby_d5c_index_probe_error_log_needle
    );
    free(bind_capture);
    return SQLITE_OK;
}

static const rsh_db_spec emby_d5c_matcher_dbs[] = {
    {
        .role = "vendor",
        .relative_path = "not-target.db",
        .kind = RSH_DB_VENDOR,
        .storage = RSH_DB_RELATIVE,
        .seed_profile = EMBY_PROFILE_D5B_FIX_CANDIDATE
    },
    {
        .role = "candidate",
        .relative_path = "library.db",
        .kind = RSH_DB_CANDIDATE,
        .storage = RSH_DB_RELATIVE,
        .seed_profile = EMBY_PROFILE_D5B_FIX_CANDIDATE
    }
};

EMBY_DEFINE_MATRIX_AXIS_1(
    emby_d5c_matcher, "dashboard-matcher-negatives", 1
);

static const rsh_matrix_phase_spec emby_dashboard_matcher_negative_phases[] = {
    EMBY_DYNAMIC_MATRIX_PHASE(
        "dashboard-matcher-negatives", "dashboard-matcher-negatives",
        emby_d5c_matcher_axes, emby_d5c_matcher_dbs,
        rsh_matrix_assert_emby_d5c_matcher_negatives, 1
    )
};

static const rsh_case_spec emby_dashboard_matcher_negative_post_close_cases[] = {
    {
        .label = "dashboard-matcher-negatives-log-assert",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_LOG_CAPTURE,
            .assert_custom = rsh_custom_adapter_emby_d5c_matcher_log_assert,
        }
    }
};

#define EMBY_D6_COMPLEX_RESUME_PREFIX \
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) )select count(*) OVER() AS TotalRecordCount,A.type,A.Id,A.SeriesPresentationUniqueKey,UserDatas.PlaybackPositionTicks," \
    "((Coalesce(A.SortParentIndexNumber,A.ParentIndexNumber, 1) * 1000000) + Coalesce(A.SortIndexNumber, A.IndexNumber, 0) + (Select Case When Coalesce(A.ParentIndexNumber,1)=0 Then 0 Else 0.5 End) + (Select Case When Coalesce(A.ParentIndexNumber,1)=0 Then (Cast(Coalesce(A.IndexNumber, 0) as REAL) / 100000) Else 0 End)) EpisodeAbsoluteIndexNumber " \
    "from mediaitems A left join (Select N.SeriesPresentationUniqueKey,((Coalesce(N.SortParentIndexNumber,N.ParentIndexNumber, 1) * 1000000) + Coalesce(N.SortIndexNumber, N.IndexNumber, 0) + (Select Case When Coalesce(N.ParentIndexNumber,1)=0 Then 0 Else 0.5 End) + (Select Case When Coalesce(N.ParentIndexNumber,1)=0 Then (Cast(Coalesce(N.IndexNumber, 0) as REAL) / 100000) Else 0 End)) AbsoluteIndexNumber,max(UserDatas_N.LastPlayedDateInt) LastPlayedDateInt,UserDatas_N.playbackPositionTicks from MediaItems N join UserDatas UserDatas_N on N.UserDataKeyId=UserDatas_N.UserDataKeyId And UserDatas_N.UserId=1 where N.Type=8 and Coalesce(N.SortParentIndexNumber,N.ParentIndexNumber,-1) <> 0 and (UserDatas_N.Played=1 or UserDatas_N.playbackPositionTicks > 0) Group By N.SeriesPresentationUniqueKey ORDER BY UserDatas_N.LastPlayedDateInt desc, AbsoluteIndexNumber desc) LastWatchedEpisodes on LastWatchedEpisodes.SeriesPresentationUniqueKey=A.SeriesPresentationUniqueKey " \
    "left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=1 where ((A.Type=5 and UserDatas.playbackPositionTicks > 0) OR (A.Type=8 AND (UserDatas.playbackPositionTicks > 0 or Coalesce(UserDatas.played,0) = 0) AND (select case when LastWatchedEpisodes.playbackPositionTicks > 0 then EpisodeAbsoluteIndexNumber >= Coalesce(LastWatchedEpisodes.AbsoluteIndexNumber,EpisodeAbsoluteIndexNumber) else EpisodeAbsoluteIndexNumber > Coalesce(LastWatchedEpisodes.AbsoluteIndexNumber,EpisodeAbsoluteIndexNumber) end) AND LastWatchedEpisodes.LastPlayedDateInt not null)) " \
    "AND (A.Type=5 OR Coalesce(A.SortParentIndexNumber,A.ParentIndexNumber, -1) <> 0) AND A.Type in (5,8) AND Coalesce(UserDatas.HideFromResume,0)=0 AND COALESCE((select hidefromresume from userdatas where userdatas.userid=1 and userdatas.userdatakeyid=(select userdatakeyid from mediaitems where mediaitems.id=A.seriesid)),0)=0 AND "
#define EMBY_D6_COMPLEX_RESUME_TAIL \
    " Group by coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey) ORDER BY COALESCE(lastwatchedepisodes.lastplayeddateint, userdatas.lastplayeddateint, 0) DESC,Min(EpisodeAbsoluteIndexNumber) ASC LIMIT 12"
#define EMBY_D6_COMPLEX_RESUME_SOURCE \
    EMBY_D6_COMPLEX_RESUME_PREFIX "A.Id in WithAncestors" \
    EMBY_D6_COMPLEX_RESUME_TAIL
#define EMBY_D6_COMPLEX_RESUME_EXPECTED \
    EMBY_D6_COMPLEX_RESUME_PREFIX \
    "EXISTS (SELECT 1 FROM AncestorIds2 WHERE AncestorIds2.itemid = A.Id AND AncestorIds2.AncestorId in (100))" \
    " AND ((A.Type=5 AND A.UserDataKeyId IN (SELECT UserDataKeyId FROM UserDatas WHERE UserId=1 AND playbackPositionTicks>0)) OR (A.Type=8 AND A.SeriesPresentationUniqueKey IN (SELECT N2.SeriesPresentationUniqueKey FROM MediaItems N2 JOIN UserDatas UN2 ON N2.UserDataKeyId=UN2.UserDataKeyId AND UN2.UserId=1 WHERE N2.Type=8 AND Coalesce(N2.SortParentIndexNumber,N2.ParentIndexNumber,-1) <> 0 AND (UN2.Played=1 OR UN2.playbackPositionTicks>0))))" \
    EMBY_D6_COMPLEX_RESUME_TAIL

static const char emby_d6_complex_resume_sql[] =
    EMBY_D6_COMPLEX_RESUME_SOURCE;
static const char emby_d6_complex_resume_expected_sql[] =
    EMBY_D6_COMPLEX_RESUME_EXPECTED;
static const char emby_d6_complex_resume_expected_ids_sql[] =
    "SELECT Id FROM (" EMBY_D6_COMPLEX_RESUME_EXPECTED ")";

#undef EMBY_D6_COMPLEX_RESUME_EXPECTED
#undef EMBY_D6_COMPLEX_RESUME_SOURCE
#undef EMBY_D6_COMPLEX_RESUME_TAIL
#undef EMBY_D6_COMPLEX_RESUME_PREFIX

static const sqlite3_int64 emby_d6_complex_resume_expected_ids[] = {
    102, 100
};

typedef struct emby_d6_row_parity_membership_spec {
    const char *vendor_collect_label;
    const char *candidate_collect_label;
    const char *vendor_assertion_label;
    const char *candidate_assertion_label;
    const char *search_term;
} emby_d6_row_parity_membership_spec;

static const emby_d6_row_parity_membership_spec
emby_d6_row_parity_single_membership = {
    .vendor_collect_label = "row-parity-original",
    .candidate_collect_label = "row-parity-rewritten",
    .vendor_assertion_label = "row-parity/original-membership",
    .candidate_assertion_label = "row-parity/candidate-membership",
    .search_term = "(\"alpha\"*)"
};

static const emby_d6_row_parity_membership_spec
emby_d6_row_parity_or_membership = {
    .vendor_collect_label = "row-parity-or-original",
    .candidate_collect_label = "row-parity-or-rewritten",
    .vendor_assertion_label = "row-parity-or/original-membership",
    .candidate_assertion_label = "row-parity-or/candidate-membership",
    .search_term = "(\"alpha\"*) OR (\"alpha\"*)"
};

static int rsh_custom_adapter_row_parity_membership(
    const rsh_case_context *context,
    const void *immutable_data
) {
    const emby_d6_row_parity_membership_spec *spec =
        (const emby_d6_row_parity_membership_spec *)immutable_data;
    sqlite3 *vendor_db = rsh_context_db(context, "vendor");
    sqlite3 *candidate_db = rsh_context_db(context, "candidate");
    unsigned int vendor_mask;
    unsigned int candidate_mask;

    vendor_mask = collect_fts_id_mask(
        vendor_db, spec->vendor_collect_label, emby_d6_row_parity_sql,
        -1, 0, spec->search_term
    );
    candidate_mask = collect_fts_id_mask(
        candidate_db, spec->candidate_collect_label, emby_d6_row_parity_sql,
        (int)strlen(emby_d6_row_parity_sql) + 1, 1, spec->search_term
    );
    if (vendor_mask != 0x1f) {
        failf("FAIL [%s]: got=0x%x want=0x1f",
              spec->vendor_assertion_label, vendor_mask);
    }
    if (candidate_mask != 0x1f) {
        failf("FAIL [%s]: got=0x%x want=0x1f",
              spec->candidate_assertion_label, candidate_mask);
    }
    return SQLITE_OK;
}

static int rsh_custom_adapter_links_two_level_identity(
    const rsh_case_context *context,
    const void *immutable_data
) {
    sqlite3 *vendor_db = rsh_context_db(context, "vendor");
    sqlite3 *candidate_db = rsh_context_db(context, "candidate");
    char *expected = make_link_type_count_shape_05_expected();
    typed_rows vendor_rows;
    typed_rows candidate_rows;

    (void)immutable_data;
    require_int(
        "links-two-level/l1-only-fixture",
        query_int(vendor_db, "links-two-level/l1-only-fixture-sql",
                  "SELECT count(*) FROM ItemLinks2 AS L JOIN AncestorIds2 AS X "
                  "ON X.itemid=L.ItemId WHERE X.AncestorId=15 AND L.LinkedId=1001 "
                  "AND L.Type IN (6,4,3,2)"),
        1
    );
    require_int(
        "links-two-level/l2-only-fixture",
        query_int(vendor_db, "links-two-level/l2-only-fixture-sql",
                  "SELECT count(*) FROM ItemLinks2 AS L2 WHERE L2.LinkedId=1002 "
                  "AND EXISTS (SELECT 1 FROM ItemLinks2 AS L1 JOIN AncestorIds2 AS X "
                  "ON X.itemid=L1.ItemId WHERE X.AncestorId=15 "
                  "AND L1.LinkedId=L2.ItemId AND L1.Type IN (7,0,1,5,6,2))"),
        1
    );
    require_int(
        "links-two-level/both-direct-duplicates",
        query_int(vendor_db, "links-two-level/both-direct-duplicates-sql",
                  "SELECT count(*) FROM ItemLinks2 WHERE ItemId=2003 "
                  "AND LinkedId=1003 AND Type=6"),
        2
    );
    require_int(
        "links-two-level/both-indirect-duplicates",
        query_int(vendor_db, "links-two-level/both-indirect-duplicates-sql",
                  "SELECT count(*) FROM ItemLinks2 WHERE ItemId=2103 "
                  "AND LinkedId=1003"),
        2
    );
    require_int(
        "links-two-level/null-linked-id-fixture",
        query_int(vendor_db, "links-two-level/null-linked-id-fixture-sql",
                  "SELECT count(*) FROM ItemLinks2 WHERE ItemId=2004 "
                  "AND LinkedId IS NULL AND Type=6"),
        1
    );
    require_int(
        "links-two-level/no-hit-fixture",
        query_int(vendor_db, "links-two-level/no-hit-fixture-sql",
                  "SELECT count(*) FROM ItemLinks2 WHERE LinkedId=1004"),
        0
    );

    expect_sql(candidate_db, "links-two-level/rewrite-fired",
               EMBY_LINK_TYPE_COUNT_SHAPE_05_SQL, -1, 2, expected, 0);
    contract_parity_require(
        vendor_db, candidate_db, contract_prepare_v2,
        "links-two-level/ordered-contract",
        EMBY_LINK_TYPE_COUNT_SHAPE_05_SQL,
        EMBY_LINK_TYPE_COUNT_SHAPE_05_SQL,
        expected, NULL, NULL, NULL, NULL
    );
    vendor_rows = collect_typed_rows(
        vendor_db, "links-two-level/vendor-typed-rows",
        EMBY_LINK_TYPE_COUNT_SHAPE_05_SQL,
        EMBY_LINK_TYPE_COUNT_SHAPE_05_SQL
    );
    candidate_rows = collect_typed_rows(
        candidate_db, "links-two-level/candidate-typed-rows",
        EMBY_LINK_TYPE_COUNT_SHAPE_05_SQL,
        expected
    );
    require_ordered_full_row_identity(
        "links-two-level/ordered-type-tagged-identity",
        &vendor_rows, &candidate_rows
    );
    require_str_eq(
        "links-two-level/vendor-exact-output",
        (const char *)vendor_rows.bytes,
        "C1;N3;RI:9;RI:29;RI:34;"
    );
    require_str_eq(
        "links-two-level/candidate-exact-output",
        (const char *)candidate_rows.bytes,
        "C1;N3;RI:9;RI:29;RI:34;"
    );

    free_typed_rows(&vendor_rows);
    free_typed_rows(&candidate_rows);
    free(expected);
    return SQLITE_OK;
}

static int rsh_custom_adapter_dashboard_mixed_identity(
    const rsh_case_context *context,
    const void *immutable_data
) {
    sqlite3 *vendor_db = rsh_context_db(context, "vendor");
    sqlite3 *candidate_db = rsh_context_db(context, "candidate");
    int analyzed;
    char *raw;
    char *expected;
    typed_rows vendor_rows;
    typed_rows candidate_rows;
    char ids[128];
    static contract_parity_prepare_fn const prepare_entries[] = {
        contract_prepare_legacy, contract_prepare_v2, contract_prepare_v3
    };
    static const char *const prepare_labels[] = {"legacy", "v2", "v3"};
    static const char *const projections[] = {
        EMBY_MIXED_LATEST_PROJECTION,
        EMBY_MIXED_LATEST_PRODUCTION_YEAR_PROJECTION,
        EMBY_MIXED_LATEST_MB1_PROJECTION
    };
    static const int projection_columns[] = {19, 20, 22};
    size_t projection_i;
    size_t prepare_i;

    (void)immutable_data;
    if (!context->matrix_cell || context->matrix_cell->axis_count != 1) {
        failf("FAIL [dashboard-mixed-identity/matrix-context]");
    }
    analyzed = context->matrix_cell->axis_indices[0] == 1;

    require_int(
        "mixed-production-year/vendor-fixture",
        query_int(
            vendor_db, "mixed-production-year/vendor-fixture-sql",
            "SELECT ProductionYear FROM MediaItems WHERE Id=2001"
        ),
        2026
    );
    require_int(
        "mixed-production-year/candidate-fixture",
        query_int(
            candidate_db, "mixed-production-year/candidate-fixture-sql",
            "SELECT ProductionYear FROM MediaItems WHERE Id=2001"
        ),
        2026
    );

    for (projection_i = 0;
         projection_i < sizeof(projections) / sizeof(projections[0]);
         projection_i++) {
        raw = make_mixed_latest_sql_projection_form(
            "100", projections[projection_i], "42", "20"
        );
        expected = make_mixed_latest_expected_projection_form(
            "100", projections[projection_i], "42", "20"
        );
        for (prepare_i = 0;
             prepare_i < sizeof(prepare_entries) / sizeof(prepare_entries[0]);
             prepare_i++) {
            char label[128];
            snprintf(
                label, sizeof(label),
                "mixed-%dcol-limit20-%s%s",
                projection_columns[projection_i], prepare_labels[prepare_i],
                analyzed ? "-after-analyze" : "-before-analyze"
            );
            contract_parity_require(
                vendor_db, candidate_db, prepare_entries[prepare_i], label,
                raw, raw, expected, NULL, NULL, NULL, NULL
            );
        }
        free(raw);
        free(expected);
    }

    raw = make_mixed_latest_sql_projection_form(
        "100", EMBY_MIXED_LATEST_PRODUCTION_YEAR_PROJECTION, "42", "20"
    );
    expected = make_mixed_latest_expected_projection_form(
        "100", EMBY_MIXED_LATEST_PRODUCTION_YEAR_PROJECTION, "42", "20"
    );
    require_mixed_production_year_row(
        vendor_db, "mixed-production-year/vendor-result", raw, raw
    );
    require_mixed_production_year_row(
        candidate_db, "mixed-production-year/candidate-result", raw, expected
    );
    free(raw);
    free(expected);

    raw = make_mixed_latest_sql_projection_form(
        "100", EMBY_MIXED_LATEST_MB1_PROJECTION, "42", "20"
    );
    {
        char *production_order = replace_once(
            raw, "where A.Type in (8,5) ", "where A.Type in (5,8) "
        );
        free(raw);
        raw = production_order;
    }
    expected = make_mixed_latest_expected_projection_form(
        "100", EMBY_MIXED_LATEST_MB1_PROJECTION, "42", "20"
    );
    for (prepare_i = 0;
         prepare_i < sizeof(prepare_entries) / sizeof(prepare_entries[0]);
         prepare_i++) {
        char label[128];
        snprintf(
            label, sizeof(label), "mixed-mb1-exact-limit20-%s%s",
            prepare_labels[prepare_i],
            analyzed ? "-after-analyze" : "-before-analyze"
        );
        contract_parity_require(
            vendor_db, candidate_db, prepare_entries[prepare_i], label,
            raw, raw, expected, NULL, NULL, NULL, NULL
        );
    }
    free(raw);
    free(expected);

    raw = make_mixed_latest_sql("42", "3");
    expected = make_mixed_latest_expected("42", "3");
    vendor_rows = collect_typed_rows(
        vendor_db,
        analyzed ? "mixed-vendor-after-analyze" : "mixed-vendor-before-analyze",
        raw, raw
    );
    candidate_rows = collect_typed_rows(
        candidate_db,
        analyzed ? "mixed-candidate-after-analyze" : "mixed-candidate-before-analyze",
        raw, expected
    );
    require_ordered_full_row_identity(
        analyzed ? "mixed-byte-identity-after-analyze" :
                   "mixed-byte-identity-before-analyze",
        &vendor_rows, &candidate_rows
    );
    free_typed_rows(&vendor_rows);
    free_typed_rows(&candidate_rows);
    free(raw);
    free(expected);
    if (!analyzed) return SQLITE_OK;

    raw = make_mixed_latest_sql("?", "?");
    expected = make_mixed_latest_expected("?", "?");
    {
        mixed_limit_bind_case limit_cases[] = {
            MIXED_LIMIT_ZERO,
            MIXED_LIMIT_NEGATIVE,
            MIXED_LIMIT_NULL,
            MIXED_LIMIT_TEXT_THREE,
            MIXED_LIMIT_TEXT_GARBAGE
        };
        size_t i;
        for (i = 0; i < sizeof(limit_cases) / sizeof(limit_cases[0]); i++) {
            char vendor_label[96];
            char candidate_label[96];
            int vendor_rc;
            int candidate_rc;
            snprintf(vendor_label, sizeof(vendor_label),
                     "mixed-bound-edge-vendor-%lu", (unsigned long)i);
            snprintf(candidate_label, sizeof(candidate_label),
                     "mixed-bound-edge-candidate-%lu", (unsigned long)i);
            vendor_rows = collect_bound_mixed_rows(
                vendor_db, vendor_label, raw, raw, limit_cases[i], &vendor_rc
            );
            candidate_rows = collect_bound_mixed_rows(
                candidate_db, candidate_label, raw, expected,
                limit_cases[i], &candidate_rc
            );
            require_int("mixed-bound-edge/rc-parity", candidate_rc, vendor_rc);
            require_ordered_full_row_identity(
                "mixed-bound-edge/typed-row-parity", &vendor_rows, &candidate_rows
            );
            free_typed_rows(&vendor_rows);
            free_typed_rows(&candidate_rows);
        }
    }
    free(raw);
    free(expected);

#define EMBY_D6_MIXED_ID_ASSERT( \
    ancestor, collect_label, assertion_label, expected_ids) \
    do { \
        raw = make_mixed_latest_sql_form((ancestor), "42", "3"); \
        expected = make_mixed_latest_expected_form((ancestor), "42", "3"); \
        collect_int_column(candidate_db, (collect_label), raw, expected, -1, 1, \
                           ids, sizeof(ids)); \
        require_str_eq((assertion_label), ids, (expected_ids)); \
        free(raw); \
        free(expected); \
    } while (0)

    EMBY_D6_MIXED_ID_ASSERT(
        "101", "mixed-all-null-date", "mixed-all-null-date/lower-id", "2010,"
    );
    EMBY_D6_MIXED_ID_ASSERT(
        "102", "mixed-played-state", "mixed-played-state/exclusion", "2021,"
    );
    EMBY_D6_MIXED_ID_ASSERT(
        "103", "mixed-ancestor-invisible",
        "mixed-ancestor-invisible/exclusion", "2031,"
    );
    EMBY_D6_MIXED_ID_ASSERT(
        "105", "mixed-same-date-lower-id",
        "mixed-same-date-lower-id/selection", "2039,"
    );
    /* The optimized plan resolves equal DateCreated groups in descending
     * group-key order.  Lock that LIMIT-boundary behavior independently of
     * the synthetic fixture's planner/index state. */
    EMBY_D6_MIXED_ID_ASSERT(
        "104", "mixed-limit-boundary-gk", "mixed-limit-boundary-gk/order",
        "2050,2052,2053,"
    );

#undef EMBY_D6_MIXED_ID_ASSERT

    return SQLITE_OK;
}

static const rsh_db_spec emby_d6_row_parity_dbs[] = {
    {
        .role = "vendor",
        .relative_path = "not-target.db",
        .kind = RSH_DB_VENDOR,
        .storage = RSH_DB_RELATIVE
    },
    {
        .role = "candidate",
        .relative_path = "library.db",
        .kind = RSH_DB_CANDIDATE,
        .storage = RSH_DB_RELATIVE
    }
};

static const rsh_case_spec emby_d6_row_parity_cases[] = {
    {
        .label = "emby-fts-contract",
        .kind = RSH_CASE_CONTRACT_PARITY,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.contract_parity = {
            .vendor_role = "vendor",
            .candidate_role = "candidate",
            .vendor_sql = emby_d6_row_parity_sql,
            .candidate_source_sql = emby_d6_row_parity_sql,
            .expected_candidate_sql = emby_d6_row_parity_expected_sql,
            .prepare = EMBY_PREPARE_V2,
            .bind = bind_search_term,
            .bind_ctx = (void *)"(\"alpha\"*)",
            .row_exception = accept_emby_fts_legal_difference,
            .minimum_rows = 5
        }
    },
    {
        .label = "row-parity/membership",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_IDENTITY,
            .assert_custom = rsh_custom_adapter_row_parity_membership,
            .immutable_data = &emby_d6_row_parity_single_membership
        }
    },
    {
        .label = "row-parity/ordered-contract",
        .kind = RSH_CASE_CONTRACT_PARITY,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.contract_parity = {
            .vendor_role = "vendor",
            .candidate_role = "candidate",
            .vendor_sql = emby_d6_row_parity_ordered_sql,
            .candidate_source_sql = emby_d6_row_parity_ordered_sql,
            .expected_candidate_sql = emby_d6_row_parity_ordered_expected_sql,
            .prepare = EMBY_PREPARE_V2,
            .bind = bind_search_term,
            .bind_ctx = (void *)"(\"alpha\"*)",
            .minimum_rows = 5
        }
    },
    {
        .label = "row-parity-or/membership",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_IDENTITY,
            .assert_custom = rsh_custom_adapter_row_parity_membership,
            .immutable_data = &emby_d6_row_parity_or_membership
        }
    },
    {
        .label = "row-parity-or/ordered-contract",
        .kind = RSH_CASE_CONTRACT_PARITY,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.contract_parity = {
            .vendor_role = "vendor",
            .candidate_role = "candidate",
            .vendor_sql = emby_d6_row_parity_ordered_sql,
            .candidate_source_sql = emby_d6_row_parity_ordered_sql,
            .expected_candidate_sql = emby_d6_row_parity_ordered_expected_sql,
            .prepare = EMBY_PREPARE_V2,
            .bind = bind_search_term,
            .bind_ctx = (void *)"(\"alpha\"*) OR (\"alpha\"*)",
            .minimum_rows = 5
        }
    }
};

static const rsh_phase_spec emby_d6_row_parity_phases[] = {
    {
        .label = "row-parity",
        .dbs = emby_d6_row_parity_dbs,
        .db_count = sizeof(emby_d6_row_parity_dbs) /
                    sizeof(emby_d6_row_parity_dbs[0]),
        .cases = emby_d6_row_parity_cases,
        .case_count = sizeof(emby_d6_row_parity_cases) /
                      sizeof(emby_d6_row_parity_cases[0])
    }
};

static const rsh_db_spec emby_d6_complex_resume_dbs[] = {
    {
        .role = "vendor",
        .relative_path = "not-target.db",
        .kind = RSH_DB_VENDOR,
        .storage = RSH_DB_RELATIVE,
        .setup_profile = EMBY_PROFILE_D6_COMPLEX_RESUME_SEED
    },
    {
        .role = "candidate",
        .relative_path = "library.db",
        .kind = RSH_DB_CANDIDATE,
        .storage = RSH_DB_RELATIVE,
        .setup_profile = EMBY_PROFILE_D6_COMPLEX_RESUME_SEED
    }
};

static const rsh_case_spec emby_d6_complex_resume_cases[] = {
    {
        .label = "emby-fanout-contract",
        .kind = RSH_CASE_CONTRACT_PARITY,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.contract_parity = {
            .vendor_role = "vendor",
            .candidate_role = "candidate",
            .vendor_sql = emby_d6_complex_resume_sql,
            .candidate_source_sql = emby_d6_complex_resume_sql,
            .expected_candidate_sql = emby_d6_complex_resume_expected_sql,
            .prepare = EMBY_PREPARE_V2,
            .minimum_rows = 1
        }
    },
    {
        .label = "complex-resume-watched-progress-row-parity/expected",
        .kind = RSH_CASE_EXACT_IDS,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.exact_ids = {
            .role = "candidate",
            .sql = emby_d6_complex_resume_expected_ids_sql,
            .prepare = EMBY_PREPARE_V2,
            .expected_ids = emby_d6_complex_resume_expected_ids,
            .expected_id_count = sizeof(emby_d6_complex_resume_expected_ids) /
                                 sizeof(emby_d6_complex_resume_expected_ids[0])
        }
    }
};

static const rsh_phase_spec emby_d6_complex_resume_phases[] = {
    {
        .label = "complex-resume-watched-progress-row-parity",
        .dbs = emby_d6_complex_resume_dbs,
        .db_count = sizeof(emby_d6_complex_resume_dbs) /
                    sizeof(emby_d6_complex_resume_dbs[0]),
        .cases = emby_d6_complex_resume_cases,
        .case_count = sizeof(emby_d6_complex_resume_cases) /
                      sizeof(emby_d6_complex_resume_cases[0])
    }
};

static const rsh_db_spec emby_d6_links_two_level_dbs[] = {
    {
        .role = "vendor",
        .relative_path = "not-target.db",
        .kind = RSH_DB_VENDOR,
        .storage = RSH_DB_RELATIVE,
        .setup_profile = EMBY_PROFILE_D6_LINKS_TWO_LEVEL_SEED
    },
    {
        .role = "candidate",
        .relative_path = "library.db",
        .kind = RSH_DB_CANDIDATE,
        .storage = RSH_DB_RELATIVE,
        .setup_profile = EMBY_PROFILE_D6_LINKS_TWO_LEVEL_SEED
    }
};

static const rsh_case_spec emby_d6_links_two_level_cases[] = {
    {
        .label = "links-two-level-row-parity",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_IDENTITY,
            .assert_custom = rsh_custom_adapter_links_two_level_identity,
        }
    }
};

static const rsh_phase_spec emby_d6_links_two_level_phases[] = {
    {
        .label = "links-two-level-row-parity",
        .dbs = emby_d6_links_two_level_dbs,
        .db_count = sizeof(emby_d6_links_two_level_dbs) /
                    sizeof(emby_d6_links_two_level_dbs[0]),
        .cases = emby_d6_links_two_level_cases,
        .case_count = sizeof(emby_d6_links_two_level_cases) /
                      sizeof(emby_d6_links_two_level_cases[0])
    }
};

static const rsh_db_spec emby_d6_mixed_identity_dbs[] = {
    {
        .role = "vendor",
        .relative_path = "not-target.db",
        .kind = RSH_DB_VENDOR,
        .storage = RSH_DB_RELATIVE,
        .setup_profile = EMBY_PROFILE_D6_MIXED_IDENTITY
    },
    {
        .role = "candidate",
        .relative_path = "library.db",
        .kind = RSH_DB_CANDIDATE,
        .storage = RSH_DB_RELATIVE,
        .setup_profile = EMBY_PROFILE_D6_MIXED_IDENTITY
    }
};

static const rsh_matrix_axis_value emby_d6_mixed_identity_stat_values[] = {
    {.label = "before-analyze", .integer = 0},
    {.label = "after-analyze", .integer = 1}
};

static const rsh_matrix_axis emby_d6_mixed_identity_axes[] = {
    {
        .name = "stat-state",
        .values = emby_d6_mixed_identity_stat_values,
        .value_count = sizeof(emby_d6_mixed_identity_stat_values) /
                       sizeof(emby_d6_mixed_identity_stat_values[0])
    }
};

static const rsh_case_spec emby_d6_mixed_identity_cases[] = {
    {
        .label = "dashboard-mixed-identity",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_IDENTITY,
            .assert_custom = rsh_custom_adapter_dashboard_mixed_identity,
        }
    }
};

static const rsh_matrix_cell_step emby_d6_mixed_identity_steps[] = {
    {
        .kind = RSH_CELL_ANALYZE_IF,
        .predicate = {.axis_index = 0, .value_index = 1}
    },
    {
        .kind = RSH_CELL_ASSERT,
        .cases = emby_d6_mixed_identity_cases,
        .case_count = sizeof(emby_d6_mixed_identity_cases) /
                      sizeof(emby_d6_mixed_identity_cases[0])
    }
};

static const rsh_matrix_phase_spec emby_d6_mixed_identity_matrix_phases[] = {
    {
        .label = "dashboard-mixed-identity",
        .cell_dir_prefix = "dashboard-mixed-identity",
        .axes = emby_d6_mixed_identity_axes,
        .axis_count = sizeof(emby_d6_mixed_identity_axes) /
                      sizeof(emby_d6_mixed_identity_axes[0]),
        .dbs = emby_d6_mixed_identity_dbs,
        .db_count = sizeof(emby_d6_mixed_identity_dbs) /
                    sizeof(emby_d6_mixed_identity_dbs[0]),
        .steps = emby_d6_mixed_identity_steps,
        .step_count = sizeof(emby_d6_mixed_identity_steps) /
                      sizeof(emby_d6_mixed_identity_steps[0]),
        .expected_cells = 2
    }
};

enum emby_d7_release_counter_id {
    EMBY_D7_RELEASE_GUARDED = 0,
    EMBY_D7_RELEASE_PASSTHROUGH,
    EMBY_D7_RELEASE_COUNTER_COUNT
};

static const rsh_matrix_axis_value emby_d7_stat_state_values[] = {
    {.label = "before-analyze", .integer = 0},
    {.label = "after-analyze", .integer = 1}
};

static const rsh_matrix_axis_value emby_d7_ancestor_values[] = {
    {.label = "100", .integer = 100},
    {.label = "200", .integer = 200},
    {.label = "999", .integer = 999}
};

static const rsh_matrix_axis_value emby_d7_user_values[] = {
    {.label = "42", .integer = 42},
    {.label = "777", .integer = 777}
};

static const rsh_matrix_axis_value emby_d7_limit_values[] = {
    {.label = "12", .integer = 12},
    {.label = "16", .integer = 16},
    {.label = "20", .integer = 20}
};

static const rsh_matrix_axis emby_d7_release_grid_axes[] = {
    {
        .name = "stat-state",
        .values = emby_d7_stat_state_values,
        .value_count = sizeof(emby_d7_stat_state_values) /
                       sizeof(emby_d7_stat_state_values[0])
    },
    {
        .name = "ancestor",
        .values = emby_d7_ancestor_values,
        .value_count = sizeof(emby_d7_ancestor_values) /
                       sizeof(emby_d7_ancestor_values[0])
    },
    {
        .name = "user",
        .values = emby_d7_user_values,
        .value_count = sizeof(emby_d7_user_values) /
                       sizeof(emby_d7_user_values[0])
    },
    {
        .name = "limit",
        .values = emby_d7_limit_values,
        .value_count = sizeof(emby_d7_limit_values) /
                       sizeof(emby_d7_limit_values[0])
    }
};

static const rsh_matrix_axis emby_d7_stat_state_axes[] = {
    {
        .name = "stat-state",
        .values = emby_d7_stat_state_values,
        .value_count = sizeof(emby_d7_stat_state_values) /
                       sizeof(emby_d7_stat_state_values[0])
    }
};

static int rsh_d7_typed_rows_equal(
    const typed_rows *left,
    const typed_rows *right
) {
    return left->columns == right->columns && left->rows == right->rows &&
           left->len == right->len &&
           memcmp(left->bytes, right->bytes, left->len) == 0;
}

static void rsh_d7_require_typed_rows_differ(
    const char *label,
    const typed_rows *left,
    const typed_rows *right
) {
    if (rsh_d7_typed_rows_equal(left, right)) {
        failf("FAIL [%s]: got=equal(\"%s\") want=different", label,
              (const char *)left->bytes);
    }
}

static int rsh_custom_adapter_dashboard_release_preflight(
    const rsh_case_context *context,
    const void *immutable_data
) {
    sqlite3 *candidate_db = rsh_context_db(context, "candidate");
    typed_rows streams[3][2];
    int ancestor_index;
    int user_index;

    (void)immutable_data;
    require_str_eq("matrix/hot-literal", emby_d7_ancestor_values[0].label, "100");
    require_str_eq("matrix/typical-literal", emby_d7_ancestor_values[1].label, "200");
    require_str_eq("matrix/empty-literal", emby_d7_ancestor_values[2].label, "999");
    require_str_eq("matrix/heavy-user-literal", emby_d7_user_values[0].label, "42");
    require_str_eq("matrix/zero-user-literal", emby_d7_user_values[1].label, "777");
    if (!strcmp(emby_d7_ancestor_values[0].label,
                emby_d7_ancestor_values[1].label) ||
        !strcmp(emby_d7_ancestor_values[0].label,
                emby_d7_ancestor_values[2].label) ||
        !strcmp(emby_d7_ancestor_values[1].label,
                emby_d7_ancestor_values[2].label) ||
        !strcmp(emby_d7_user_values[0].label, emby_d7_user_values[1].label)) {
        failf(
            "FAIL [matrix/literal-distinctness]: got={%s,%s,%s;%s,%s} "
            "want=all-distinct-per-axis",
            emby_d7_ancestor_values[0].label,
            emby_d7_ancestor_values[1].label,
            emby_d7_ancestor_values[2].label,
            emby_d7_user_values[0].label,
            emby_d7_user_values[1].label
        );
    }

    require_int(
        "matrix/hot-count",
        query_int(
            candidate_db, "matrix/hot-count-sql",
            "SELECT count(*) FROM AncestorIds2 WHERE AncestorId=100 AND itemid BETWEEN 5001 AND 5030"
        ),
        30
    );
    require_int(
        "matrix/typical-count",
        query_int(
            candidate_db, "matrix/typical-count-sql",
            "SELECT count(*) FROM AncestorIds2 WHERE AncestorId=200 AND itemid BETWEEN 6001 AND 6024"
        ),
        24
    );
    require_int(
        "matrix/empty-count",
        query_int(
            candidate_db, "matrix/empty-count-sql",
            "SELECT count(*) FROM AncestorIds2 WHERE AncestorId=999 AND itemid BETWEEN 5001 AND 6024"
        ),
        0
    );
    require_int(
        "matrix/user42-count",
        query_int(
            candidate_db, "matrix/user42-count-sql",
            "SELECT count(*) FROM UserDatas WHERE UserId=42 AND UserDataKeyId BETWEEN 7001 AND 8024"
        ),
        54
    );
    require_int(
        "matrix/user42-played",
        query_int(
            candidate_db, "matrix/user42-played-sql",
            "SELECT count(*) FROM UserDatas WHERE UserId=42 AND Played=1 AND UserDataKeyId BETWEEN 7001 AND 8024"
        ),
        10
    );
    require_int(
        "matrix/user777-count",
        query_int(
            candidate_db, "matrix/user777-count-sql",
            "SELECT count(*) FROM UserDatas WHERE UserId=777"
        ),
        0
    );
    for (ancestor_index = 0; ancestor_index < 3; ancestor_index++) {
        for (user_index = 0; user_index < 2; user_index++) {
            char *raw = make_movies_latest_sql(
                1, emby_d7_ancestor_values[ancestor_index].label,
                emby_d7_user_values[user_index].label, "20"
            );
            char *expected = make_movies_latest_expected(
                emby_d7_ancestor_values[ancestor_index].label,
                emby_d7_user_values[user_index].label, "20"
            );
            streams[ancestor_index][user_index] = collect_typed_rows(
                candidate_db, "matrix/distinct-preflight", raw, expected
            );
            free(raw);
            free(expected);
        }
    }
    rsh_d7_require_typed_rows_differ(
        "matrix/hot-vs-typical-user42", &streams[0][0], &streams[1][0]
    );
    rsh_d7_require_typed_rows_differ(
        "matrix/hot-vs-typical-user777", &streams[0][1], &streams[1][1]
    );
    rsh_d7_require_typed_rows_differ(
        "matrix/hot-users", &streams[0][0], &streams[0][1]
    );
    rsh_d7_require_typed_rows_differ(
        "matrix/typical-users", &streams[1][0], &streams[1][1]
    );
    require_int("matrix/empty-user42-rows", streams[2][0].rows, 0);
    require_int("matrix/empty-user777-rows", streams[2][1].rows, 0);
    for (ancestor_index = 0; ancestor_index < 3; ancestor_index++) {
        for (user_index = 0; user_index < 2; user_index++) {
            free_typed_rows(&streams[ancestor_index][user_index]);
        }
    }
    return SQLITE_OK;
}

static int rsh_custom_adapter_dashboard_release_grid(
    const rsh_case_context *context,
    const void *immutable_data
) {
    const rsh_matrix_cell *cell = context->matrix_cell;
    sqlite3 *vendor_db = rsh_context_db(context, "vendor");
    sqlite3 *candidate_db = rsh_context_db(context, "candidate");
    const char *ancestor;
    const char *user;
    const char *limit;
    char *raw;
    char *expected;
    typed_rows vendor_rows;
    typed_rows candidate_rows;
    int want_rows;

    (void)immutable_data;
    if (!cell || cell->axis_count != 4) {
        failf("FAIL [matrix/context-axis-count]: got=%lu want=4",
              (unsigned long)(cell ? cell->axis_count : 0));
    }
    if (cell->counter_count != EMBY_D7_RELEASE_COUNTER_COUNT) {
        failf("FAIL [matrix/context-counter-count]: got=%lu want=%d",
              (unsigned long)cell->counter_count,
              EMBY_D7_RELEASE_COUNTER_COUNT);
    }
    ancestor = cell->axis_values[1]->label;
    user = cell->axis_values[2]->label;
    limit = cell->axis_values[3]->label;
    want_rows = cell->axis_indices[1] == 2
        ? 0 : (int)cell->axis_values[3]->integer;

    raw = make_movies_latest_sql(1, ancestor, user, limit);
    expected = make_movies_latest_expected(ancestor, user, limit);
    vendor_rows = collect_typed_rows(vendor_db, "matrix/vendor", raw, raw);
    candidate_rows = collect_typed_rows(
        candidate_db, "matrix/candidate", raw, expected
    );
    require_int("matrix/vendor-row-count", vendor_rows.rows, want_rows);
    require_int("matrix/candidate-row-count", candidate_rows.rows, want_rows);
    require_ordered_full_row_identity(
        "matrix/guarded-identity", &vendor_rows, &candidate_rows
    );
    if (cell->axis_indices[1] < 2 && cell->axis_indices[3] > 0) {
        char current_ids[1024];
        char prior_ids[1024];
        const char *prior_limit =
            emby_d7_limit_values[cell->axis_indices[3] - 1].label;
        char *prior_raw = make_movies_latest_sql(
            1, ancestor, user, prior_limit
        );
        collect_int_column(
            vendor_db, "matrix/current-ids", raw, raw, -1, 0,
            current_ids, sizeof(current_ids)
        );
        collect_int_column(
            vendor_db, "matrix/prior-ids", prior_raw, prior_raw, -1, 0,
            prior_ids, sizeof(prior_ids)
        );
        if (strncmp(current_ids, prior_ids, strlen(prior_ids)) != 0) {
            failf("FAIL [matrix/row-slice]: got=%s want_prefix=%s",
                  current_ids, prior_ids);
        }
        free(prior_raw);
    }
    cell->counters[EMBY_D7_RELEASE_GUARDED]++;
    free_typed_rows(&vendor_rows);
    free_typed_rows(&candidate_rows);
    free(raw);

    raw = make_movies_latest_sql(0, ancestor, user, limit);
    vendor_rows = collect_typed_rows(
        vendor_db, "matrix/no-guard-vendor", raw, raw
    );
    candidate_rows = collect_typed_rows(
        candidate_db, "matrix/no-guard-candidate", raw, raw
    );
    require_ordered_full_row_identity(
        "matrix/no-guard-identity", &vendor_rows, &candidate_rows
    );
    cell->counters[EMBY_D7_RELEASE_PASSTHROUGH]++;
    free_typed_rows(&vendor_rows);
    free_typed_rows(&candidate_rows);
    free(raw);
    free(expected);
    return SQLITE_OK;
}

static int rsh_custom_adapter_dashboard_release_episodes_semantic(
    const rsh_case_context *context,
    const void *immutable_data
) {
    sqlite3 *vendor_db = rsh_context_db(context, "vendor");
    sqlite3 *candidate_db = rsh_context_db(context, "candidate");

    (void)immutable_data;
    require_episodes_latest_semantics(vendor_db, candidate_db);
    return SQLITE_OK;
}

static const sqlite3_int64 emby_d7_movies_expanded_expected_ids[] = {
    9001, 9002, 9003, 9004, 9005, 9006, 9007, 9008, 9009, 9010,
    9119, 9101, 9104, 9107, 9111, 9112, 9113, 9118, 9116, 9108
};

static int rsh_custom_adapter_dashboard_release_movies_expanded_identity(
    const rsh_case_context *context,
    const void *immutable_data
) {
    sqlite3 *vendor_db = rsh_context_db(context, "vendor");
    sqlite3 *candidate_db = rsh_context_db(context, "candidate");
    char *raw = make_movies_latest_sql(1, "300", "42", "20");
    char *expected = make_movies_latest_expected("300", "42", "20");
    typed_rows vendor_rows = collect_typed_rows(
        vendor_db, "expanded/vendor", raw, raw
    );
    typed_rows candidate_rows = collect_typed_rows(
        candidate_db, "expanded/candidate", raw, expected
    );

    (void)immutable_data;
    require_int("expanded/vendor-row-count", vendor_rows.rows, 20);
    require_int("expanded/candidate-row-count", candidate_rows.rows, 20);
    require_movies_latest_candidate_rows(
        candidate_db, "expanded/candidate-contract", raw, expected,
        EMBY_MOVIES_LATEST_COMPACT_PROJECTION,
        emby_d7_movies_expanded_expected_ids,
        (int)(sizeof(emby_d7_movies_expanded_expected_ids) /
              sizeof(emby_d7_movies_expanded_expected_ids[0]))
    );
    free_typed_rows(&vendor_rows);
    free_typed_rows(&candidate_rows);
    free(raw);
    free(expected);
    return SQLITE_OK;
}

static int rsh_custom_adapter_dashboard_release_movies_expanded_order(
    const rsh_case_context *context,
    const void *immutable_data
) {
    sqlite3 *candidate_db = rsh_context_db(context, "candidate");
    char order_ids[128];
    char *raw = make_movies_latest_sql(1, "301", "42", "12");
    char *expected = make_movies_latest_expected("301", "42", "12");

    (void)immutable_data;
    collect_int_column(
        candidate_db, "expanded/rewrite-order", raw, expected, -1, 0,
        order_ids, sizeof(order_ids)
    );
    require_str_eq(
        "expanded/rewrite-order-ids", order_ids, "9202,9201,9204,9203,"
    );
    free(raw);
    free(expected);
    return SQLITE_OK;
}

static const rsh_db_spec emby_d7_release_preflight_dbs[] = {
    {
        .role = "candidate",
        .relative_path = "library.db",
        .kind = RSH_DB_CANDIDATE,
        .storage = RSH_DB_RELATIVE,
        .setup_profile = EMBY_PROFILE_FIXTURE_CANARY
    }
};

static const rsh_case_spec emby_d7_release_preflight_cases[] = {
    {
        .label = "release-preflight",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_IDENTITY,
            .assert_custom = rsh_custom_adapter_dashboard_release_preflight,
        }
    }
};

static const rsh_phase_spec emby_d7_release_preflight_phases[] = {
    {
        .label = "release-preflight",
        .dbs = emby_d7_release_preflight_dbs,
        .db_count = sizeof(emby_d7_release_preflight_dbs) /
                    sizeof(emby_d7_release_preflight_dbs[0]),
        .cases = emby_d7_release_preflight_cases,
        .case_count = sizeof(emby_d7_release_preflight_cases) /
                      sizeof(emby_d7_release_preflight_cases[0])
    }
};

static const rsh_db_spec emby_d7_release_grid_dbs[] = {
    {
        .role = "vendor",
        .relative_path = "not-target.db",
        .kind = RSH_DB_VENDOR,
        .storage = RSH_DB_RELATIVE,
        .seed_profile = EMBY_PROFILE_MOVIES_SEED_ONLY
    },
    {
        .role = "candidate",
        .relative_path = "library.db",
        .kind = RSH_DB_CANDIDATE,
        .storage = RSH_DB_RELATIVE,
        .seed_profile = EMBY_PROFILE_MOVIES_SEED_ONLY,
        .setup_profile = EMBY_PROFILE_D7_DASHBOARD_INDEXES
    }
};

static const rsh_case_spec emby_d7_release_grid_cases[] = {
    {
        .label = "release-grid",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_IDENTITY,
            .assert_custom = rsh_custom_adapter_dashboard_release_grid,
        }
    }
};

static const rsh_matrix_cell_step emby_d7_release_grid_steps[] = {
    {
        .kind = RSH_CELL_ANALYZE_IF,
        .predicate = {.axis_index = 0, .value_index = 1}
    },
    {
        .kind = RSH_CELL_ASSERT,
        .cases = emby_d7_release_grid_cases,
        .case_count = sizeof(emby_d7_release_grid_cases) /
                      sizeof(emby_d7_release_grid_cases[0])
    }
};

static const rsh_matrix_counter_spec emby_d7_release_grid_counters[] = {
    {.name = "guarded-cells", .expected = 36},
    {.name = "no-guard-cells", .expected = 36}
};

static const rsh_db_spec emby_d7_episodes_semantic_dbs[] = {
    {
        .role = "vendor",
        .relative_path = "not-target.db",
        .kind = RSH_DB_VENDOR,
        .storage = RSH_DB_RELATIVE,
        .seed_profile = EMBY_PROFILE_D7_EPISODES_SEMANTIC_SEED
    },
    {
        .role = "candidate",
        .relative_path = "library.db",
        .kind = RSH_DB_CANDIDATE,
        .storage = RSH_DB_RELATIVE,
        .seed_profile = EMBY_PROFILE_D7_EPISODES_SEMANTIC_SEED,
        .setup_profile = EMBY_PROFILE_D7_DASHBOARD_INDEXES
    }
};

static const rsh_case_spec emby_d7_episodes_semantic_cases[] = {
    {
        .label = "episodes-semantic",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_IDENTITY,
            .assert_custom =
                rsh_custom_adapter_dashboard_release_episodes_semantic,
        }
    }
};

static const rsh_matrix_cell_step emby_d7_episodes_semantic_steps[] = {
    {
        .kind = RSH_CELL_ANALYZE_IF,
        .predicate = {.axis_index = 0, .value_index = 1}
    },
    {
        .kind = RSH_CELL_ASSERT,
        .cases = emby_d7_episodes_semantic_cases,
        .case_count = sizeof(emby_d7_episodes_semantic_cases) /
                      sizeof(emby_d7_episodes_semantic_cases[0])
    }
};

static const rsh_db_spec emby_d7_movies_expanded_dbs[] = {
    {
        .role = "vendor",
        .relative_path = "not-target.db",
        .kind = RSH_DB_VENDOR,
        .storage = RSH_DB_RELATIVE,
        .seed_profile = EMBY_PROFILE_D7_MOVIES_EXPANDED_SEED
    },
    {
        .role = "candidate",
        .relative_path = "library.db",
        .kind = RSH_DB_CANDIDATE,
        .storage = RSH_DB_RELATIVE,
        .seed_profile = EMBY_PROFILE_D7_MOVIES_EXPANDED_SEED,
        .setup_profile = EMBY_PROFILE_D7_DASHBOARD_INDEXES
    }
};

static const rsh_case_spec emby_d7_movies_expanded_identity_cases[] = {
    {
        .label = "movies-expanded-identity",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_IDENTITY,
            .assert_custom =
                rsh_custom_adapter_dashboard_release_movies_expanded_identity,
        }
    }
};

static const rsh_case_spec emby_d7_movies_expanded_order_cases[] = {
    {
        .label = "movies-expanded-order",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_IDENTITY,
            .assert_custom =
                rsh_custom_adapter_dashboard_release_movies_expanded_order,
        }
    }
};

static const rsh_matrix_cell_step emby_d7_movies_expanded_steps[] = {
    {
        .kind = RSH_CELL_ANALYZE_IF,
        .predicate = {.axis_index = 0, .value_index = 1}
    },
    {
        .kind = RSH_CELL_ASSERT,
        .cases = emby_d7_movies_expanded_identity_cases,
        .case_count = sizeof(emby_d7_movies_expanded_identity_cases) /
                      sizeof(emby_d7_movies_expanded_identity_cases[0])
    },
    {
        .kind = RSH_CELL_SETUP_PROFILE,
        .role = "candidate",
        .setup_profile = EMBY_PROFILE_D7_MOVIES_REWRITE_ORDER_SEED
    },
    {
        .kind = RSH_CELL_ASSERT,
        .cases = emby_d7_movies_expanded_order_cases,
        .case_count = sizeof(emby_d7_movies_expanded_order_cases) /
                      sizeof(emby_d7_movies_expanded_order_cases[0])
    }
};

static const rsh_matrix_phase_spec emby_d7_release_matrix_phases[] = {
    {
        .label = "release-grid",
        .cell_dir_prefix = "dashboard-release-grid",
        .axes = emby_d7_release_grid_axes,
        .axis_count = sizeof(emby_d7_release_grid_axes) /
                      sizeof(emby_d7_release_grid_axes[0]),
        .dbs = emby_d7_release_grid_dbs,
        .db_count = sizeof(emby_d7_release_grid_dbs) /
                    sizeof(emby_d7_release_grid_dbs[0]),
        .steps = emby_d7_release_grid_steps,
        .step_count = sizeof(emby_d7_release_grid_steps) /
                      sizeof(emby_d7_release_grid_steps[0]),
        .expected_cells = 36,
        .counters = emby_d7_release_grid_counters,
        .counter_count = sizeof(emby_d7_release_grid_counters) /
                         sizeof(emby_d7_release_grid_counters[0])
    },
    {
        .label = "episodes-semantic",
        .cell_dir_prefix = "dashboard-release-episodes",
        .axes = emby_d7_stat_state_axes,
        .axis_count = sizeof(emby_d7_stat_state_axes) /
                      sizeof(emby_d7_stat_state_axes[0]),
        .dbs = emby_d7_episodes_semantic_dbs,
        .db_count = sizeof(emby_d7_episodes_semantic_dbs) /
                    sizeof(emby_d7_episodes_semantic_dbs[0]),
        .steps = emby_d7_episodes_semantic_steps,
        .step_count = sizeof(emby_d7_episodes_semantic_steps) /
                      sizeof(emby_d7_episodes_semantic_steps[0]),
        .expected_cells = 2
    },
    {
        .label = "movies-expanded",
        .cell_dir_prefix = "dashboard-release-expanded",
        .axes = emby_d7_stat_state_axes,
        .axis_count = sizeof(emby_d7_stat_state_axes) /
                      sizeof(emby_d7_stat_state_axes[0]),
        .dbs = emby_d7_movies_expanded_dbs,
        .db_count = sizeof(emby_d7_movies_expanded_dbs) /
                    sizeof(emby_d7_movies_expanded_dbs[0]),
        .steps = emby_d7_movies_expanded_steps,
        .step_count = sizeof(emby_d7_movies_expanded_steps) /
                      sizeof(emby_d7_movies_expanded_steps[0]),
        .expected_cells = 2
    }
};

typedef struct emby_d8_index_definition_expectation {
    const char *label;
    int rewritten;
    int movies;
} emby_d8_index_definition_expectation;

static const emby_d8_index_definition_expectation
    emby_d8_malformed_episodes_group_expectation = {
        "malformed-episodes-group-index-fail-open", 0, 0
    };
static const emby_d8_index_definition_expectation
    emby_d8_malformed_episodes_date_expectation = {
        "malformed-episodes-date-index-fail-open", 0, 0
    };
static const emby_d8_index_definition_expectation
    emby_d8_canonical_episodes_expectation = {
        "canonical-episodes-indexes-rewrite", 1, 0
    };
static const emby_d8_index_definition_expectation
    emby_d8_malformed_movies_outer_expectation = {
        "malformed-movies-outer-index-fail-open", 0, 1
    };
static const emby_d8_index_definition_expectation
    emby_d8_malformed_movies_inner_expectation = {
        "malformed-movies-inner-index-fail-open", 0, 1
    };
static const emby_d8_index_definition_expectation
    emby_d8_canonical_movies_expectation = {
        "canonical-movies-indexes-rewrite", 1, 1
    };

static int rsh_assert_emby_d8_index_definition_gate(
    const rsh_case_context *context,
    int mode_id,
    int expected_delta,
    void *ctx
) {
    const emby_d8_index_definition_expectation *expectation =
        (const emby_d8_index_definition_expectation *)ctx;
    sqlite3 *candidate = rsh_context_db(context, "candidate");
    char *sql;
    char *rewritten = NULL;
    const char *expected;

    (void)mode_id;
    (void)expected_delta;
    if (!expectation->movies) {
        sql = make_latest_sql("A.Id ", "12");
        if (expectation->rewritten) {
            rewritten = make_latest_expected("A.Id ", "12");
        }
    } else {
        sql = make_movies_latest_sql(1, "100", "42", "12");
        if (expectation->rewritten) {
            rewritten = make_movies_latest_expected("100", "42", "12");
        }
    }
    expected = rewritten ? rewritten : sql;
    expect_sql(candidate, expectation->label, sql, -1, 2, expected, 0);
    free(rewritten);
    free(sql);
    return SQLITE_OK;
}

static int rsh_custom_adapter_emby_d8_index_definition_log_assert(
    const rsh_case_context *context,
    const void *immutable_data
) {
    const char *log_output = context->captured_stderr;

    (void)immutable_data;
    require_contains(
        "malformed-episodes-index-definitions/index-missing-log", log_output,
        "reason=index_missing mode=dashboard+episodes_latest"
    );
    require_contains(
        "canonical-episodes-index-definitions/applied-log", log_output,
        "event=rewrite_applied target=emby mode=dashboard+episodes_latest"
    );
    require_contains(
        "malformed-movies-index-definitions/index-missing-log", log_output,
        "reason=index_missing mode=dashboard+movies_latest"
    );
    require_contains(
        "canonical-movies-index-definitions/applied-log", log_output,
        "event=rewrite_applied target=emby mode=dashboard+movies_latest"
    );
    return SQLITE_OK;
}

#define EMBY_D8_DEFINE_INDEX_DEFINITION_PHASE( \
    name, phase_label, profile_value, mode_value, expectation_value) \
    static const rsh_db_spec name##_dbs[] = { \
        { \
            .role = "candidate", \
            .relative_path = "library.db", \
            .kind = RSH_DB_CANDIDATE, \
            .storage = RSH_DB_RELATIVE, \
            .setup_profile = (profile_value) \
        } \
    }; \
    static const rsh_case_spec name##_cases[] = { \
        { \
            .label = (phase_label), \
            .kind = RSH_CASE_INDEX_PROBE, \
            .build_mask = RSH_BUILD_EMBY_LINKED, \
            .data.index_probe = { \
                .role = "candidate", \
                .mode_id = (mode_value), \
                .expected_delta = 0, \
                .assert_probe = rsh_assert_emby_d8_index_definition_gate, \
                .assert_ctx = (void *)&(expectation_value) \
            } \
        } \
    }

EMBY_D8_DEFINE_INDEX_DEFINITION_PHASE(
    emby_d8_malformed_episodes_group,
    "malformed-episodes-group-index-fail-open",
    EMBY_PROFILE_D8_EPISODES_GROUP_MALFORMED,
    EMBY_SMOKE_MODE_EPISODES_LATEST,
    emby_d8_malformed_episodes_group_expectation
);
EMBY_D8_DEFINE_INDEX_DEFINITION_PHASE(
    emby_d8_malformed_episodes_date,
    "malformed-episodes-date-index-fail-open",
    EMBY_PROFILE_D8_EPISODES_DATE_MALFORMED,
    EMBY_SMOKE_MODE_EPISODES_LATEST,
    emby_d8_malformed_episodes_date_expectation
);
EMBY_D8_DEFINE_INDEX_DEFINITION_PHASE(
    emby_d8_canonical_episodes,
    "canonical-episodes-indexes-rewrite",
    EMBY_PROFILE_LATEST_INDEXES,
    EMBY_SMOKE_MODE_EPISODES_LATEST,
    emby_d8_canonical_episodes_expectation
);
EMBY_D8_DEFINE_INDEX_DEFINITION_PHASE(
    emby_d8_malformed_movies_outer,
    "malformed-movies-outer-index-fail-open",
    EMBY_PROFILE_D8_MOVIES_OUTER_MALFORMED,
    EMBY_SMOKE_MODE_MOVIES_LATEST,
    emby_d8_malformed_movies_outer_expectation
);
EMBY_D8_DEFINE_INDEX_DEFINITION_PHASE(
    emby_d8_malformed_movies_inner,
    "malformed-movies-inner-index-fail-open",
    EMBY_PROFILE_D8_MOVIES_INNER_MALFORMED,
    EMBY_SMOKE_MODE_MOVIES_LATEST,
    emby_d8_malformed_movies_inner_expectation
);
EMBY_D8_DEFINE_INDEX_DEFINITION_PHASE(
    emby_d8_canonical_movies,
    "canonical-movies-indexes-rewrite",
    EMBY_PROFILE_D8_MOVIES_CANONICAL,
    EMBY_SMOKE_MODE_MOVIES_LATEST,
    emby_d8_canonical_movies_expectation
);

static const rsh_phase_spec emby_d8_index_definition_phases[] = {
    {
        .label = "malformed-episodes-group-index",
        .dbs = emby_d8_malformed_episodes_group_dbs,
        .db_count = sizeof(emby_d8_malformed_episodes_group_dbs) /
                    sizeof(emby_d8_malformed_episodes_group_dbs[0]),
        .cases = emby_d8_malformed_episodes_group_cases,
        .case_count = sizeof(emby_d8_malformed_episodes_group_cases) /
                      sizeof(emby_d8_malformed_episodes_group_cases[0])
    },
    {
        .label = "malformed-episodes-date-index",
        .dbs = emby_d8_malformed_episodes_date_dbs,
        .db_count = sizeof(emby_d8_malformed_episodes_date_dbs) /
                    sizeof(emby_d8_malformed_episodes_date_dbs[0]),
        .cases = emby_d8_malformed_episodes_date_cases,
        .case_count = sizeof(emby_d8_malformed_episodes_date_cases) /
                      sizeof(emby_d8_malformed_episodes_date_cases[0])
    },
    {
        .label = "canonical-episodes-indexes",
        .dbs = emby_d8_canonical_episodes_dbs,
        .db_count = sizeof(emby_d8_canonical_episodes_dbs) /
                    sizeof(emby_d8_canonical_episodes_dbs[0]),
        .cases = emby_d8_canonical_episodes_cases,
        .case_count = sizeof(emby_d8_canonical_episodes_cases) /
                      sizeof(emby_d8_canonical_episodes_cases[0])
    },
    {
        .label = "malformed-movies-outer-index",
        .dbs = emby_d8_malformed_movies_outer_dbs,
        .db_count = sizeof(emby_d8_malformed_movies_outer_dbs) /
                    sizeof(emby_d8_malformed_movies_outer_dbs[0]),
        .cases = emby_d8_malformed_movies_outer_cases,
        .case_count = sizeof(emby_d8_malformed_movies_outer_cases) /
                      sizeof(emby_d8_malformed_movies_outer_cases[0])
    },
    {
        .label = "malformed-movies-inner-index",
        .dbs = emby_d8_malformed_movies_inner_dbs,
        .db_count = sizeof(emby_d8_malformed_movies_inner_dbs) /
                    sizeof(emby_d8_malformed_movies_inner_dbs[0]),
        .cases = emby_d8_malformed_movies_inner_cases,
        .case_count = sizeof(emby_d8_malformed_movies_inner_cases) /
                      sizeof(emby_d8_malformed_movies_inner_cases[0])
    },
    {
        .label = "canonical-movies-indexes",
        .dbs = emby_d8_canonical_movies_dbs,
        .db_count = sizeof(emby_d8_canonical_movies_dbs) /
                    sizeof(emby_d8_canonical_movies_dbs[0]),
        .cases = emby_d8_canonical_movies_cases,
        .case_count = sizeof(emby_d8_canonical_movies_cases) /
                      sizeof(emby_d8_canonical_movies_cases[0])
    }
};

static const rsh_case_spec emby_d8_index_definition_post_close_cases[] = {
    {
        .label = "dashboard-index-definition-gate-log-assert",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_LOG_CAPTURE,
            .assert_custom =
                rsh_custom_adapter_emby_d8_index_definition_log_assert,
        }
    }
};

static const char emby_d8_latest_series_browse_sql[] =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) )select A.Id,A.SeriesName from mediaitems A left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=42 where A.Type=8 AND Coalesce(UserDatas.played, 0)=0 AND A.Id in WithAncestors Group by coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey) ORDER BY SeriesName LIMIT 12";

static int rsh_custom_adapter_emby_d8_observability_primary_producer(
    const rsh_case_context *context,
    const void *immutable_data
) {
    sqlite3 *db = rsh_context_db(context, "candidate");
    char *links_sql;
    char *links_repl;
    char *links_expected;
    char *links_two_level_expected;
    char *links_miss_sql;
    char *resume_simple_sql;
    char *resume_simple_expected;
    char *resume_early_sql;
    char *similar_sql;
    char *similar_expected;
    char *latest_sql;
    char *latest_expected;
    char *latest_new_corr_sql;
    char *latest_new_corr_expected;
    char *latest_miss_sql;
    char *latest_unsupported_sql;
    char *latest_bind_sql;
    char *movies_sql;
    char *movies_expected;
    char *movies_miss_sql;
    char *movies_unsupported_sql;
    char *movies_bind_sql;
    char *movies_no_guard_sql;
    char *mixed_sql;
    char *mixed_expected;
    char *mixed_miss_sql;
    char *mixed_bind_sql;

    (void)immutable_data;
    links_sql = make_links_search_sql("2,3");
    links_repl = make_exists_links_one("100", "2,3");
    links_expected = replace_once(
        links_sql, "A.Id in WithItemLinkItemIds", links_repl
    );
    expect_sql(
        db, "obs-fanout-links-applied", links_sql, -1, 2,
        links_expected, 0
    );
    links_two_level_expected = make_link_type_count_shape_05_expected();
    expect_sql(
        db, "obs-fanout-links-two-level-applied",
        EMBY_LINK_TYPE_COUNT_SHAPE_05_SQL, -1, 2,
        links_two_level_expected, 0
    );

    links_miss_sql = make_links_search_sql("'bad'");
    expect_sql(
        db, "obs-fanout-links-capture-miss", links_miss_sql, -1, 2,
        links_miss_sql, 0
    );

    resume_simple_sql = make_resume_simple_sql(1, "12");
    resume_simple_expected = make_resume_simple_expected(resume_simple_sql);
    expect_sql(
        db, "obs-fanout-resume-simple-applied", resume_simple_sql, -1, 2,
        resume_simple_expected, 0
    );
    {
        char *resume_sql = make_resume_sql();
        resume_early_sql = replace_once(
            resume_sql,
            "AND LastWatchedEpisodes.LastPlayedDateInt not null",
            "AND 1=1"
        );
        free(resume_sql);
    }
    expect_sql(
        db, "obs-fanout-resume-early-miss", resume_early_sql, -1, 2,
        resume_early_sql, 0
    );

    similar_sql = make_similar_sql();
    similar_expected = make_similar_expected(similar_sql);
    expect_sql(
        db, "obs-fanout-similar-applied", similar_sql, -1, 2,
        similar_expected, 0
    );

    latest_sql = make_latest_sql("A.Id ", "12");
    latest_expected = make_latest_expected("A.Id ", "12");
    expect_sql(
        db, "obs-dashboard-latest-applied", latest_sql, -1, 2,
        latest_expected, 0
    );
    latest_new_corr_sql = replace_once(latest_sql, "in (100)", "in (101)");
    latest_new_corr_expected = make_latest_expected_form("A.Id ", "101", "12");
    expect_sql(
        db, "obs-dashboard-latest-new-corr", latest_new_corr_sql, -1, 2,
        latest_new_corr_expected, 0
    );
    expect_sql(
        db, "obs-dashboard-latest-new-corr-repeat", latest_new_corr_sql,
        -1, 2, latest_new_corr_expected, 0
    );

    latest_miss_sql = make_latest_sql("(A.DateCreated) ", "12");
    expect_sql(
        db, "obs-dashboard-capture-miss", latest_miss_sql, -1, 2,
        latest_miss_sql, 0
    );
    latest_unsupported_sql = make_latest_sql("A.Id ", "11");
    expect_sql(
        db, "obs-dashboard-limit-unsupported", latest_unsupported_sql,
        -1, 2, latest_unsupported_sql, 0
    );
    latest_bind_sql = replace_once(
        latest_sql,
        "select A.Id from mediaitems A",
        "select ? AS bound,A.Id from mediaitems A"
    );
    expect_sql(
        db, "obs-dashboard-bind-out-of-scope", latest_bind_sql, -1, 2,
        latest_bind_sql, 0
    );
    expect_sql(
        db, "obs-dashboard-series-browse-clean-miss",
        emby_d8_latest_series_browse_sql, -1, 2,
        emby_d8_latest_series_browse_sql, 0
    );

    movies_sql = make_movies_latest_sql(1, "100", "42", "12");
    movies_expected = make_movies_latest_expected("100", "42", "12");
    expect_sql(
        db, "obs-dashboard-movies-applied", movies_sql, -1, 2,
        movies_expected, 0
    );
    movies_miss_sql = replace_once(
        movies_sql, " Group by A.PresentationUniqueKey", " Group by A.Id"
    );
    expect_sql(
        db, "obs-dashboard-movies-capture-miss", movies_miss_sql, -1, 2,
        movies_miss_sql, 0
    );
    movies_unsupported_sql = make_movies_latest_sql(1, "100", "42", "21");
    expect_sql(
        db, "obs-dashboard-movies-limit-unsupported",
        movies_unsupported_sql, -1, 2, movies_unsupported_sql, 0
    );
    movies_bind_sql = replace_once(
        movies_sql, "A.Id,A.Name", "? AS bound,A.Id,A.Name"
    );
    expect_sql(
        db, "obs-dashboard-movies-bind-out-of-scope", movies_bind_sql,
        -1, 2, movies_bind_sql, 0
    );
    movies_no_guard_sql = make_movies_latest_sql(0, "100", "42", "12");
    expect_sql(
        db, "obs-dashboard-movies-no-guard", movies_no_guard_sql,
        -1, 2, movies_no_guard_sql, 0
    );

    mixed_sql = make_mixed_latest_sql("42", "3");
    mixed_expected = make_mixed_latest_expected("42", "3");
    expect_mixed_latest_sql(
        db, "obs-dashboard-mixed-applied", mixed_sql, mixed_expected, 0, 0
    );
    mixed_miss_sql = replace_once(mixed_sql, "A.type,A.Id", "A.Type,A.Id");
    expect_sql(
        db, "obs-dashboard-mixed-capture-miss", mixed_miss_sql, -1, 2,
        mixed_miss_sql, 0
    );
    mixed_bind_sql = make_mixed_latest_sql("?1", "3");
    expect_sql(
        db, "obs-dashboard-mixed-bind-out-of-scope", mixed_bind_sql,
        -1, 2, mixed_bind_sql, 0
    );

    free(links_sql);
    free(links_repl);
    free(links_expected);
    free(links_two_level_expected);
    free(links_miss_sql);
    free(resume_simple_sql);
    free(resume_simple_expected);
    free(resume_early_sql);
    free(similar_sql);
    free(similar_expected);
    free(latest_sql);
    free(latest_expected);
    free(latest_new_corr_sql);
    free(latest_new_corr_expected);
    free(latest_miss_sql);
    free(latest_unsupported_sql);
    free(latest_bind_sql);
    free(movies_sql);
    free(movies_expected);
    free(movies_miss_sql);
    free(movies_unsupported_sql);
    free(movies_bind_sql);
    free(movies_no_guard_sql);
    free(mixed_sql);
    free(mixed_expected);
    free(mixed_miss_sql);
    free(mixed_bind_sql);
    return SQLITE_OK;
}

typedef enum emby_d8_observability_missing_kind {
    EMBY_D8_MISSING_MOVIES_OUTER = 0,
    EMBY_D8_MISSING_MOVIES_INNER,
    EMBY_D8_MISSING_MIXED,
    EMBY_D8_MISSING_EPISODES_DATE,
    EMBY_D8_MISSING_EPISODES_GROUP,
    EMBY_D8_MISSING_EPISODES_BOTH
} emby_d8_observability_missing_kind;

typedef struct emby_d8_observability_missing_spec {
    emby_d8_observability_missing_kind kind;
    const char *first_label;
    const char *second_label;
} emby_d8_observability_missing_spec;

static const emby_d8_observability_missing_spec
    emby_d8_movies_outer_missing_spec = {
        EMBY_D8_MISSING_MOVIES_OUTER,
        "obs-dashboard-movies-outer-missing",
        "obs-dashboard-movies-outer-missing-repeat"
    };
static const emby_d8_observability_missing_spec
    emby_d8_movies_inner_missing_spec = {
        EMBY_D8_MISSING_MOVIES_INNER,
        "obs-dashboard-movies-inner-missing",
        "obs-dashboard-movies-inner-missing-repeat"
    };
static const emby_d8_observability_missing_spec emby_d8_mixed_missing_spec = {
    EMBY_D8_MISSING_MIXED,
    "obs-dashboard-mixed-index-missing",
    "obs-dashboard-mixed-index-missing-repeat"
};
static const emby_d8_observability_missing_spec
    emby_d8_episodes_date_missing_spec = {
        EMBY_D8_MISSING_EPISODES_DATE,
        "obs-dashboard-date-index-missing",
        "obs-dashboard-date-index-missing-repeat"
    };
static const emby_d8_observability_missing_spec
    emby_d8_episodes_group_missing_spec = {
        EMBY_D8_MISSING_EPISODES_GROUP,
        "obs-dashboard-group-index-missing-new-connection",
        NULL
    };
static const emby_d8_observability_missing_spec
    emby_d8_episodes_both_missing_spec = {
        EMBY_D8_MISSING_EPISODES_BOTH,
        "obs-dashboard-both-indexes-missing-new-connection",
        NULL
    };

static int rsh_custom_adapter_emby_d8_observability_missing_producer(
    const rsh_case_context *context,
    const void *immutable_data
) {
    const emby_d8_observability_missing_spec *spec =
        (const emby_d8_observability_missing_spec *)immutable_data;
    sqlite3 *db = rsh_context_db(context, "candidate");
    char *sql;

    if (spec->kind == EMBY_D8_MISSING_MOVIES_OUTER ||
        spec->kind == EMBY_D8_MISSING_MOVIES_INNER) {
        sql = make_movies_latest_sql(1, "100", "42", "12");
    } else if (spec->kind == EMBY_D8_MISSING_MIXED) {
        sql = make_mixed_latest_sql("42", "3");
    } else {
        sql = make_latest_sql("A.Id ", "12");
    }
    expect_sql(db, spec->first_label, sql, -1, 2, sql, 0);
    if (spec->second_label) {
        expect_sql(db, spec->second_label, sql, -1, 2, sql, 0);
    }
    free(sql);
    return SQLITE_OK;
}

static int rsh_custom_adapter_emby_d8_observability_probe_error_producer(
    const rsh_case_context *context,
    const void *immutable_data
) {
    sqlite3 *db = rsh_context_db(context, "candidate");
    char *latest_sql = make_latest_sql("A.Id ", "12");
    char *movies_sql = make_movies_latest_sql(1, "100", "42", "12");
    char *mixed_sql = make_mixed_latest_sql("42", "3");

    (void)immutable_data;
    require_int(
        "observability/probe-authorizer/set",
        sqlite3_set_authorizer(db, deny_sqlite_master_read, NULL), SQLITE_OK
    );
    expect_sql(
        db, "obs-dashboard-index-probe-error", latest_sql, -1, 2,
        latest_sql, 0
    );
    expect_sql(
        db, "obs-dashboard-index-probe-error-repeat", latest_sql, -1, 2,
        latest_sql, 0
    );
    expect_sql(
        db, "obs-dashboard-movies-index-probe-error", movies_sql, -1, 2,
        movies_sql, 0
    );
    expect_sql(
        db, "obs-dashboard-movies-index-probe-error-repeat", movies_sql,
        -1, 2, movies_sql, 0
    );
    expect_sql(
        db, "obs-dashboard-mixed-index-probe-error", mixed_sql, -1, 2,
        mixed_sql, 0
    );
    expect_sql(
        db, "obs-dashboard-mixed-index-probe-error-repeat", mixed_sql,
        -1, 2, mixed_sql, 0
    );
    require_int(
        "observability/probe-authorizer/clear",
        sqlite3_set_authorizer(db, NULL, NULL), SQLITE_OK
    );
    free(latest_sql);
    free(movies_sql);
    free(mixed_sql);
    return SQLITE_OK;
}

static const rsh_db_spec emby_d8_observability_primary_dbs[] = {
    {
        .role = "candidate",
        .relative_path = "library.db",
        .kind = RSH_DB_CANDIDATE,
        .storage = RSH_DB_RELATIVE,
        .setup_profile = EMBY_PROFILE_D8_MIXED_ALL_INDEXES_WITH_MOVIES_SEED
    }
};

static const rsh_case_spec emby_d8_observability_primary_cases[] = {
    {
        .label = "observability-primary-producer",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_IDENTITY,
            .assert_custom =
                rsh_custom_adapter_emby_d8_observability_primary_producer,
        }
    }
};

#define EMBY_D8_DEFINE_OBSERVABILITY_MISSING_PHASE( \
    name, phase_label, profile_value, spec_value) \
    static const rsh_db_spec name##_dbs[] = { \
        { \
            .role = "candidate", \
            .relative_path = "library.db", \
            .kind = RSH_DB_CANDIDATE, \
            .storage = RSH_DB_RELATIVE, \
            .setup_profile = (profile_value) \
        } \
    }; \
    static const rsh_case_spec name##_cases[] = { \
        { \
            .label = (phase_label), \
            .kind = RSH_CASE_CUSTOM, \
            .build_mask = RSH_BUILD_EMBY_LINKED, \
            .data.custom = { \
                .kind = RSH_CUSTOM_FAULT_MATRIX, \
                .assert_custom = rsh_custom_adapter_emby_d8_observability_missing_producer, \
                .immutable_data = &(spec_value) \
            } \
        } \
    }

EMBY_D8_DEFINE_OBSERVABILITY_MISSING_PHASE(
    emby_d8_movies_outer_missing,
    "observability-movies-outer-missing",
    EMBY_PROFILE_MOVIES_OUTER_INDEX_MISSING,
    emby_d8_movies_outer_missing_spec
);
EMBY_D8_DEFINE_OBSERVABILITY_MISSING_PHASE(
    emby_d8_movies_inner_missing,
    "observability-movies-inner-missing",
    EMBY_PROFILE_MOVIES_INNER_INDEX_MISSING,
    emby_d8_movies_inner_missing_spec
);
EMBY_D8_DEFINE_OBSERVABILITY_MISSING_PHASE(
    emby_d8_mixed_missing,
    "observability-mixed-missing",
    EMBY_PROFILE_D5B_MIXED_OUTER_MISSING,
    emby_d8_mixed_missing_spec
);
EMBY_D8_DEFINE_OBSERVABILITY_MISSING_PHASE(
    emby_d8_episodes_date_missing,
    "observability-date-index-missing",
    EMBY_PROFILE_D8_EPISODES_DATE_MISSING,
    emby_d8_episodes_date_missing_spec
);
EMBY_D8_DEFINE_OBSERVABILITY_MISSING_PHASE(
    emby_d8_episodes_group_missing,
    "observability-group-index-missing-new-connection",
    EMBY_PROFILE_D8_EPISODES_GROUP_MISSING,
    emby_d8_episodes_group_missing_spec
);
EMBY_D8_DEFINE_OBSERVABILITY_MISSING_PHASE(
    emby_d8_episodes_both_missing,
    "observability-both-indexes-missing-new-connection",
    EMBY_PROFILE_D8_EPISODES_BOTH_MISSING,
    emby_d8_episodes_both_missing_spec
);

static const rsh_db_spec emby_d8_observability_probe_error_dbs[] = {
    {
        .role = "candidate",
        .relative_path = "library.db",
        .kind = RSH_DB_CANDIDATE,
        .storage = RSH_DB_RELATIVE,
        .setup_profile = EMBY_PROFILE_D8_MIXED_ALL_INDEXES_WITH_MOVIES_SEED
    }
};

static const rsh_case_spec emby_d8_observability_probe_error_cases[] = {
    {
        .label = "observability-probe-error-producer",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_FAULT_MATRIX,
            .assert_custom =
                rsh_custom_adapter_emby_d8_observability_probe_error_producer,
        }
    }
};

static const rsh_phase_spec emby_d8_observability_phases[] = {
    {
        .label = "observability-primary",
        .dbs = emby_d8_observability_primary_dbs,
        .db_count = sizeof(emby_d8_observability_primary_dbs) /
                    sizeof(emby_d8_observability_primary_dbs[0]),
        .cases = emby_d8_observability_primary_cases,
        .case_count = sizeof(emby_d8_observability_primary_cases) /
                      sizeof(emby_d8_observability_primary_cases[0])
    },
    {
        .label = "observability-movies-outer-missing",
        .dbs = emby_d8_movies_outer_missing_dbs,
        .db_count = sizeof(emby_d8_movies_outer_missing_dbs) /
                    sizeof(emby_d8_movies_outer_missing_dbs[0]),
        .cases = emby_d8_movies_outer_missing_cases,
        .case_count = sizeof(emby_d8_movies_outer_missing_cases) /
                      sizeof(emby_d8_movies_outer_missing_cases[0])
    },
    {
        .label = "observability-movies-inner-missing",
        .dbs = emby_d8_movies_inner_missing_dbs,
        .db_count = sizeof(emby_d8_movies_inner_missing_dbs) /
                    sizeof(emby_d8_movies_inner_missing_dbs[0]),
        .cases = emby_d8_movies_inner_missing_cases,
        .case_count = sizeof(emby_d8_movies_inner_missing_cases) /
                      sizeof(emby_d8_movies_inner_missing_cases[0])
    },
    {
        .label = "observability-mixed-missing",
        .dbs = emby_d8_mixed_missing_dbs,
        .db_count = sizeof(emby_d8_mixed_missing_dbs) /
                    sizeof(emby_d8_mixed_missing_dbs[0]),
        .cases = emby_d8_mixed_missing_cases,
        .case_count = sizeof(emby_d8_mixed_missing_cases) /
                      sizeof(emby_d8_mixed_missing_cases[0])
    },
    {
        .label = "observability-date-index-missing",
        .dbs = emby_d8_episodes_date_missing_dbs,
        .db_count = sizeof(emby_d8_episodes_date_missing_dbs) /
                    sizeof(emby_d8_episodes_date_missing_dbs[0]),
        .cases = emby_d8_episodes_date_missing_cases,
        .case_count = sizeof(emby_d8_episodes_date_missing_cases) /
                      sizeof(emby_d8_episodes_date_missing_cases[0])
    },
    {
        .label = "observability-group-index-missing-new-connection",
        .dbs = emby_d8_episodes_group_missing_dbs,
        .db_count = sizeof(emby_d8_episodes_group_missing_dbs) /
                    sizeof(emby_d8_episodes_group_missing_dbs[0]),
        .cases = emby_d8_episodes_group_missing_cases,
        .case_count = sizeof(emby_d8_episodes_group_missing_cases) /
                      sizeof(emby_d8_episodes_group_missing_cases[0])
    },
    {
        .label = "observability-both-indexes-missing-new-connection",
        .dbs = emby_d8_episodes_both_missing_dbs,
        .db_count = sizeof(emby_d8_episodes_both_missing_dbs) /
                    sizeof(emby_d8_episodes_both_missing_dbs[0]),
        .cases = emby_d8_episodes_both_missing_cases,
        .case_count = sizeof(emby_d8_episodes_both_missing_cases) /
                      sizeof(emby_d8_episodes_both_missing_cases[0])
    },
    {
        .label = "observability-probe-error",
        .dbs = emby_d8_observability_probe_error_dbs,
        .db_count = sizeof(emby_d8_observability_probe_error_dbs) /
                    sizeof(emby_d8_observability_probe_error_dbs[0]),
        .cases = emby_d8_observability_probe_error_cases,
        .case_count = sizeof(emby_d8_observability_probe_error_cases) /
                      sizeof(emby_d8_observability_probe_error_cases[0])
    }
};

static int rsh_custom_adapter_emby_d8_observability_log_assert(
    const rsh_case_context *context,
    const void *immutable_data
) {
    const char *log_output = context->captured_stderr;
    char *resume_sql;
    char *resume_early_sql;
    char *latest_sql;
    char *latest_new_corr_sql;
    char *latest_miss_sql;

    (void)immutable_data;
    resume_sql = make_resume_sql();
    resume_early_sql = replace_once(
        resume_sql,
        "AND LastWatchedEpisodes.LastPlayedDateInt not null",
        "AND 1=1"
    );
    free(resume_sql);
    latest_sql = make_latest_sql("A.Id ", "12");
    latest_new_corr_sql = replace_once(latest_sql, "in (100)", "in (101)");
    latest_miss_sql = make_latest_sql("(A.DateCreated) ", "12");

    require_contains(
        "observability/fanout-applied",
        log_output,
        "event=rewrite_applied target=emby mode=fanout+links_search"
    );
    require_int(
        "observability/fanout-links-applied-count",
        count_occurrences(
            log_output,
            "event=rewrite_applied target=emby mode=fanout+links_search"
        ),
        2
    );
    require_contains(
        "observability/fanout-resume-simple-applied",
        log_output,
        "event=rewrite_applied target=emby mode=fanout+resume_simple"
    );
    require_contains(
        "observability/fanout-similar-applied",
        log_output,
        "event=rewrite_applied target=emby mode=fanout+similar"
    );
    require_contains(
        "observability/dashboard-applied",
        log_output,
        "event=rewrite_applied target=emby mode=dashboard+episodes_latest"
    );
    require_int(
        "observability/dashboard-applied-count",
        count_occurrences(
            log_output,
            "event=rewrite_applied target=emby mode=dashboard+episodes_latest"
        ),
        2
    );
    require_contains(
        "observability/dashboard-applied-new",
        log_output,
        "sample=new count=2"
    );
    require_contains(
        "observability/dashboard-applied-new-source",
        log_output,
        latest_new_corr_sql
    );
    require_contains(
        "observability/dashboard-movies-applied",
        log_output,
        "event=rewrite_applied target=emby mode=dashboard+movies_latest"
    );
    require_contains(
        "observability/dashboard-mixed-applied",
        log_output,
        "event=rewrite_applied target=emby mode=dashboard+mixed_latest"
    );
    require_contains(
        "observability/fanout-capture-miss",
        log_output,
        "event=rewrite_skipped target=emby reason=capture_miss mode=fanout+links_search sub_reason=type_slot"
    );
    require_contains(
        "observability/dashboard-capture-miss",
        log_output,
        "event=rewrite_skipped target=emby reason=capture_miss mode=dashboard+episodes_latest sub_reason=projection"
    );
    {
        char metadata[160];
        int rc = snprintf(
            metadata, sizeof(metadata),
            "sql_len=%lu corr=%016llx",
            (unsigned long)strlen(latest_miss_sql),
            (unsigned long long)sql_corr_key(
                latest_miss_sql, strlen(latest_miss_sql)
            )
        );
        if (rc < 0 || (size_t)rc >= sizeof(metadata)) {
            failf("FATAL: observability capture metadata overflow");
        }
        require_contains(
            "observability/dashboard-capture-correlation",
            log_output, metadata
        );
        require_contains(
            "observability/dashboard-capture-source",
            log_output, latest_miss_sql
        );
    }
    require_absent(
        "observability/series-early-miss-source", log_output,
        emby_d8_latest_series_browse_sql
    );
    require_absent(
        "observability/resume-early-miss-source", log_output,
        resume_early_sql
    );
    require_int(
        "observability/dashboard-capture-miss-count",
        count_occurrences(
            log_output,
            "event=rewrite_skipped target=emby reason=capture_miss mode=dashboard+episodes_latest"
        ),
        1
    );
    require_int(
        "observability/dashboard-movies-capture-miss-count",
        count_occurrences(
            log_output,
            "event=rewrite_skipped target=emby reason=capture_miss mode=dashboard+movies_latest"
        ),
        2
    );
    require_same_line(
        "observability/dashboard-mixed-capture-miss",
        log_output,
        "reason=capture_miss mode=dashboard+mixed_latest sub_reason=projection",
        "sample=first count=1"
    );
    require_int(
        "observability/dashboard-out-of-scope-count",
        count_occurrences(
            log_output,
            "event=rewrite_skipped target=emby reason=out_of_scope mode=dashboard+episodes_latest"
        ),
        2
    );
    require_same_line(
        "observability/dashboard-limit-unsupported",
        log_output,
        "reason=out_of_scope mode=dashboard+episodes_latest sub_reason=limit_unsupported",
        "sample=first count=1"
    );
    require_same_line(
        "observability/dashboard-bind-out-of-scope",
        log_output,
        "reason=out_of_scope mode=dashboard+episodes_latest sub_reason=bind",
        "sample=new count=2"
    );
    require_int(
        "observability/dashboard-movies-out-of-scope-count",
        count_occurrences(
            log_output,
            "event=rewrite_skipped target=emby reason=out_of_scope mode=dashboard+movies_latest"
        ),
        2
    );
    require_same_line(
        "observability/dashboard-movies-limit-unsupported",
        log_output,
        "reason=out_of_scope mode=dashboard+movies_latest sub_reason=limit_unsupported",
        "sample=first count=1"
    );
    require_same_line(
        "observability/dashboard-movies-bind-out-of-scope",
        log_output,
        "reason=out_of_scope mode=dashboard+movies_latest sub_reason=bind",
        "sample=new count=2"
    );
    require_same_line(
        "observability/dashboard-mixed-bind-out-of-scope",
        log_output,
        "reason=out_of_scope mode=dashboard+mixed_latest sub_reason=bind",
        "sample=first count=1"
    );
    require_contains(
        "observability/dashboard-index-missing",
        log_output,
        "event=rewrite_skipped target=emby reason=index_missing mode=dashboard+episodes_latest"
    );
    require_int(
        "observability/dashboard-index-missing-count",
        count_occurrences(
            log_output,
            "event=rewrite_skipped target=emby reason=index_missing mode=dashboard+episodes_latest"
        ),
        1
    );
    require_same_line(
        "observability/dashboard-index-missing-sample",
        log_output,
        "reason=index_missing mode=dashboard+episodes_latest",
        "sample=first count=1"
    );
    require_contains(
        "observability/dashboard-index-probe-error",
        log_output,
        "event=rewrite_skipped target=emby reason=index_probe_error mode=dashboard+episodes_latest"
    );
    require_int(
        "observability/dashboard-index-probe-error-count",
        count_occurrences(
            log_output,
            "event=rewrite_skipped target=emby reason=index_probe_error mode=dashboard+episodes_latest"
        ),
        2
    );
    require_int(
        "observability/dashboard-movies-index-missing-count",
        count_occurrences(
            log_output,
            "event=rewrite_skipped target=emby reason=index_missing mode=dashboard+movies_latest"
        ),
        1
    );
    require_same_line(
        "observability/dashboard-movies-index-missing-sample",
        log_output,
        "reason=index_missing mode=dashboard+movies_latest",
        "sample=first count=1"
    );
    require_int(
        "observability/dashboard-movies-index-probe-error-count",
        count_occurrences(
            log_output,
            "event=rewrite_skipped target=emby reason=index_probe_error mode=dashboard+movies_latest"
        ),
        2
    );
    require_int(
        "observability/dashboard-mixed-index-missing-count",
        count_occurrences(
            log_output,
            "event=rewrite_skipped target=emby reason=index_missing mode=dashboard+mixed_latest"
        ),
        1
    );
    require_same_line(
        "observability/dashboard-mixed-index-missing-sample",
        log_output,
        "reason=index_missing mode=dashboard+mixed_latest",
        "sample=first count=1"
    );
    require_int(
        "observability/dashboard-mixed-index-probe-error-count",
        count_occurrences(
            log_output,
            "event=rewrite_skipped target=emby reason=index_probe_error mode=dashboard+mixed_latest"
        ),
        2
    );
    require_absent(
        "observability/dashboard-movies-no-rewritten-prepare-failure",
        log_output,
        "event=rewrite_skipped target=emby reason=rewritten_prepare_failed mode=dashboard+movies_latest"
    );
    require_absent(
        "observability/old-dashboard-mode", log_output, "dashboard+" "latest"
    );

    free(resume_early_sql);
    free(latest_sql);
    free(latest_new_corr_sql);
    free(latest_miss_sql);
    return SQLITE_OK;
}

static const rsh_case_spec emby_d8_observability_post_close_cases[] = {
    {
        .label = "observability-log-assert",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_LOG_CAPTURE,
            .assert_custom =
                rsh_custom_adapter_emby_d8_observability_log_assert,
        }
    }
};

static int rsh_custom_adapter_emby_d8_master_disabled_producer(
    const rsh_case_context *context,
    const void *immutable_data
) {
    sqlite3 *db = rsh_context_db(context, "candidate");
    sqlite3_stmt *stmt = NULL;
    char *latest_sql = make_latest_sql("A.Id ", "12");
    char *latest_expected = make_latest_expected("A.Id ", "12");
    char *latest_miss_sql = make_latest_sql("(A.DateCreated) ", "12");
    const char *failure_stage = "prepare";
    int rc;

    (void)immutable_data;
    expect_sql(
        db, "master-disabled-applied", latest_sql, -1, 2,
        latest_expected, 0
    );
    expect_sql(
        db, "master-disabled-capture", latest_miss_sql, -1, 2,
        latest_miss_sql, 0
    );
    rc = sqlite3_prepare_v2(db, latest_sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK && !stmt) rc = SQLITE_ERROR;
    if (rc == SQLITE_OK) {
        failure_stage = "step";
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {}
        if (rc == SQLITE_DONE) rc = SQLITE_OK;
    }
    if (stmt) {
        int finalize_rc = sqlite3_finalize(stmt);
        stmt = NULL;
        if (rc == SQLITE_OK && finalize_rc != SQLITE_OK) {
            failure_stage = "finalize";
            rc = finalize_rc;
        }
    }
    if (rc != SQLITE_OK) {
        failf(
            "FAIL [master-disabled-stmt]: expected=%d actual=%d stage=%s err=%s",
            SQLITE_OK, rc, failure_stage, sqlite3_errmsg(db)
        );
    }
    free(latest_sql);
    free(latest_expected);
    free(latest_miss_sql);
    return SQLITE_OK;
}

static int rsh_custom_adapter_emby_d8_master_disabled_log_assert(
    const rsh_case_context *context,
    const void *immutable_data
) {
    const char *log_output = context->captured_stderr;

    (void)immutable_data;
    require_absent("master-disabled/capture", log_output, "reason=capture_miss");
    require_absent("master-disabled/applied", log_output, "event=rewrite_applied");
    require_absent("master-disabled/stmt", log_output, "event=SQLITE_TRACE_STMT");
    return SQLITE_OK;
}

static const rsh_db_spec emby_d8_master_disabled_dbs[] = {
    {
        .role = "candidate",
        .relative_path = "library.db",
        .kind = RSH_DB_CANDIDATE,
        .storage = RSH_DB_RELATIVE,
        .setup_profile = EMBY_PROFILE_LATEST_INDEXES
    }
};

static const rsh_case_spec emby_d8_master_disabled_producer_cases[] = {
    {
        .label = "observability-master-disabled-producer",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_IDENTITY,
            .assert_custom =
                rsh_custom_adapter_emby_d8_master_disabled_producer,
        }
    }
};

static const rsh_phase_spec emby_d8_master_disabled_phases[] = {
    {
        .label = "observability-master-disabled",
        .dbs = emby_d8_master_disabled_dbs,
        .db_count = sizeof(emby_d8_master_disabled_dbs) /
                    sizeof(emby_d8_master_disabled_dbs[0]),
        .cases = emby_d8_master_disabled_producer_cases,
        .case_count = sizeof(emby_d8_master_disabled_producer_cases) /
                      sizeof(emby_d8_master_disabled_producer_cases[0])
    }
};

static const rsh_case_spec emby_d8_master_disabled_post_close_cases[] = {
    {
        .label = "observability-master-disabled-log-assert",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_LOG_CAPTURE,
            .assert_custom =
                rsh_custom_adapter_emby_d8_master_disabled_log_assert,
        }
    }
};

#ifdef EMBY_FTS_REWRITE_DIRECT_TEST

#ifdef EMBY_FTS_REWRITE_TEST_LITERAL_MATCH
static const rsh_case_spec emby_direct_test_api_literal_assertion = {
    .label = "direct-literal-rhs-positive",
    .kind = RSH_CASE_SQL_EXACT,
    .build_mask = RSH_BUILD_EMBY_DIRECT,
    .data.sql_exact = {
        .role = "candidate",
        .sql = emby_positive_literal_sql,
        .expected_sql = emby_positive_literal_expected_sql,
        .prepare = EMBY_PREPARE_V2
    }
};
#endif

static int rsh_custom_adapter_emby_direct_test_api(
    const rsh_case_context *context,
    const void *immutable_data
) {
    sqlite3 *db = rsh_context_db(context, "candidate");

    (void)immutable_data;
#ifdef EMBY_FTS_REWRITE_TEST_LITERAL_MATCH
    rsh_run_sql_exact(
        &emby_suite_spec, &emby_direct_test_api_literal_assertion, context,
        &emby_direct_test_api_literal_assertion.data.sql_exact
    );
#endif
#ifdef EMBY_FTS_REWRITE_TEST_API
    require_int(
        "direct-duplicate-anchor-guard",
        emby_fts_rewrite_test_duplicate_anchor_guard(), 0
    );
    require_int(
        "direct-string-anchor-immunity",
        emby_fts_rewrite_test_string_anchor_immunity(), 1
    );
    expect_scalar_counter(db, emby_fts_env_sql);
#endif
    return SQLITE_OK;
}

static const rsh_case_spec emby_direct_test_api_cases[] = {
    {
        .label = "direct-test-api",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_EMBY_DIRECT,
        .data.custom = {
            .kind = RSH_CUSTOM_FAULT_MATRIX,
            .assert_custom = rsh_custom_adapter_emby_direct_test_api,
        }
    }
};
DEFINE_EMBY_SCALAR_PHASE(
    emby_direct_test_api_phases, "direct-test-api",
    emby_env_candidate_db, emby_direct_test_api_cases
);

typedef struct emby_direct_fail_open_family_spec {
    int movies;
} emby_direct_fail_open_family_spec;

static const emby_direct_fail_open_family_spec
    emby_direct_fail_open_episodes_spec = {0};
static const emby_direct_fail_open_family_spec
    emby_direct_fail_open_movies_spec = {1};

static int rsh_custom_adapter_emby_direct_fail_open_family(
    const rsh_case_context *context,
    const void *immutable_data
) {
    static const enum direct_fault_mode faults[] = {
        DIRECT_FAULT_CANDIDATE_ERROR,
        DIRECT_FAULT_CANDIDATE_ERROR_WITH_STMT,
        DIRECT_FAULT_CANDIDATE_WRONG_TAIL
    };
    const emby_direct_fail_open_family_spec *spec =
        (const emby_direct_fail_open_family_spec *)immutable_data;
    sqlite3 *db = rsh_context_db(context, "candidate");
    sqlite3_stmt *baseline[64];
    int baseline_n = 0;
    sqlite3_stmt *baseline_stmt;
    char *raw = spec->movies
        ? make_movies_latest_sql(1, "100", "42", "12")
        : make_latest_sql("A.Id ", "12");
    size_t i;

    for (baseline_stmt = sqlite3_next_stmt(db, NULL); baseline_stmt;
         baseline_stmt = sqlite3_next_stmt(db, baseline_stmt)) {
        if (baseline_n >= (int)(sizeof(baseline) / sizeof(baseline[0]))) {
            failf(
                "FAIL [direct-fail-open/baseline-overflow]: got=%d want<%lu",
                baseline_n + 1,
                (unsigned long)(sizeof(baseline) / sizeof(baseline[0]))
            );
        }
        baseline[baseline_n++] = baseline_stmt;
    }

    for (i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
        sqlite3_stmt *stmt = NULL;
        const char *tail = NULL;
        sqlite3_stmt *open_stmt;
        int found = 0;
        int rc;

        direct_original_sql = raw;
        direct_fault = faults[i];
        direct_candidate_calls = 0;
        direct_original_calls = 0;
        rc = emby_fts_rewrite_prepare(
            db, raw, -1, 0, &stmt, &tail, FTS_REWRITE_PREPARE_V2
        );
        require_int("direct-fail-open/rc", rc, SQLITE_OK);
        require_int(
            "direct-fail-open/candidate-calls", direct_candidate_calls, 1
        );
        require_int(
            "direct-fail-open/original-calls", direct_original_calls, 1
        );
        require_str_eq("direct-fail-open/sql", sqlite3_sql(stmt), raw);
        if (tail != raw + strlen(raw)) {
            failf(
                "FAIL [direct-fail-open/tail]: got=%ld want=%ld",
                (long)(tail - raw), (long)strlen(raw)
            );
        }
        for (open_stmt = sqlite3_next_stmt(db, NULL); open_stmt;
             open_stmt = sqlite3_next_stmt(db, open_stmt)) {
            if (open_stmt == stmt) {
                found = 1;
            } else if (!direct_stmt_is_baseline(
                           open_stmt, baseline, baseline_n
                       )) {
                failf(
                    "FAIL [direct-fail-open/next-stmt]: "
                    "got=non-baseline-non-result want=baseline-or-result "
                    "fault=%d",
                    (int)faults[i]
                );
            }
        }
        if (!found) {
            failf(
                "FAIL [direct-fail-open/next-stmt]: got_result_count=0 "
                "want_result_count=1 fault=%d",
                (int)faults[i]
            );
        }
        require_int(
            "direct-fail-open/finalize", sqlite3_finalize(stmt), SQLITE_OK
        );
        for (open_stmt = sqlite3_next_stmt(db, NULL); open_stmt;
             open_stmt = sqlite3_next_stmt(db, open_stmt)) {
            if (!direct_stmt_is_baseline(open_stmt, baseline, baseline_n)) {
                failf(
                    "FAIL [direct-fail-open/post-finalize]: "
                    "got=non-baseline want=baseline-only fault=%d",
                    (int)faults[i]
                );
            }
        }
    }

    {
        char *invalid = make_latest_sql("A.NoSuchColumn ", "12");
        sqlite3_stmt *stmt = NULL;
        const char *tail = NULL;
        int rc;

        direct_original_sql = invalid;
        direct_fault = DIRECT_FAULT_CANDIDATE_ERROR;
        direct_candidate_calls = 0;
        direct_original_calls = 0;
        rc = emby_fts_rewrite_prepare(
            db, invalid, -1, 0, &stmt, &tail, FTS_REWRITE_PREPARE_V2
        );
        require_int("direct-invalid-original/rc", rc, SQLITE_ERROR);
        require_int(
            "direct-invalid-original/candidate-calls", direct_candidate_calls, 1
        );
        require_int(
            "direct-invalid-original/original-calls", direct_original_calls, 1
        );
        if (stmt != NULL) {
            failf(
                "FAIL [direct-invalid-original/stmt]: got=non-NULL want=NULL"
            );
        }
        free(invalid);
    }

    direct_original_sql = NULL;
    direct_fault = DIRECT_FAULT_NONE;
    free(raw);
    return SQLITE_OK;
}

static const rsh_db_spec emby_direct_fail_open_family_dbs[] = {
    {
        .role = "candidate",
        .relative_path = "library.db",
        .kind = RSH_DB_CANDIDATE,
        .storage = RSH_DB_RELATIVE,
        .setup_profile = EMBY_PROFILE_FIXTURE_CANARY
    }
};

static const rsh_case_spec emby_direct_fail_open_episodes_cases[] = {
    {
        .label = "direct-fail-open-episodes",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_EMBY_DIRECT,
        .data.custom = {
            .kind = RSH_CUSTOM_FAULT_MATRIX,
            .assert_custom = rsh_custom_adapter_emby_direct_fail_open_family,
            .immutable_data = &emby_direct_fail_open_episodes_spec
        }
    }
};
DEFINE_EMBY_SCALAR_PHASE(
    emby_direct_fail_open_episodes_phases, "direct-fail-open-episodes",
    emby_direct_fail_open_family_dbs, emby_direct_fail_open_episodes_cases
);

static const rsh_case_spec emby_direct_fail_open_movies_cases[] = {
    {
        .label = "direct-fail-open-movies",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_EMBY_DIRECT,
        .data.custom = {
            .kind = RSH_CUSTOM_FAULT_MATRIX,
            .assert_custom = rsh_custom_adapter_emby_direct_fail_open_family,
            .immutable_data = &emby_direct_fail_open_movies_spec
        }
    }
};
DEFINE_EMBY_SCALAR_PHASE(
    emby_direct_fail_open_movies_phases, "direct-fail-open-movies",
    emby_direct_fail_open_family_dbs, emby_direct_fail_open_movies_cases
);

static const direct_mixed_fail_open_case emby_direct_fail_open_mixed_cells[] = {
    {"build-failure", DIRECT_FAULT_NONE, 1, 0, "build_failed", 0, -1, -1, -1, -1},
    {"rewritten-prepare-failure", DIRECT_FAULT_CANDIDATE_ERROR, 0, 1,
     "rewritten_prepare_failed", 0, -1, -1, 1, 1},
    {"partial-candidate", DIRECT_FAULT_CANDIDATE_ERROR_WITH_STMT, 0, 1,
     "rewritten_prepare_failed", 1, 0, 19, 1, 1},
    {"tail-mismatch", DIRECT_FAULT_CANDIDATE_WRONG_TAIL, 0, 1,
     "tail_mismatch", 1, 0, 19, 1, 1},
    {"accept-bind-mismatch", DIRECT_FAULT_MIXED_BIND_MISMATCH, 0, 1,
     "candidate_bind_mismatch", 1, 1, 1, 1, 1},
    {"accept-column-mismatch", DIRECT_FAULT_MIXED_COLUMN_MISMATCH, 0, 1,
     "candidate_column_mismatch", 1, 0, 1, 1, 1},
    {"accept-index-mismatch", DIRECT_FAULT_MIXED_INDEX_MISMATCH, 0, 1,
     "candidate_index_mismatch", 1, 0, 19, 0, 1}
};

static void rsh_require_native_direct_mixed_stmt_set(
    const char *case_name,
    sqlite3 *db,
    sqlite3_stmt **baseline,
    int baseline_n,
    sqlite3_stmt *result_stmt
) {
    sqlite3_stmt *open_stmt;
    int result_count = 0;

    for (open_stmt = sqlite3_next_stmt(db, NULL); open_stmt;
         open_stmt = sqlite3_next_stmt(db, open_stmt)) {
        if (open_stmt == result_stmt) {
            result_count++;
        } else if (!direct_stmt_is_baseline(open_stmt, baseline, baseline_n)) {
            failf(
                "FAIL [direct-fail-open-mixed/%s/next-stmt]: "
                "got=non-baseline-non-result want=baseline-or-result",
                case_name
            );
        }
    }
    if (result_count != 1) {
        failf(
            "FAIL [direct-fail-open-mixed/%s/next-stmt]: "
            "got_result_count=%d want_result_count=1",
            case_name, result_count
        );
    }
}

static void rsh_require_native_direct_mixed_baseline_only(
    const char *case_name,
    sqlite3 *db,
    sqlite3_stmt **baseline,
    int baseline_n
) {
    sqlite3_stmt *open_stmt;

    for (open_stmt = sqlite3_next_stmt(db, NULL); open_stmt;
         open_stmt = sqlite3_next_stmt(db, open_stmt)) {
        if (!direct_stmt_is_baseline(open_stmt, baseline, baseline_n)) {
            failf(
                "FAIL [direct-fail-open-mixed/%s/post-finalize]: "
                "got=non-baseline want=baseline-only",
                case_name
            );
        }
    }
}

static int rsh_custom_adapter_emby_direct_fail_open_mixed(
    const rsh_case_context *context,
    const void *immutable_data
) {
    sqlite3 *db = rsh_context_db(context, "candidate");
    sqlite3_stmt *baseline[64];
    int baseline_n = 0;
    sqlite3_stmt *baseline_stmt;
    char *raw = make_mixed_latest_sql("42", "3");
    char *expected = make_mixed_latest_expected("42", "3");
    int prior_sql_limit;
    size_t i;

    (void)immutable_data;
    require_int(
        "direct-fail-open-mixed/rewrite-longer-than-source",
        strlen(expected) > strlen(raw), 1
    );
    require_int(
        "direct-fail-open-mixed/rewrite-length-fits-int",
        strlen(expected) <= (size_t)INT_MAX, 1
    );
    prior_sql_limit = sqlite3_limit(db, SQLITE_LIMIT_SQL_LENGTH, -1);
    for (baseline_stmt = sqlite3_next_stmt(db, NULL); baseline_stmt;
         baseline_stmt = sqlite3_next_stmt(db, baseline_stmt)) {
        if (baseline_n >= (int)(sizeof(baseline) / sizeof(baseline[0]))) {
            failf(
                "FAIL [direct-fail-open-mixed/baseline-overflow]: "
                "got=%d want<%lu",
                baseline_n + 1,
                (unsigned long)(sizeof(baseline) / sizeof(baseline[0]))
            );
        }
        baseline[baseline_n++] = baseline_stmt;
    }

    for (i = 0;
         i < sizeof(emby_direct_fail_open_mixed_cells) /
             sizeof(emby_direct_fail_open_mixed_cells[0]);
         i++) {
        const direct_mixed_fail_open_case *test_case =
            &emby_direct_fail_open_mixed_cells[i];
        sqlite3_stmt *stmt = NULL;
        const char *tail = NULL;
        char label[160];
        int rc;

        direct_original_sql = raw;
        direct_fault = test_case->fault;
        direct_candidate_calls = 0;
        direct_original_calls = 0;
        direct_candidate_input_len = 0;
        direct_candidate_had_stmt = 0;
        direct_candidate_bind_count = -1;
        direct_candidate_column_count = -1;
        direct_candidate_outer_index_count = -1;
        direct_candidate_inner_index_count = -1;
        direct_skipped_calls = 0;
        direct_skipped_reason = NULL;
        direct_skipped_mode = OBS_MODE_NONE;

        if (test_case->constrain_sql_limit) {
            int constrained_limit = (int)strlen(expected) - 1;
            require_int(
                "direct-fail-open-mixed/build/source-fits-limit",
                strlen(raw) <= (size_t)constrained_limit, 1
            );
            sqlite3_limit(db, SQLITE_LIMIT_SQL_LENGTH, constrained_limit);
            require_int(
                "direct-fail-open-mixed/build/sql-limit",
                sqlite3_limit(db, SQLITE_LIMIT_SQL_LENGTH, -1),
                constrained_limit
            );
        }

        rc = emby_fts_rewrite_prepare(
            db, raw, -1, 0, &stmt, &tail, FTS_REWRITE_PREPARE_V2
        );
        if (test_case->constrain_sql_limit) {
            sqlite3_limit(db, SQLITE_LIMIT_SQL_LENGTH, prior_sql_limit);
        }

        snprintf(
            label, sizeof(label), "direct-fail-open-mixed/%s/rc",
            test_case->name
        );
        require_int(label, rc, SQLITE_OK);
        snprintf(
            label, sizeof(label), "direct-fail-open-mixed/%s/result-stmt",
            test_case->name
        );
        require_int(label, stmt != NULL, 1);
        snprintf(
            label, sizeof(label), "direct-fail-open-mixed/%s/candidate-calls",
            test_case->name
        );
        require_int(label, direct_candidate_calls, test_case->candidate_calls);
        snprintf(
            label, sizeof(label), "direct-fail-open-mixed/%s/original-calls",
            test_case->name
        );
        require_int(label, direct_original_calls, 1);
        snprintf(
            label, sizeof(label), "direct-fail-open-mixed/%s/skipped-calls",
            test_case->name
        );
        require_int(label, direct_skipped_calls, 1);
        snprintf(
            label, sizeof(label), "direct-fail-open-mixed/%s/skipped-reason",
            test_case->name
        );
        require_str_eq(label, direct_skipped_reason, test_case->skip_reason);
        snprintf(
            label, sizeof(label), "direct-fail-open-mixed/%s/skipped-mode",
            test_case->name
        );
        require_int(label, direct_skipped_mode, EMBY_SMOKE_MODE_MIXED_LATEST);
        snprintf(
            label, sizeof(label), "direct-fail-open-mixed/%s/candidate-length",
            test_case->name
        );
        require_int(
            label, (int)direct_candidate_input_len,
            test_case->candidate_calls ? (int)strlen(expected) : 0
        );
        snprintf(
            label, sizeof(label), "direct-fail-open-mixed/%s/candidate-had-stmt",
            test_case->name
        );
        require_int(
            label, direct_candidate_had_stmt, test_case->candidate_had_stmt
        );
        snprintf(
            label, sizeof(label), "direct-fail-open-mixed/%s/candidate-binds",
            test_case->name
        );
        require_int(
            label, direct_candidate_bind_count, test_case->candidate_bind_count
        );
        snprintf(
            label, sizeof(label), "direct-fail-open-mixed/%s/candidate-columns",
            test_case->name
        );
        require_int(
            label, direct_candidate_column_count,
            test_case->candidate_column_count
        );
        snprintf(
            label, sizeof(label), "direct-fail-open-mixed/%s/outer-index-count",
            test_case->name
        );
        require_int(
            label, direct_candidate_outer_index_count,
            test_case->candidate_outer_index_count
        );
        snprintf(
            label, sizeof(label), "direct-fail-open-mixed/%s/inner-index-count",
            test_case->name
        );
        require_int(
            label, direct_candidate_inner_index_count,
            test_case->candidate_inner_index_count
        );
        snprintf(
            label, sizeof(label), "direct-fail-open-mixed/%s/source-sql",
            test_case->name
        );
        require_str_eq(label, sqlite3_sql(stmt), raw);
        snprintf(
            label, sizeof(label), "direct-fail-open-mixed/%s/source-tail",
            test_case->name
        );
        require_int(label, tail == raw + strlen(raw), 1);
        rsh_require_native_direct_mixed_stmt_set(
            test_case->name, db, baseline, baseline_n, stmt
        );
        snprintf(
            label, sizeof(label), "direct-fail-open-mixed/%s/finalize",
            test_case->name
        );
        require_int(label, sqlite3_finalize(stmt), SQLITE_OK);
        rsh_require_native_direct_mixed_baseline_only(
            test_case->name, db, baseline, baseline_n
        );
    }

    direct_original_sql = NULL;
    direct_fault = DIRECT_FAULT_NONE;
    free(raw);
    free(expected);
    return SQLITE_OK;
}

static const rsh_db_spec emby_direct_fail_open_mixed_dbs[] = {
    {
        .role = "candidate",
        .relative_path = "library.db",
        .kind = RSH_DB_CANDIDATE,
        .storage = RSH_DB_RELATIVE,
        .setup_profile = EMBY_PROFILE_D6_MIXED_IDENTITY
    }
};

static const rsh_case_spec emby_direct_fail_open_mixed_cases[] = {
    {
        .label = "direct-fail-open-mixed",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_EMBY_DIRECT,
        .data.custom = {
            .kind = RSH_CUSTOM_FAULT_MATRIX,
            .assert_custom = rsh_custom_adapter_emby_direct_fail_open_mixed,
        }
    }
};
DEFINE_EMBY_SCALAR_PHASE(
    emby_direct_fail_open_mixed_phases, "direct-fail-open-mixed",
    emby_direct_fail_open_mixed_dbs, emby_direct_fail_open_mixed_cases
);

#endif

#undef EMBY_D8_DEFINE_OBSERVABILITY_MISSING_PHASE

#undef EMBY_D8_DEFINE_INDEX_DEFINITION_PHASE

#undef EMBY_D5B_CASE_MATRIX_PHASE
#undef EMBY_D5B_PROFILE_DBS

#undef EMBY_DASHBOARD_MATRIX_DBS
#undef EMBY_DEFINE_MATRIX_AXIS_3
#undef EMBY_DEFINE_MATRIX_AXIS_2
#undef EMBY_DEFINE_MATRIX_AXIS_1
#undef EMBY_DYNAMIC_MATRIX_PHASE
#undef EMBY_MATRIX_AXIS_VALUE

#undef EMBY_FIXTURE_NEGATIVE_CASE
#undef EMBY_FTS_FIXTURE_SQL_CASE
#undef EMBY_FIXTURE_SQL_CASE

#undef DEFINE_EMBY_DASHBOARD_OFF_ROW
#undef DEFINE_EMBY_SCALAR_PHASE
#undef EMBY_FTS_SQL_CASE
#undef EMBY_SQL_CASE
#undef EMBY_PREPARE_V2

static const rsh_env_assignment emby_negative_control_env[] = {
    {
        .name = "SQLITE3_DISABLE_AUTOPRAGMA",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    },
    {
        .name = "SQLITE3_DISABLE_RUNTIME_OPTIMIZE",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    },
    {
        .name = "SQLITE3_DISABLE_OBSERVABILITY",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    },
    {
        .name = "SQLITE3_DISABLE_PLEX_FTS_REWRITE",
        .value = {.state = RSH_ENV_UNSET}
    },
    {
        .name = "SQLITE3_DISABLE_EMBY_FTS_REWRITE",
        .value = {.state = RSH_ENV_UNSET}
    },
    {
        .name = "SQLITE3_DISABLE_EMBY_FANOUT_REWRITE",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    },
    {
        .name = "SQLITE3_DISABLE_EMBY_DASHBOARD_REWRITE",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    },
    {
        .name = "SQLITE3_DISABLE_STMT_TRACE",
        .value = {.state = RSH_ENV_UNSET}
    },
    {
        .name = "SQLITE3_DISABLE_REWRITE_APPLIED_SQL",
        .value = {.state = RSH_ENV_UNSET}
    }
};

static const rsh_db_spec emby_negative_control_dbs[] = {
    {
        .role = "vendor",
        .relative_path = "not-target.db",
        .kind = RSH_DB_VENDOR,
        .storage = RSH_DB_RELATIVE
    },
    {
        .role = "candidate",
        .relative_path = "library.db",
        .kind = RSH_DB_CANDIDATE,
        .storage = RSH_DB_RELATIVE
    }
};

static const char emby_vendor_prepare_control_sql[] =
    "SELECT 'emby-negative-control-vendor-prepare' FROM";

static const rsh_case_spec emby_vendor_prepare_control_cases[] = {
    {
        .label = "negative-control-vendor-prepare",
        .kind = RSH_CASE_EXPECT_ABORT,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.expect_abort.negative = {
            .source_kind = RSH_NEGATIVE_STATIC,
            .sql = emby_vendor_prepare_control_sql,
            .discriminating_needle = "emby-negative-control-vendor-prepare",
            .prepare = {
                .kind = RSH_PREPARE_V2,
                .nbyte_kind = RSH_NBYTE_MINUS_ONE,
                .tail_kind = RSH_TAIL_FULL
            },
            .vendor_role = "vendor",
            .candidate_role = "candidate"
        }
    }
};

static const rsh_case_spec emby_matcher_miss_control_cases[] = {
    {
        .label = "negative-control-matcher-miss",
        .kind = RSH_CASE_EXPECT_ABORT,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.expect_abort.negative = {
            .source_kind = RSH_NEGATIVE_FIXTURE,
            .fixture_path = "tests/fixtures/emby-fts-rewrite/slow-type.sql",
            .strip_fixture_final_lf = 1,
            .discriminating_needle = "fts_search9 match @SearchTerm",
            .prepare = {
                .kind = RSH_PREPARE_V2,
                .nbyte_kind = RSH_NBYTE_MINUS_ONE,
                .tail_kind = RSH_TAIL_FULL
            },
            .vendor_role = "vendor",
            .candidate_role = "candidate"
        }
    }
};

static const rsh_phase_spec emby_vendor_prepare_control_phases[] = {
    {
        .label = "negative-control-vendor-prepare",
        .dbs = emby_negative_control_dbs,
        .db_count = sizeof(emby_negative_control_dbs) /
                    sizeof(emby_negative_control_dbs[0]),
        .cases = emby_vendor_prepare_control_cases,
        .case_count = sizeof(emby_vendor_prepare_control_cases) /
                      sizeof(emby_vendor_prepare_control_cases[0])
    }
};

static const rsh_phase_spec emby_matcher_miss_control_phases[] = {
    {
        .label = "negative-control-matcher-miss",
        .dbs = emby_negative_control_dbs,
        .db_count = sizeof(emby_negative_control_dbs) /
                    sizeof(emby_negative_control_dbs[0]),
        .cases = emby_matcher_miss_control_cases,
        .case_count = sizeof(emby_matcher_miss_control_cases) /
                      sizeof(emby_matcher_miss_control_cases[0])
    }
};

static const char *const emby_vendor_prepare_earlier_labels[] = {
    "FAIL [negative-control-vendor-prepare/needle-unique]"
};

static const rsh_abort_expectation emby_vendor_prepare_abort_expectation = {
    .expected_exit = 1,
    .expected_stage_label =
        "FAIL [negative-control-vendor-prepare/vendor-prepare]",
    .earlier_stage_labels = emby_vendor_prepare_earlier_labels,
    .earlier_stage_label_count = sizeof(emby_vendor_prepare_earlier_labels) /
                                 sizeof(emby_vendor_prepare_earlier_labels[0])
};

static const char *const emby_matcher_miss_earlier_labels[] = {
    "FAIL [negative-control-matcher-miss/needle-unique]",
    "FAIL [negative-control-matcher-miss/vendor-prepare]",
    "FAIL [negative-control-matcher-miss/vendor-sql]",
    "FAIL [negative-control-matcher-miss/vendor-tail]",
    "FAIL [negative-control-matcher-miss/vendor-finalize]",
    "FAIL [negative-control-matcher-miss/matcher-prepare]"
};

static const rsh_abort_expectation emby_matcher_miss_abort_expectation = {
    .expected_exit = 1,
    .expected_stage_label =
        "FAIL [negative-control-matcher-miss/matcher-miss]",
    .earlier_stage_labels = emby_matcher_miss_earlier_labels,
    .earlier_stage_label_count = sizeof(emby_matcher_miss_earlier_labels) /
                                 sizeof(emby_matcher_miss_earlier_labels[0])
};

static const rsh_suite_spec emby_suite_spec = {
#ifdef EMBY_FTS_REWRITE_DIRECT_TEST
    .suite_name = "emby fts rewrite direct test passed",
    .prepare = rsh_direct_prepare,
#else
    .suite_name = "emby fts rewrite smoke passed",
    .prepare = rsh_public_prepare,
#endif
    .temp_prefix = "/tmp/emby-fts-rewrite-smoke",
    .vendor_basename = "not-target.db",
    .target_basename = "library.db",
    .controlled_env_gates = emby_controlled_env_gates,
    .controlled_env_gate_count =
        sizeof(emby_controlled_env_gates) / sizeof(emby_controlled_env_gates[0]),
    .open = rsh_open,
    .base_seed = rsh_base_seed,
    .resolve_setup_profile = rsh_resolve_native_setup_profile,
    .failure = failf
};

#define NATIVE_RUN(dispatch, env, phase_rows) \
    { \
        .dispatch_name = (dispatch), \
        .pass_label = (dispatch), \
        .process_kind = RSH_PROCESS_FORK, \
        .outcome = RSH_OUTCOME_SUCCESS, \
        .build_mask = RSH_BUILD_EMBY_LINKED, \
        .preload_env = (env), \
        .preload_env_count = sizeof(env) / sizeof((env)[0]), \
        .capture_scope = RSH_CAPTURE_NONE, \
        .phases = (phase_rows), \
        .phase_count = sizeof(phase_rows) / sizeof((phase_rows)[0]) \
    }

#define NATIVE_DIRECT_RUN(dispatch, env, phase_rows) \
    { \
        .dispatch_name = (dispatch), \
        .pass_label = (dispatch), \
        .process_kind = RSH_PROCESS_FORK, \
        .outcome = RSH_OUTCOME_SUCCESS, \
        .build_mask = RSH_BUILD_EMBY_DIRECT, \
        .preload_env = (env), \
        .preload_env_count = sizeof(env) / sizeof((env)[0]), \
        .capture_scope = RSH_CAPTURE_NONE, \
        .phases = (phase_rows), \
        .phase_count = sizeof(phase_rows) / sizeof((phase_rows)[0]) \
    }

#define NATIVE_MATRIX_RUN(dispatch, env, matrix_rows) \
    { \
        .dispatch_name = (dispatch), \
        .pass_label = (dispatch), \
        .process_kind = RSH_PROCESS_FORK, \
        .outcome = RSH_OUTCOME_SUCCESS, \
        .build_mask = RSH_BUILD_EMBY_LINKED, \
        .preload_env = (env), \
        .preload_env_count = sizeof(env) / sizeof((env)[0]), \
        .capture_scope = RSH_CAPTURE_NONE, \
        .matrix_phases = (matrix_rows), \
        .matrix_phase_count = sizeof(matrix_rows) / sizeof((matrix_rows)[0]) \
    }

#define NATIVE_EXEC_LOG_RUN(dispatch, env, phase_rows, post_close_rows) \
    { \
        .dispatch_name = (dispatch), \
        .pass_label = (dispatch), \
        .process_kind = RSH_PROCESS_EXEC, \
        .outcome = RSH_OUTCOME_SUCCESS, \
        .build_mask = RSH_BUILD_EMBY_LINKED, \
        .preload_env = (env), \
        .preload_env_count = sizeof(env) / sizeof((env)[0]), \
        .capture_scope = RSH_CAPTURE_STDERR, \
        .phases = (phase_rows), \
        .phase_count = sizeof(phase_rows) / sizeof((phase_rows)[0]), \
        .post_close_cases = (post_close_rows), \
        .post_close_case_count = \
            sizeof(post_close_rows) / sizeof((post_close_rows)[0]) \
    }

static const rsh_run_spec emby_runs[] = {
#ifdef EMBY_FTS_REWRITE_DIRECT_TEST
    NATIVE_DIRECT_RUN(
        "direct-test-api", emby_fts_zero_env, emby_direct_test_api_phases
    ),
    NATIVE_DIRECT_RUN(
        "direct-fail-open-episodes", emby_latest_native_env,
        emby_direct_fail_open_episodes_phases
    ),
    NATIVE_DIRECT_RUN(
        "direct-fail-open-movies", emby_latest_native_env,
        emby_direct_fail_open_movies_phases
    ),
    NATIVE_DIRECT_RUN(
        "direct-fail-open-mixed", emby_latest_native_env,
        emby_direct_fail_open_mixed_phases
    ),
#endif
    NATIVE_RUN(
        "positive", emby_fts_default_env, emby_positive_phases
    ),
    NATIVE_RUN("fts-default-on", emby_fts_default_env, emby_fts_default_phases),
    NATIVE_RUN("fts-zero-enables", emby_fts_zero_env, emby_fts_zero_phases),
    NATIVE_RUN("fts-one-disables", emby_fts_one_env, emby_fts_one_phases),
    NATIVE_RUN(
        "fts-garbage-enables", emby_fts_garbage_env,
        emby_fts_garbage_phases
    ),
    NATIVE_RUN(
        "path-negative", emby_fts_zero_env, emby_path_negative_phases
    ),
    NATIVE_RUN("nonmatch", emby_fts_zero_env, emby_nonmatch_phases),
    NATIVE_RUN("collision", emby_fts_zero_env, emby_collision_phases),
    NATIVE_RUN(
        "authorizer-ownership", emby_fts_zero_env,
        emby_authorizer_ownership_phases
    ),
    NATIVE_RUN(
        "fixture-canary", emby_fixture_canary_env,
        emby_fixture_canary_phases
    ),
    NATIVE_RUN(
        "row-parity", emby_fts_zero_env, emby_d6_row_parity_phases
    ),
    NATIVE_RUN(
        "complex-resume-watched-progress-row-parity",
        emby_d6_fanout_zero_env, emby_d6_complex_resume_phases
    ),
    NATIVE_RUN(
        "fanout-default-on", emby_fanout_default_env,
        emby_fanout_default_phases
    ),
    NATIVE_RUN(
        "fanout-zero-enables", emby_fanout_zero_env,
        emby_fanout_zero_phases
    ),
    NATIVE_RUN(
        "fanout-one-disables", emby_fanout_one_env,
        emby_fanout_one_phases
    ),
    NATIVE_RUN(
        "fanout-garbage-enables", emby_fanout_garbage_env,
        emby_fanout_garbage_phases
    ),
    NATIVE_MATRIX_RUN(
        "fanout-matrix", emby_fanout_zero_env, emby_fanout_matrix_phases
    ),
    NATIVE_RUN(
        "links-two-level-row-parity", emby_d6_fanout_zero_env,
        emby_d6_links_two_level_phases
    ),
    NATIVE_MATRIX_RUN(
        "emby-slow-search-matrix", emby_fanout_zero_env,
        emby_slow_search_matrix_phases
    ),
    NATIVE_RUN(
        "dashboard-default-off", emby_dashboard_default_env,
        emby_dashboard_default_phases
    ),
    NATIVE_RUN(
        "dashboard-one-off", emby_dashboard_one_env,
        emby_dashboard_one_phases
    ),
    NATIVE_RUN(
        "dashboard-empty-off", emby_dashboard_empty_env,
        emby_dashboard_empty_phases
    ),
    NATIVE_RUN(
        "dashboard-garbage-off", emby_dashboard_garbage_env,
        emby_dashboard_garbage_phases
    ),
    NATIVE_MATRIX_RUN(
        "dashboard-matrix", emby_latest_native_env,
        emby_dashboard_matrix_phases
    ),
    {
        .dispatch_name = "dashboard-fix-b-c",
        .pass_label = "dashboard-fix-b-c",
        .process_kind = RSH_PROCESS_FORK,
        .outcome = RSH_OUTCOME_SUCCESS,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .preload_env = emby_latest_native_env,
        .preload_env_count = sizeof(emby_latest_native_env) /
                             sizeof(emby_latest_native_env[0]),
        .capture_scope = RSH_CAPTURE_STDERR,
        .phases = emby_dashboard_fix_b_c_identity_phases,
        .phase_count = sizeof(emby_dashboard_fix_b_c_identity_phases) /
                       sizeof(emby_dashboard_fix_b_c_identity_phases[0]),
        .matrix_phases = emby_dashboard_fix_b_c_matrix_phases,
        .matrix_phase_count = sizeof(emby_dashboard_fix_b_c_matrix_phases) /
                              sizeof(emby_dashboard_fix_b_c_matrix_phases[0]),
        .post_close_cases = emby_dashboard_fix_b_c_post_close_cases,
        .post_close_case_count =
            sizeof(emby_dashboard_fix_b_c_post_close_cases) /
            sizeof(emby_dashboard_fix_b_c_post_close_cases[0])
    },
    {
        .dispatch_name = "dashboard-mixed-latest",
        .pass_label = "dashboard-mixed-latest-real-path",
        .process_kind = RSH_PROCESS_FORK,
        .outcome = RSH_OUTCOME_SUCCESS,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .preload_env = emby_latest_native_env,
        .preload_env_count = sizeof(emby_latest_native_env) /
                             sizeof(emby_latest_native_env[0]),
        .capture_scope = RSH_CAPTURE_NONE,
        .matrix_phases = emby_dashboard_mixed_latest_matrix_phases,
        .matrix_phase_count =
            sizeof(emby_dashboard_mixed_latest_matrix_phases) /
            sizeof(emby_dashboard_mixed_latest_matrix_phases[0])
    },
    {
        .dispatch_name = "dashboard-mixed-disabled",
        .pass_label = "dashboard-mixed-disabled",
        .process_kind = RSH_PROCESS_FORK,
        .outcome = RSH_OUTCOME_SUCCESS,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .preload_env = emby_dashboard_default_env,
        .preload_env_count = sizeof(emby_dashboard_default_env) /
                             sizeof(emby_dashboard_default_env[0]),
        .capture_scope = RSH_CAPTURE_NONE,
        .matrix_phases = emby_dashboard_mixed_disabled_matrix_phases,
        .matrix_phase_count =
            sizeof(emby_dashboard_mixed_disabled_matrix_phases) /
            sizeof(emby_dashboard_mixed_disabled_matrix_phases[0])
    },
    NATIVE_MATRIX_RUN(
        "dashboard-mixed-identity", emby_latest_native_env,
        emby_d6_mixed_identity_matrix_phases
    ),
    {
        .dispatch_name = "dashboard-matcher-negatives",
        .pass_label = "dashboard-matcher-negatives",
        .process_kind = RSH_PROCESS_FORK,
        .outcome = RSH_OUTCOME_SUCCESS,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .preload_env = emby_dashboard_matcher_negatives_env,
        .preload_env_count =
            sizeof(emby_dashboard_matcher_negatives_env) /
            sizeof(emby_dashboard_matcher_negatives_env[0]),
        .capture_scope = RSH_CAPTURE_STDERR,
        .matrix_phases = emby_dashboard_matcher_negative_phases,
        .matrix_phase_count =
            sizeof(emby_dashboard_matcher_negative_phases) /
            sizeof(emby_dashboard_matcher_negative_phases[0]),
        .post_close_cases = emby_dashboard_matcher_negative_post_close_cases,
        .post_close_case_count =
            sizeof(emby_dashboard_matcher_negative_post_close_cases) /
            sizeof(emby_dashboard_matcher_negative_post_close_cases[0])
    },
    {
        .dispatch_name = "dashboard-release-matrix",
        .pass_label = "dashboard-release-matrix",
        .process_kind = RSH_PROCESS_FORK,
        .outcome = RSH_OUTCOME_SUCCESS,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .preload_env = emby_latest_native_env,
        .preload_env_count = sizeof(emby_latest_native_env) /
                             sizeof(emby_latest_native_env[0]),
        .capture_scope = RSH_CAPTURE_NONE,
        .phases = emby_d7_release_preflight_phases,
        .phase_count = sizeof(emby_d7_release_preflight_phases) /
                       sizeof(emby_d7_release_preflight_phases[0]),
        .matrix_phases = emby_d7_release_matrix_phases,
        .matrix_phase_count = sizeof(emby_d7_release_matrix_phases) /
                              sizeof(emby_d7_release_matrix_phases[0])
    },
    NATIVE_RUN(
        "latest-limit20", emby_latest_native_env,
        emby_latest_limit20_phases
    ),
    NATIVE_RUN(
        "latest-capture-miss-negative", emby_latest_native_env,
        emby_latest_capture_miss_negative_phases
    ),
    NATIVE_RUN(
        "latest-series-browse-negative", emby_latest_native_env,
        emby_latest_series_browse_negative_phases
    ),
    NATIVE_RUN(
        "latest-resume-no-misfire", emby_latest_native_env,
        emby_latest_resume_no_misfire_phases
    ),
    NATIVE_EXEC_LOG_RUN(
        "dashboard-index-definition-gate", emby_d8_index_definition_env,
        emby_d8_index_definition_phases,
        emby_d8_index_definition_post_close_cases
    ),
    NATIVE_EXEC_LOG_RUN(
        "observability-logs", emby_d8_observability_env,
        emby_d8_observability_phases,
        emby_d8_observability_post_close_cases
    ),
    NATIVE_EXEC_LOG_RUN(
        "observability-master-disabled", emby_d8_master_disabled_env,
        emby_d8_master_disabled_phases,
        emby_d8_master_disabled_post_close_cases
    ),
    {
        .dispatch_name = "negative-control-vendor-prepare",
        .pass_label = "negative-control-vendor-prepare",
        .process_kind = RSH_PROCESS_FORK,
        .outcome = RSH_OUTCOME_EXPECT_ABORT,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .preload_env = emby_negative_control_env,
        .preload_env_count = sizeof(emby_negative_control_env) /
                             sizeof(emby_negative_control_env[0]),
        .capture_scope = RSH_CAPTURE_NONE,
        .phases = emby_vendor_prepare_control_phases,
        .phase_count = sizeof(emby_vendor_prepare_control_phases) /
                       sizeof(emby_vendor_prepare_control_phases[0]),
        .abort_expectation = &emby_vendor_prepare_abort_expectation
    },
    {
        .dispatch_name = "negative-control-matcher-miss",
        .pass_label = "negative-control-matcher-miss",
        .process_kind = RSH_PROCESS_FORK,
        .outcome = RSH_OUTCOME_EXPECT_ABORT,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .preload_env = emby_negative_control_env,
        .preload_env_count = sizeof(emby_negative_control_env) /
                             sizeof(emby_negative_control_env[0]),
        .capture_scope = RSH_CAPTURE_NONE,
        .phases = emby_matcher_miss_control_phases,
        .phase_count = sizeof(emby_matcher_miss_control_phases) /
                       sizeof(emby_matcher_miss_control_phases[0]),
        .abort_expectation = &emby_matcher_miss_abort_expectation
    }
};

#undef NATIVE_EXEC_LOG_RUN
#undef NATIVE_MATRIX_RUN
#undef NATIVE_DIRECT_RUN
#undef NATIVE_RUN

int main(int argc, char **argv) {
#ifdef EMBY_FTS_REWRITE_DIRECT_TEST
    const uint32_t active_build = RSH_BUILD_EMBY_DIRECT;
#else
    const uint32_t active_build = RSH_BUILD_EMBY_LINKED;
#endif

    if (argc == 3 && strcmp(argv[1], "--child") == 0) {
        return rsh_run_child(
            &emby_suite_spec, emby_runs,
            sizeof(emby_runs) / sizeof(emby_runs[0]), active_build, argv[2]
        );
    }
    return rsh_run_all(
        &emby_suite_spec, emby_runs,
        sizeof(emby_runs) / sizeof(emby_runs[0]), argv[0], active_build
    );
}
