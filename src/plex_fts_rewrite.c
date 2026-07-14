#include "plex_fts_rewrite.h"
#include "fts_lex.h"
#include "observability.h"

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define REWRITE_OPEN "unlikely("
#define REWRITE_OPEN_LEN (sizeof(REWRITE_OPEN) - 1)
#define REWRITE_CLOSE_LEN 1
#define REWRITE_MAX_QUERY_BLOCKS 64
#define REWRITE_MAX_QUERY_DEPTH 64
#define PLEX_SLOT_CAP (64u * 1024u)
#define PLEX_TAG_INDEX_NAME "idx_dshadow_taggings_tag_id_metadata_item_id"
#define PLEX_ONDECK_INDEX_NAME "idx_dshadow_metadata_item_views_account_grandparent_guid"
#define PLEX_TAG_MEMBERSHIP_OPEN " AND metadata_items.id IN (SELECT metadata_item_id FROM taggings WHERE tag_id="
#define PLEX_TAG_MEMBERSHIP_CLOSE ")"
#define PLEX_ONDECK_COLUMN_COUNT 7

static const char PLEX_GUID_LIKE_SQL[] =
    "SELECT mt.`id` FROM metadata_items mt WHERE (mt.`guid` LIKE :1) LIMIT :2";
static const char PLEX_GUID_LIKE_REWRITTEN_SQL[] =
    "SELECT mt.`id` FROM metadata_items mt WHERE :1 IS NOT NULL AND (mt.`guid` LIKE :1) LIMIT :2";

static const char PLEX_ONDECK_AFTER_IDS[] =
    " and metadata_item_settings.view_count>0  and metadata_item_views.account_id=";
static const char PLEX_ONDECK_AFTER_ACCOUNT[] =
    " group by grandparents.id order by viewed_at desc";

typedef struct rewrite_target {
    const char *fts_table;
    const char *predicate_column;
    const char *db_basename;
} rewrite_target;

/* Plex-only target identifiers. A second target (e.g. Emby) is not a trivial config
   add: it needs a different DB path, MATCH syntax, and rewrite shape -- so treat this
   as identifier isolation, not an extension point. */
static const rewrite_target PLEX_FTS_TARGET = {
    "fts4_tag_titles_icu",
    "tag_type",
    "com.plexapp.plugins.library.db"
};

typedef struct rewrite_match {
    size_t open_off;
    size_t close_off;
} rewrite_match;

typedef struct plex_span {
    size_t start;
    size_t end;
} plex_span;

typedef enum plex_ondeck_selector_kind {
    PLEX_ONDECK_SELECTOR_IDS = 0,
    PLEX_ONDECK_SELECTOR_THRESHOLD = 1
} plex_ondeck_selector_kind;

typedef struct plex_ondeck_selector {
    plex_ondeck_selector_kind kind;
    union {
        plex_span id_list;
        plex_span threshold;
    } value;
} plex_ondeck_selector;

typedef enum query_clause {
    QUERY_CLAUSE_NONE = 0,
    QUERY_CLAUSE_SELECT,
    QUERY_CLAUSE_FROM,
    QUERY_CLAUSE_PREDICATE,
    QUERY_CLAUSE_OTHER
} query_clause;

typedef enum plex_match_result {
    PLEX_MATCH_MISS = 0,
    PLEX_MATCH_BUILT = 1,
    PLEX_MATCH_BUILD_FAILED = 2
} plex_match_result;

typedef enum plex_index_state {
    PLEX_INDEX_PROBE_ERROR = -1,
    PLEX_INDEX_MISSING = 0,
    PLEX_INDEX_PRESENT = 1
} plex_index_state;

typedef struct query_block_match {
    query_clause clause;
    int fts_match;
    int predicate_count;
    rewrite_match predicate;
} query_block_match;

typedef struct tag_block_match {
    query_clause clause;
    int metadata_join;
    int tag_join;
    int tag_id_count;
    int membership_count;
    int predicate_unsafe;
    rewrite_match predicate;
    plex_span tag_id;
} tag_block_match;

typedef struct plex_rewrite_candidate {
    char *sql;
    size_t scan_out_len;
    size_t rewrite_len;
    int expected_bind_count;
    int verify_bind_count;
    int expected_column_count;
    int verify_column_count;
} plex_rewrite_candidate;

__attribute__((visibility("hidden"))) SQLITE_API int sqlite3_prepare_real(
    sqlite3 *db,
    const char *zSql,
    int nByte,
    sqlite3_stmt **ppStmt,
    const char **pzTail
);
__attribute__((visibility("hidden"))) SQLITE_API int sqlite3_prepare_v2_real(
    sqlite3 *db,
    const char *zSql,
    int nByte,
    sqlite3_stmt **ppStmt,
    const char **pzTail
);
__attribute__((visibility("hidden"))) SQLITE_API int sqlite3_prepare_v3_real(
    sqlite3 *db,
    const char *zSql,
    int nByte,
    unsigned int prepFlags,
    sqlite3_stmt **ppStmt,
    const char **pzTail
);
extern int auto_extension_path_is_target(const char *raw_fn);

static void plex_log_rewrite_applied(
    sqlite3 *db,
    const char *source,
    size_t source_len,
    const char *rewritten,
    size_t rewritten_len,
    obs_rewrite_mode mode
);

static pthread_once_t g_plex_rewrite_once = PTHREAD_ONCE_INIT;
static atomic_int g_plex_rewrite_enabled;
static pthread_once_t g_plex_guid_like_rewrite_once = PTHREAD_ONCE_INIT;
static atomic_int g_plex_guid_like_rewrite_enabled;
static pthread_once_t g_plex_tag_rewrite_once = PTHREAD_ONCE_INIT;
static atomic_int g_plex_tag_rewrite_enabled;
static pthread_once_t g_plex_ondeck_rewrite_once = PTHREAD_ONCE_INIT;
static atomic_int g_plex_ondeck_rewrite_enabled;

static void plex_fts_rewrite_init_once(void) {
    const char *value = getenv("SQLITE3_DISABLE_PLEX_FTS_REWRITE");
    atomic_store_explicit(
        &g_plex_rewrite_enabled,
        (value && strcmp(value, "1") == 0) ? 0 : 1,
        memory_order_release
    );
}

static int plex_rewrite_enabled(void) {
    pthread_once(&g_plex_rewrite_once, plex_fts_rewrite_init_once);
    return atomic_load_explicit(&g_plex_rewrite_enabled, memory_order_acquire);
}

static void plex_guid_like_rewrite_init_once(void) {
    const char *value = getenv("SQLITE3_DISABLE_PLEX_GUID_LIKE_REWRITE");
    atomic_store_explicit(
        &g_plex_guid_like_rewrite_enabled,
        (value && strcmp(value, "0") == 0) ? 1 : 0,
        memory_order_release
    );
}

static int plex_guid_like_rewrite_enabled(void) {
    pthread_once(&g_plex_guid_like_rewrite_once, plex_guid_like_rewrite_init_once);
    return atomic_load_explicit(
        &g_plex_guid_like_rewrite_enabled, memory_order_acquire
    );
}

static void plex_tag_rewrite_init_once(void) {
    const char *value = getenv("SQLITE3_DISABLE_PLEX_TAGGINGS_REWRITE");
    atomic_store_explicit(
        &g_plex_tag_rewrite_enabled,
        (value && strcmp(value, "1") == 0) ? 0 : 1,
        memory_order_release
    );
}

static int plex_tag_rewrite_enabled(void) {
    pthread_once(&g_plex_tag_rewrite_once, plex_tag_rewrite_init_once);
    return atomic_load_explicit(&g_plex_tag_rewrite_enabled, memory_order_acquire);
}

static void plex_ondeck_rewrite_init_once(void) {
    const char *value = getenv("SQLITE3_DISABLE_PLEX_ONDECK_REWRITE");
    atomic_store_explicit(
        &g_plex_ondeck_rewrite_enabled,
        (value && strcmp(value, "0") == 0) ? 1 : 0,
        memory_order_release
    );
}

static int plex_ondeck_rewrite_enabled(void) {
    pthread_once(&g_plex_ondeck_rewrite_once, plex_ondeck_rewrite_init_once);
    return atomic_load_explicit(&g_plex_ondeck_rewrite_enabled, memory_order_acquire);
}

