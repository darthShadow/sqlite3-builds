#include "emby_fts_rewrite.h"
#include "fts_lex.h"
#include "observability.h"

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define EMBY_DB_BASENAME "library.db"
#define EMBY_SCALAR_NAME "dshadow_emby_fts_rewrite"
#define EMBY_SCALAR_OPEN "dshadow_emby_fts_rewrite("
#define EMBY_SCALAR_OPEN_LEN (sizeof(EMBY_SCALAR_OPEN) - 1)
#define EMBY_SCALAR_CLOSE_LEN 1
#define EMBY_SLOT_CAP (64u * 1024u)
#define EMBY_SCALAR_INPUT_CAP (64u * 1024u)
#define EMBY_CLIENTDATA_KEY "sqlite3-builds-emby-fts-rewrite"
#define EMBY_OWNER_PROBE "__dshadow_emby_owner_probe__"
#define EMBY_LATEST_INDEX_NAME "idx_dshadow_emby_latest_gk_dc"
#define EMBY_LATEST_MOVIES_INDEX_NAME "idx_dshadow_emby_latest_movies_dcn_puk"
#define EMBY_LATEST_MOVIES_PUK_DC_INDEX_NAME "idx_dshadow_emby_latest_movies_puk_dc_cover"
typedef struct emby_span {
    size_t start;
    size_t end;
} emby_span;

typedef struct emby_slots {
    emby_span l1;
    emby_span t1;
    emby_span t2;
    emby_span l2;
    emby_span membership;
} emby_slots;

__attribute__((visibility("hidden"))) SQLITE_API int sqlite3_prepare_v2_real(
    sqlite3 *db,
    const char *zSql,
    int nByte,
    sqlite3_stmt **ppStmt,
    const char **pzTail
);
static pthread_once_t g_emby_rewrite_once = PTHREAD_ONCE_INIT;
static atomic_int g_emby_rewrite_enabled;
static pthread_once_t g_emby_fanout_once = PTHREAD_ONCE_INIT;
static atomic_int g_emby_fanout_enabled;
static pthread_once_t g_emby_dashboard_once = PTHREAD_ONCE_INIT;
static atomic_int g_emby_dashboard_enabled;
static char g_emby_scalar_owner_sentinel;
static char g_emby_scalar_ready_sentinel;
static char g_emby_scalar_bypass_sentinel;
static _Thread_local int g_emby_owner_canary_seen;

#ifdef EMBY_FTS_REWRITE_TEST_API
static atomic_int g_emby_scalar_calls;

__attribute__((visibility("hidden"))) int emby_fts_rewrite_test_scalar_calls(void) {
    return atomic_load_explicit(&g_emby_scalar_calls, memory_order_acquire);
}

__attribute__((visibility("hidden"))) void emby_fts_rewrite_test_reset_scalar_calls(void) {
    atomic_store_explicit(&g_emby_scalar_calls, 0, memory_order_release);
}
#endif

static void emby_fts_rewrite_init_once(void) {
    const char *value = getenv("SQLITE3_DISABLE_EMBY_FTS_REWRITE");
    /* Opt-out: literal "1" disables; unset, "0", and every other value enable. */
    atomic_store_explicit(
        &g_emby_rewrite_enabled,
        (value && strcmp(value, "1") == 0) ? 0 : 1,
        memory_order_release
    );
}

static int emby_rewrite_enabled(void) {
    pthread_once(&g_emby_rewrite_once, emby_fts_rewrite_init_once);
    return atomic_load_explicit(&g_emby_rewrite_enabled, memory_order_acquire);
}

static void emby_fanout_rewrite_init_once(void) {
    const char *value = getenv("SQLITE3_DISABLE_EMBY_FANOUT_REWRITE");
    /* Opt-out: literal "1" disables; unset, "0", and every other value enable. */
    atomic_store_explicit(
        &g_emby_fanout_enabled,
        (value && strcmp(value, "1") == 0) ? 0 : 1,
        memory_order_release
    );
}

static int emby_fanout_enabled(void) {
    pthread_once(&g_emby_fanout_once, emby_fanout_rewrite_init_once);
    return atomic_load_explicit(&g_emby_fanout_enabled, memory_order_acquire);
}

static void emby_dashboard_rewrite_init_once(void) {
    const char *value = getenv("SQLITE3_DISABLE_EMBY_DASHBOARD_REWRITE");
    atomic_store_explicit(
        &g_emby_dashboard_enabled,
        (value && strcmp(value, "0") == 0) ? 1 : 0,
        memory_order_release
    );
}

static int emby_dashboard_enabled(void) {
    pthread_once(&g_emby_dashboard_once, emby_dashboard_rewrite_init_once);
    return atomic_load_explicit(&g_emby_dashboard_enabled, memory_order_acquire);
}

static int call_downstream_prepare(
    fts_rewrite_prepare_kind kind,
    sqlite3 *db,
    const char *zSql,
    int nByte,
    unsigned int prepFlags,
    sqlite3_stmt **ppStmt,
    const char **pzTail
) {
    return plex_fts_rewrite_prepare(db, zSql, nByte, prepFlags, ppStmt, pzTail, kind);
}

static int emby_prepare_input_lengths(
    const char *zSql,
    int nByte,
    size_t *bounded_len,
    size_t *scan_len
) {
    if (nByte < 0) {
        size_t n = strlen(zSql);
        if (n > SIZE_MAX - EMBY_SCALAR_OPEN_LEN - EMBY_SCALAR_CLOSE_LEN - 1) return 0;
        *bounded_len = n;
        *scan_len = n;
        return 1;
    }

    if (nByte == 0) return 0;
    *bounded_len = (size_t)nByte;
    *scan_len = *bounded_len;
    if (*scan_len > 0 && zSql[*scan_len - 1] == 0) {
        (*scan_len)--;
    }
    if (memchr(zSql, 0, *scan_len) != NULL) return 0;
    return 1;
}

static int validate_single_statement_and_match(
    const char *zSql,
    size_t len,
    emby_span *rhs_span
) {
    fts_lex lx;
    int depth = 0;
    int matches = 0;

    fts_lex_init(&lx, zSql, len, FTS_LEX_PARAM_NUMBERED_OR_NAMED);
    rhs_span->start = 0;
    rhs_span->end = 0;

    for (;;) {
        fts_lex_token tok = fts_lex_next_token(&lx);
        if (tok.type == FTS_LEX_TOK_ERROR) return 0;
        if (tok.type == FTS_LEX_TOK_EOF) break;
        if (tok.type == FTS_LEX_TOK_SYMBOL && tok.symbol == ';') return 0;
        if (tok.type == FTS_LEX_TOK_SYMBOL && tok.symbol == '(') {
            depth++;
            continue;
        }
        if (tok.type == FTS_LEX_TOK_SYMBOL && tok.symbol == ')') {
            if (depth == 0) return 0;
            depth--;
            continue;
        }
        if (tok.type == FTS_LEX_TOK_IDENT && fts_lex_token_text_eq(zSql, &tok, "fts_search9")) {
            fts_lex look = lx;
            fts_lex_token match = fts_lex_next_token(&look);
            fts_lex_token rhs;
            if (match.type == FTS_LEX_TOK_IDENT && fts_lex_token_text_eq(zSql, &match, "match")) {
                rhs = fts_lex_next_token(&look);
                if (rhs.type == FTS_LEX_TOK_PARAM) {
                    if (!fts_lex_match_rhs_is_complete(&look, &rhs)) return 0;
                    matches++;
                    rhs_span->start = rhs.start;
                    rhs_span->end = rhs.end;
#ifdef EMBY_FTS_REWRITE_TEST_LITERAL_MATCH
                } else if (rhs.type == FTS_LEX_TOK_STRING) {
                    if (!fts_lex_match_rhs_is_complete(&look, &rhs)) return 0;
                    matches++;
                    rhs_span->start = rhs.start;
                    rhs_span->end = rhs.end;
#endif
                } else {
                    return 0;
                }
            }
        }
    }
    if (depth != 0) return 0;
    return matches == 1;
}

static int count_tokens_after(
    const char *sql,
    size_t len,
    size_t start,
    const char *needle,
    emby_span *out
) {
    size_t needle_len = strlen(needle);
    fts_lex lx;
    int found = 0;

    if (needle_len == 0 || start > len || needle_len > len) return -1;
    fts_lex_init(&lx, sql, len, FTS_LEX_PARAM_NUMBERED_OR_NAMED);

    for (;;) {
        fts_lex_token tok = fts_lex_next_token(&lx);
        if (tok.type == FTS_LEX_TOK_ERROR) return -1;
        if (tok.type == FTS_LEX_TOK_EOF) break;
        if (tok.start < start || tok.start > len - needle_len) continue;
        if (memcmp(sql + tok.start, needle, needle_len) != 0) continue;
        if (found == 0) {
            out->start = tok.start;
            out->end = tok.start + needle_len;
        }
        found++;
    }
    return found;
}

static int find_unique_token_after(
    const char *sql,
    size_t len,
    size_t start,
    const char *needle,
    emby_span *out
) {
    return count_tokens_after(sql, len, start, needle, out) == 1;
}

#ifdef EMBY_FTS_REWRITE_TEST_API
__attribute__((visibility("hidden"))) int emby_fts_rewrite_test_duplicate_anchor_guard(void) {
    static const char sql[] = "select 1 select 2";
    emby_span out;
    return find_unique_token_after(sql, sizeof(sql) - 1, 0, "select", &out);
}

__attribute__((visibility("hidden"))) int emby_fts_rewrite_test_string_anchor_immunity(void) {
    static const char sql[] = "select 'select'";
    emby_span out;
    if (!find_unique_token_after(sql, sizeof(sql) - 1, 0, "select", &out)) return 0;
    return out.start == 0 && out.end == 6;
}
#endif

static int add_size(size_t *acc, size_t v);
static void append_bytes(char **p, const char *z, size_t n);
static void append_slot(char **p, const char *sql, const emby_span *slot);

static int validate_numeric_slot(const char *sql, const emby_span *slot) {
    size_t i;
    int saw_digit = 0;
    if (slot->end <= slot->start) return 0;
    if (slot->end - slot->start > EMBY_SLOT_CAP) return 0;
    for (i = slot->start; i < slot->end; i++) {
        unsigned char c = (unsigned char)sql[i];
        if (c >= '0' && c <= '9') {
            saw_digit = 1;
            continue;
        }
        if (c == ',' || fts_lex_is_space(c)) continue;
        return 0;
    }
    return saw_digit;
}

static int emby_sql_has_bytes(const char *sql, size_t len, const char *needle) {
    size_t needle_len = strlen(needle);
    size_t i;
    if (needle_len == 0 || needle_len > len) return 0;
    for (i = 0; i <= len - needle_len; i++) {
        if (memcmp(sql + i, needle, needle_len) == 0) return 1;
    }
    return 0;
}

static int emby_validate_single_statement(const char *zSql, size_t len) {
    fts_lex lx;
    int depth = 0;

    fts_lex_init(&lx, zSql, len, FTS_LEX_PARAM_NUMBERED_OR_NAMED);
    for (;;) {
        fts_lex_token tok = fts_lex_next_token(&lx);
        if (tok.type == FTS_LEX_TOK_ERROR) return 0;
        if (tok.type == FTS_LEX_TOK_EOF) break;
        if (tok.type == FTS_LEX_TOK_SYMBOL && tok.symbol == ';') return 0;
        if (tok.type == FTS_LEX_TOK_SYMBOL && tok.symbol == '(') {
            depth++;
            continue;
        }
        if (tok.type == FTS_LEX_TOK_SYMBOL && tok.symbol == ')') {
            if (depth == 0) return 0;
            depth--;
            continue;
        }
    }
    return depth == 0;
}

static int emby_statement_has_bind_parameter(const char *zSql, size_t len) {
    fts_lex lx;

    fts_lex_init(&lx, zSql, len, FTS_LEX_PARAM_SQLITE_VARIABLE);
    for (;;) {
        fts_lex_token tok = fts_lex_next_token(&lx);
        if (tok.type == FTS_LEX_TOK_ERROR) return 1;
        if (tok.type == FTS_LEX_TOK_PARAM) return 1;
        if (tok.type == FTS_LEX_TOK_EOF) return 0;
    }
}

static int emby_token_present(const char *sql, size_t len, const char *word) {
    fts_lex lx;

    fts_lex_init(&lx, sql, len, FTS_LEX_PARAM_NUMBERED_OR_NAMED);
    for (;;) {
        fts_lex_token tok = fts_lex_next_token(&lx);
        if (tok.type == FTS_LEX_TOK_ERROR) return 0;
        if (tok.type == FTS_LEX_TOK_EOF) return 0;
        if (tok.type == FTS_LEX_TOK_IDENT && fts_lex_token_text_eq(sql, &tok, word)) return 1;
    }
}

static void emby_span_rtrim(const char *sql, emby_span *span) {
    while (span->end > span->start && fts_lex_is_space((unsigned char)sql[span->end - 1])) {
        span->end--;
    }
}

static int emby_validate_integer_slot(const char *sql, const emby_span *slot) {
    size_t i;
    if (slot->end <= slot->start) return 0;
    if (slot->end - slot->start > EMBY_SLOT_CAP) return 0;
    for (i = slot->start; i < slot->end; i++) {
        if (sql[i] < '0' || sql[i] > '9') return 0;
    }
    return 1;
}