static int call_real_prepare(
    fts_rewrite_prepare_kind kind,
    sqlite3 *db,
    const char *zSql,
    int nByte,
    unsigned int prepFlags,
    sqlite3_stmt **ppStmt,
    const char **pzTail
) {
    switch (kind) {
        case FTS_REWRITE_PREPARE_LEGACY:
            return sqlite3_prepare_real(db, zSql, nByte, ppStmt, pzTail);
        case FTS_REWRITE_PREPARE_V2:
            return sqlite3_prepare_v2_real(db, zSql, nByte, ppStmt, pzTail);
        case FTS_REWRITE_PREPARE_V3:
            return sqlite3_prepare_v3_real(db, zSql, nByte, prepFlags, ppStmt, pzTail);
        default:
            return sqlite3_prepare_v2_real(db, zSql, nByte, ppStmt, pzTail);
    }
}

static int parse_match_value(fts_lex *lx, fts_lex_token *value) {
    *value = fts_lex_next_token(lx);
    if (!(value->type == FTS_LEX_TOK_NUMBER ||
          value->type == FTS_LEX_TOK_STRING ||
          value->type == FTS_LEX_TOK_PARAM)) {
        return 0;
    }
    return fts_lex_match_rhs_is_complete(lx, value);
}

static int is_predicate_boundary_keyword(const char *sql, const fts_lex_token *tok) {
    return fts_lex_token_text_eq(sql, tok, "and") ||
           fts_lex_token_text_eq(sql, tok, "or") ||
           fts_lex_token_text_eq(sql, tok, "group") ||
           fts_lex_token_text_eq(sql, tok, "having") ||
           fts_lex_token_text_eq(sql, tok, "order") ||
           fts_lex_token_text_eq(sql, tok, "limit") ||
           fts_lex_token_text_eq(sql, tok, "union") ||
           fts_lex_token_text_eq(sql, tok, "except") ||
           fts_lex_token_text_eq(sql, tok, "intersect") ||
           fts_lex_token_text_eq(sql, tok, "window");
}

static int is_predicate_value_boundary(const char *sql, const fts_lex_token *tok) {
    if (tok->type == FTS_LEX_TOK_EOF) return 1;
    if (tok->type == FTS_LEX_TOK_SYMBOL && tok->symbol == ')') return 1;
    if (tok->type == FTS_LEX_TOK_IDENT) return is_predicate_boundary_keyword(sql, tok);
    return 0;
}

static int is_predicate_left_boundary(const char *sql, const fts_lex_token *tok) {
    if (tok->type == FTS_LEX_TOK_EOF) return 1;
    if (tok->type == FTS_LEX_TOK_SYMBOL && tok->symbol == '(') return 1;
    if (tok->type == FTS_LEX_TOK_IDENT) {
        return fts_lex_token_text_eq(sql, tok, "and") ||
               fts_lex_token_text_eq(sql, tok, "or") ||
               fts_lex_token_text_eq(sql, tok, "where") ||
               fts_lex_token_text_eq(sql, tok, "on");
    }
    return 0;
}

static int parse_predicate_value(fts_lex *lx, fts_lex_token *value) {
    fts_lex look;
    fts_lex_token boundary;

    *value = fts_lex_next_token(lx);
    if (!(value->type == FTS_LEX_TOK_NUMBER || value->type == FTS_LEX_TOK_STRING ||
          (value->type == FTS_LEX_TOK_PARAM && value->end == value->start + 1))) {
        return 0;
    }
    if (value->type == FTS_LEX_TOK_PARAM) {
        if (value->end < lx->len &&
            lx->sql[value->end] >= '0' &&
            lx->sql[value->end] <= '9') {
            return 0;
        }
    }
    look = *lx;
    boundary = fts_lex_next_token(&look);
    if (boundary.type == FTS_LEX_TOK_ERROR) return 0;
    return is_predicate_value_boundary(lx->sql, &boundary);
}

static int token_starts_fts_match(const char *sql, const fts_lex *after_target) {
    fts_lex look = *after_target;
    fts_lex_token next = fts_lex_next_token(&look);
    fts_lex_token value;

    if (next.type == FTS_LEX_TOK_IDENT && fts_lex_token_text_eq(sql, &next, "match")) {
        return parse_match_value(&look, &value);
    }
    if (next.type == FTS_LEX_TOK_SYMBOL && next.symbol == '.') {
        fts_lex_token column = fts_lex_next_token(&look);
        fts_lex_token match = fts_lex_next_token(&look);
        if (column.type == FTS_LEX_TOK_IDENT &&
            match.type == FTS_LEX_TOK_IDENT &&
            fts_lex_token_text_eq(sql, &match, "match") &&
            parse_match_value(&look, &value)) {
            return 1;
        }
    }
    return 0;
}

static int token_is_keyword(
    const char *sql,
    const fts_lex_token *tok,
    const char *keyword
) {
    return tok->type == FTS_LEX_TOK_IDENT && fts_lex_token_text_eq(sql, tok, keyword);
}

static void update_query_clause(query_block_match *block, const char *sql, const fts_lex_token *tok) {
    if (fts_lex_token_text_eq(sql, tok, "select")) {
        block->clause = QUERY_CLAUSE_SELECT;
    } else if (fts_lex_token_text_eq(sql, tok, "from") || fts_lex_token_text_eq(sql, tok, "join")) {
        block->clause = QUERY_CLAUSE_FROM;
    } else if (fts_lex_token_text_eq(sql, tok, "where") || fts_lex_token_text_eq(sql, tok, "on")) {
        block->clause = QUERY_CLAUSE_PREDICATE;
    } else if (fts_lex_token_text_eq(sql, tok, "group") || fts_lex_token_text_eq(sql, tok, "having") ||
               fts_lex_token_text_eq(sql, tok, "order") || fts_lex_token_text_eq(sql, tok, "limit") ||
               fts_lex_token_text_eq(sql, tok, "union") || fts_lex_token_text_eq(sql, tok, "except") ||
               fts_lex_token_text_eq(sql, tok, "intersect") || fts_lex_token_text_eq(sql, tok, "window")) {
        block->clause = QUERY_CLAUSE_OTHER;
    }
}

static void update_tag_query_clause(tag_block_match *block, const char *sql, const fts_lex_token *tok) {
    query_block_match tmp;

    memset(&tmp, 0, sizeof(tmp));
    tmp.clause = block->clause;
    update_query_clause(&tmp, sql, tok);
    block->clause = tmp.clause;
}

static int plex_add_size(size_t *acc, size_t v) {
    if (v > SIZE_MAX - *acc) return 0;
    *acc += v;
    return 1;
}

static void plex_append_bytes(char **p, const char *z, size_t n) {
    memcpy(*p, z, n);
    *p += n;
}

static void plex_append_span(char **p, const char *sql, const plex_span *span) {
    plex_append_bytes(p, sql + span->start, span->end - span->start);
}

static int plex_validate_integer_span(const char *sql, const plex_span *span) {
    size_t i;

    if (span->end <= span->start) return 0;
    if (span->end - span->start > PLEX_SLOT_CAP) return 0;
    for (i = span->start; i < span->end; i++) {
        if (sql[i] < '0' || sql[i] > '9') return 0;
    }
    return 1;
}

static int plex_validate_integer_list_span(const char *sql, const plex_span *span) {
    size_t i;

    if (span->end <= span->start) return 0;
    if (span->end - span->start > PLEX_SLOT_CAP) return 0;
    for (i = span->start; i < span->end; i++) {
        if ((sql[i] >= '0' && sql[i] <= '9') || sql[i] == ',' ||
            fts_lex_is_space((unsigned char)sql[i])) {
            continue;
        }
        return 0;
    }
    return 1;
}

static int token_starts_column_ref(
    const char *sql,
    const fts_lex *after_table,
    const fts_lex_token *table,
    const char *want_table,
    const char *want_column,
    fts_lex *after_ref
) {
    fts_lex look;
    fts_lex_token dot;
    fts_lex_token column;

    if (table->type != FTS_LEX_TOK_IDENT ||
        !fts_lex_token_text_eq(sql, table, want_table)) {
        return 0;
    }
    look = *after_table;
    dot = fts_lex_next_token(&look);
    column = fts_lex_next_token(&look);
    if (dot.type != FTS_LEX_TOK_SYMBOL || dot.symbol != '.' ||
        column.type != FTS_LEX_TOK_IDENT ||
        !fts_lex_token_text_eq(sql, &column, want_column)) {
        return 0;
    }
    if (after_ref) *after_ref = look;
    return 1;
}

static int is_column_equality_boundary_keyword(const char *sql, const fts_lex_token *tok) {
    return fts_lex_token_text_eq(sql, tok, "and") ||
           fts_lex_token_text_eq(sql, tok, "or") ||
           fts_lex_token_text_eq(sql, tok, "where") ||
           fts_lex_token_text_eq(sql, tok, "on") ||
           fts_lex_token_text_eq(sql, tok, "join") ||
           fts_lex_token_text_eq(sql, tok, "left") ||
           fts_lex_token_text_eq(sql, tok, "right") ||
           fts_lex_token_text_eq(sql, tok, "inner") ||
           fts_lex_token_text_eq(sql, tok, "outer") ||
           fts_lex_token_text_eq(sql, tok, "cross") ||
           fts_lex_token_text_eq(sql, tok, "full") ||
           fts_lex_token_text_eq(sql, tok, "group") ||
           fts_lex_token_text_eq(sql, tok, "having") ||
           fts_lex_token_text_eq(sql, tok, "order") ||
           fts_lex_token_text_eq(sql, tok, "limit") ||
           fts_lex_token_text_eq(sql, tok, "union") ||
           fts_lex_token_text_eq(sql, tok, "except") ||
           fts_lex_token_text_eq(sql, tok, "intersect") ||
           fts_lex_token_text_eq(sql, tok, "window");
}

static int is_column_equality_boundary(const char *sql, const fts_lex_token *tok) {
    if (tok->type == FTS_LEX_TOK_EOF) return 1;
    if (tok->type == FTS_LEX_TOK_SYMBOL && tok->symbol == ')') return 1;
    if (tok->type == FTS_LEX_TOK_IDENT) return is_column_equality_boundary_keyword(sql, tok);
    return 0;
}

static int token_starts_column_equality_one_way(
    const char *sql,
    const fts_lex *after_table,
    const fts_lex_token *table,
    const char *left_table,
    const char *left_column,
    const char *right_table,
    const char *right_column
) {
    fts_lex after_left;
    fts_lex after_eq;
    fts_lex after_right;
    fts_lex_token eq;
    fts_lex_token right;
    fts_lex_token boundary;

    if (!token_starts_column_ref(sql, after_table, table, left_table, left_column, &after_left)) {
        return 0;
    }
    after_eq = after_left;
    eq = fts_lex_next_token(&after_eq);
    if (eq.type != FTS_LEX_TOK_SYMBOL || eq.symbol != '=') return 0;
    right = fts_lex_next_token(&after_eq);
    if (!token_starts_column_ref(sql, &after_eq, &right, right_table, right_column, &after_right)) {
        return 0;
    }
    boundary = fts_lex_next_token(&after_right);
    if (boundary.type == FTS_LEX_TOK_ERROR) return 0;
    return is_column_equality_boundary(sql, &boundary);
}

static int token_starts_column_equality_pair(
    const char *sql,
    const fts_lex *after_table,
    const fts_lex_token *table,
    const char *left_table,
    const char *left_column,
    const char *right_table,
    const char *right_column
) {
    return token_starts_column_equality_one_way(
               sql, after_table, table, left_table, left_column, right_table, right_column
           ) ||
           token_starts_column_equality_one_way(
               sql, after_table, table, right_table, right_column, left_table, left_column
           );
}

static int token_starts_column_equality_predicate(
    const char *sql,
    const fts_lex *after_table,
    const fts_lex_token *table,
    const fts_lex_token *prev,
    const char *left_table,
    const char *left_column,
    const char *right_table,
    const char *right_column
) {
    if (!token_starts_column_equality_pair(
            sql, after_table, table, left_table, left_column, right_table, right_column
        )) {
        return 0;
    }
    return is_predicate_left_boundary(sql, prev) ? 1 : -1;
}

static int token_starts_any_column_ref(
    const char *sql,
    const fts_lex *after_table,
    const fts_lex_token *table
) {
    fts_lex look;
    fts_lex_token dot;
    fts_lex_token column;

    if (table->type != FTS_LEX_TOK_IDENT) return 0;
    look = *after_table;
    dot = fts_lex_next_token(&look);
    column = fts_lex_next_token(&look);
    return dot.type == FTS_LEX_TOK_SYMBOL && dot.symbol == '.' &&
           column.type == FTS_LEX_TOK_IDENT;
}

static int token_starts_tag_id_predicate(
    const char *sql,
    const fts_lex *after_table,
    const fts_lex_token *table,
    const fts_lex_token *prev,
    rewrite_match *predicate,
    plex_span *tag_id
) {
    fts_lex after_ref;
    fts_lex after_eq;
    fts_lex_token eq;
    fts_lex_token value;

    if (!token_starts_column_ref(sql, after_table, table, "tags", "id", &after_ref)) {
        return 0;
    }
    after_eq = after_ref;
    eq = fts_lex_next_token(&after_eq);
    if (eq.type != FTS_LEX_TOK_SYMBOL || eq.symbol != '=') return 0;
    if (!is_predicate_left_boundary(sql, prev)) return -1;
    value = fts_lex_next_token(&after_eq);
    if (token_starts_any_column_ref(sql, &after_eq, &value)) return 0;
    after_eq = after_ref;
    (void)fts_lex_next_token(&after_eq);
    if (!parse_predicate_value(&after_eq, &value)) return -1;
    if (value.type != FTS_LEX_TOK_NUMBER) return -1;
    tag_id->start = value.start;
    tag_id->end = value.end;
    if (!plex_validate_integer_span(sql, tag_id)) return -1;
    predicate->open_off = table->start;
    predicate->close_off = value.end;
    return 1;
}

static int token_starts_tag_membership_conjunct(
    const char *sql,
    const fts_lex *after_table,
    const fts_lex_token *table
) {
    fts_lex look;
    fts_lex after_ref;
    fts_lex_token in_tok;
    fts_lex_token open_tok;
    fts_lex_token select_tok;
    fts_lex_token metadata_item_id_tok;
    fts_lex_token from_tok;
    fts_lex_token taggings_tok;
    fts_lex_token where_tok;
    fts_lex_token tag_id_tok;

    if (!token_starts_column_ref(sql, after_table, table, "metadata_items", "id", &after_ref)) {
        return 0;
    }
    look = after_ref;
    in_tok = fts_lex_next_token(&look);
    open_tok = fts_lex_next_token(&look);
    select_tok = fts_lex_next_token(&look);
    metadata_item_id_tok = fts_lex_next_token(&look);
    from_tok = fts_lex_next_token(&look);
    taggings_tok = fts_lex_next_token(&look);
    where_tok = fts_lex_next_token(&look);
    tag_id_tok = fts_lex_next_token(&look);
    return token_is_keyword(sql, &in_tok, "in") &&
           open_tok.type == FTS_LEX_TOK_SYMBOL && open_tok.symbol == '(' &&
           token_is_keyword(sql, &select_tok, "select") &&
           token_is_keyword(sql, &metadata_item_id_tok, "metadata_item_id") &&
           token_is_keyword(sql, &from_tok, "from") &&
           token_is_keyword(sql, &taggings_tok, "taggings") &&
           token_is_keyword(sql, &where_tok, "where") &&
           token_is_keyword(sql, &tag_id_tok, "tag_id");
}

static int token_is_value_comparison_operator(const char *sql, const fts_lex_token *tok) {
    if (tok->type == FTS_LEX_TOK_SYMBOL) {
        return tok->symbol == '=' || tok->symbol == '<' ||
               tok->symbol == '>' || tok->symbol == '!';
    }
    if (tok->type != FTS_LEX_TOK_IDENT) return 0;
    return fts_lex_token_text_eq(sql, tok, "is") ||
           fts_lex_token_text_eq(sql, tok, "in") ||
           fts_lex_token_text_eq(sql, tok, "like") ||
           fts_lex_token_text_eq(sql, tok, "glob") ||
           fts_lex_token_text_eq(sql, tok, "match") ||
           fts_lex_token_text_eq(sql, tok, "between") ||
           fts_lex_token_text_eq(sql, tok, "regexp");
}

static int token_opens_value_scope(const char *sql, const fts_lex_token *prev) {
    if (prev->type == FTS_LEX_TOK_SYMBOL) {
        return prev->symbol != '(';
    }
    if (prev->type != FTS_LEX_TOK_IDENT) return 0;
    return !(fts_lex_token_text_eq(sql, prev, "and") ||
             fts_lex_token_text_eq(sql, prev, "or") ||
             fts_lex_token_text_eq(sql, prev, "where") ||
             fts_lex_token_text_eq(sql, prev, "on"));
}

static int token_closes_into_value_scope(const char *sql, const fts_lex *after_close) {
    fts_lex look = *after_close;
    fts_lex_token next = fts_lex_next_token(&look);

    if (next.type == FTS_LEX_TOK_ERROR) return 1;
    return token_is_value_comparison_operator(sql, &next);
}

static int plex_trailing_space_only(const char *sql, size_t len, size_t pos) {
    size_t i;

    if (pos > len) return 0;
    for (i = pos; i < len; i++) {
        if (!fts_lex_is_space((unsigned char)sql[i])) return 0;
    }
    return 1;
}