static int emby_latest_limit_supported(const char *sql, const emby_span *slot) {
    size_t n = slot->end - slot->start;
    return (n == 2 && memcmp(sql + slot->start, "12", 2) == 0) ||
           (n == 2 && memcmp(sql + slot->start, "16", 2) == 0) ||
           (n == 2 && memcmp(sql + slot->start, "20", 2) == 0);
}

typedef struct emby_piece {
    const char *lit;
    const emby_span *slot;
} emby_piece;

static char *emby_build_pieces(
    const char *sql,
    const emby_piece *pieces,
    size_t n,
    size_t *out_len
) {
    size_t i;
    size_t len = 0;
    char *buf;
    char *p;

    for (i = 0; i < n; i++) {
        if (pieces[i].lit && !add_size(&len, strlen(pieces[i].lit))) return NULL;
        if (pieces[i].slot &&
            !add_size(&len, pieces[i].slot->end - pieces[i].slot->start)) {
            return NULL;
        }
    }
    buf = (char *)malloc(len + 1);
    if (!buf) return NULL;
    p = buf;
    for (i = 0; i < n; i++) {
        if (pieces[i].lit) append_bytes(&p, pieces[i].lit, strlen(pieces[i].lit));
        if (pieces[i].slot) append_slot(&p, sql, pieces[i].slot);
    }
    *p = 0;
    *out_len = len;
    return buf;
}

static char *emby_splice_span(
    sqlite3 *db,
    const char *zSql,
    size_t bounded_len,
    size_t scan_len,
    emby_span span,
    const char *repl,
    size_t repl_len,
    size_t *scan_out_len,
    size_t *rewrite_len
) {
    size_t old_len;
    size_t out_len;
    size_t limit;
    char *buf;
    char *p;

    if (span.start > span.end || span.end > scan_len) return NULL;
    old_len = span.end - span.start;
    if (bounded_len < old_len) return NULL;
    out_len = bounded_len - old_len;
    if (!add_size(&out_len, repl_len)) return NULL;
    limit = (size_t)sqlite3_limit(db, SQLITE_LIMIT_SQL_LENGTH, -1);
    if (out_len > limit || out_len > (size_t)INT_MAX) return NULL;
    buf = (char *)malloc(out_len + 1);
    if (!buf) return NULL;
    p = buf;
    append_bytes(&p, zSql, span.start);
    append_bytes(&p, repl, repl_len);
    append_bytes(&p, zSql + span.end, bounded_len - span.end);
    buf[out_len] = 0;
    *scan_out_len = scan_len - old_len + repl_len;
    *rewrite_len = out_len;
    return buf;
}

typedef enum emby_latest_index_state {
    EMBY_LATEST_INDEX_PROBE_ERROR = -1,
    EMBY_LATEST_INDEX_MISSING = 0,
    EMBY_LATEST_INDEX_PRESENT = 1
} emby_latest_index_state;

static emby_latest_index_state emby_latest_index_ready(sqlite3 *db) {
    static const char probe_sql[] =
        "SELECT 1 FROM sqlite_master "
        "WHERE type='index' "
        "AND name='" EMBY_LATEST_INDEX_NAME "' "
        "AND tbl_name='MediaItems' COLLATE NOCASE "
        "LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    const char *tail = NULL;
    int rc;
    int step_rc;
    int final_rc;
    int ready = 0;

    rc = sqlite3_prepare_v2_real(db, probe_sql, -1, &stmt, &tail);
    if (rc != SQLITE_OK) {
        if (stmt) sqlite3_finalize(stmt);
        return EMBY_LATEST_INDEX_PROBE_ERROR;
    }
    if (!tail || tail != probe_sql + strlen(probe_sql)) {
        sqlite3_finalize(stmt);
        return EMBY_LATEST_INDEX_PROBE_ERROR;
    }
    step_rc = sqlite3_step(stmt);
    if (step_rc == SQLITE_ROW) {
        ready = 1;
    } else if (step_rc != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return EMBY_LATEST_INDEX_PROBE_ERROR;
    }
    final_rc = sqlite3_finalize(stmt);
    if (final_rc != SQLITE_OK) return EMBY_LATEST_INDEX_PROBE_ERROR;
    return ready ? EMBY_LATEST_INDEX_PRESENT : EMBY_LATEST_INDEX_MISSING;
}

static emby_latest_index_state emby_latest_movies_indexes_ready(sqlite3 *db) {
    static const char probe_sql[] =
        "SELECT 1 WHERE "
        "EXISTS (SELECT 1 FROM sqlite_master "
        "WHERE type='index' "
        "AND name='" EMBY_LATEST_MOVIES_INDEX_NAME "' "
        "AND tbl_name='MediaItems' COLLATE NOCASE) "
        "AND EXISTS (SELECT 1 FROM sqlite_master "
        "WHERE type='index' "
        "AND name='" EMBY_LATEST_MOVIES_PUK_DC_INDEX_NAME "' "
        "AND tbl_name='MediaItems' COLLATE NOCASE)";
    sqlite3_stmt *stmt = NULL;
    const char *tail = NULL;
    int rc;
    int step_rc;
    int final_rc;
    int ready = 0;

    rc = sqlite3_prepare_v2_real(db, probe_sql, -1, &stmt, &tail);
    if (rc != SQLITE_OK) {
        if (stmt) sqlite3_finalize(stmt);
        return EMBY_LATEST_INDEX_PROBE_ERROR;
    }
    if (!tail || tail != probe_sql + strlen(probe_sql)) {
        sqlite3_finalize(stmt);
        return EMBY_LATEST_INDEX_PROBE_ERROR;
    }
    step_rc = sqlite3_step(stmt);
    if (step_rc == SQLITE_ROW) {
        ready = 1;
    } else if (step_rc != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return EMBY_LATEST_INDEX_PROBE_ERROR;
    }
    final_rc = sqlite3_finalize(stmt);
    if (final_rc != SQLITE_OK) return EMBY_LATEST_INDEX_PROBE_ERROR;
    return ready ? EMBY_LATEST_INDEX_PRESENT : EMBY_LATEST_INDEX_MISSING;
}

static int capture_membership_slots(const char *sql, size_t len, emby_slots *slots) {
    static const char pre_l1[] =
        "WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (";
    static const char after_l1[] =
        ") ),WithItemLinkItemIds AS (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (";
    static const char after_t1[] =
        ") union select ItemLinks2TwoLevel.LinkedId from ItemLinks2 ItemLinks2TwoLevel where itemid in (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (";
    static const char after_t2[] =
        ")))select";
    static const char pre_l2[] =
        "(A.Id in WithAncestors OR A.Id in (select ListItemsExemptionForPlaylists.ListId from ListItems ListItemsExemptionForPlaylists join ancestorids2 AncestorIdExemptionForPlaylists on ListItemsExemptionForPlaylists.ListItemId=AncestorIdExemptionForPlaylists.itemid and AncestorIdExemptionForPlaylists.AncestorId in (";
    static const char after_l2[] =
        ")) OR A.Id in (select PersonId from itemPeople2 where ItemId in WithAncestors) OR A.Id in WithItemLinkItemIds)";
    emby_span a;
    emby_span b;
    emby_span c;
    emby_span d;
    emby_span e;
    emby_span f;

    memset(slots, 0, sizeof(*slots));
    if (!find_unique_token_after(sql, len, 0, pre_l1, &a)) return 0;
    if (!find_unique_token_after(sql, len, a.end, after_l1, &b)) return 0;
    if (!find_unique_token_after(sql, len, b.end, after_t1, &c)) return 0;
    if (!find_unique_token_after(sql, len, c.end, after_t2, &d)) return 0;
    if (!find_unique_token_after(sql, len, d.end, pre_l2, &e)) return 0;
    if (!find_unique_token_after(sql, len, e.end, after_l2, &f)) return 0;

    slots->l1.start = a.end;
    slots->l1.end = b.start;
    slots->t1.start = b.end;
    slots->t1.end = c.start;
    slots->t2.start = c.end;
    slots->t2.end = d.start;
    slots->l2.start = e.end;
    slots->l2.end = f.start;
    slots->membership.start = e.start;
    slots->membership.end = f.end;

    if (!validate_numeric_slot(sql, &slots->l1)) return 0;
    if (!validate_numeric_slot(sql, &slots->t1)) return 0;
    if (!validate_numeric_slot(sql, &slots->t2)) return 0;
    if (!validate_numeric_slot(sql, &slots->l2)) return 0;
    return 1;
}

static int add_size(size_t *acc, size_t v) {
    if (*acc > SIZE_MAX - v) return 0;
    *acc += v;
    return 1;
}

typedef enum emby_slot_ref {
    EMBY_SLOT_L1 = 0,
    EMBY_SLOT_L2,
    EMBY_SLOT_T1,
    EMBY_SLOT_T2
} emby_slot_ref;

static const char *const EMBY_EXISTS_PARTS[] = {
    "(EXISTS (SELECT 1 FROM AncestorIds2 WHERE AncestorIds2.itemid = A.Id AND AncestorIds2.AncestorId in (",
    ")) OR  exists (select 1 from ListItems join ancestorids2 on ListItems.ListItemId=ancestorids2.itemid and ancestorids2.AncestorId in (",
    ") where ListItems.ListId=A.Id) OR EXISTS (SELECT 1 FROM itemPeople2 JOIN AncestorIds2 ON AncestorIds2.itemid = itemPeople2.ItemId WHERE itemPeople2.PersonId = A.Id and AncestorIds2.AncestorId in (",
    ")) OR Exists (select 1 From ItemLinks2 join ancestorids2 on ancestorids2.itemid=itemlinks2.itemid and ancestorids2.ancestorid in (",
    ")  where ItemLinks2.LinkedId = A.Id AND ItemLinks2.Type in (",
    ")) OR Exists (select 1 from ItemLinks2 ItemLinks2TwoLevel where exists (select 1 From ItemLinks2 join ancestorids2 on ancestorids2.itemid=itemlinks2.itemid and ancestorids2.ancestorid in (",
    ")  where itemlinks2.linkedid = itemlinks2twolevel.itemid AND ItemLinks2.Type in (",
    ")) and ItemLinks2TwoLevel.LinkedId=A.Id))"
};

static const emby_slot_ref EMBY_EXISTS_SLOT_ORDER[] = {
    EMBY_SLOT_L1,
    EMBY_SLOT_L2,
    EMBY_SLOT_L1,
    EMBY_SLOT_L1,
    EMBY_SLOT_T1,
    EMBY_SLOT_L1,
    EMBY_SLOT_T2
};

static const emby_span *slot_by_ref(const emby_slots *slots, emby_slot_ref ref) {
    switch (ref) {
        case EMBY_SLOT_L1:
            return &slots->l1;
        case EMBY_SLOT_L2:
            return &slots->l2;
        case EMBY_SLOT_T1:
            return &slots->t1;
        case EMBY_SLOT_T2:
            return &slots->t2;
        default:
            return &slots->l1;
    }
}

static void append_bytes(char **p, const char *z, size_t n) {
    if (n) {
        memcpy(*p, z, n);
        *p += n;
    }
}

static void append_slot(char **p, const char *sql, const emby_span *slot) {
    append_bytes(p, sql + slot->start, slot->end - slot->start);
}

static size_t exists_arms_static_len(void) {
    size_t i;
    size_t n = 0;
    for (i = 0; i < sizeof(EMBY_EXISTS_PARTS) / sizeof(EMBY_EXISTS_PARTS[0]); i++) {
        n += strlen(EMBY_EXISTS_PARTS[i]);
    }
    return n;
}

static char *build_exists_arms(const char *sql, const emby_slots *slots, size_t *out_len) {
    size_t n = exists_arms_static_len();
    char *buf;
    char *p;
    size_t i;

    for (i = 0; i < sizeof(EMBY_EXISTS_SLOT_ORDER) / sizeof(EMBY_EXISTS_SLOT_ORDER[0]); i++) {
        const emby_span *slot = slot_by_ref(slots, EMBY_EXISTS_SLOT_ORDER[i]);
        if (!add_size(&n, slot->end - slot->start)) return NULL;
    }
    buf = (char *)malloc(n + 1);
    if (!buf) return NULL;
    p = buf;
    for (i = 0; i < sizeof(EMBY_EXISTS_SLOT_ORDER) / sizeof(EMBY_EXISTS_SLOT_ORDER[0]); i++) {
        append_bytes(&p, EMBY_EXISTS_PARTS[i], strlen(EMBY_EXISTS_PARTS[i]));
        append_slot(&p, sql, slot_by_ref(slots, EMBY_EXISTS_SLOT_ORDER[i]));
    }
    append_bytes(&p, EMBY_EXISTS_PARTS[i], strlen(EMBY_EXISTS_PARTS[i]));
    *p = 0;
    *out_len = (size_t)(p - buf);
    return buf;
}

typedef enum emby_exists_arm_id {
    EMBY_EXISTS_ARM_ANCESTOR = 0,
    EMBY_EXISTS_ARM_PEOPLE,
    EMBY_EXISTS_ARM_LINKS_ONE_LEVEL,
    EMBY_EXISTS_ARM_LINKS_TWO_LEVEL
} emby_exists_arm_id;

static const char EMBY_EXISTS_ANCESTOR_0[] =
    "EXISTS (SELECT 1 FROM AncestorIds2 WHERE AncestorIds2.itemid = A.Id AND AncestorIds2.AncestorId in (";
static const char EMBY_EXISTS_ANCESTOR_1[] = "))";

static const char EMBY_EXISTS_PEOPLE_0[] =
    "EXISTS (SELECT 1 FROM itemPeople2 JOIN AncestorIds2 ON AncestorIds2.itemid = itemPeople2.ItemId WHERE itemPeople2.PersonId = A.Id and AncestorIds2.AncestorId in (";
static const char EMBY_EXISTS_PEOPLE_1[] = "))";

static const char EMBY_EXISTS_LINKS_ONE_0[] =
    "Exists (select 1 From ItemLinks2 join ancestorids2 on ancestorids2.itemid=itemlinks2.itemid and ancestorids2.ancestorid in (";
static const char EMBY_EXISTS_LINKS_ONE_1[] =
    ")  where ItemLinks2.LinkedId = A.Id AND ItemLinks2.Type in (";
static const char EMBY_EXISTS_LINKS_ONE_2[] = "))";

static const char EMBY_EXISTS_LINKS_TWO_0[] =
    "Exists (select 1 from ItemLinks2 ItemLinks2TwoLevel where exists (select 1 From ItemLinks2 join ancestorids2 on ancestorids2.itemid=itemlinks2.itemid and ancestorids2.ancestorid in (";
static const char EMBY_EXISTS_LINKS_TWO_1[] =
    ")  where itemlinks2.linkedid = itemlinks2twolevel.itemid AND ItemLinks2.Type in (";
static const char EMBY_EXISTS_LINKS_TWO_2[] =
    ")) and ItemLinks2TwoLevel.LinkedId=A.Id)";

static const char EMBY_EXISTS_JOIN[] = " OR ";

static int exists_arm_len(
    const emby_slots *slots,
    emby_exists_arm_id arm,
    size_t *n
) {
    switch (arm) {
        case EMBY_EXISTS_ARM_ANCESTOR:
            return add_size(n, strlen(EMBY_EXISTS_ANCESTOR_0)) &&
                   add_size(n, slots->l1.end - slots->l1.start) &&
                   add_size(n, strlen(EMBY_EXISTS_ANCESTOR_1));
        case EMBY_EXISTS_ARM_PEOPLE:
            return add_size(n, strlen(EMBY_EXISTS_PEOPLE_0)) &&
                   add_size(n, slots->l1.end - slots->l1.start) &&
                   add_size(n, strlen(EMBY_EXISTS_PEOPLE_1));
        case EMBY_EXISTS_ARM_LINKS_ONE_LEVEL:
            return add_size(n, strlen(EMBY_EXISTS_LINKS_ONE_0)) &&
                   add_size(n, slots->l1.end - slots->l1.start) &&
                   add_size(n, strlen(EMBY_EXISTS_LINKS_ONE_1)) &&
                   add_size(n, slots->t1.end - slots->t1.start) &&
                   add_size(n, strlen(EMBY_EXISTS_LINKS_ONE_2));
        case EMBY_EXISTS_ARM_LINKS_TWO_LEVEL:
            return add_size(n, strlen(EMBY_EXISTS_LINKS_TWO_0)) &&
                   add_size(n, slots->l1.end - slots->l1.start) &&
                   add_size(n, strlen(EMBY_EXISTS_LINKS_TWO_1)) &&
                   add_size(n, slots->t2.end - slots->t2.start) &&
                   add_size(n, strlen(EMBY_EXISTS_LINKS_TWO_2));
        default:
            return 0;
    }
}

static int append_exists_arm(
    char **p,
    const char *sql,
    const emby_slots *slots,
    emby_exists_arm_id arm
) {
    switch (arm) {
        case EMBY_EXISTS_ARM_ANCESTOR:
            append_bytes(p, EMBY_EXISTS_ANCESTOR_0, strlen(EMBY_EXISTS_ANCESTOR_0));
            append_slot(p, sql, &slots->l1);
            append_bytes(p, EMBY_EXISTS_ANCESTOR_1, strlen(EMBY_EXISTS_ANCESTOR_1));
            return 1;
        case EMBY_EXISTS_ARM_PEOPLE:
            append_bytes(p, EMBY_EXISTS_PEOPLE_0, strlen(EMBY_EXISTS_PEOPLE_0));
            append_slot(p, sql, &slots->l1);
            append_bytes(p, EMBY_EXISTS_PEOPLE_1, strlen(EMBY_EXISTS_PEOPLE_1));
            return 1;
        case EMBY_EXISTS_ARM_LINKS_ONE_LEVEL:
            append_bytes(p, EMBY_EXISTS_LINKS_ONE_0, strlen(EMBY_EXISTS_LINKS_ONE_0));
            append_slot(p, sql, &slots->l1);
            append_bytes(p, EMBY_EXISTS_LINKS_ONE_1, strlen(EMBY_EXISTS_LINKS_ONE_1));
            append_slot(p, sql, &slots->t1);
            append_bytes(p, EMBY_EXISTS_LINKS_ONE_2, strlen(EMBY_EXISTS_LINKS_ONE_2));
            return 1;
        case EMBY_EXISTS_ARM_LINKS_TWO_LEVEL:
            append_bytes(p, EMBY_EXISTS_LINKS_TWO_0, strlen(EMBY_EXISTS_LINKS_TWO_0));
            append_slot(p, sql, &slots->l1);
            append_bytes(p, EMBY_EXISTS_LINKS_TWO_1, strlen(EMBY_EXISTS_LINKS_TWO_1));
            append_slot(p, sql, &slots->t2);
            append_bytes(p, EMBY_EXISTS_LINKS_TWO_2, strlen(EMBY_EXISTS_LINKS_TWO_2));
            return 1;
        default:
            return 0;
    }
}

static char *build_exists_arms_for_family(
    const char *sql,
    const emby_slots *slots,
    const emby_exists_arm_id *arms,
    size_t arm_count,
    int wrap_outer,
    size_t *out_len
) {
    size_t n = 0;
    size_t i;
    char *buf;
    char *p;

    if (arm_count == 0) return NULL;
    if (wrap_outer && !add_size(&n, 2)) return NULL;
    for (i = 0; i < arm_count; i++) {
        if (i > 0 && !add_size(&n, strlen(EMBY_EXISTS_JOIN))) return NULL;
        if (!exists_arm_len(slots, arms[i], &n)) return NULL;
    }
    buf = (char *)malloc(n + 1);
    if (!buf) return NULL;
    p = buf;
    if (wrap_outer) *p++ = '(';
    for (i = 0; i < arm_count; i++) {
        if (i > 0) append_bytes(&p, EMBY_EXISTS_JOIN, strlen(EMBY_EXISTS_JOIN));
        if (!append_exists_arm(&p, sql, slots, arms[i])) {
            free(buf);
            return NULL;
        }
    }
    if (wrap_outer) *p++ = ')';
    *p = 0;
    *out_len = (size_t)(p - buf);
    return buf;
}

static const char EMBY_ANCHOR_PRE_L1[] =
    "WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (";
static const char EMBY_ANCHOR_PRE_L1_SCALAR[] =
    "WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId=";
static const char EMBY_ANCHOR_LINKS_CTE_IN[] =
    ") ),WithItemLinkItemIds AS (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (";
static const char EMBY_ANCHOR_LINKS_CTE_EQ[] =
    ") ),WithItemLinkItemIds AS (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type=";
static const char EMBY_ANCHOR_TWOLEVEL_BARE[] =
    "union select ItemLinks2TwoLevel.LinkedId from ItemLinks2 ItemLinks2TwoLevel where itemid in (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (";
static const char EMBY_ANCHOR_CTE_END3[] = ")))select";
static const char EMBY_ANCHOR_CTE_END2[] = "))select";
static const char EMBY_ANCHOR_SELECT_AFTER_L1[] = ") )select";
static const char EMBY_ANCHOR_SIMILAR_AFTER_L1[] = ") ),SimB_Ids AS (";
static const char EMBY_MEMBER_LINKS[] = "A.Id in WithItemLinkItemIds";
static const char EMBY_MEMBER_ANCESTORS[] = "A.Id in WithAncestors";
static const char EMBY_MEMBER_FAVORITES[] =
    "(A.Id in WithAncestors OR A.Id in WithItemLinkItemIds)";
static const char EMBY_MEMBER_PEOPLE[] =
    "A.Id in (select PersonId from itemPeople2 where ItemId in WithAncestors)";
static const char EMBY_ANCHOR_RESUME_GROUP[] = " Group by coalesce(";
static const char EMBY_GUARD_RESUME_LASTWATCHED_NOT_NULL[] =
    "AND LastWatchedEpisodes.LastPlayedDateInt not null";
static const char EMBY_ANCHOR_RESUME_TAIL[] =
    "Group by coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey) ORDER BY COALESCE(lastwatchedepisodes.lastplayeddateint, userdatas.lastplayeddateint, 0) DESC,Min(EpisodeAbsoluteIndexNumber) ASC LIMIT";
static const char EMBY_ANCHOR_RESUME_SIMPLE_TAIL[] =
    "AND A.Id in WithAncestors Group by A.PresentationUniqueKey ORDER BY UserDatas.LastPlayedDateInt DESC LIMIT";
static const char EMBY_ANCHOR_SIMILAR_TAIL[] =
    "Group by A.PresentationUniqueKey ORDER BY SimilarityScore DESC,RANDOM() ASC LIMIT";
static const char EMBY_GUARD_PLAYLISTS[] = "ListItemsExemptionForPlaylists";

static const char EMBY_TYPE_EPISODES_LATEST[] = "where A.Type=8 ";
static const char EMBY_TYPE_MOVIES_LATEST[] = "where A.Type=5 ";
static const char EMBY_ANCHOR_LATEST_SELECT[] = ") )select ";
static const char EMBY_ANCHOR_LATEST_SELECT_SCALAR[] = ")select ";
static const char EMBY_ANCHOR_LATEST_FROM[] =
    "from mediaitems A left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=";
static const char EMBY_ANCHOR_LATEST_TAIL[] =
    "where A.Type=8 AND Coalesce(UserDatas.played, 0)=0 AND A.Id in WithAncestors Group by coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey) ORDER BY MAX(A.DateCreated) DESC LIMIT";
static const char EMBY_ANCHOR_MOVIES_LATEST_TAIL[] =
    "where A.Type=5 AND Coalesce(UserDatas.played, 0)=0 AND A.Id in WithAncestors Group by A.PresentationUniqueKey ORDER BY A.DateCreated DESC LIMIT";
/* Correctness relies on Emby's UserDatas PK uniqueness on (UserDataKeyId, UserId). */
static const char EMBY_LATEST_TPL_0[] =
    "WITH keys(gk) AS MATERIALIZED (SELECT DISTINCT coalesce(A0.SeriesPresentationUniqueKey, A0.PresentationUniqueKey) FROM (SELECT DISTINCT itemid FROM AncestorIds2 WHERE AncestorId IN (";
static const char EMBY_LATEST_TPL_1[] =
    ")) AS W CROSS JOIN MediaItems AS A0 WHERE A0.Id = W.itemid AND A0.Type = 8 AND NOT EXISTS (SELECT 1 FROM UserDatas AS U0 WHERE U0.UserDataKeyId = A0.UserDataKeyId AND U0.UserId = ";
static const char EMBY_LATEST_TPL_2[] =
    " AND U0.played <> 0)), picked AS MATERIALIZED (SELECT K.gk, (SELECT A2.Id FROM MediaItems AS A2 WHERE A2.Type = 8 AND coalesce(A2.SeriesPresentationUniqueKey, A2.PresentationUniqueKey) IS K.gk AND EXISTS (SELECT 1 FROM AncestorIds2 AS X WHERE X.ItemId = A2.Id AND X.AncestorId IN (";
static const char EMBY_LATEST_TPL_3[] =
    ")) AND NOT EXISTS (SELECT 1 FROM UserDatas AS U2 WHERE U2.UserDataKeyId = A2.UserDataKeyId AND U2.UserId = ";
static const char EMBY_LATEST_TPL_4[] =
    " AND U2.played <> 0) ORDER BY A2.DateCreated DESC LIMIT 1) AS id FROM keys AS K), exact_groups AS MATERIALIZED (SELECT P.gk, P.id, Amax.DateCreated AS maxdc FROM picked AS P JOIN MediaItems AS Amax ON Amax.Id = P.id WHERE P.id IS NOT NULL), ranked AS MATERIALIZED (SELECT gk, id, maxdc FROM exact_groups ORDER BY maxdc DESC LIMIT ";
static const char EMBY_LATEST_TPL_5[] = ") SELECT ";
static const char EMBY_LATEST_TPL_6[] =
    " FROM ranked AS R JOIN MediaItems AS A ON A.Id = R.id LEFT JOIN UserDatas ON A.UserDataKeyId = UserDatas.UserDataKeyId AND UserDatas.UserId = ";
static const char EMBY_LATEST_TPL_7[] = " ORDER BY R.maxdc DESC LIMIT ";

/* SQLite leaves the vendor GROUP BY's bare dashboard columns undefined on ties.
   Newest-non-NULL-date/lower-Id ranking picks one row deterministically; different
   nullable values are inherent in the vendor tie, not the rewrite. Keep it
   non-NULL-hardened because consumers must tolerate vendor results. */
static const char EMBY_MOVIES_LATEST_TPL_0[] =
    "WITH ranked(id, dc, puk) AS MATERIALIZED ("
    "  SELECT A.Id, A.DateCreated, A.PresentationUniqueKey "
    "  FROM MediaItems AS A INDEXED BY " EMBY_LATEST_MOVIES_INDEX_NAME " "
    "  WHERE A.Type = 5 AND EXISTS ( SELECT 1 FROM AncestorIds2 AS X WHERE X.ItemId = A.Id AND X.AncestorId IN (";