static int match_target_prefix_query(
    const rewrite_target *target,
    const char *sql,
    size_t len,
    rewrite_match *out
) {
    fts_lex lx;
    fts_lex_token prev;
    query_block_match blocks[REWRITE_MAX_QUERY_BLOCKS];
    int block_at_depth[REWRITE_MAX_QUERY_DEPTH];
    int block_count = 0;
    int total_predicate_count = 0;
    int depth = 0;
    int i;

    fts_lex_init(&lx, sql, len, FTS_LEX_PARAM_BARE_QMARK);
    memset(blocks, 0, sizeof(blocks));
    for (i = 0; i < REWRITE_MAX_QUERY_DEPTH; i++) block_at_depth[i] = -1;
    memset(&prev, 0, sizeof(prev));
    prev.type = FTS_LEX_TOK_EOF;

    for (;;) {
        fts_lex look;
        fts_lex_token tok;
        int current_block;

        tok = fts_lex_next_token(&lx);
        if (tok.type == FTS_LEX_TOK_ERROR) return 0;
        if (tok.type == FTS_LEX_TOK_EOF) {
            if (depth != 0) return 0;
            break;
        }
        if (tok.type == FTS_LEX_TOK_SYMBOL && tok.symbol == ';') return 0;

        current_block = block_at_depth[depth];
        if (tok.type == FTS_LEX_TOK_SYMBOL && tok.symbol == '(') {
            if (depth + 1 >= REWRITE_MAX_QUERY_DEPTH) return 0;
            block_at_depth[depth + 1] = current_block;
            depth++;
            prev = tok;
            continue;
        }
        if (tok.type == FTS_LEX_TOK_SYMBOL && tok.symbol == ')') {
            if (depth == 0) return 0;
            block_at_depth[depth] = -1;
            depth--;
            prev = tok;
            continue;
        }
        if (token_is_keyword(sql, &tok, "select")) {
            if (block_count >= REWRITE_MAX_QUERY_BLOCKS) return 0;
            current_block = block_count++;
            memset(&blocks[current_block], 0, sizeof(blocks[current_block]));
            blocks[current_block].clause = QUERY_CLAUSE_SELECT;
            block_at_depth[depth] = current_block;
            prev = tok;
            continue;
        }

        current_block = block_at_depth[depth];
        if (current_block >= 0 && tok.type == FTS_LEX_TOK_IDENT) {
            update_query_clause(&blocks[current_block], sql, &tok);
            if (fts_lex_token_text_eq(sql, &tok, target->fts_table) &&
                token_starts_fts_match(sql, &lx)) {
                blocks[current_block].fts_match = 1;
            }
        }
        if (tok.type == FTS_LEX_TOK_IDENT && fts_lex_token_text_eq(sql, &tok, target->predicate_column)) {
            fts_lex_token eq;
            fts_lex_token value;

            look = lx;
            eq = fts_lex_next_token(&look);
            if (eq.type == FTS_LEX_TOK_ERROR) return 0;
            if (eq.type == FTS_LEX_TOK_SYMBOL && eq.symbol == '=') {
                if (!is_predicate_left_boundary(sql, &prev)) return 0;
                if (!parse_predicate_value(&look, &value)) return 0;
                total_predicate_count++;
                if (total_predicate_count > 1) return 0;
                if (current_block >= 0 &&
                    blocks[current_block].clause == QUERY_CLAUSE_PREDICATE) {
                    blocks[current_block].predicate_count++;
                    blocks[current_block].predicate.open_off = tok.start;
                    blocks[current_block].predicate.close_off = value.end;
                }
            }
        }
        prev = tok;
    }

    if (total_predicate_count != 1) return 0;
    for (i = 0; i < block_count; i++) {
        if (blocks[i].fts_match && blocks[i].predicate_count == 1) {
            *out = blocks[i].predicate;
            return 1;
        }
    }
    return 0;
}

static plex_index_state plex_probe_index_sql(sqlite3 *db, const char *probe_sql) {
    sqlite3_stmt *stmt = NULL;
    const char *tail = NULL;
    int rc;
    int step_rc;
    int final_rc;
    int ready = 0;

    rc = sqlite3_prepare_v2_real(db, probe_sql, -1, &stmt, &tail);
    if (rc != SQLITE_OK) {
        if (stmt) sqlite3_finalize(stmt);
        return PLEX_INDEX_PROBE_ERROR;
    }
    if (!tail || tail != probe_sql + strlen(probe_sql)) {
        sqlite3_finalize(stmt);
        return PLEX_INDEX_PROBE_ERROR;
    }
    step_rc = sqlite3_step(stmt);
    if (step_rc == SQLITE_ROW) {
        ready = 1;
    } else if (step_rc != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return PLEX_INDEX_PROBE_ERROR;
    }
    final_rc = sqlite3_finalize(stmt);
    if (final_rc != SQLITE_OK) return PLEX_INDEX_PROBE_ERROR;
    return ready ? PLEX_INDEX_PRESENT : PLEX_INDEX_MISSING;
}

static plex_index_state plex_tag_index_ready(sqlite3 *db) {
    static const char probe_sql[] =
        "SELECT 1 FROM sqlite_master "
        "WHERE type='index' "
        "AND name='" PLEX_TAG_INDEX_NAME "' "
        "AND tbl_name='taggings' COLLATE NOCASE "
        "LIMIT 1";

    return plex_probe_index_sql(db, probe_sql);
}

static plex_index_state plex_ondeck_index_ready(sqlite3 *db) {
    static const char probe_sql[] =
        "SELECT 1 FROM sqlite_master "
        "WHERE type='index' "
        "AND name='" PLEX_ONDECK_INDEX_NAME "' "
        "AND tbl_name='metadata_item_views' COLLATE NOCASE "
        "LIMIT 1";

    return plex_probe_index_sql(db, probe_sql);
}

static char *plex_build_tag_membership_sql(
    sqlite3 *db,
    const char *zSql,
    size_t bounded_len,
    size_t scan_len,
    const rewrite_match *predicate,
    const plex_span *tag_id,
    size_t *scan_out_len,
    size_t *rewrite_len
) {
    const size_t open_len = sizeof(PLEX_TAG_MEMBERSHIP_OPEN) - 1;
    const size_t close_len = sizeof(PLEX_TAG_MEMBERSHIP_CLOSE) - 1;
    size_t tag_id_len;
    size_t out_len = bounded_len;
    size_t limit;
    char *rewritten;
    char *p;

    if (predicate->open_off > predicate->close_off ||
        predicate->close_off > scan_len ||
        tag_id->start > tag_id->end ||
        tag_id->end > predicate->close_off) {
        return NULL;
    }
    tag_id_len = tag_id->end - tag_id->start;
    if (!plex_add_size(&out_len, open_len) ||
        !plex_add_size(&out_len, tag_id_len) ||
        !plex_add_size(&out_len, close_len)) {
        return NULL;
    }
    limit = (size_t)sqlite3_limit(db, SQLITE_LIMIT_SQL_LENGTH, -1);
    if (out_len > limit || out_len > (size_t)INT_MAX) return NULL;

    rewritten = (char *)malloc(out_len + 1);
    if (!rewritten) return NULL;
    p = rewritten;
    plex_append_bytes(&p, zSql, predicate->close_off);
    plex_append_bytes(&p, PLEX_TAG_MEMBERSHIP_OPEN, open_len);
    plex_append_span(&p, zSql, tag_id);
    plex_append_bytes(&p, PLEX_TAG_MEMBERSHIP_CLOSE, close_len);
    plex_append_bytes(&p, zSql + predicate->close_off, bounded_len - predicate->close_off);
    rewritten[out_len] = 0;
    *scan_out_len = scan_len + open_len + tag_id_len + close_len;
    *rewrite_len = out_len;
    return rewritten;
}