static const char EMBY_MOVIES_LATEST_TPL_1[] =
    ") ) AND NOT EXISTS ( SELECT 1 FROM UserDatas AS U0 WHERE U0.UserDataKeyId = A.UserDataKeyId AND U0.UserId = ";
static const char EMBY_MOVIES_LATEST_TPL_2[] =
    " AND U0.played <> 0 ) AND NOT EXISTS ("
    "      SELECT 1 "
    "      FROM MediaItems AS B INDEXED BY " EMBY_LATEST_MOVIES_PUK_DC_INDEX_NAME " "
    "      WHERE B.Type = 5 AND B.PresentationUniqueKey IS A.PresentationUniqueKey AND ( (B.DateCreated IS NOT NULL AND A.DateCreated IS NULL) OR B.DateCreated > A.DateCreated OR (B.DateCreated IS A.DateCreated AND B.Id < A.Id) ) AND EXISTS ( SELECT 1 FROM AncestorIds2 AS XB WHERE XB.ItemId = B.Id AND XB.AncestorId IN (";
static const char EMBY_MOVIES_LATEST_TPL_3[] =
    ") ) AND NOT EXISTS ( SELECT 1 FROM UserDatas AS UB WHERE UB.UserDataKeyId = B.UserDataKeyId AND UB.UserId = ";
static const char EMBY_MOVIES_LATEST_TPL_4[] =
    " AND UB.played <> 0 ) ) ORDER BY (A.DateCreated IS NULL) ASC, A.DateCreated DESC, A.PresentationUniqueKey ASC LIMIT ";
static const char EMBY_MOVIES_LATEST_TPL_5[] = " ) SELECT ";
static const char EMBY_MOVIES_LATEST_TPL_6[] =
    "FROM ranked AS R JOIN MediaItems AS A ON A.Id = R.id LEFT JOIN UserDatas ON A.UserDataKeyId = UserDatas.UserDataKeyId AND UserDatas.UserId = ";
static const char EMBY_MOVIES_LATEST_TPL_7[] =
    " ORDER BY (R.dc IS NULL) ASC, R.dc DESC, R.puk ASC LIMIT ";

typedef enum emby_match_result {
    EMBY_MATCH_MISS = 0,
    EMBY_MATCH_BUILT,
    EMBY_MATCH_BUILD_FAILED
} emby_match_result;

typedef struct emby_rewrite_candidate {
    char *sql;
    size_t scan_out_len;
    size_t rewrite_len;
    obs_rewrite_mode mode;
} emby_rewrite_candidate;

static int emby_find_bytes_after(
    const char *sql,
    size_t len,
    size_t start,
    const char *needle,
    emby_span *out
) {
    size_t needle_len = strlen(needle);
    size_t i;
    if (needle_len == 0 || start > len || needle_len > len) return 0;
    for (i = start; i <= len - needle_len; i++) {
        if (memcmp(sql + i, needle, needle_len) == 0) {
            out->start = i;
            out->end = i + needle_len;
            return 1;
        }
    }
    return 0;
}

static int emby_find_unique_bytes_between(
    const char *sql,
    size_t start,
    size_t end,
    const char *needle,
    emby_span *out
) {
    size_t needle_len = strlen(needle);
    size_t i;
    int found = 0;
    if (needle_len == 0 || start > end || needle_len > end - start) return 0;
    for (i = start; i <= end - needle_len; i++) {
        if (memcmp(sql + i, needle, needle_len) != 0) continue;
        if (found) return 0;
        out->start = i;
        out->end = i + needle_len;
        found = 1;
    }
    return found;
}

static int emby_find_unique_token_between(
    const char *sql,
    size_t len,
    size_t start,
    size_t end,
    const char *needle,
    emby_span *out
) {
    size_t needle_len = strlen(needle);
    fts_lex lx;
    int found = 0;
    if (needle_len == 0 || start > end || end > len || needle_len > end - start) return 0;
    fts_lex_init(&lx, sql, len, FTS_LEX_PARAM_NUMBERED_OR_NAMED);

    for (;;) {
        fts_lex_token tok = fts_lex_next_token(&lx);
        if (tok.type == FTS_LEX_TOK_ERROR) return 0;
        if (tok.type == FTS_LEX_TOK_EOF) break;
        if (tok.start < start) continue;
        if (tok.start > end - needle_len) break;
        if (memcmp(sql + tok.start, needle, needle_len) != 0) continue;
        if (found) return 0;
        out->start = tok.start;
        out->end = tok.start + needle_len;
        found = 1;
    }
    return found;
}

static int emby_parse_trailing_integer(
    const char *sql,
    size_t len,
    size_t start,
    emby_span *slot
) {
    fts_lex lx;
    fts_lex_token tok;
    fts_lex_token eof_tok;

    if (start > len) return 0;
    fts_lex_init(&lx, sql, len, FTS_LEX_PARAM_NUMBERED_OR_NAMED);
    lx.pos = start;
    tok = fts_lex_next_token(&lx);
    if (tok.type != FTS_LEX_TOK_NUMBER) return 0;
    eof_tok = fts_lex_next_token(&lx);
    if (eof_tok.type != FTS_LEX_TOK_EOF) return 0;
    slot->start = tok.start;
    slot->end = tok.end;
    return emby_validate_integer_slot(sql, slot);
}

static int emby_span_has_byte(const char *sql, const emby_span *span, char c) {
    size_t i;
    for (i = span->start; i < span->end; i++) {
        if (sql[i] == c) return 1;
    }
    return 0;
}

static int emby_projection_has_rejected_ident(
    const char *sql,
    const emby_span *span
) {
    static const char *const rejected[] = {
        "distinct", "all", "max", "min", "count", "sum", "avg", "total", "group_concat",
        "row_number", "rank", "dense_rank", "first_value", "last_value",
        "lead", "lag", "over"
    };
    fts_lex lx;
    fts_lex_token prev = {0};
    fts_lex_token prev_prev = {0};
    size_t i;

    fts_lex_init(&lx, sql, span->end, FTS_LEX_PARAM_NUMBERED_OR_NAMED);
    lx.pos = span->start;
    for (;;) {
        fts_lex_token tok = fts_lex_next_token(&lx);
        if (tok.type == FTS_LEX_TOK_ERROR) return 1;
        if (tok.type == FTS_LEX_TOK_EOF || tok.start >= span->end) return 0;
        if (tok.type == FTS_LEX_TOK_SYMBOL && tok.symbol == '*' &&
            !(prev.type == FTS_LEX_TOK_SYMBOL && prev.symbol == '.' &&
              prev_prev.type == FTS_LEX_TOK_IDENT)) {
            return 1;
        }
        if (tok.type == FTS_LEX_TOK_IDENT) {
            for (i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
                if (fts_lex_token_text_eq(sql, &tok, rejected[i])) return 1;
            }
        }
        prev_prev = prev;
        prev = tok;
    }
}

static emby_match_result emby_build_splice_candidate(
    sqlite3 *db,
    const char *zSql,
    size_t bounded_len,
    size_t scan_len,
    emby_span target,
    const emby_slots *slots,
    const emby_exists_arm_id *arms,
    size_t arm_count,
    int wrap_outer,
    obs_rewrite_mode mode,
    emby_rewrite_candidate *candidate
) {
    size_t repl_len = 0;
    char *repl = build_exists_arms_for_family(zSql, slots, arms, arm_count, wrap_outer, &repl_len);
    candidate->mode = mode;
    if (!repl) return EMBY_MATCH_BUILD_FAILED;
    candidate->sql = emby_splice_span(
        db, zSql, bounded_len, scan_len, target, repl, repl_len,
        &candidate->scan_out_len, &candidate->rewrite_len
    );
    free(repl);
    if (!candidate->sql) return EMBY_MATCH_BUILD_FAILED;
    return EMBY_MATCH_BUILT;
}

static emby_match_result emby_capture_miss(
    sqlite3 *db,
    int enabled,
    obs_miss_reason reason,
    obs_rewrite_mode mode,
    const char *sub_reason,
    const char *zSql,
    size_t scan_len
) {
    if (enabled) {
        uint64_t shape = 0;

        if (!obs_is_disabled()) shape = fts_lex_shape_key(zSql, scan_len);
        obs_log_rewrite_miss(
            mode, reason, sub_reason, db, zSql, scan_len, shape
        );
    }
    return EMBY_MATCH_MISS;
}

static int emby_spans_byte_equal(
    const char *sql,
    const emby_span *left,
    const emby_span *right
) {
    size_t left_len = left->end - left->start;
    size_t right_len = right->end - right->start;
    return left_len == right_len &&
           memcmp(sql + left->start, sql + right->start, left_len) == 0;
}

static int emby_span_is_single_space(const char *sql, size_t start, size_t end) {
    return end == start + 1 && sql[start] == ' ';
}

static int emby_resume_membership_has_context(
    const char *sql,
    size_t len,
    size_t start,
    const emby_span *membership,
    const emby_span *tail
) {
    fts_lex lx;
    fts_lex_token prev = {FTS_LEX_TOK_EOF, 0, 0, 0};
    fts_lex_token tok;

    if (start > len || membership->start < start || membership->end > tail->start) return 0;
    if (!emby_span_is_single_space(sql, membership->end, tail->start)) return 0;

    fts_lex_init(&lx, sql, len, FTS_LEX_PARAM_NUMBERED_OR_NAMED);
    lx.pos = start;
    for (;;) {
        tok = fts_lex_next_token(&lx);
        if (tok.type == FTS_LEX_TOK_ERROR) return 0;
        if (tok.type == FTS_LEX_TOK_EOF || tok.start >= membership->start) break;
        prev = tok;
    }
    if (tok.type != FTS_LEX_TOK_IDENT || tok.start != membership->start) return 0;
    return prev.type == FTS_LEX_TOK_IDENT && fts_lex_token_text_eq(sql, &prev, "AND");
}

static int emby_capture_literal_user_id_after(
    const char *sql,
    size_t len,
    size_t start,
    const char *anchor,
    emby_span *slot
) {
    emby_span a;
    fts_lex lx;
    fts_lex_token tok;

    if (!find_unique_token_after(sql, len, start, anchor, &a)) return 0;
    fts_lex_init(&lx, sql, len, FTS_LEX_PARAM_NUMBERED_OR_NAMED);
    lx.pos = a.end;
    tok = fts_lex_next_token(&lx);
    if (tok.type != FTS_LEX_TOK_NUMBER) return 0;
    slot->start = tok.start;
    slot->end = tok.end;
    return emby_validate_integer_slot(sql, slot);
}

static char *emby_build_resume_conjunct(
    const char *sql,
    const emby_span *user_id,
    size_t *out_len
) {
    emby_piece pieces[3];

    pieces[0].lit =
        " AND ((A.Type=5 AND A.UserDataKeyId IN (SELECT UserDataKeyId FROM UserDatas WHERE UserId=";
    pieces[0].slot = user_id;
    pieces[1].lit =
        " AND playbackPositionTicks>0)) OR (A.Type=8 AND A.SeriesPresentationUniqueKey IN (SELECT N2.SeriesPresentationUniqueKey FROM MediaItems N2 JOIN UserDatas UN2 ON N2.UserDataKeyId=UN2.UserDataKeyId AND UN2.UserId=";
    pieces[1].slot = user_id;
    pieces[2].lit =
        " WHERE N2.Type=8 AND Coalesce(N2.SortParentIndexNumber,N2.ParentIndexNumber,-1) <> 0 AND (UN2.Played=1 OR UN2.playbackPositionTicks>0))))";
    pieces[2].slot = NULL;
    return emby_build_pieces(sql, pieces, 3, out_len);
}

static emby_match_result emby_build_resume_candidate(
    sqlite3 *db,
    const char *zSql,
    size_t bounded_len,
    size_t scan_len,
    emby_span target,
    emby_span insert_before,
    const emby_slots *slots,
    const emby_exists_arm_id *arms,
    size_t arm_count,
    const emby_span *user_id,
    emby_rewrite_candidate *candidate
) {
    size_t repl_len = 0;
    size_t conjunct_len = 0;
    size_t old_len;
    size_t out_len;
    size_t limit;
    char *repl = NULL;
    char *conjunct = NULL;
    char *buf = NULL;
    char *p;

    candidate->mode = OBS_MODE_EMBY_RESUME;
    if (target.start > target.end || target.end > scan_len ||
        insert_before.start > scan_len || target.end > insert_before.start) {
        return EMBY_MATCH_BUILD_FAILED;
    }
    repl = build_exists_arms_for_family(zSql, slots, arms, arm_count, 0, &repl_len);
    conjunct = emby_build_resume_conjunct(zSql, user_id, &conjunct_len);
    if (!repl || !conjunct) goto fail;

    old_len = target.end - target.start;
    if (bounded_len < old_len) goto fail;
    out_len = bounded_len - old_len;
    if (!add_size(&out_len, repl_len) || !add_size(&out_len, conjunct_len)) goto fail;
    limit = (size_t)sqlite3_limit(db, SQLITE_LIMIT_SQL_LENGTH, -1);
    if (out_len > limit || out_len > (size_t)INT_MAX) goto fail;

    buf = (char *)malloc(out_len + 1);
    if (!buf) goto fail;
    p = buf;
    append_bytes(&p, zSql, target.start);
    append_bytes(&p, repl, repl_len);
    append_bytes(&p, zSql + target.end, insert_before.start - target.end);
    append_bytes(&p, conjunct, conjunct_len);
    append_bytes(&p, zSql + insert_before.start, bounded_len - insert_before.start);
    buf[out_len] = 0;

    candidate->sql = buf;
    candidate->scan_out_len = scan_len - old_len + repl_len + conjunct_len;
    candidate->rewrite_len = out_len;
    free(repl);
    free(conjunct);
    return EMBY_MATCH_BUILT;

fail:
    free(repl);
    free(conjunct);
    free(buf);
    return EMBY_MATCH_BUILD_FAILED;
}

static emby_match_result emby_match_resume(
    sqlite3 *db,
    const char *zSql,
    size_t bounded_len,
    size_t scan_len,
    emby_rewrite_candidate *candidate
) {
    static const emby_exists_arm_id arms[] = {EMBY_EXISTS_ARM_ANCESTOR};
    emby_slots slots;
    emby_span a1;
    emby_span a2;
    emby_span a3;
    emby_span group_anchor;
    emby_span limit_slot;
    emby_span not_null_gate;
    emby_span user_derived;
    emby_span user_outer;
    emby_span user_hide;

    memset(&slots, 0, sizeof(slots));
    if (!emby_token_present(zSql, scan_len, "LastWatchedEpisodes")) return EMBY_MATCH_MISS;
    if (!find_unique_token_after(zSql, scan_len, 0, EMBY_ANCHOR_PRE_L1, &a1)) return EMBY_MATCH_MISS;
    if (!find_unique_token_after(zSql, scan_len, a1.end, EMBY_ANCHOR_SELECT_AFTER_L1, &a2)) return EMBY_MATCH_MISS;
    slots.l1.start = a1.end;
    slots.l1.end = a2.start;
    emby_span_rtrim(zSql, &slots.l1);
    if (!validate_numeric_slot(zSql, &slots.l1)) return EMBY_MATCH_MISS;
    if (!find_unique_token_after(zSql, scan_len, a2.end, EMBY_ANCHOR_RESUME_TAIL, &a3)) return EMBY_MATCH_MISS;
    if (!emby_parse_trailing_integer(zSql, scan_len, a3.end, &limit_slot)) return EMBY_MATCH_MISS;
    if (!find_unique_token_after(zSql, scan_len, a2.end, EMBY_GUARD_RESUME_LASTWATCHED_NOT_NULL,
                                 &not_null_gate)) {
        return EMBY_MATCH_MISS;
    }
    if (not_null_gate.end > a3.start) return EMBY_MATCH_MISS;
    if (!emby_find_unique_token_between(zSql, scan_len, 0, scan_len, EMBY_MEMBER_ANCESTORS,
                                        &slots.membership)) {
        return EMBY_MATCH_MISS;
    }
    if (slots.membership.end > a3.start || slots.membership.start < a2.end) return EMBY_MATCH_MISS;
    if (!emby_resume_membership_has_context(zSql, scan_len, a2.end, &slots.membership, &a3)) {
        return EMBY_MATCH_MISS;
    }
    if (!emby_find_unique_bytes_between(zSql, slots.membership.end, a3.end, EMBY_ANCHOR_RESUME_GROUP,
                                        &group_anchor)) {
        return EMBY_MATCH_MISS;
    }
    if (!emby_capture_literal_user_id_after(zSql, scan_len, a2.end, "UserDatas_N.UserId=",
                                            &user_derived)) {
        return EMBY_MATCH_MISS;
    }
    if (!emby_capture_literal_user_id_after(zSql, scan_len, a2.end, "UserDatas.UserId=",
                                            &user_outer)) {
        return EMBY_MATCH_MISS;
    }
    if (!emby_capture_literal_user_id_after(zSql, scan_len, a2.end, "userdatas.userid=",
                                            &user_hide)) {
        return EMBY_MATCH_MISS;
    }
    if (!emby_spans_byte_equal(zSql, &user_derived, &user_outer) ||
        !emby_spans_byte_equal(zSql, &user_derived, &user_hide)) {
        return EMBY_MATCH_MISS;
    }
    return emby_build_resume_candidate(
        db, zSql, bounded_len, scan_len, slots.membership, group_anchor, &slots,
        arms, sizeof(arms) / sizeof(arms[0]), &user_derived, candidate
    );
}

static emby_match_result emby_match_favorites(
    sqlite3 *db,
    const char *zSql,
    size_t bounded_len,
    size_t scan_len,
    emby_rewrite_candidate *candidate
) {
    static const emby_exists_arm_id arms[] = {
        EMBY_EXISTS_ARM_ANCESTOR, EMBY_EXISTS_ARM_LINKS_ONE_LEVEL
    };
    emby_slots slots;
    emby_span a1;
    emby_span a2;
    emby_span a3;
    emby_span a4;

    if (emby_token_present(zSql, scan_len, EMBY_GUARD_PLAYLISTS)) return EMBY_MATCH_MISS;
    memset(&slots, 0, sizeof(slots));
    if (!find_unique_token_after(zSql, scan_len, 0, EMBY_ANCHOR_PRE_L1, &a1)) return EMBY_MATCH_MISS;
    if (!find_unique_token_after(zSql, scan_len, a1.end, EMBY_ANCHOR_LINKS_CTE_IN, &a2)) return EMBY_MATCH_MISS;
    slots.l1.start = a1.end;
    slots.l1.end = a2.start;
    emby_span_rtrim(zSql, &slots.l1);
    if (!validate_numeric_slot(zSql, &slots.l1)) return EMBY_MATCH_MISS;
    if (!find_unique_token_after(zSql, scan_len, a2.end, EMBY_ANCHOR_CTE_END2, &a3)) return EMBY_MATCH_MISS;
    slots.t1.start = a2.end;
    slots.t1.end = a3.start;
    emby_span_rtrim(zSql, &slots.t1);
    if (!validate_numeric_slot(zSql, &slots.t1)) return EMBY_MATCH_MISS;
    if (!find_unique_token_after(zSql, scan_len, a3.end, EMBY_MEMBER_FAVORITES, &a4)) return EMBY_MATCH_MISS;
    slots.membership = a4;
    return emby_build_splice_candidate(
        db, zSql, bounded_len, scan_len, slots.membership, &slots,
        arms, sizeof(arms) / sizeof(arms[0]), 1, OBS_MODE_EMBY_FAVORITES, candidate
    );
}

static emby_match_result emby_match_browse(
    sqlite3 *db,
    const char *zSql,
    size_t bounded_len,
    size_t scan_len,
    emby_rewrite_candidate *candidate
) {
    static const emby_exists_arm_id arms[] = {
        EMBY_EXISTS_ARM_LINKS_ONE_LEVEL, EMBY_EXISTS_ARM_LINKS_TWO_LEVEL
    };
    emby_slots slots;
    emby_span a1;
    emby_span a2;
    emby_span a3;
    emby_span a4;
    emby_span a5;

    if (emby_token_present(zSql, scan_len, EMBY_GUARD_PLAYLISTS)) return EMBY_MATCH_MISS;
    if (emby_sql_has_bytes(zSql, scan_len, EMBY_MEMBER_ANCESTORS)) return EMBY_MATCH_MISS;
    memset(&slots, 0, sizeof(slots));
    if (!find_unique_token_after(zSql, scan_len, 0, EMBY_ANCHOR_PRE_L1, &a1)) return EMBY_MATCH_MISS;
    if (!find_unique_token_after(zSql, scan_len, a1.end, EMBY_ANCHOR_LINKS_CTE_EQ, &a2)) return EMBY_MATCH_MISS;
    slots.l1.start = a1.end;
    slots.l1.end = a2.start;
    emby_span_rtrim(zSql, &slots.l1);
    if (!validate_numeric_slot(zSql, &slots.l1)) return EMBY_MATCH_MISS;
    if (!find_unique_token_after(zSql, scan_len, a2.end, EMBY_ANCHOR_TWOLEVEL_BARE, &a3)) return EMBY_MATCH_MISS;
    slots.t1.start = a2.end;
    slots.t1.end = a3.start;
    emby_span_rtrim(zSql, &slots.t1);
    if (!emby_validate_integer_slot(zSql, &slots.t1)) return EMBY_MATCH_MISS;
    if (!find_unique_token_after(zSql, scan_len, a3.end, EMBY_ANCHOR_CTE_END3, &a4)) return EMBY_MATCH_MISS;
    slots.t2.start = a3.end;
    slots.t2.end = a4.start;
    emby_span_rtrim(zSql, &slots.t2);
    if (!validate_numeric_slot(zSql, &slots.t2)) return EMBY_MATCH_MISS;
    if (!find_unique_token_after(zSql, scan_len, a4.end, EMBY_MEMBER_LINKS, &a5)) return EMBY_MATCH_MISS;
    slots.membership = a5;
    return emby_build_splice_candidate(
        db, zSql, bounded_len, scan_len, slots.membership, &slots,
        arms, sizeof(arms) / sizeof(arms[0]), 1, OBS_MODE_EMBY_BROWSE, candidate
    );
}

static emby_match_result emby_match_people(
    sqlite3 *db,
    const char *zSql,
    size_t bounded_len,
    size_t scan_len,
    emby_rewrite_candidate *candidate
) {
    static const emby_exists_arm_id arms[] = {EMBY_EXISTS_ARM_PEOPLE};
    int capture_boundary;
    emby_slots slots;
    emby_span a1;
    emby_span a2;
    emby_span a3;

    if (emby_token_present(zSql, scan_len, "WithItemLinkItemIds")) return EMBY_MATCH_MISS;
    if (emby_token_present(zSql, scan_len, EMBY_GUARD_PLAYLISTS)) return EMBY_MATCH_MISS;
    capture_boundary = emby_token_present(zSql, scan_len, "itemPeople2");
    memset(&slots, 0, sizeof(slots));
    if (!find_unique_token_after(zSql, scan_len, 0, EMBY_ANCHOR_PRE_L1, &a1)) {
        return emby_capture_miss(
            db, capture_boundary, OBS_MISS_CAPTURE,
            OBS_MODE_EMBY_PEOPLE, "pre_l1", zSql, scan_len
        );
    }
    if (!find_unique_token_after(zSql, scan_len, a1.end, EMBY_ANCHOR_SELECT_AFTER_L1, &a2)) {
        return emby_capture_miss(
            db, capture_boundary, OBS_MISS_CAPTURE,
            OBS_MODE_EMBY_PEOPLE, "select_anchor", zSql, scan_len
        );
    }
    slots.l1.start = a1.end;
    slots.l1.end = a2.start;
    emby_span_rtrim(zSql, &slots.l1);
    if (!validate_numeric_slot(zSql, &slots.l1)) {
        return emby_capture_miss(
            db, capture_boundary, OBS_MISS_CAPTURE,
            OBS_MODE_EMBY_PEOPLE, "ancestor_slot", zSql, scan_len
        );
    }
    if (!find_unique_token_after(zSql, scan_len, a2.end, EMBY_MEMBER_PEOPLE, &a3)) {
        return emby_capture_miss(
            db, capture_boundary, OBS_MISS_CAPTURE,
            OBS_MODE_EMBY_PEOPLE, "membership", zSql, scan_len
        );
    }
    slots.membership = a3;
    return emby_build_splice_candidate(
        db, zSql, bounded_len, scan_len, slots.membership, &slots,
        arms, sizeof(arms) / sizeof(arms[0]), 0, OBS_MODE_EMBY_PEOPLE, candidate
    );
}

static emby_match_result emby_match_links_search(
    sqlite3 *db,
    const char *zSql,
    size_t bounded_len,
    size_t scan_len,
    emby_rewrite_candidate *candidate
) {
    static const emby_exists_arm_id arms[] = {EMBY_EXISTS_ARM_LINKS_ONE_LEVEL};
    int capture_boundary;
    emby_slots slots;
    emby_span a1;
    emby_span a2;
    emby_span a3;
    emby_span a4;

    if (emby_token_present(zSql, scan_len, EMBY_GUARD_PLAYLISTS)) return EMBY_MATCH_MISS;
    if (emby_sql_has_bytes(zSql, scan_len, EMBY_MEMBER_ANCESTORS)) return EMBY_MATCH_MISS;
    capture_boundary = emby_token_present(zSql, scan_len, "WithItemLinkItemIds");
    memset(&slots, 0, sizeof(slots));
    if (!find_unique_token_after(zSql, scan_len, 0, EMBY_ANCHOR_PRE_L1, &a1)) {
        return emby_capture_miss(
            db, capture_boundary, OBS_MISS_CAPTURE,
            OBS_MODE_EMBY_LINKS_SEARCH, "pre_l1", zSql, scan_len
        );
    }
    if (!find_unique_token_after(zSql, scan_len, a1.end, EMBY_ANCHOR_LINKS_CTE_IN, &a2)) {
        return emby_capture_miss(
            db, capture_boundary, OBS_MISS_CAPTURE,
            OBS_MODE_EMBY_LINKS_SEARCH, "select_anchor", zSql, scan_len
        );
    }
    slots.l1.start = a1.end;
    slots.l1.end = a2.start;
    emby_span_rtrim(zSql, &slots.l1);
    if (!validate_numeric_slot(zSql, &slots.l1)) {
        return emby_capture_miss(
            db, capture_boundary, OBS_MISS_CAPTURE,
            OBS_MODE_EMBY_LINKS_SEARCH, "ancestor_slot", zSql, scan_len
        );
    }
    if (!find_unique_token_after(zSql, scan_len, a2.end, EMBY_ANCHOR_CTE_END2, &a3)) {
        return emby_capture_miss(
            db, capture_boundary, OBS_MISS_CAPTURE,
            OBS_MODE_EMBY_LINKS_SEARCH, "tail_anchor", zSql, scan_len
        );
    }
    slots.t1.start = a2.end;
    slots.t1.end = a3.start;
    emby_span_rtrim(zSql, &slots.t1);
    if (!validate_numeric_slot(zSql, &slots.t1)) {
        return emby_capture_miss(
            db, capture_boundary, OBS_MISS_CAPTURE,
            OBS_MODE_EMBY_LINKS_SEARCH, "type_slot", zSql, scan_len
        );
    }
    if (!find_unique_token_after(zSql, scan_len, a3.end, EMBY_MEMBER_LINKS, &a4)) {
        return emby_capture_miss(
            db, capture_boundary, OBS_MISS_CAPTURE,
            OBS_MODE_EMBY_LINKS_SEARCH, "membership", zSql, scan_len
        );
    }
    slots.membership = a4;
    return emby_build_splice_candidate(
        db, zSql, bounded_len, scan_len, slots.membership, &slots,
        arms, sizeof(arms) / sizeof(arms[0]), 0, OBS_MODE_EMBY_LINKS_SEARCH, candidate
    );
}