static plex_match_result match_tag_membership_query(
    sqlite3 *db,
    const char *sql,
    size_t bounded_len,
    size_t scan_len,
    plex_rewrite_candidate *candidate
) {
    fts_lex lx;
    fts_lex_token prev;
    tag_block_match blocks[REWRITE_MAX_QUERY_BLOCKS];
    int block_at_depth[REWRITE_MAX_QUERY_DEPTH];
    int unsafe_depth[REWRITE_MAX_QUERY_DEPTH];
    int block_count = 0;
    int total_tag_predicates = 0;
    int invalid_predicate = 0;
    int depth = 0;
    int i;

    fts_lex_init(&lx, sql, scan_len, FTS_LEX_PARAM_NUMBERED_OR_NAMED);
    memset(blocks, 0, sizeof(blocks));
    for (i = 0; i < REWRITE_MAX_QUERY_DEPTH; i++) block_at_depth[i] = -1;
    memset(unsafe_depth, 0, sizeof(unsafe_depth));
    memset(&prev, 0, sizeof(prev));
    prev.type = FTS_LEX_TOK_EOF;

    for (;;) {
        fts_lex_token tok;
        int current_block;

        tok = fts_lex_next_token(&lx);
        if (tok.type == FTS_LEX_TOK_ERROR) return PLEX_MATCH_MISS;
        if (tok.type == FTS_LEX_TOK_EOF) {
            if (depth != 0) return PLEX_MATCH_MISS;
            break;
        }
        if (tok.type == FTS_LEX_TOK_SYMBOL && tok.symbol == ';') return PLEX_MATCH_MISS;

        current_block = block_at_depth[depth];
        if (tok.type == FTS_LEX_TOK_SYMBOL && tok.symbol == '(') {
            if (depth + 1 >= REWRITE_MAX_QUERY_DEPTH) return PLEX_MATCH_MISS;
            block_at_depth[depth + 1] = current_block;
            unsafe_depth[depth + 1] = unsafe_depth[depth];
            if (current_block >= 0 &&
                blocks[current_block].clause == QUERY_CLAUSE_PREDICATE &&
                token_opens_value_scope(sql, &prev)) {
                unsafe_depth[depth + 1] = 1;
            }
            depth++;
            prev = tok;
            continue;
        }
        if (tok.type == FTS_LEX_TOK_SYMBOL && tok.symbol == ')') {
            if (depth == 0) return PLEX_MATCH_MISS;
            if (current_block >= 0 &&
                blocks[current_block].clause == QUERY_CLAUSE_PREDICATE &&
                token_closes_into_value_scope(sql, &lx)) {
                blocks[current_block].predicate_unsafe = 1;
            }
            block_at_depth[depth] = -1;
            unsafe_depth[depth] = 0;
            depth--;
            prev = tok;
            continue;
        }
        if (token_is_keyword(sql, &tok, "select")) {
            if (block_count >= REWRITE_MAX_QUERY_BLOCKS) return PLEX_MATCH_MISS;
            current_block = block_count++;
            memset(&blocks[current_block], 0, sizeof(blocks[current_block]));
            blocks[current_block].clause = QUERY_CLAUSE_SELECT;
            block_at_depth[depth] = current_block;
            prev = tok;
            continue;
        }

        current_block = block_at_depth[depth];
        if (current_block >= 0 && tok.type == FTS_LEX_TOK_IDENT) {
            int tag_predicate;
            int join_predicate;

            update_tag_query_clause(&blocks[current_block], sql, &tok);
            if (blocks[current_block].clause == QUERY_CLAUSE_PREDICATE &&
                (fts_lex_token_text_eq(sql, &tok, "or") ||
                 fts_lex_token_text_eq(sql, &tok, "not"))) {
                blocks[current_block].predicate_unsafe = 1;
            }
            if (fts_lex_token_text_eq(sql, &tok, "fts4_metadata_titles_icu")) {
                return PLEX_MATCH_MISS;
            }
            if (blocks[current_block].clause == QUERY_CLAUSE_PREDICATE) {
                join_predicate = token_starts_column_equality_predicate(
                    sql, &lx, &tok,
                    &prev,
                    "taggings", "metadata_item_id",
                    "metadata_items", "id"
                );
                if (join_predicate < 0 || (join_predicate > 0 && unsafe_depth[depth])) {
                    invalid_predicate = 1;
                } else if (join_predicate > 0) {
                    blocks[current_block].metadata_join = 1;
                }
            }
            if (blocks[current_block].clause == QUERY_CLAUSE_PREDICATE) {
                join_predicate = token_starts_column_equality_predicate(
                    sql, &lx, &tok,
                    &prev,
                    "taggings", "tag_id",
                    "tags", "id"
                );
                if (join_predicate < 0 || (join_predicate > 0 && unsafe_depth[depth])) {
                    invalid_predicate = 1;
                } else if (join_predicate > 0) {
                    blocks[current_block].tag_join = 1;
                }
            }
            if (blocks[current_block].clause == QUERY_CLAUSE_PREDICATE &&
                token_starts_tag_membership_conjunct(sql, &lx, &tok)) {
                blocks[current_block].membership_count++;
            }
            if (blocks[current_block].clause == QUERY_CLAUSE_PREDICATE) {
                rewrite_match predicate;
                plex_span tag_id;

                tag_predicate = token_starts_tag_id_predicate(
                    sql, &lx, &tok, &prev, &predicate, &tag_id
                );
                if (tag_predicate < 0 || (tag_predicate > 0 && unsafe_depth[depth])) {
                    invalid_predicate = 1;
                } else if (tag_predicate > 0) {
                    total_tag_predicates++;
                    blocks[current_block].tag_id_count++;
                    blocks[current_block].predicate = predicate;
                    blocks[current_block].tag_id = tag_id;
                }
            }
        }
        prev = tok;
    }

    if (invalid_predicate || total_tag_predicates != 1) return PLEX_MATCH_MISS;
    for (i = 0; i < block_count; i++) {
        plex_index_state index_state;

        if (!blocks[i].metadata_join || !blocks[i].tag_join ||
            blocks[i].tag_id_count != 1 || blocks[i].membership_count != 0 ||
            blocks[i].predicate_unsafe) {
            continue;
        }
        index_state = plex_tag_index_ready(db);
        if (index_state != PLEX_INDEX_PRESENT) {
            if (index_state == PLEX_INDEX_MISSING) {
                obs_log_index_missing(db, OBS_MODE_PLEX_TAGGINGS);
            } else {
                obs_log_rewrite_skipped(
                    db, "index_probe_error", OBS_MODE_PLEX_TAGGINGS
                );
            }
            return PLEX_MATCH_MISS;
        }
        candidate->sql = plex_build_tag_membership_sql(
            db, sql, bounded_len, scan_len, &blocks[i].predicate, &blocks[i].tag_id,
            &candidate->scan_out_len, &candidate->rewrite_len
        );
        return candidate->sql ? PLEX_MATCH_BUILT : PLEX_MATCH_BUILD_FAILED;
    }
    return PLEX_MATCH_MISS;
}

static int plex_expect_bytes(
    const char *sql,
    size_t len,
    size_t pos,
    const char *want,
    size_t *after
) {
    size_t want_len = strlen(want);

    if (pos > len || want_len > len - pos) return 0;
    if (memcmp(sql + pos, want, want_len) != 0) return 0;
    *after = pos + want_len;
    return 1;
}

static int plex_parse_ondeck_value_at(
    const char *sql,
    size_t len,
    size_t pos,
    int variable_limit,
    int *max_index,
    plex_span *slot,
    size_t *after
) {
    fts_lex lx;
    fts_lex_token tok;
    size_t i;

    if (pos > len) return 0;
    fts_lex_init(&lx, sql, len, FTS_LEX_PARAM_SQLITE_VARIABLE);
    lx.pos = pos;
    tok = fts_lex_next_token(&lx);
    if (tok.start != pos) return 0;
    if (tok.type == FTS_LEX_TOK_NUMBER) {
        slot->start = tok.start;
        slot->end = tok.end;
        if (!plex_validate_integer_span(sql, slot)) return 0;
        *after = tok.end;
        return 1;
    }
    if (tok.type != FTS_LEX_TOK_PARAM) return 0;
    if (sql[tok.start] != '?') return 0;
    if (tok.end == tok.start + 1) {
        if (*max_index == INT_MAX || *max_index >= variable_limit) return 0;
        (*max_index)++;
    } else {
        int value = 0;

        if (sql[tok.start + 1] < '1' || sql[tok.start + 1] > '9') return 0;
        for (i = tok.start + 1; i < tok.end; i++) {
            int digit;
            if (sql[i] < '0' || sql[i] > '9') return 0;
            digit = sql[i] - '0';
            if (value > (INT_MAX - digit) / 10) return 0;
            value = value * 10 + digit;
        }
        if (value > variable_limit) return 0;
        if (value > *max_index) *max_index = value;
    }

    slot->start = tok.start;
    slot->end = tok.end;
    *after = tok.end;
    return 1;
}

static void plex_log_ondeck_miss(
    sqlite3 *db,
    const char *sql,
    size_t scan_len,
    obs_miss_reason reason,
    const char *sub_reason
) {
    uint64_t shape = 0;

    if (!obs_is_disabled()) shape = fts_lex_shape_key(sql, scan_len);
    obs_log_rewrite_miss(
        OBS_MODE_PLEX_ONDECK, reason, sub_reason,
        db, sql, scan_len, shape
    );
}