static emby_match_result emby_match_resume_simple(
    sqlite3 *db,
    const char *zSql,
    size_t bounded_len,
    size_t scan_len,
    emby_rewrite_candidate *candidate
) {
    static const emby_exists_arm_id arms[] = {EMBY_EXISTS_ARM_ANCESTOR};
    emby_slots slots;
    emby_span a1;
    emby_span a2;
    emby_span a3;
    emby_span limit_slot;

    if (emby_token_present(zSql, scan_len, EMBY_GUARD_PLAYLISTS)) return EMBY_MATCH_MISS;
    if (emby_token_present(zSql, scan_len, "LastWatchedEpisodes")) return EMBY_MATCH_MISS;
    if (emby_token_present(zSql, scan_len, "SimB_Ids")) return EMBY_MATCH_MISS;
    if (!emby_sql_has_bytes(zSql, scan_len, "UserDatas.playbackPositionTicks > 0")) {
        return EMBY_MATCH_MISS;
    }
    memset(&slots, 0, sizeof(slots));
    if (!find_unique_token_after(zSql, scan_len, 0, EMBY_ANCHOR_PRE_L1, &a1)) return EMBY_MATCH_MISS;
    if (!find_unique_token_after(zSql, scan_len, a1.end, EMBY_ANCHOR_SELECT_AFTER_L1, &a2)) return EMBY_MATCH_MISS;
    slots.l1.start = a1.end;
    slots.l1.end = a2.start;
    emby_span_rtrim(zSql, &slots.l1);
    if (!validate_numeric_slot(zSql, &slots.l1)) return EMBY_MATCH_MISS;
    if (!find_unique_token_after(zSql, scan_len, a2.end, EMBY_ANCHOR_RESUME_SIMPLE_TAIL, &a3)) {
        return EMBY_MATCH_MISS;
    }
    if (!emby_parse_trailing_integer(zSql, scan_len, a3.end, &limit_slot)) return EMBY_MATCH_MISS;
    if (!emby_find_unique_token_between(zSql, scan_len, 0, scan_len, EMBY_MEMBER_ANCESTORS,
                                        &slots.membership)) {
        return EMBY_MATCH_MISS;
    }
    if (slots.membership.start < a3.start || slots.membership.end > a3.end) return EMBY_MATCH_MISS;
    return emby_build_splice_candidate(
        db, zSql, bounded_len, scan_len, slots.membership, &slots,
        arms, sizeof(arms) / sizeof(arms[0]), 0, OBS_MODE_EMBY_RESUME_SIMPLE, candidate
    );
}

static emby_match_result emby_match_similar(
    sqlite3 *db,
    const char *zSql,
    size_t bounded_len,
    size_t scan_len,
    emby_rewrite_candidate *candidate
) {
    static const emby_exists_arm_id arms[] = {EMBY_EXISTS_ARM_ANCESTOR};
    emby_slots slots;
    emby_span a1;
    emby_span a2;
    emby_span a3;
    emby_span limit_slot;

    if (emby_token_present(zSql, scan_len, EMBY_GUARD_PLAYLISTS)) return EMBY_MATCH_MISS;
    if (!emby_token_present(zSql, scan_len, "SimB_Ids")) return EMBY_MATCH_MISS;
    if (!emby_token_present(zSql, scan_len, "LinkedCounts")) return EMBY_MATCH_MISS;
    if (!emby_token_present(zSql, scan_len, "SimilarityScore")) return EMBY_MATCH_MISS;
    memset(&slots, 0, sizeof(slots));
    if (!find_unique_token_after(zSql, scan_len, 0, EMBY_ANCHOR_PRE_L1, &a1)) return EMBY_MATCH_MISS;
    if (!find_unique_token_after(zSql, scan_len, a1.end, EMBY_ANCHOR_SIMILAR_AFTER_L1, &a2)) {
        return EMBY_MATCH_MISS;
    }
    slots.l1.start = a1.end;
    slots.l1.end = a2.start;
    emby_span_rtrim(zSql, &slots.l1);
    if (!validate_numeric_slot(zSql, &slots.l1)) return EMBY_MATCH_MISS;
    if (!find_unique_token_after(zSql, scan_len, a2.end, EMBY_ANCHOR_SIMILAR_TAIL, &a3)) {
        return EMBY_MATCH_MISS;
    }
    if (!emby_parse_trailing_integer(zSql, scan_len, a3.end, &limit_slot)) return EMBY_MATCH_MISS;
    if (!emby_find_unique_token_between(zSql, scan_len, 0, scan_len, EMBY_MEMBER_ANCESTORS,
                                        &slots.membership)) {
        return EMBY_MATCH_MISS;
    }
    if (slots.membership.end > a3.start || slots.membership.start < a2.end) return EMBY_MATCH_MISS;
    return emby_build_splice_candidate(
        db, zSql, bounded_len, scan_len, slots.membership, &slots,
        arms, sizeof(arms) / sizeof(arms[0]), 0, OBS_MODE_EMBY_SIMILAR, candidate
    );
}

static int latest_tail_has_guard(const char *zSql, size_t scan_len, const char *guard) {
    return emby_sql_has_bytes(zSql, scan_len, guard);
}

static int emby_latest_prefix_is_with(const char *zSql, size_t prefix_len) {
    fts_lex lx;
    fts_lex_token tok;
    fts_lex_token eof_tok;

    fts_lex_init(&lx, zSql, prefix_len, FTS_LEX_PARAM_NUMBERED_OR_NAMED);
    tok = fts_lex_next_token(&lx);
    if (tok.type != FTS_LEX_TOK_IDENT || !fts_lex_token_text_eq(zSql, &tok, "with")) {
        return 0;
    }
    eof_tok = fts_lex_next_token(&lx);
    return eof_tok.type == FTS_LEX_TOK_EOF;
}

static int emby_find_latest_ancestor_slot(
    const char *zSql,
    size_t scan_len,
    emby_span *slot,
    emby_span *select_anchor,
    const char **sub_reason
) {
    emby_span list_pre;
    emby_span scalar_pre;
    const emby_span *pre;
    const char *select_needle;
    int list_count;
    int scalar_count;
    int select_count;
    int scalar;

    *sub_reason = "prefix";
    list_count = count_tokens_after(
        zSql, scan_len, 0, EMBY_ANCHOR_PRE_L1, &list_pre
    );
    scalar_count = count_tokens_after(
        zSql, scan_len, 0, EMBY_ANCHOR_PRE_L1_SCALAR, &scalar_pre
    );
    if (list_count < 0 || scalar_count < 0 || list_count > 1 || scalar_count > 1 ||
        (list_count == 1) == (scalar_count == 1)) {
        return 0;
    }
    scalar = scalar_count == 1;
    pre = scalar ? &scalar_pre : &list_pre;
    select_needle = scalar ? EMBY_ANCHOR_LATEST_SELECT_SCALAR
                           : EMBY_ANCHOR_LATEST_SELECT;
    if (!emby_latest_prefix_is_with(zSql, pre->start)) {
        return 0;
    }
    *sub_reason = "select_anchor";
    select_count = count_tokens_after(
        zSql, scan_len, pre->end, select_needle, select_anchor
    );
    if (select_count != 1) return 0;
    slot->start = pre->end;
    slot->end = select_anchor->start;
    emby_span_rtrim(zSql, slot);
    *sub_reason = "ancestor_slot";
    return scalar ? emby_validate_integer_slot(zSql, slot)
                  : validate_numeric_slot(zSql, slot);
}

static emby_match_result emby_match_episodes_latest(
    sqlite3 *db,
    const char *zSql,
    size_t scan_len,
    emby_rewrite_candidate *candidate
) {
    emby_span a1;
    emby_span a2;
    emby_span a3;
    emby_span a4;
    emby_span l1;
    emby_span projection;
    emby_span user_id;
    emby_span limit_slot;
    emby_piece pieces[8];
    emby_latest_index_state index_state;
    const char *sub_reason;
    size_t limit;

    if (!find_unique_token_after(zSql, scan_len, 0, EMBY_TYPE_EPISODES_LATEST, &a1)) {
        return EMBY_MATCH_MISS;
    }
    if (emby_token_present(zSql, scan_len, "over")) return EMBY_MATCH_MISS;
    if (emby_sql_has_bytes(zSql, scan_len, "LastWatchedEpisodes")) return EMBY_MATCH_MISS;
    if (latest_tail_has_guard(zSql, scan_len, "ORDER BY SeriesName")) return EMBY_MATCH_MISS;
    if (latest_tail_has_guard(zSql, scan_len, "COLLATE NATURALSORT")) return EMBY_MATCH_MISS;
    if (!emby_find_latest_ancestor_slot(
            zSql, scan_len, &l1, &a2, &sub_reason
        )) {
        return emby_capture_miss(
            db, 1, OBS_MISS_CAPTURE,
            OBS_MODE_EMBY_EPISODES_LATEST, sub_reason, zSql, scan_len
        );
    }
    if (!find_unique_token_after(zSql, scan_len, a2.end, EMBY_ANCHOR_LATEST_FROM, &a3)) {
        return emby_capture_miss(
            db, 1, OBS_MISS_CAPTURE,
            OBS_MODE_EMBY_EPISODES_LATEST, "from_anchor", zSql, scan_len
        );
    }

    projection.start = a2.end;
    projection.end = a3.start;
    if (projection.end <= projection.start || projection.end - projection.start > EMBY_SLOT_CAP ||
        emby_sql_has_bytes(zSql + projection.start, projection.end - projection.start, "WithAncestors") ||
        emby_span_has_byte(zSql, &projection, '(') ||
        emby_projection_has_rejected_ident(zSql, &projection)) {
        return emby_capture_miss(
            db, 1, OBS_MISS_CAPTURE,
            OBS_MODE_EMBY_EPISODES_LATEST, "projection", zSql, scan_len
        );
    }
    if (!find_unique_token_after(zSql, scan_len, a3.end, EMBY_ANCHOR_LATEST_TAIL, &a4)) {
        return emby_capture_miss(
            db, 1, OBS_MISS_CAPTURE,
            OBS_MODE_EMBY_EPISODES_LATEST, "tail_anchor", zSql, scan_len
        );
    }
    user_id.start = a3.end;
    user_id.end = a4.start;
    emby_span_rtrim(zSql, &user_id);
    if (!emby_validate_integer_slot(zSql, &user_id)) {
        return emby_capture_miss(
            db, 1, OBS_MISS_CAPTURE,
            OBS_MODE_EMBY_EPISODES_LATEST, "user_slot", zSql, scan_len
        );
    }
    if (!emby_parse_trailing_integer(zSql, scan_len, a4.end, &limit_slot)) {
        return emby_capture_miss(
            db, 1, OBS_MISS_CAPTURE,
            OBS_MODE_EMBY_EPISODES_LATEST, "limit", zSql, scan_len
        );
    }
    if (!emby_latest_limit_supported(zSql, &limit_slot)) {
        return emby_capture_miss(
            db, 1, OBS_MISS_OUT_OF_SCOPE,
            OBS_MODE_EMBY_EPISODES_LATEST, "limit_unsupported", zSql, scan_len
        );
    }
    if (emby_statement_has_bind_parameter(zSql, scan_len)) {
        return emby_capture_miss(
            db, 1, OBS_MISS_OUT_OF_SCOPE,
            OBS_MODE_EMBY_EPISODES_LATEST, "bind", zSql, scan_len
        );
    }
    index_state = emby_latest_index_ready(db);
    if (index_state != EMBY_LATEST_INDEX_PRESENT) {
        if (index_state == EMBY_LATEST_INDEX_MISSING) {
            obs_log_index_missing(db, OBS_MODE_EMBY_EPISODES_LATEST);
        } else {
            obs_log_rewrite_skipped(
                db, "index_probe_error", OBS_MODE_EMBY_EPISODES_LATEST
            );
        }
        return EMBY_MATCH_MISS;
    }

    pieces[0].lit = EMBY_LATEST_TPL_0;
    pieces[0].slot = &l1;
    pieces[1].lit = EMBY_LATEST_TPL_1;
    pieces[1].slot = &user_id;
    pieces[2].lit = EMBY_LATEST_TPL_2;
    pieces[2].slot = &l1;
    pieces[3].lit = EMBY_LATEST_TPL_3;
    pieces[3].slot = &user_id;
    pieces[4].lit = EMBY_LATEST_TPL_4;
    pieces[4].slot = &limit_slot;
    pieces[5].lit = EMBY_LATEST_TPL_5;
    pieces[5].slot = &projection;
    pieces[6].lit = EMBY_LATEST_TPL_6;
    pieces[6].slot = &user_id;
    pieces[7].lit = EMBY_LATEST_TPL_7;
    pieces[7].slot = &limit_slot;
    candidate->mode = OBS_MODE_EMBY_EPISODES_LATEST;
    candidate->sql = emby_build_pieces(zSql, pieces, sizeof(pieces) / sizeof(pieces[0]),
                                       &candidate->rewrite_len);
    if (!candidate->sql) return EMBY_MATCH_BUILD_FAILED;
    limit = (size_t)sqlite3_limit(db, SQLITE_LIMIT_SQL_LENGTH, -1);
    if (candidate->rewrite_len > limit || candidate->rewrite_len > (size_t)INT_MAX) {
        free(candidate->sql);
        candidate->sql = NULL;
        return EMBY_MATCH_BUILD_FAILED;
    }
    candidate->scan_out_len = candidate->rewrite_len;
    return EMBY_MATCH_BUILT;
}

static emby_match_result emby_match_movies_latest(
    sqlite3 *db,
    const char *zSql,
    size_t scan_len,
    emby_rewrite_candidate *candidate
) {
    emby_span type_gate;
    emby_span a2;
    emby_span a3;
    emby_span a4;
    emby_span l1;
    emby_span projection;
    emby_span user_id;
    emby_span limit_slot;
    emby_piece pieces[8];
    emby_latest_index_state index_state;
    const char *sub_reason;
    size_t limit;

    if (!find_unique_token_after(zSql, scan_len, 0, EMBY_TYPE_MOVIES_LATEST, &type_gate)) {
        return EMBY_MATCH_MISS;
    }
    if (emby_token_present(zSql, scan_len, "over")) return EMBY_MATCH_MISS;
    if (emby_sql_has_bytes(zSql, scan_len, "LastWatchedEpisodes")) return EMBY_MATCH_MISS;
    if (!emby_find_latest_ancestor_slot(
            zSql, scan_len, &l1, &a2, &sub_reason
        )) {
        return emby_capture_miss(
            db, 1, OBS_MISS_CAPTURE,
            OBS_MODE_EMBY_MOVIES_LATEST, sub_reason, zSql, scan_len
        );
    }
    if (!find_unique_token_after(zSql, scan_len, a2.end, EMBY_ANCHOR_LATEST_FROM, &a3)) {
        return emby_capture_miss(
            db, 1, OBS_MISS_CAPTURE,
            OBS_MODE_EMBY_MOVIES_LATEST, "from_anchor", zSql, scan_len
        );
    }
    projection.start = a2.end;
    projection.end = a3.start;
    if (projection.end <= projection.start || projection.end - projection.start > EMBY_SLOT_CAP ||
        emby_sql_has_bytes(zSql + projection.start, projection.end - projection.start, "WithAncestors") ||
        emby_span_has_byte(zSql, &projection, '(') ||
        emby_projection_has_rejected_ident(zSql, &projection)) {
        return emby_capture_miss(
            db, 1, OBS_MISS_CAPTURE,
            OBS_MODE_EMBY_MOVIES_LATEST, "projection", zSql, scan_len
        );
    }
    if (!find_unique_token_after(zSql, scan_len, a3.end,
                                 EMBY_ANCHOR_MOVIES_LATEST_TAIL, &a4)) {
        return emby_capture_miss(
            db, 1, OBS_MISS_CAPTURE,
            OBS_MODE_EMBY_MOVIES_LATEST, "tail_anchor", zSql, scan_len
        );
    }
    user_id.start = a3.end;
    user_id.end = a4.start;
    emby_span_rtrim(zSql, &user_id);
    if (!emby_validate_integer_slot(zSql, &user_id)) {
        return emby_capture_miss(
            db, 1, OBS_MISS_CAPTURE,
            OBS_MODE_EMBY_MOVIES_LATEST, "user_slot", zSql, scan_len
        );
    }
    if (!emby_parse_trailing_integer(zSql, scan_len, a4.end, &limit_slot)) {
        return emby_capture_miss(
            db, 1, OBS_MISS_CAPTURE,
            OBS_MODE_EMBY_MOVIES_LATEST, "limit", zSql, scan_len
        );
    }
    if (!emby_latest_limit_supported(zSql, &limit_slot)) {
        return emby_capture_miss(
            db, 1, OBS_MISS_OUT_OF_SCOPE,
            OBS_MODE_EMBY_MOVIES_LATEST, "limit_unsupported", zSql, scan_len
        );
    }
    if (emby_statement_has_bind_parameter(zSql, scan_len)) {
        return emby_capture_miss(
            db, 1, OBS_MISS_OUT_OF_SCOPE,
            OBS_MODE_EMBY_MOVIES_LATEST, "bind", zSql, scan_len
        );
    }
    index_state = emby_latest_movies_indexes_ready(db);
    if (index_state != EMBY_LATEST_INDEX_PRESENT) {
        if (index_state == EMBY_LATEST_INDEX_MISSING) {
            obs_log_index_missing(db, OBS_MODE_EMBY_MOVIES_LATEST);
        } else {
            obs_log_rewrite_skipped(
                db, "index_probe_error", OBS_MODE_EMBY_MOVIES_LATEST
            );
        }
        return EMBY_MATCH_MISS;
    }

    pieces[0].lit = EMBY_MOVIES_LATEST_TPL_0;
    pieces[0].slot = &l1;
    pieces[1].lit = EMBY_MOVIES_LATEST_TPL_1;
    pieces[1].slot = &user_id;
    pieces[2].lit = EMBY_MOVIES_LATEST_TPL_2;
    pieces[2].slot = &l1;
    pieces[3].lit = EMBY_MOVIES_LATEST_TPL_3;
    pieces[3].slot = &user_id;
    pieces[4].lit = EMBY_MOVIES_LATEST_TPL_4;
    pieces[4].slot = &limit_slot;
    pieces[5].lit = EMBY_MOVIES_LATEST_TPL_5;
    pieces[5].slot = &projection;
    pieces[6].lit = EMBY_MOVIES_LATEST_TPL_6;
    pieces[6].slot = &user_id;
    pieces[7].lit = EMBY_MOVIES_LATEST_TPL_7;
    pieces[7].slot = &limit_slot;
    candidate->mode = OBS_MODE_EMBY_MOVIES_LATEST;
    candidate->sql = emby_build_pieces(zSql, pieces, sizeof(pieces) / sizeof(pieces[0]),
                                       &candidate->rewrite_len);
    if (!candidate->sql) return EMBY_MATCH_BUILD_FAILED;
    limit = (size_t)sqlite3_limit(db, SQLITE_LIMIT_SQL_LENGTH, -1);
    if (candidate->rewrite_len > limit || candidate->rewrite_len > (size_t)INT_MAX) {
        free(candidate->sql);
        candidate->sql = NULL;
        return EMBY_MATCH_BUILD_FAILED;
    }
    candidate->scan_out_len = candidate->rewrite_len;
    return EMBY_MATCH_BUILT;
}

static char *emby_build_rewritten_sql(
    sqlite3 *db,
    const char *zSql,
    size_t bounded_len,
    size_t scan_len,
    const emby_span *rhs_span,
    const emby_slots *slots,
    size_t *scan_out_len,
    size_t *rewrite_len
) {
    size_t arms_len = 0;
    char *arms = build_exists_arms(zSql, slots, &arms_len);
    size_t membership_old_len = slots->membership.end - slots->membership.start;
    size_t rhs_len = rhs_span->end - rhs_span->start;
    size_t out_len = bounded_len;
    size_t limit;
    char *rewritten;
    char *p;

    if (!arms) return NULL;
    if (rhs_span->start > rhs_span->end ||
        slots->membership.start > slots->membership.end ||
        rhs_span->end > scan_len ||
        slots->membership.end > scan_len) {
        free(arms);
        return NULL;
    }
    if (rhs_span->end > slots->membership.start) {
        free(arms);
        return NULL;
    }

    if (out_len < membership_old_len) {
        free(arms);
        return NULL;
    }
    out_len -= membership_old_len;
    if (!add_size(&out_len, arms_len)) {
        free(arms);
        return NULL;
    }
    if (!add_size(&out_len, EMBY_SCALAR_OPEN_LEN + EMBY_SCALAR_CLOSE_LEN)) {
        free(arms);
        return NULL;
    }

    limit = (size_t)sqlite3_limit(db, SQLITE_LIMIT_SQL_LENGTH, -1);
    if (out_len > limit || out_len > (size_t)INT_MAX) {
        free(arms);
        return NULL;
    }

    rewritten = (char *)malloc(out_len + 1);
    if (!rewritten) {
        free(arms);
        return NULL;
    }
    p = rewritten;

    append_bytes(&p, zSql, rhs_span->start);
    append_bytes(&p, EMBY_SCALAR_OPEN, EMBY_SCALAR_OPEN_LEN);
    append_bytes(&p, zSql + rhs_span->start, rhs_len);
    *p++ = ')';
    append_bytes(&p, zSql + rhs_span->end, slots->membership.start - rhs_span->end);
    append_bytes(&p, arms, arms_len);
    append_bytes(&p, zSql + slots->membership.end, bounded_len - slots->membership.end);

    rewritten[out_len] = 0;
    *scan_out_len = scan_len - membership_old_len + arms_len +
                    EMBY_SCALAR_OPEN_LEN + EMBY_SCALAR_CLOSE_LEN;
    *rewrite_len = out_len;
    free(arms);
    return rewritten;
}

static int emby_map_end_tail(
    const char *zSql,
    const char *rewritten,
    const char *rewritten_tail,
    size_t original_sql_len,
    size_t rewritten_sql_len,
    const char **mapped_tail
) {
    if (!rewritten_tail) {
        *mapped_tail = NULL;
        return 1;
    }
    if (rewritten_tail != rewritten + rewritten_sql_len) return 0;
    *mapped_tail = zSql + original_sql_len;
    return 1;
}

static void emby_log_rewrite_applied(
    sqlite3 *db,
    const char *source,
    size_t source_len,
    const char *rewritten,
    size_t rewritten_len,
    obs_rewrite_mode mode
) {
    obs_log_rewrite_applied(
        mode, db, source, source_len, rewritten, rewritten_len
    );
}

static void maybe_log_l1_l2_mismatch(sqlite3 *db, const char *sql, const emby_slots *slots) {
    size_t l1_len = slots->l1.end - slots->l1.start;
    size_t l2_len = slots->l2.end - slots->l2.start;
    if (l1_len == l2_len &&
        memcmp(sql + slots->l1.start, sql + slots->l2.start, l1_len) == 0) {
        return;
    }
    obs_logf("emby_fts_rewrite",
             "event=slot_mismatch target=emby slot=L1_L2 db=%p l1_len=%zu l2_len=%zu",
             (void*)db, l1_len, l2_len);
}

static int emby_retry_original_after_rewrite_failure(
    fts_rewrite_prepare_kind kind,
    sqlite3 *db,
    const char *zSql,
    int nByte,
    unsigned int prepFlags,
    sqlite3_stmt **ppStmt,
    const char **pzTail
) {
    if (ppStmt && *ppStmt) {
        sqlite3_finalize(*ppStmt);
        *ppStmt = NULL;
    }
    return call_downstream_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
}

static emby_match_result emby_try_fanout_matchers(
    sqlite3 *db,
    const char *zSql,
    size_t bounded_len,
    size_t scan_len,
    emby_rewrite_candidate *candidate
) {
    emby_match_result mr;

    mr = emby_match_resume(db, zSql, bounded_len, scan_len, candidate);
    if (mr != EMBY_MATCH_MISS) return mr;
    mr = emby_match_favorites(db, zSql, bounded_len, scan_len, candidate);
    if (mr != EMBY_MATCH_MISS) return mr;
    mr = emby_match_browse(db, zSql, bounded_len, scan_len, candidate);
    if (mr != EMBY_MATCH_MISS) return mr;
    mr = emby_match_people(db, zSql, bounded_len, scan_len, candidate);
    if (mr != EMBY_MATCH_MISS) return mr;
    mr = emby_match_links_search(db, zSql, bounded_len, scan_len, candidate);
    if (mr != EMBY_MATCH_MISS) return mr;
    mr = emby_match_resume_simple(db, zSql, bounded_len, scan_len, candidate);
    if (mr != EMBY_MATCH_MISS) return mr;
    return emby_match_similar(db, zSql, bounded_len, scan_len, candidate);
}