static int plex_ondeck_is_per_guid_variant(
    const char *sql,
    size_t scan_len,
    size_t pos,
    int variable_limit
) {
    static const char guid_open[] = " and grandparents.guid='";
    plex_span account_id;
    int max_index = 0;
    size_t guid_start;

    if (!plex_expect_bytes(sql, scan_len, pos, " and true", &pos)) return 0;
    if (!plex_expect_bytes(
            sql, scan_len, pos, PLEX_ONDECK_AFTER_IDS, &pos
        )) {
        return 0;
    }
    if (!plex_parse_ondeck_value_at(
            sql, scan_len, pos, variable_limit, &max_index, &account_id, &pos
        )) {
        return 0;
    }
    if (!plex_expect_bytes(sql, scan_len, pos, guid_open, &pos)) return 0;
    guid_start = pos;
    while (pos < scan_len && sql[pos] != '\'') pos++;
    if (pos == guid_start || pos >= scan_len) return 0;
    pos++;
    if (!plex_expect_bytes(sql, scan_len, pos, " ", &pos)) return 0;
    if (!plex_expect_bytes(
            sql, scan_len, pos, PLEX_ONDECK_AFTER_ACCOUNT, &pos
        )) {
        return 0;
    }
    return plex_trailing_space_only(sql, scan_len, pos);
}

static int plex_parse_integer_list_at(
    const char *sql,
    size_t len,
    size_t pos,
    plex_span *slot,
    size_t *after
) {
    fts_lex lx;
    fts_lex_token tok;
    int count = 0;

    if (pos > len) return 0;
    fts_lex_init(&lx, sql, len, FTS_LEX_PARAM_NUMBERED_OR_NAMED);
    lx.pos = pos;
    slot->start = pos;
    for (;;) {
        tok = fts_lex_next_token(&lx);
        if (tok.type != FTS_LEX_TOK_NUMBER) return 0;
        count++;
        tok = fts_lex_next_token(&lx);
        if (tok.type == FTS_LEX_TOK_SYMBOL && tok.symbol == ',') continue;
        if (tok.type == FTS_LEX_TOK_SYMBOL && tok.symbol == ')') {
            slot->end = tok.start;
            if (!plex_validate_integer_list_span(sql, slot)) return 0;
            *after = tok.end;
            return count > 0;
        }
        return 0;
    }
}

static int plex_parse_ondeck_threshold_at(
    const char *sql,
    size_t len,
    size_t pos,
    plex_span *slot,
    size_t *after
) {
    uint64_t value = 0;
    size_t i = pos;

    if (pos >= len || sql[pos] < '0' || sql[pos] > '9') return 0;
    while (i < len && sql[i] >= '0' && sql[i] <= '9') {
        unsigned int digit = (unsigned int)(sql[i] - '0');

        if (i - pos >= PLEX_SLOT_CAP) return 0;
        if (value > ((uint64_t)INT64_MAX - digit) / 10u) return 0;
        value = value * 10u + digit;
        i++;
    }
    slot->start = pos;
    slot->end = i;
    *after = i;
    return 1;
}

static char *plex_build_ondeck_sql(
    sqlite3 *db,
    const char *sql,
    const plex_span *section_id,
    const plex_ondeck_selector *selector,
    const plex_span *account_id,
    size_t *rewrite_len
) {
    static const char tpl0[] =
        "SELECT grandparents_id AS id,\n"
        "       originally_available_at AS originally_available_at,\n"
        "       parent_index AS parent_index,\n"
        "       metadata_item_views_index AS \"index\",\n"
        "       +viewed_at AS \"max(viewed_at)\",\n"
        "       library_section_id AS library_section_id,\n"
        "       grandparents_extra_data AS extra_data\n"
        "FROM (\n"
        "  SELECT grandparents.id AS grandparents_id,\n"
        "         metadata_item_views.originally_available_at AS originally_available_at,\n"
        "         metadata_item_views.parent_index AS parent_index,\n"
        "         metadata_item_views.`index` AS metadata_item_views_index,\n"
        "         metadata_item_views.viewed_at AS viewed_at,\n"
        "         grandparents.library_section_id AS library_section_id,\n"
        "         grandparentsSettings.extra_data AS grandparents_extra_data,\n"
        /* SQLite leaves bare columns undefined when max(viewed_at) ties. The final
           keys choose one representative deterministically; any different values,
           including NULL originally_available_at, are inherent in the vendor tie.
           Keep it non-NULL-hardened: the consumer must tolerate vendor results. */
        "         row_number() OVER (PARTITION BY grandparents.id ORDER BY metadata_item_views.viewed_at DESC, metadata_item_views.id DESC, grandparentsSettings.id DESC, metadata_item_settings.id DESC) AS dshadow_on_deck_rank\n"
        "  FROM metadata_items AS grandparents\n"
        "  JOIN metadata_item_views\n"
        "  JOIN metadata_item_settings\n"
        "  JOIN metadata_item_settings AS grandparentsSettings\n"
        "  WHERE grandparents.guid=metadata_item_views.grandparent_guid\n"
        "    AND metadata_item_settings.guid=metadata_item_views.guid\n"
        "    AND metadata_item_views.account_id=metadata_item_settings.account_id\n"
        "    AND grandparentsSettings.guid=metadata_item_views.grandparent_guid\n"
        "    AND metadata_item_views.account_id=grandparentsSettings.account_id\n"
        "    AND metadata_item_views.library_section_id=";
    static const char ids_open[] =
        "\n"
        "    AND grandparents.id IN (";
    static const char ids_close[] = ")";
    static const char threshold_open[] =
        "\n"
        "    AND metadata_item_views.viewed_at > ";
    static const char predicate_tail[] =
        "\n"
        "    AND metadata_item_settings.view_count>0\n"
        "    AND metadata_item_views.account_id=";
    static const char tpl3[] =
        "\n"
        ") AS dshadow_on_deck_ranked\n"
        "WHERE dshadow_on_deck_rank=1\n"
        "ORDER BY viewed_at DESC, grandparents_id DESC;";
    size_t out_len = 0;
    size_t limit;
    char *rewritten;
    char *p;
    size_t section_len;
    size_t account_len;
    const char *selector_open;
    size_t selector_open_len;
    const char *selector_close;
    size_t selector_close_len;
    const plex_span *selector_value;

    section_len = section_id->end - section_id->start;
    account_len = account_id->end - account_id->start;
    if (selector->kind == PLEX_ONDECK_SELECTOR_IDS) {
        selector_open = ids_open;
        selector_open_len = sizeof(ids_open) - 1;
        selector_close = ids_close;
        selector_close_len = sizeof(ids_close) - 1;
        selector_value = &selector->value.id_list;
    } else if (selector->kind == PLEX_ONDECK_SELECTOR_THRESHOLD) {
        selector_open = threshold_open;
        selector_open_len = sizeof(threshold_open) - 1;
        selector_close = "";
        selector_close_len = 0;
        selector_value = &selector->value.threshold;
    } else {
        return NULL;
    }

    if (!plex_add_size(&out_len, sizeof(tpl0) - 1) ||
        !plex_add_size(&out_len, section_len) ||
        !plex_add_size(&out_len, selector_open_len) ||
        !plex_add_size(&out_len, selector_value->end - selector_value->start) ||
        !plex_add_size(&out_len, selector_close_len) ||
        !plex_add_size(&out_len, sizeof(predicate_tail) - 1) ||
        !plex_add_size(&out_len, account_len) ||
        !plex_add_size(&out_len, sizeof(tpl3) - 1)) {
        return NULL;
    }
    limit = (size_t)sqlite3_limit(db, SQLITE_LIMIT_SQL_LENGTH, -1);
    if (out_len > limit || out_len > (size_t)INT_MAX) return NULL;

    rewritten = (char *)malloc(out_len + 1);
    if (!rewritten) return NULL;
    p = rewritten;
    plex_append_bytes(&p, tpl0, sizeof(tpl0) - 1);
    plex_append_span(&p, sql, section_id);
    plex_append_bytes(&p, selector_open, selector_open_len);
    plex_append_span(&p, sql, selector_value);
    plex_append_bytes(&p, selector_close, selector_close_len);
    plex_append_bytes(&p, predicate_tail, sizeof(predicate_tail) - 1);
    plex_append_span(&p, sql, account_id);
    plex_append_bytes(&p, tpl3, sizeof(tpl3) - 1);
    rewritten[out_len] = 0;
    *rewrite_len = out_len;
    return rewritten;
}

static plex_match_result match_ondeck_query(
    sqlite3 *db,
    const char *sql,
    size_t scan_len,
    plex_rewrite_candidate *candidate
) {
    static const char head[] =
        "select grandparents.id,metadata_item_views.originally_available_at,metadata_item_views.parent_index,metadata_item_views.`index`,max(viewed_at),grandparents.library_section_id,grandparentsSettings.extra_data from metadata_item_views indexed by index_metadata_item_views_on_guid join metadata_items as grandparents indexed by index_metadata_items_on_guid on grandparents.guid=grandparent_guid join metadata_item_settings indexed by index_metadata_item_settings_on_account_id on metadata_item_settings.guid=metadata_item_views.guid and metadata_item_views.account_id=metadata_item_settings.account_id join metadata_item_settings as grandparentsSettings indexed by index_metadata_item_settings_on_guid on grandparentsSettings.guid=metadata_item_views.grandparent_guid and metadata_item_views.account_id=grandparentsSettings.account_id where metadata_item_views.library_section_id=";
    static const char after_section[] = " and grandparents.id in (";
    static const char after_threshold[] = " and viewed_at > ";
    static const char conjunction[] = " and ";
    plex_span section_id;
    plex_ondeck_selector selector;
    plex_span account_id;
    plex_index_state index_state;
    int max_index = 0;
    int variable_limit;
    size_t pos = 0;

    if (scan_len < sizeof(head) - 1 || memcmp(sql, head, sizeof(head) - 1) != 0) {
        return PLEX_MATCH_MISS;
    }
    variable_limit = sqlite3_limit(db, SQLITE_LIMIT_VARIABLE_NUMBER, -1);
    pos = sizeof(head) - 1;
    if (!plex_parse_ondeck_value_at(
            sql, scan_len, pos, variable_limit, &max_index, &section_id, &pos
        )) {
        plex_log_ondeck_miss(
            db, sql, scan_len, OBS_MISS_CAPTURE, "section"
        );
        return PLEX_MATCH_MISS;
    }
    if (pos > scan_len || sizeof(conjunction) - 1 > scan_len - pos ||
        memcmp(sql + pos, conjunction, sizeof(conjunction) - 1) != 0 ||
        pos + sizeof(conjunction) - 1 >= scan_len) {
        plex_log_ondeck_miss(
            db, sql, scan_len, OBS_MISS_CAPTURE, "selector"
        );
        return PLEX_MATCH_MISS;
    }
    switch (sql[pos + sizeof(conjunction) - 1]) {
        case 'g':
            selector.kind = PLEX_ONDECK_SELECTOR_IDS;
            if (!plex_expect_bytes(sql, scan_len, pos, after_section, &pos)) {
                plex_log_ondeck_miss(
                    db, sql, scan_len, OBS_MISS_CAPTURE, "selector"
                );
                return PLEX_MATCH_MISS;
            }
            if (!plex_parse_integer_list_at(
                    sql, scan_len, pos, &selector.value.id_list, &pos
                )) {
                plex_log_ondeck_miss(
                    db, sql, scan_len, OBS_MISS_CAPTURE, "id_list"
                );
                return PLEX_MATCH_MISS;
            }
            break;
        case 'v':
            selector.kind = PLEX_ONDECK_SELECTOR_THRESHOLD;
            if (!plex_expect_bytes(sql, scan_len, pos, after_threshold, &pos)) {
                plex_log_ondeck_miss(
                    db, sql, scan_len, OBS_MISS_CAPTURE, "selector"
                );
                return PLEX_MATCH_MISS;
            }
            if (!plex_parse_ondeck_threshold_at(
                    sql, scan_len, pos, &selector.value.threshold, &pos
                )) {
                plex_log_ondeck_miss(
                    db, sql, scan_len, OBS_MISS_CAPTURE, "threshold"
                );
                return PLEX_MATCH_MISS;
            }
            break;
        default:
            if (plex_ondeck_is_per_guid_variant(
                    sql, scan_len, pos, variable_limit
                )) {
                plex_log_ondeck_miss(
                    db, sql, scan_len,
                    OBS_MISS_OUT_OF_SCOPE, "ondeck_per_guid"
                );
            } else {
                plex_log_ondeck_miss(
                    db, sql, scan_len, OBS_MISS_CAPTURE, "selector"
                );
            }
            return PLEX_MATCH_MISS;
    }
    if (!plex_expect_bytes(
            sql, scan_len, pos, PLEX_ONDECK_AFTER_IDS, &pos
        )) {
        plex_log_ondeck_miss(
            db, sql, scan_len, OBS_MISS_CAPTURE, "post_id"
        );
        return PLEX_MATCH_MISS;
    }
    if (!plex_parse_ondeck_value_at(
            sql, scan_len, pos, variable_limit, &max_index, &account_id, &pos
        )) {
        plex_log_ondeck_miss(
            db, sql, scan_len, OBS_MISS_CAPTURE, "account"
        );
        return PLEX_MATCH_MISS;
    }
    if (!plex_expect_bytes(
            sql, scan_len, pos, PLEX_ONDECK_AFTER_ACCOUNT, &pos
        )) {
        plex_log_ondeck_miss(
            db, sql, scan_len, OBS_MISS_CAPTURE, "tail"
        );
        return PLEX_MATCH_MISS;
    }
    if (!plex_trailing_space_only(sql, scan_len, pos)) {
        plex_log_ondeck_miss(
            db, sql, scan_len, OBS_MISS_CAPTURE, "trailing"
        );
        return PLEX_MATCH_MISS;
    }
    index_state = plex_ondeck_index_ready(db);
    if (index_state != PLEX_INDEX_PRESENT) {
        if (index_state == PLEX_INDEX_MISSING) {
            obs_log_index_missing(db, OBS_MODE_PLEX_ONDECK);
        } else {
            obs_log_rewrite_skipped(
                db, "index_probe_error", OBS_MODE_PLEX_ONDECK
            );
        }
        return PLEX_MATCH_MISS;
    }
    candidate->sql = plex_build_ondeck_sql(
        db, sql, &section_id, &selector, &account_id, &candidate->rewrite_len
    );
    if (!candidate->sql) return PLEX_MATCH_BUILD_FAILED;
    candidate->scan_out_len = candidate->rewrite_len;
    candidate->expected_bind_count = max_index;
    candidate->verify_bind_count = 1;
    candidate->expected_column_count = PLEX_ONDECK_COLUMN_COUNT;
    candidate->verify_column_count = 1;
    return PLEX_MATCH_BUILT;
}

static int plex_prepare_input_lengths(
    const char *zSql,
    int nByte,
    size_t *bounded_len,
    size_t *scan_len
) {
    if (nByte < 0) {
        size_t n = strlen(zSql);
        if (n > SIZE_MAX - REWRITE_OPEN_LEN - REWRITE_CLOSE_LEN - 1) return 0;
        *bounded_len = n;
        *scan_len = n;
        return 1;
    }

    if (nByte == 0) return 0;
    if (nByte > INT_MAX - (REWRITE_OPEN_LEN + REWRITE_CLOSE_LEN)) return 0;
    *bounded_len = (size_t)nByte;
    *scan_len = *bounded_len;
    if (*scan_len > 0 && zSql[*scan_len - 1] == 0) {
        (*scan_len)--;
    }
    return 1;
}

static plex_match_result match_guid_like_query(
    const char *sql,
    size_t bounded_len,
    size_t scan_len,
    plex_rewrite_candidate *candidate
) {
    const size_t source_len = sizeof(PLEX_GUID_LIKE_SQL) - 1;
    const size_t rewritten_len = sizeof(PLEX_GUID_LIKE_REWRITTEN_SQL) - 1;
    size_t suffix_len;

    if (scan_len != source_len ||
        memcmp(sql, PLEX_GUID_LIKE_SQL, source_len) != 0) {
        return PLEX_MATCH_MISS;
    }
    if (bounded_len < scan_len || bounded_len - scan_len > 1) {
        return PLEX_MATCH_MISS;
    }

    suffix_len = bounded_len - scan_len;
    if (rewritten_len > SIZE_MAX - suffix_len - 1) {
        return PLEX_MATCH_BUILD_FAILED;
    }
    candidate->rewrite_len = rewritten_len + suffix_len;
    candidate->sql = (char *)malloc(candidate->rewrite_len + 1);
    if (!candidate->sql) return PLEX_MATCH_BUILD_FAILED;

    memcpy(candidate->sql, PLEX_GUID_LIKE_REWRITTEN_SQL, rewritten_len);
    memcpy(candidate->sql + rewritten_len, sql + scan_len, suffix_len);
    candidate->sql[candidate->rewrite_len] = 0;
    candidate->scan_out_len = rewritten_len;
    candidate->expected_bind_count = 2;
    candidate->verify_bind_count = 1;
    candidate->expected_column_count = 1;
    candidate->verify_column_count = 1;
    return PLEX_MATCH_BUILT;
}