static int emby_slow_rewrite_prepare(
    fts_rewrite_prepare_kind kind,
    sqlite3 *db,
    const char *zSql,
    int nByte,
    unsigned int prepFlags,
    sqlite3_stmt **ppStmt,
    const char **pzTail,
    size_t bounded_len,
    size_t scan_len,
    int fanout_on,
    int dashboard_on
) {
    emby_rewrite_candidate candidate;
    emby_match_result mr = EMBY_MATCH_MISS;
    int rewrite_nbyte;
    const char *rewritten_tail = NULL;
    const char *mapped_tail = NULL;
    int rc;

    if (!fanout_on && !dashboard_on) {
        return call_downstream_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
    }
    if (!emby_sql_has_bytes(zSql, scan_len, "WithAncestors")) {
        return call_downstream_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
    }
    if (!emby_validate_single_statement(zSql, scan_len)) {
        return call_downstream_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
    }

    memset(&candidate, 0, sizeof(candidate));
    if (fanout_on) {
        mr = emby_try_fanout_matchers(db, zSql, bounded_len, scan_len, &candidate);
    }
    if (mr == EMBY_MATCH_MISS && dashboard_on) {
        mr = emby_match_episodes_latest(db, zSql, scan_len, &candidate);
        if (mr == EMBY_MATCH_MISS) {
            mr = emby_match_movies_latest(db, zSql, scan_len, &candidate);
        }
    }
    if (mr == EMBY_MATCH_MISS) {
        return call_downstream_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
    }
    if (mr == EMBY_MATCH_BUILD_FAILED) {
        obs_log_rewrite_skipped(db, "build_failed", candidate.mode);
        free(candidate.sql);
        return call_downstream_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
    }

    rewrite_nbyte = nByte < 0 ? -1 : (int)candidate.rewrite_len;
    if (ppStmt) *ppStmt = NULL;
    rc = call_downstream_prepare(
        kind, db, candidate.sql, rewrite_nbyte, prepFlags, ppStmt, &rewritten_tail
    );
    if (rc != SQLITE_OK) {
        obs_log_rewrite_skipped(db, "rewritten_prepare_failed", candidate.mode);
        free(candidate.sql);
        return emby_retry_original_after_rewrite_failure(
            kind, db, zSql, nByte, prepFlags, ppStmt, pzTail
        );
    }
    if (!emby_map_end_tail(zSql, candidate.sql, rewritten_tail, scan_len,
                           candidate.scan_out_len, &mapped_tail)) {
        obs_log_rewrite_skipped(db, "tail_mismatch", candidate.mode);
        free(candidate.sql);
        return emby_retry_original_after_rewrite_failure(
            kind, db, zSql, nByte, prepFlags, ppStmt, pzTail
        );
    }
    emby_log_rewrite_applied(
        db, zSql, scan_len, candidate.sql, candidate.scan_out_len, candidate.mode
    );
    if (pzTail) *pzTail = mapped_tail;
    free(candidate.sql);
    return rc;
}

static int probe_existing_function(sqlite3 *db, int *exists) {
    static const char probe_sql[] =
        "SELECT 1 FROM pragma_function_list WHERE name='dshadow_emby_fts_rewrite' COLLATE NOCASE LIMIT 1";
    sqlite3_stmt *probe_stmt = NULL;
    const char *probe_tail = NULL;
    int rc;
    int step_rc;
    int final_rc;

    *exists = 0;
    rc = sqlite3_prepare_v2_real(db, probe_sql, -1, &probe_stmt, &probe_tail);
    if (rc != SQLITE_OK) {
        if (probe_stmt) sqlite3_finalize(probe_stmt);
        return 0;
    }
    if (!probe_tail || probe_tail != probe_sql + strlen(probe_sql)) {
        sqlite3_finalize(probe_stmt);
        return 0;
    }
    step_rc = sqlite3_step(probe_stmt);
    if (step_rc == SQLITE_ROW) {
        *exists = 1;
    } else if (step_rc != SQLITE_DONE) {
        sqlite3_finalize(probe_stmt);
        return 0;
    }
    final_rc = sqlite3_finalize(probe_stmt);
    if (final_rc != SQLITE_OK) return 0;
    return 1;
}

static void *scalar_connection_state(sqlite3 *db) {
    return sqlite3_get_clientdata(db, EMBY_CLIENTDATA_KEY);
}

static int set_scalar_connection_state(sqlite3 *db, void *state) {
    return sqlite3_set_clientdata(db, EMBY_CLIENTDATA_KEY, state, NULL);
}

static int parse_scalar_atom(const unsigned char *z, int n, int start, int *end) {
    int p = start;
    if (p + 5 > n) return 0;
    if (z[p++] != '(') return 0;
    if (z[p++] != '"') return 0;
    if (p >= n || z[p] == '"') return 0;
    /* ASCII-only OR-to-AND is intentional; any atom byte >= 0x80 fails open to original OR text. */
    while (p < n && z[p] != '"') {
        if (z[p] >= 0x80 || fts_lex_is_space(z[p]) || z[p] == '(' || z[p] == ')' || z[p] == '*') {
            return 0;
        }
        p++;
    }
    if (p >= n || z[p++] != '"') return 0;
    if (p >= n || z[p++] != '*') return 0;
    if (p >= n || z[p++] != ')') return 0;
    *end = p;
    return 1;
}

static char *rewrite_scalar_or_to_and(const unsigned char *z, int n, int *out_n) {
    int p = 0;
    int atom_end = 0;
    int count = 0;
    char *out;
    char *w;

    if (n <= 0 || n > (int)EMBY_SCALAR_INPUT_CAP) return NULL;
    if (!parse_scalar_atom(z, n, p, &atom_end)) return NULL;
    count = 1;
    p = atom_end;
    while (p < n) {
        if (p + 4 > n || memcmp(z + p, " OR ", 4) != 0) return NULL;
        p += 4;
        if (!parse_scalar_atom(z, n, p, &atom_end)) return NULL;
        count++;
        p = atom_end;
    }
    if (count < 2) return NULL;

    *out_n = n + (count - 1);
    out = (char *)malloc((size_t)*out_n + 1);
    if (!out) return NULL;
    w = out;
    p = 0;
    for (;;) {
        parse_scalar_atom(z, n, p, &atom_end);
        memcpy(w, z + p, (size_t)(atom_end - p));
        w += atom_end - p;
        p = atom_end;
        if (p >= n) break;
        memcpy(w, " AND ", 5);
        w += 5;
        p += 4;
    }
    out[*out_n] = 0;
    return out;
}

static void emby_fts_scalar(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    void *owner = sqlite3_user_data(ctx);
    int value_type;
    const unsigned char *text;
    int n;
    int out_n = 0;
    char *rewritten;

    if (argc != 1) {
        sqlite3_result_null(ctx);
        return;
    }
#ifdef EMBY_FTS_REWRITE_TEST_API
    atomic_fetch_add_explicit(&g_emby_scalar_calls, 1, memory_order_acq_rel);
#endif
    value_type = sqlite3_value_type(argv[0]);
    if (value_type == SQLITE_NULL) {
        sqlite3_result_null(ctx);
        return;
    }
    if (value_type != SQLITE_TEXT) {
        sqlite3_result_value(ctx, argv[0]);
        return;
    }
    text = sqlite3_value_text(argv[0]);
    n = sqlite3_value_bytes(argv[0]);
    if (!text) {
        sqlite3_result_value(ctx, argv[0]);
        return;
    }
    if (n == (int)strlen(EMBY_OWNER_PROBE) &&
        memcmp(text, EMBY_OWNER_PROBE, (size_t)n) == 0 &&
        owner == &g_emby_scalar_owner_sentinel) {
        g_emby_owner_canary_seen = 1;
        sqlite3_result_value(ctx, argv[0]);
        return;
    }
    rewritten = rewrite_scalar_or_to_and(text, n, &out_n);
    if (!rewritten) {
        sqlite3_result_value(ctx, argv[0]);
        return;
    }
    sqlite3_result_text(ctx, rewritten, out_n, free);
}

static int ownership_canary_ok(sqlite3 *db) {
    static const char canary_sql[] =
        "SELECT dshadow_emby_fts_rewrite('__dshadow_emby_owner_probe__')";
    sqlite3_stmt *stmt = NULL;
    const char *tail = NULL;
    int rc;
    int step_rc;
    int ok = 0;

    g_emby_owner_canary_seen = 0;
    rc = sqlite3_prepare_v2_real(db, canary_sql, -1, &stmt, &tail);
    if (rc != SQLITE_OK) {
        if (stmt) sqlite3_finalize(stmt);
        g_emby_owner_canary_seen = 0;
        return 0;
    }
    if (!tail || tail != canary_sql + strlen(canary_sql)) {
        sqlite3_finalize(stmt);
        g_emby_owner_canary_seen = 0;
        return 0;
    }
    step_rc = sqlite3_step(stmt);
    if (step_rc == SQLITE_ROW) {
        ok = g_emby_owner_canary_seen;
    }
    rc = sqlite3_finalize(stmt);
    if (rc != SQLITE_OK) ok = 0;
    g_emby_owner_canary_seen = 0;
    return ok;
}

static int ensure_scalar_ready(sqlite3 *db) {
    void *state = scalar_connection_state(db);
    int exists = 0;
    int rc;

    if (state == &g_emby_scalar_bypass_sentinel) return 0;
    /* Per-prepare ready-sentinel re-verification trades cost for collision safety; do not remove it as dead work. */
    if (state == &g_emby_scalar_ready_sentinel) return ownership_canary_ok(db);
    if (state) return 0;

    /* The get/probe-create/set path is idempotent for one-thread-per-connection use; benign double-register races fail open. */
    if (!probe_existing_function(db, &exists)) return 0;
    if (exists) {
        (void)set_scalar_connection_state(db, &g_emby_scalar_bypass_sentinel);
        return 0;
    }

    rc = sqlite3_create_function_v2(
        db,
        EMBY_SCALAR_NAME,
        1,
        SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_DIRECTONLY,
        &g_emby_scalar_owner_sentinel,
        emby_fts_scalar,
        NULL,
        NULL,
        NULL
    );
    if (rc != SQLITE_OK) return 0;
    /* OOM-only safe fail-open: next candidate prepare converges this connection to permanent bypass. */
    if (set_scalar_connection_state(db, &g_emby_scalar_ready_sentinel) != SQLITE_OK) return 0;
    return ownership_canary_ok(db);
}

__attribute__((visibility("hidden"))) int emby_fts_rewrite_prepare(
    sqlite3 *db,
    const char *zSql,
    int nByte,
    unsigned int prepFlags,
    sqlite3_stmt **ppStmt,
    const char **pzTail,
    fts_rewrite_prepare_kind kind
) {
    size_t bounded_len = 0;
    size_t scan_len = 0;
    size_t scan_out_len = 0;
    size_t rewrite_len = 0;
    int rewrite_nbyte = -1;
    emby_span rhs_span;
    emby_slots slots;
    const char *raw_fn;
    const char *rewritten_tail = NULL;
    const char *mapped_tail = NULL;
    char *rewritten = NULL;
    int fts_on;
    int fanout_on;
    int dashboard_on;
    int rc;

    fts_on = emby_rewrite_enabled();
    fanout_on = emby_fanout_enabled();
    dashboard_on = emby_dashboard_enabled();

    if ((!fts_on && !fanout_on && !dashboard_on) || !db || !zSql || !ppStmt || nByte == 0) {
        return call_downstream_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
    }

    raw_fn = sqlite3_db_filename(db, "main");
    if (!fts_rewrite_db_basename_matches(raw_fn, EMBY_DB_BASENAME)) {
        return call_downstream_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
    }
    if (!emby_prepare_input_lengths(zSql, nByte, &bounded_len, &scan_len)) {
        return call_downstream_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
    }
    if (fts_on &&
        validate_single_statement_and_match(zSql, scan_len, &rhs_span) &&
        capture_membership_slots(zSql, scan_len, &slots)) {
        if (!ensure_scalar_ready(db)) {
            obs_log_rewrite_skipped(db, "scalar_unavailable", OBS_MODE_EMBY_FTS);
            return call_downstream_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
        }

        rewritten = emby_build_rewritten_sql(
            db, zSql, bounded_len, scan_len, &rhs_span, &slots, &scan_out_len, &rewrite_len
        );
        if (!rewritten) {
            obs_log_rewrite_skipped(db, "build_failed", OBS_MODE_EMBY_FTS);
            return call_downstream_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
        }

        rewrite_nbyte = nByte < 0 ? -1 : (int)rewrite_len;
        if (ppStmt) *ppStmt = NULL;
        rc = call_downstream_prepare(
            kind, db, rewritten, rewrite_nbyte, prepFlags, ppStmt, &rewritten_tail
        );
        if (rc != SQLITE_OK) {
            obs_log_rewrite_skipped(
                db, "rewritten_prepare_failed", OBS_MODE_EMBY_FTS
            );
            free(rewritten);
            return emby_retry_original_after_rewrite_failure(
                kind, db, zSql, nByte, prepFlags, ppStmt, pzTail
            );
        }
        if (!emby_map_end_tail(zSql, rewritten, rewritten_tail, scan_len, scan_out_len, &mapped_tail)) {
            obs_log_rewrite_skipped(db, "tail_mismatch", OBS_MODE_EMBY_FTS);
            free(rewritten);
            return emby_retry_original_after_rewrite_failure(
                kind, db, zSql, nByte, prepFlags, ppStmt, pzTail
            );
        }

        maybe_log_l1_l2_mismatch(db, zSql, &slots);
        emby_log_rewrite_applied(
            db, zSql, scan_len, rewritten, scan_out_len, OBS_MODE_EMBY_FTS
        );
        if (pzTail) *pzTail = mapped_tail;
        free(rewritten);
        return rc;
    }

    return emby_slow_rewrite_prepare(
        kind, db, zSql, nByte, prepFlags, ppStmt, pzTail,
        bounded_len, scan_len, fanout_on, dashboard_on
    );
}