static char *plex_build_rewritten_sql(
    const char *zSql,
    size_t bounded_len,
    const rewrite_match *match
) {
    char *rewritten;
    size_t out_len = bounded_len + REWRITE_OPEN_LEN + REWRITE_CLOSE_LEN;

    if (match->open_off > match->close_off || match->close_off > bounded_len) return NULL;
    rewritten = (char *)malloc(out_len + 1);
    if (!rewritten) return NULL;

    memcpy(rewritten, zSql, match->open_off);
    memcpy(rewritten + match->open_off, REWRITE_OPEN, REWRITE_OPEN_LEN);
    memcpy(rewritten + match->open_off + REWRITE_OPEN_LEN,
           zSql + match->open_off,
           match->close_off - match->open_off);
    rewritten[match->close_off + REWRITE_OPEN_LEN] = ')';
    memcpy(rewritten + match->close_off + REWRITE_OPEN_LEN + REWRITE_CLOSE_LEN,
           zSql + match->close_off,
           bounded_len - match->close_off);
    rewritten[out_len] = 0;
    return rewritten;
}

static int plex_map_end_tail(
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

static void plex_log_rewrite_applied(
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

static int plex_retry_original_after_rewrite_failure(
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
    return call_real_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
}

static int plex_prepare_rewritten_sql(
    fts_rewrite_prepare_kind kind,
    sqlite3 *db,
    const char *zSql,
    int nByte,
    unsigned int prepFlags,
    sqlite3_stmt **ppStmt,
    const char **pzTail,
    char *rewritten,
    size_t scan_len,
    size_t scan_out_len,
    size_t rewrite_len,
    int expected_bind_count,
    int verify_bind_count,
    int expected_column_count,
    int verify_column_count,
    obs_rewrite_mode mode
) {
    int rewrite_nbyte = nByte < 0 ? -1 : (int)rewrite_len;
    const char *rewritten_tail = NULL;
    const char *mapped_tail = NULL;
    int rc;

    if (ppStmt) *ppStmt = NULL;
    rc = call_real_prepare(
        kind, db, rewritten, rewrite_nbyte, prepFlags, ppStmt, &rewritten_tail
    );
    if (rc != SQLITE_OK) {
        obs_log_rewrite_skipped(db, "rewritten_prepare_failed", mode);
        free(rewritten);
        return plex_retry_original_after_rewrite_failure(
            kind, db, zSql, nByte, prepFlags, ppStmt, pzTail
        );
    }
    if (!plex_map_end_tail(zSql, rewritten, rewritten_tail, scan_len, scan_out_len, &mapped_tail)) {
        obs_log_rewrite_skipped(db, "tail_mismatch", mode);
        free(rewritten);
        return plex_retry_original_after_rewrite_failure(
            kind, db, zSql, nByte, prepFlags, ppStmt, pzTail
        );
    }
    if (verify_bind_count &&
        sqlite3_bind_parameter_count(*ppStmt) != expected_bind_count) {
        obs_log_rewrite_skipped(db, "bind_count_mismatch", mode);
        free(rewritten);
        return plex_retry_original_after_rewrite_failure(
            kind, db, zSql, nByte, prepFlags, ppStmt, pzTail
        );
    }
    if (verify_column_count &&
        sqlite3_column_count(*ppStmt) != expected_column_count) {
        obs_log_rewrite_skipped(db, "column_count_mismatch", mode);
        free(rewritten);
        return plex_retry_original_after_rewrite_failure(
            kind, db, zSql, nByte, prepFlags, ppStmt, pzTail
        );
    }

    plex_log_rewrite_applied(
        db, zSql, scan_len, rewritten, scan_out_len, mode
    );
    if (pzTail) *pzTail = mapped_tail;
    free(rewritten);
    return rc;
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
#ifdef SQLITE_ENABLE_ICU
    size_t bounded_len = 0;
    size_t scan_len = 0;
    rewrite_match match;
    plex_rewrite_candidate candidate;
    plex_match_result match_result;
    const char *raw_fn;
    char *rewritten = NULL;
    int fts_on;
    int guid_like_on;
    int tag_on;
    int ondeck_on;

    fts_on = plex_rewrite_enabled();
    guid_like_on = plex_guid_like_rewrite_enabled();
    tag_on = plex_tag_rewrite_enabled();
    ondeck_on = plex_ondeck_rewrite_enabled();
    if ((!fts_on && !guid_like_on && !tag_on && !ondeck_on) ||
        !db || !zSql || !ppStmt || nByte == 0) {
        return call_real_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
    }

    raw_fn = sqlite3_db_filename(db, "main");
    if (!auto_extension_path_is_target(raw_fn) ||
        !fts_rewrite_db_basename_matches(raw_fn, PLEX_FTS_TARGET.db_basename)) {
        return call_real_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
    }
    if (!plex_prepare_input_lengths(zSql, nByte, &bounded_len, &scan_len)) {
        return call_real_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
    }

    if (guid_like_on) {
        memset(&candidate, 0, sizeof(candidate));
        match_result = match_guid_like_query(zSql, bounded_len, scan_len, &candidate);
        if (match_result == PLEX_MATCH_BUILT) {
            return plex_prepare_rewritten_sql(
                kind, db, zSql, nByte, prepFlags, ppStmt, pzTail,
                candidate.sql, scan_len, candidate.scan_out_len, candidate.rewrite_len,
                candidate.expected_bind_count, candidate.verify_bind_count,
                candidate.expected_column_count, candidate.verify_column_count,
                OBS_MODE_PLEX_GUID_LIKE
            );
        }
        if (match_result == PLEX_MATCH_BUILD_FAILED) {
            obs_log_rewrite_skipped(db, "build_failed", OBS_MODE_PLEX_GUID_LIKE);
            free(candidate.sql);
            return call_real_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
        }
    }

    if (fts_on && match_target_prefix_query(&PLEX_FTS_TARGET, zSql, scan_len, &match)) {
        rewritten = plex_build_rewritten_sql(zSql, bounded_len, &match);
        if (!rewritten) {
            obs_log_rewrite_skipped(db, "build_failed", OBS_MODE_PLEX_FTS);
            return call_real_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
        }
        return plex_prepare_rewritten_sql(
            kind, db, zSql, nByte, prepFlags, ppStmt, pzTail,
            rewritten,
            scan_len,
            scan_len + REWRITE_OPEN_LEN + REWRITE_CLOSE_LEN,
            bounded_len + REWRITE_OPEN_LEN + REWRITE_CLOSE_LEN,
            0, 0, 0, 0,
            OBS_MODE_PLEX_FTS
        );
    }

    memset(&candidate, 0, sizeof(candidate));
    if (tag_on) {
        match_result = match_tag_membership_query(db, zSql, bounded_len, scan_len, &candidate);
        if (match_result == PLEX_MATCH_BUILT) {
            return plex_prepare_rewritten_sql(
                kind, db, zSql, nByte, prepFlags, ppStmt, pzTail,
                candidate.sql, scan_len, candidate.scan_out_len, candidate.rewrite_len,
                0, 0, 0, 0,
                OBS_MODE_PLEX_TAGGINGS
            );
        }
        if (match_result == PLEX_MATCH_BUILD_FAILED) {
            obs_log_rewrite_skipped(db, "build_failed", OBS_MODE_PLEX_TAGGINGS);
            free(candidate.sql);
            return call_real_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
        }
    }

    memset(&candidate, 0, sizeof(candidate));
    if (ondeck_on) {
        match_result = match_ondeck_query(db, zSql, scan_len, &candidate);
        if (match_result == PLEX_MATCH_BUILT) {
            return plex_prepare_rewritten_sql(
                kind, db, zSql, nByte, prepFlags, ppStmt, pzTail,
                candidate.sql, scan_len, candidate.scan_out_len, candidate.rewrite_len,
                candidate.expected_bind_count, candidate.verify_bind_count,
                candidate.expected_column_count, candidate.verify_column_count,
                OBS_MODE_PLEX_ONDECK
            );
        }
        if (match_result == PLEX_MATCH_BUILD_FAILED) {
            obs_log_rewrite_skipped(db, "build_failed", OBS_MODE_PLEX_ONDECK);
            free(candidate.sql);
            return call_real_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
        }
    }

    return call_real_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
#else
    return call_real_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
#endif
}
