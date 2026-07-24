#ifndef SQLITE3_BUILDS_REWRITE_SMOKE_HARNESS_H
#define SQLITE3_BUILDS_REWRITE_SMOKE_HARNESS_H

#include <sqlite3.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define RSH_DB_RELATIVE_PATH_CAP 160
#define RSH_MATRIX_MAX_AXES 4
#define RSH_MATRIX_MAX_AXIS_VALUES 3
#define RSH_MATRIX_MAX_STEPS 5
#define RSH_MATRIX_CELL_DIR_CAP 96
#define RSH_MATRIX_MAX_CELLS 64

#define RSH_PATH_CAP 1024
#define RSH_FAILURE_MESSAGE_CAP 8192
#define RSH_PROFILE_NONE 0

#define RSH_BUILD_PLEX_LINKED (UINT32_C(1) << 0)
#define RSH_BUILD_EMBY_LINKED (UINT32_C(1) << 1)
#define RSH_BUILD_EMBY_DIRECT (UINT32_C(1) << 2)
#define RSH_BUILD_ALL \
    (RSH_BUILD_PLEX_LINKED | RSH_BUILD_EMBY_LINKED | RSH_BUILD_EMBY_DIRECT)

typedef struct rsh_suite_spec rsh_suite_spec;
typedef struct rsh_run_spec rsh_run_spec;
typedef struct rsh_case_spec rsh_case_spec;
typedef struct rsh_case_context rsh_case_context;
typedef struct rsh_matrix_cell rsh_matrix_cell;

typedef enum rsh_env_state {
    RSH_ENV_UNSET = 0,
    RSH_ENV_EMPTY,
    RSH_ENV_VALUE
} rsh_env_state;

typedef struct rsh_env_value {
    rsh_env_state state;
    const char *value;
} rsh_env_value;

typedef struct rsh_env_assignment {
    const char *name;
    rsh_env_value value;
} rsh_env_assignment;

typedef enum rsh_prepare_kind {
    RSH_PREPARE_LEGACY = 1,
    RSH_PREPARE_V2,
    RSH_PREPARE_V3
} rsh_prepare_kind;

/*
 * Raw prepare callbacks return rc, statement, and tail exactly as produced.
 * They never fail an assertion, call the suite failure callback, or finalize.
 */
typedef int (*rsh_prepare_fn)(
    sqlite3 *db,
    const char *sql,
    int nByte,
    rsh_prepare_kind kind,
    sqlite3_stmt **stmt_out,
    const char **tail_out
);

typedef void (*rsh_failure_fn)(const char *fmt, ...);
typedef int (*rsh_open_fn)(const char *path, sqlite3 **db_out, void *suite_ctx);
typedef int (*rsh_base_seed_fn)(sqlite3 *db, void *suite_ctx);
typedef int (*rsh_apply_profile_fn)(
    sqlite3 *db,
    int profile,
    void *suite_ctx
);
typedef rsh_apply_profile_fn (*rsh_setup_profile_resolver_fn)(
    int profile,
    void *suite_ctx
);

struct rsh_suite_spec {
    const char *suite_name;
    const char *temp_prefix;
    const char *vendor_basename;
    const char *target_basename;
    const char *const *controlled_env_gates;
    size_t controlled_env_gate_count;
    rsh_prepare_fn prepare;
    rsh_open_fn open;
    rsh_base_seed_fn base_seed;
    rsh_setup_profile_resolver_fn resolve_setup_profile;
    rsh_failure_fn failure;
    void *suite_ctx;
};

static const rsh_suite_spec *rsh_contract_suite;
static FILE *rsh_active_capture_file;
static int rsh_active_saved_stderr = -1;

static void rsh_restore_active_stderr(void) {
    if (rsh_active_saved_stderr >= 0) {
        fflush(stderr);
        (void)dup2(rsh_active_saved_stderr, STDERR_FILENO);
        (void)close(rsh_active_saved_stderr);
        rsh_active_saved_stderr = -1;
        clearerr(stderr);
    }
    if (rsh_active_capture_file) {
        (void)fclose(rsh_active_capture_file);
        rsh_active_capture_file = NULL;
    }
}

static void rsh_contract_parity_failf(const char *fmt, ...) {
    char message[RSH_FAILURE_MESSAGE_CAP];
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
    rsh_restore_active_stderr();
    if (rsh_contract_suite && rsh_contract_suite->failure) {
        rsh_contract_suite->failure("%s", message);
    }
    fprintf(stderr, "%s\n", message);
    abort();
}

/* The local parity oracle is compiled through the suite failure boundary. */
#ifdef SQLITE3_BUILDS_CONTRACT_PARITY_H
#error "include rewrite_smoke_harness.h before contract_parity.h"
#endif
#define failf rsh_contract_parity_failf
#include "contract_parity.h"
#undef failf

typedef enum rsh_db_kind {
    RSH_DB_VENDOR = 0,
    RSH_DB_CANDIDATE,
    RSH_DB_AUXILIARY
} rsh_db_kind;

typedef enum rsh_db_storage {
    RSH_DB_RELATIVE = 0,
    RSH_DB_MEMORY
} rsh_db_storage;

typedef struct rsh_db_spec {
    const char *role;
    char relative_path[RSH_DB_RELATIVE_PATH_CAP];
    rsh_db_kind kind;
    rsh_db_storage storage;
    int seed_profile;
    int setup_profile;
} rsh_db_spec;

typedef struct rsh_db_handle {
    const rsh_db_spec *spec;
    sqlite3 *db;
} rsh_db_handle;

typedef enum rsh_nbyte_kind {
    RSH_NBYTE_MINUS_ONE = 0,
    RSH_NBYTE_EXACT_LENGTH,
    RSH_NBYTE_LENGTH_WITH_NUL,
    RSH_NBYTE_LITERAL
} rsh_nbyte_kind;

typedef enum rsh_tail_kind {
    RSH_TAIL_FULL = 0,
    RSH_TAIL_OFFSET,
    RSH_TAIL_NULL_OUT,
    RSH_TAIL_IGNORE
} rsh_tail_kind;

typedef struct rsh_prepare_spec {
    rsh_prepare_kind kind;
    rsh_nbyte_kind nbyte_kind;
    int literal_nbyte;
    rsh_tail_kind tail_kind;
    size_t tail_offset;
} rsh_prepare_spec;

typedef int (*rsh_bind_fn)(sqlite3_stmt *stmt, const char *label, void *ctx);
typedef int (*rsh_counter_read_fn)(void *ctx);
typedef void (*rsh_counter_reset_fn)(void *ctx);

typedef struct rsh_column_expectation {
    const char *name;
    const char *decl_type;
    const char *table_name;
    const char *origin_name;
} rsh_column_expectation;

typedef struct rsh_occurrence_expectation {
    const char *needle;
    int expected_count;
} rsh_occurrence_expectation;

typedef struct rsh_sql_exact_spec {
    const char *role;
    const char *sql;
    const char *expected_sql;
    rsh_prepare_spec prepare;
    int check_bind_count;
    int expected_bind_count;
    const char *const *expected_bind_names;
    size_t expected_bind_name_count;
    int check_column_count;
    int expected_column_count;
    const rsh_column_expectation *expected_columns;
    size_t expected_columns_count;
} rsh_sql_exact_spec;

typedef enum rsh_negative_source_kind {
    RSH_NEGATIVE_STATIC = 0,
    RSH_NEGATIVE_FIXTURE,
    RSH_NEGATIVE_GENERATED
} rsh_negative_source_kind;

typedef struct rsh_negative_spec {
    rsh_negative_source_kind source_kind;
    const char *sql;
    const char *fixture_path;
    int strip_fixture_final_lf;
    const char *base_sql;
    const char *replacement_needle;
    const char *replacement;
    const char *discriminating_needle;
    rsh_prepare_spec prepare;
    const char *vendor_role;
    const char *candidate_role;
    int check_bind_count;
    int expected_bind_count;
    int check_column_count;
    int expected_column_count;
    const rsh_occurrence_expectation *followup_occurrences;
    size_t followup_occurrence_count;
} rsh_negative_spec;

typedef struct rsh_contract_parity_spec {
    const char *vendor_role;
    const char *candidate_role;
    const char *vendor_sql;
    const char *candidate_source_sql;
    const char *expected_candidate_sql;
    rsh_prepare_spec prepare;
    contract_parity_bind_fn bind;
    void *bind_ctx;
    contract_parity_row_exception_fn row_exception;
    void *row_exception_ctx;
    int minimum_rows;
} rsh_contract_parity_spec;

typedef enum rsh_scalar_value_kind {
    RSH_SCALAR_NULL = 0,
    RSH_SCALAR_INTEGER,
    RSH_SCALAR_FLOAT,
    RSH_SCALAR_TEXT,
    RSH_SCALAR_BLOB
} rsh_scalar_value_kind;

typedef struct rsh_scalar_spec {
    const char *role;
    const char *sql;
    const char *expected_sql;
    rsh_prepare_spec prepare;
    rsh_bind_fn bind;
    void *bind_ctx;
    rsh_scalar_value_kind value_kind;
    sqlite3_int64 integer_value;
    double float_value;
    const void *bytes_value;
    int bytes_count;
    rsh_counter_reset_fn reset_counter;
    rsh_counter_read_fn read_counter;
    void *counter_ctx;
    int expected_counter;
} rsh_scalar_spec;

typedef struct rsh_exact_ids_spec {
    const char *role;
    const char *sql;
    rsh_prepare_spec prepare;
    rsh_bind_fn bind;
    void *bind_ctx;
    const sqlite3_int64 *expected_ids;
    size_t expected_id_count;
} rsh_exact_ids_spec;

typedef enum rsh_fixture_assertion_kind {
    RSH_FIXTURE_SQL_EXACT = 0,
    RSH_FIXTURE_NEGATIVE,
    RSH_FIXTURE_CONTRACT_PARITY
} rsh_fixture_assertion_kind;

typedef struct rsh_fixture_spec {
    const char *source_path;
    const char *expected_path;
    int strip_final_lf;
    rsh_fixture_assertion_kind assertion_kind;
    rsh_sql_exact_spec sql_exact;
    rsh_negative_spec negative;
    rsh_contract_parity_spec contract_parity;
} rsh_fixture_spec;

typedef int (*rsh_path_assert_fn)(
    const rsh_case_context *context,
    const void *immutable_data
);

typedef struct rsh_path_spec {
    rsh_sql_exact_spec assertion;
    rsh_path_assert_fn assert_after_prepare;
    const void *immutable_data;
} rsh_path_spec;

typedef int (*rsh_index_probe_assert_fn)(
    const rsh_case_context *context,
    int mode_id,
    int expected_delta,
    void *ctx
);

typedef struct rsh_index_probe_spec {
    const char *role;
    int mode_id;
    int expected_delta;
    rsh_index_probe_assert_fn assert_probe;
    void *assert_ctx;
} rsh_index_probe_spec;

typedef struct rsh_abort_expectation {
    int expected_exit;
    const char *expected_stage_label;
    const char *const *earlier_stage_labels;
    size_t earlier_stage_label_count;
} rsh_abort_expectation;

typedef struct rsh_expect_abort_spec {
    rsh_negative_spec negative;
} rsh_expect_abort_spec;

typedef enum rsh_custom_kind {
    RSH_CUSTOM_IDENTITY = 0,
    RSH_CUSTOM_FAULT_MATRIX,
    RSH_CUSTOM_LOG_CAPTURE
} rsh_custom_kind;

/*
 * Custom assertions receive immutable case data and already-open handles only.
 * They may prepare, bind, step, finalize, manage connection-local fault state,
 * and assert. Environment, path, open/close, process, seed/setup, ANALYZE,
 * capture, and cleanup lifecycle remain runner-owned.
 * Per-file definitions assigned to assert_custom must use the
 * rsh_custom_adapter_ prefix so check_custom_adapter_guard.sh can enforce the
 * lifecycle boundary over their complete bodies.
 */
typedef int (*rsh_custom_assert_fn)(
    const rsh_case_context *context,
    const void *immutable_data
);

typedef struct rsh_custom_spec {
    rsh_custom_kind kind;
    rsh_custom_assert_fn assert_custom;
    const void *immutable_data;
} rsh_custom_spec;

typedef enum rsh_case_kind {
    RSH_CASE_SQL_EXACT = 0,
    RSH_CASE_NEGATIVE,
    RSH_CASE_CONTRACT_PARITY,
    RSH_CASE_SCALAR,
    RSH_CASE_EXACT_IDS,
    RSH_CASE_FIXTURE,
    RSH_CASE_PATH,
    RSH_CASE_INDEX_PROBE,
    RSH_CASE_EXPECT_ABORT,
    RSH_CASE_CUSTOM
} rsh_case_kind;

typedef int (*rsh_case_predicate_fn)(
    const rsh_case_spec *test_case,
    const rsh_case_context *context,
    void *suite_ctx
);

struct rsh_case_spec {
    const char *label;
    rsh_case_kind kind;
    uint32_t build_mask;
    /* A false runtime predicate omits the case without rendering SKIP. */
    rsh_case_predicate_fn runtime_predicate;
    union {
        rsh_sql_exact_spec sql_exact;
        rsh_negative_spec negative;
        rsh_contract_parity_spec contract_parity;
        rsh_scalar_spec scalar;
        rsh_exact_ids_spec exact_ids;
        rsh_fixture_spec fixture;
        rsh_path_spec path;
        rsh_index_probe_spec index_probe;
        rsh_expect_abort_spec expect_abort;
        rsh_custom_spec custom;
    } data;
};

typedef struct rsh_phase_spec {
    const char *label;
    const rsh_db_spec *dbs;
    size_t db_count;
    const rsh_case_spec *cases;
    size_t case_count;
} rsh_phase_spec;

typedef struct rsh_matrix_axis_value {
    const char *label;
    sqlite3_int64 integer;
    const void *immutable_data;
} rsh_matrix_axis_value;

typedef struct rsh_matrix_axis {
    const char *name;
    const rsh_matrix_axis_value *values;
    size_t value_count;
} rsh_matrix_axis;

typedef enum rsh_matrix_cell_step_kind {
    RSH_CELL_SETUP_PROFILE = 0,
    RSH_CELL_ANALYZE_IF,
    RSH_CELL_ASSERT
} rsh_matrix_cell_step_kind;

typedef struct rsh_matrix_predicate {
    size_t axis_index;
    size_t value_index;
} rsh_matrix_predicate;

typedef struct rsh_matrix_cell_step {
    rsh_matrix_cell_step_kind kind;
    const char *role;
    int setup_profile;
    rsh_matrix_predicate predicate;
    const rsh_case_spec *cases;
    size_t case_count;
} rsh_matrix_cell_step;

typedef struct rsh_matrix_counter_spec {
    const char *name;
    size_t expected;
} rsh_matrix_counter_spec;

typedef int (*rsh_matrix_assert_fn)(
    const rsh_case_context *context,
    const rsh_matrix_cell_step *step,
    void *ctx
);

typedef struct rsh_matrix_phase_spec {
    const char *label;
    char cell_dir_prefix[RSH_MATRIX_CELL_DIR_CAP];
    const rsh_matrix_axis *axes;
    size_t axis_count;
    const rsh_db_spec *dbs;
    size_t db_count;
    const rsh_matrix_cell_step *steps;
    size_t step_count;
    rsh_matrix_assert_fn assertion_adapter;
    void *assertion_ctx;
    size_t expected_cells;
    const rsh_matrix_counter_spec *counters;
    size_t counter_count;
} rsh_matrix_phase_spec;

struct rsh_matrix_cell {
    const rsh_matrix_axis *axes;
    size_t axis_count;
    size_t axis_indices[RSH_MATRIX_MAX_AXES];
    const rsh_matrix_axis_value *axis_values[RSH_MATRIX_MAX_AXES];
    const rsh_db_handle *dbs;
    size_t db_count;
    size_t *counters;
    size_t counter_count;
};

struct rsh_case_context {
    const char *suite_name;
    const char *run_name;
    const char *phase_label;
    const rsh_db_handle *dbs;
    size_t db_count;
    const rsh_matrix_cell *matrix_cell;
    const char *captured_stderr;
    uint32_t active_build;
};

typedef enum rsh_process_kind {
    RSH_PROCESS_FORK = 0,
    RSH_PROCESS_EXEC
} rsh_process_kind;

typedef enum rsh_run_outcome {
    RSH_OUTCOME_SUCCESS = 0,
    RSH_OUTCOME_EXPECT_ABORT
} rsh_run_outcome;

typedef enum rsh_capture_scope {
    RSH_CAPTURE_NONE = 0,
    RSH_CAPTURE_STDERR
} rsh_capture_scope;

typedef int (*rsh_run_predicate_fn)(
    const rsh_run_spec *run,
    void *suite_ctx
);

typedef struct rsh_pass_detail {
    const char *literal;
    const char *else_literal;
    rsh_run_predicate_fn predicate;
} rsh_pass_detail;

struct rsh_run_spec {
    const char *dispatch_name;
    const char *pass_label;
    rsh_pass_detail pass_detail;
    const char *skip_detail;
    rsh_process_kind process_kind;
    rsh_run_outcome outcome;
    uint32_t build_mask;
    const rsh_env_assignment *preload_env;
    size_t preload_env_count;
    const rsh_env_assignment *postload_env;
    size_t postload_env_count;
    rsh_capture_scope capture_scope;
    const rsh_phase_spec *phases;
    size_t phase_count;
    const rsh_matrix_phase_spec *matrix_phases;
    size_t matrix_phase_count;
    const rsh_case_spec *post_close_cases;
    size_t post_close_case_count;
    const rsh_abort_expectation *abort_expectation;
    rsh_run_predicate_fn runtime_predicate;
};

typedef struct rsh_capture {
    FILE *file;
    int saved_stderr;
} rsh_capture;

typedef struct rsh_parity_prepare_state {
    const rsh_suite_spec *suite;
    const rsh_case_spec *test_case;
    rsh_prepare_spec prepare;
} rsh_parity_prepare_state;

static rsh_parity_prepare_state rsh_parity_state;

static size_t rsh_bounded_strlen(const char *text, size_t cap) {
    size_t n = 0;

    if (!text) return cap;
    while (n < cap && text[n]) n++;
    return n;
}

static void rsh_vfailf(
    const rsh_suite_spec *suite,
    const char *fmt,
    va_list ap
) {
    char message[RSH_FAILURE_MESSAGE_CAP];

    (void)vsnprintf(message, sizeof(message), fmt, ap);
    rsh_restore_active_stderr();
    if (suite && suite->failure) suite->failure("%s", message);
    fprintf(stderr, "%s\n", message);
    abort();
}

static void rsh_failf(const rsh_suite_spec *suite, const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    rsh_vfailf(suite, fmt, ap);
    va_end(ap);
}

static void rsh_case_failf(
    const rsh_suite_spec *suite,
    const rsh_case_spec *test_case,
    const char *stage,
    const char *fmt,
    ...
) {
    char detail[RSH_FAILURE_MESSAGE_CAP / 2];
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);
    rsh_failf(
        suite,
        "FAIL [%s/%s]: %s",
        test_case && test_case->label ? test_case->label : "(unlabelled)",
        stage,
        detail
    );
}

static int rsh_count_occurrences(const char *text, const char *needle) {
    int count = 0;
    size_t needle_len;
    const char *cursor;

    if (!text || !needle || !needle[0]) return 0;
    needle_len = strlen(needle);
    cursor = text;
    while ((cursor = strstr(cursor, needle)) != NULL) {
        count++;
        cursor += needle_len;
    }
    return count;
}

static int rsh_nullable_text_equal(const char *left, const char *right) {
    if (!left || !right) return left == right;
    return strcmp(left, right) == 0;
}

static int rsh_build_enabled(uint32_t row_mask, uint32_t active_build) {
    return (row_mask & active_build) != 0;
}

static int rsh_compute_nbyte(
    const rsh_suite_spec *suite,
    const rsh_case_spec *test_case,
    const char *sql,
    const rsh_prepare_spec *prepare
) {
    size_t len = strlen(sql);

    if (prepare->nbyte_kind == RSH_NBYTE_MINUS_ONE) return -1;
    if (prepare->nbyte_kind == RSH_NBYTE_LITERAL) return prepare->literal_nbyte;
    if (len > (size_t)INT_MAX) {
        rsh_case_failf(suite, test_case, "nbyte", "length=%lu exceeds INT_MAX",
                       (unsigned long)len);
    }
    if (prepare->nbyte_kind == RSH_NBYTE_LENGTH_WITH_NUL) {
        if (len == (size_t)INT_MAX) {
            rsh_case_failf(suite, test_case, "nbyte", "NUL-inclusive length exceeds INT_MAX");
        }
        return (int)len + 1;
    }
    if (prepare->nbyte_kind != RSH_NBYTE_EXACT_LENGTH) {
        rsh_case_failf(suite, test_case, "nbyte", "invalid nbyte kind=%d",
                       (int)prepare->nbyte_kind);
    }
    return (int)len;
}

static int rsh_validate_prepare_spec(const rsh_prepare_spec *prepare) {
    if (!prepare) return 0;
    if (prepare->kind != RSH_PREPARE_LEGACY &&
        prepare->kind != RSH_PREPARE_V2 &&
        prepare->kind != RSH_PREPARE_V3) {
        return 0;
    }
    if (prepare->nbyte_kind < RSH_NBYTE_MINUS_ONE ||
        prepare->nbyte_kind > RSH_NBYTE_LITERAL) {
        return 0;
    }
    if (prepare->nbyte_kind == RSH_NBYTE_LITERAL && prepare->literal_nbyte < -1) {
        return 0;
    }
    return prepare->tail_kind >= RSH_TAIL_FULL &&
           prepare->tail_kind <= RSH_TAIL_IGNORE;
}

static int rsh_relative_path_valid(const char *path, size_t cap) {
    size_t len;
    size_t component_start = 0;
    size_t i;

    len = rsh_bounded_strlen(path, cap);
    if (len == 0 || len >= cap || path[0] == '/' || path[len - 1] == '/') return 0;
    for (i = 0; i <= len; i++) {
        if (i == len || path[i] == '/') {
            size_t component_len = i - component_start;
            if (component_len == 0) return 0;
            if (component_len == 1 && path[component_start] == '.') return 0;
            if (component_len == 2 && path[component_start] == '.' &&
                path[component_start + 1] == '.') {
                return 0;
            }
            component_start = i + 1;
        }
    }
    return 1;
}

static int rsh_absolute_prefix_valid(const char *path) {
    size_t len;
    const char *cursor;

    if (!path || path[0] != '/' || path[1] == '\0') return 0;
    len = strlen(path);
    if (path[len - 1] == '/') return 0;
    cursor = path + 1;
    while (*cursor) {
        const char *slash = strchr(cursor, '/');
        size_t component_len = slash ? (size_t)(slash - cursor) : strlen(cursor);
        if (component_len == 0 ||
            (component_len == 1 && cursor[0] == '.') ||
            (component_len == 2 && cursor[0] == '.' && cursor[1] == '.')) {
            return 0;
        }
        if (!slash) break;
        cursor = slash + 1;
    }
    return 1;
}

static const char *rsh_final_component(const char *relative_path) {
    const char *slash = strrchr(relative_path, '/');
    return slash ? slash + 1 : relative_path;
}

static int rsh_join_path(
    char *output,
    size_t output_cap,
    const char *left,
    const char *right
) {
    int rc;

    if (!right || !right[0]) rc = snprintf(output, output_cap, "%s", left);
    else rc = snprintf(output, output_cap, "%s/%s", left, right);
    return rc >= 0 && (size_t)rc < output_cap;
}

static int rsh_path_with_suffix(
    char *output,
    size_t output_cap,
    const char *path,
    const char *suffix
) {
    int rc = snprintf(output, output_cap, "%s%s", path, suffix);
    return rc >= 0 && (size_t)rc < output_cap;
}

/* Lifecycle code supplies the owner PID explicitly; parent cleanup records it at fork. */
static int rsh_root_for_pid(
    const rsh_suite_spec *suite,
    pid_t owner_pid,
    char *output,
    size_t output_cap
) {
    int rc;

    if (!suite || !suite->temp_prefix || !suite->temp_prefix[0] || owner_pid <= 0) {
        return 0;
    }
    rc = snprintf(output, output_cap, "%s-%ld", suite->temp_prefix, (long)owner_pid);
    return rc >= 0 && (size_t)rc < output_cap;
}

static int rsh_ensure_directory(const char *path, char *error, size_t error_cap) {
    struct stat st;

    if (mkdir(path, 0700) == 0) return 1;
    if (errno != EEXIST) {
        (void)snprintf(error, error_cap, "mkdir(%s): %s", path, strerror(errno));
        return 0;
    }
    if (lstat(path, &st) != 0 || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
        (void)snprintf(error, error_cap, "existing path is not a directory: %s", path);
        return 0;
    }
    return 1;
}

static int rsh_ensure_relative_directories(
    const char *root,
    const char *relative_directory,
    char *error,
    size_t error_cap
) {
    char path[RSH_PATH_CAP];
    char relative[RSH_PATH_CAP];
    char *cursor;

    if (!relative_directory || !relative_directory[0]) return 1;
    if (strlen(relative_directory) >= sizeof(relative)) {
        (void)snprintf(error, error_cap, "relative directory too long");
        return 0;
    }
    strcpy(relative, relative_directory);
    cursor = relative;
    for (;;) {
        char *slash = strchr(cursor, '/');
        if (slash) *slash = '\0';
        if (!rsh_join_path(path, sizeof(path), root, relative)) {
            (void)snprintf(error, error_cap, "directory path too long");
            return 0;
        }
        if (!rsh_ensure_directory(path, error, error_cap)) return 0;
        if (!slash) break;
        *slash = '/';
        cursor = slash + 1;
    }
    return 1;
}

static int rsh_ensure_parent_directories(
    const char *root,
    const char *relative_path,
    char *error,
    size_t error_cap
) {
    char parent[RSH_DB_RELATIVE_PATH_CAP];
    char *slash;
    size_t len = strlen(relative_path);

    if (len >= sizeof(parent)) {
        (void)snprintf(error, error_cap, "relative DB path too long");
        return 0;
    }
    strcpy(parent, relative_path);
    slash = strrchr(parent, '/');
    if (!slash) return 1;
    *slash = '\0';
    return rsh_ensure_relative_directories(root, parent, error, error_cap);
}

static int rsh_unlink_if_present(
    const char *path,
    char *error,
    size_t error_cap
) {
    if (unlink(path) == 0 || errno == ENOENT) return 1;
    (void)snprintf(error, error_cap, "unlink(%s): %s", path, strerror(errno));
    return 0;
}

static int rsh_remove_db_files(
    const char *path,
    char *error,
    size_t error_cap
) {
    char sidecar[RSH_PATH_CAP];
    int ok = 1;

    if (!rsh_unlink_if_present(path, error, error_cap)) ok = 0;
    if (!rsh_path_with_suffix(sidecar, sizeof(sidecar), path, "-wal")) {
        if (ok) (void)snprintf(error, error_cap, "WAL path too long");
        ok = 0;
    } else if (!rsh_unlink_if_present(sidecar, error, error_cap)) {
        ok = 0;
    }
    if (!rsh_path_with_suffix(sidecar, sizeof(sidecar), path, "-shm")) {
        if (ok) (void)snprintf(error, error_cap, "SHM path too long");
        ok = 0;
    } else if (!rsh_unlink_if_present(sidecar, error, error_cap)) {
        ok = 0;
    }
    return ok;
}

static size_t rsh_path_component_count(const char *path) {
    size_t count = 1;
    const char *cursor;

    for (cursor = path; *cursor; cursor++) {
        if (*cursor == '/') count++;
    }
    return count;
}

static int rsh_parent_at_depth(
    const char *relative_path,
    size_t depth,
    char *output,
    size_t output_cap
) {
    const char *cursor = relative_path;
    size_t components = 0;
    size_t bytes = 0;

    while (*cursor && components < depth) {
        const char *slash = strchr(cursor, '/');
        size_t component_len;
        if (!slash) return 0;
        component_len = (size_t)(slash - cursor);
        if (bytes != 0) {
            if (bytes + 1 >= output_cap) return 0;
            output[bytes++] = '/';
        }
        if (bytes + component_len >= output_cap) return 0;
        memcpy(output + bytes, cursor, component_len);
        bytes += component_len;
        components++;
        cursor = slash + 1;
    }
    if (components != depth) return 0;
    output[bytes] = '\0';
    return 1;
}

static int rsh_cleanup_db_specs_at_root(
    const char *root,
    const rsh_db_spec *dbs,
    size_t db_count,
    char *error,
    size_t error_cap
) {
    char path[RSH_PATH_CAP];
    char relative_dir[RSH_DB_RELATIVE_PATH_CAP];
    size_t max_parent_depth = 0;
    size_t i;
    size_t depth;
    int errors = 0;

    for (i = 0; i < db_count; i++) {
        size_t component_count;
        if (dbs[i].storage == RSH_DB_MEMORY) continue;
        if (!rsh_join_path(path, sizeof(path), root, dbs[i].relative_path)) {
            if (errors++ == 0) (void)snprintf(error, error_cap, "DB cleanup path too long");
            continue;
        }
        if (!rsh_remove_db_files(path, error, error_cap)) errors++;
        component_count = rsh_path_component_count(dbs[i].relative_path);
        if (component_count > 1 && component_count - 1 > max_parent_depth) {
            max_parent_depth = component_count - 1;
        }
    }
    for (depth = max_parent_depth; depth > 0; depth--) {
        for (i = 0; i < db_count; i++) {
            if (dbs[i].storage == RSH_DB_MEMORY ||
                !rsh_parent_at_depth(
                    dbs[i].relative_path, depth, relative_dir, sizeof(relative_dir)
                )) {
                continue;
            }
            if (!rsh_join_path(path, sizeof(path), root, relative_dir)) {
                if (errors++ == 0) {
                    (void)snprintf(error, error_cap, "directory cleanup path too long");
                }
                continue;
            }
            if (rmdir(path) != 0 && errno != ENOENT) {
                if (errors++ == 0) {
                    (void)snprintf(error, error_cap, "rmdir(%s): %s", path, strerror(errno));
                }
            }
        }
    }
    return errors;
}

static int rsh_remove_relative_directory_chain(
    const char *root,
    const char *relative_directory,
    char *error,
    size_t error_cap
) {
    char relative[RSH_PATH_CAP];
    char path[RSH_PATH_CAP];
    char *slash;
    int errors = 0;

    if (!relative_directory || !relative_directory[0]) return 0;
    if (strlen(relative_directory) >= sizeof(relative)) {
        (void)snprintf(error, error_cap, "relative cleanup directory too long");
        return 1;
    }
    strcpy(relative, relative_directory);
    for (;;) {
        if (!rsh_join_path(path, sizeof(path), root, relative)) {
            if (errors++ == 0) (void)snprintf(error, error_cap, "cleanup path too long");
        } else if (rmdir(path) != 0 && errno != ENOENT) {
            if (errors++ == 0) {
                (void)snprintf(error, error_cap, "rmdir(%s): %s", path, strerror(errno));
            }
        }
        slash = strrchr(relative, '/');
        if (!slash) break;
        *slash = '\0';
    }
    return errors;
}

static void rsh_validate_suite(const rsh_suite_spec *suite) {
    size_t i;
    size_t j;

    if (!suite || !suite->suite_name || !suite->suite_name[0] ||
        !suite->temp_prefix || !rsh_absolute_prefix_valid(suite->temp_prefix) ||
        !suite->vendor_basename || !suite->vendor_basename[0] ||
        !suite->target_basename || !suite->target_basename[0] ||
        !suite->controlled_env_gates || suite->controlled_env_gate_count == 0 ||
        !suite->prepare || !suite->open || !suite->base_seed ||
        !suite->resolve_setup_profile || !suite->failure) {
        rsh_failf(suite, "FATAL: incomplete rewrite-smoke suite specification");
    }
    if (strchr(suite->vendor_basename, '/') || strchr(suite->target_basename, '/')) {
        rsh_failf(suite, "FATAL: suite DB basenames must be final path components");
    }
    for (i = 0; i < suite->controlled_env_gate_count; i++) {
        const char *gate = suite->controlled_env_gates[i];
        if (!gate || !gate[0] || strchr(gate, '=') != NULL) {
            rsh_failf(suite, "FATAL: invalid controlled environment gate at index %lu",
                      (unsigned long)i);
        }
        for (j = 0; j < i; j++) {
            if (strcmp(gate, suite->controlled_env_gates[j]) == 0) {
                rsh_failf(suite, "FATAL: duplicate controlled environment gate \"%s\"",
                          gate);
            }
        }
    }
}

static void rsh_validate_db_specs(
    const rsh_suite_spec *suite,
    const char *run_label,
    const char *phase_label,
    const rsh_db_spec *dbs,
    size_t db_count
) {
    size_t i;
    size_t j;

    if (!dbs || db_count == 0) {
        rsh_failf(suite, "FAIL [%s/%s/db-specs]: no database roles declared",
                  run_label, phase_label);
    }
    for (i = 0; i < db_count; i++) {
        const char *basename;
        if (!dbs[i].role || !dbs[i].role[0]) {
            rsh_failf(suite, "FAIL [%s/%s/db-%lu]: empty role",
                      run_label, phase_label, (unsigned long)i);
        }
        if (dbs[i].kind < RSH_DB_VENDOR || dbs[i].kind > RSH_DB_AUXILIARY) {
            rsh_failf(suite, "FAIL [%s/%s/%s/db-kind]: got=%d",
                      run_label, phase_label, dbs[i].role, (int)dbs[i].kind);
        }
        if (dbs[i].storage == RSH_DB_MEMORY) {
            if (strcmp(dbs[i].relative_path, ":memory:") != 0 ||
                dbs[i].kind != RSH_DB_AUXILIARY) {
                rsh_failf(
                    suite,
                    "FAIL [%s/%s/%s/memory-path]: path=\"%s\" kind=%d",
                    run_label, phase_label, dbs[i].role,
                    dbs[i].relative_path, (int)dbs[i].kind
                );
            }
        } else if (dbs[i].storage != RSH_DB_RELATIVE ||
                   !rsh_relative_path_valid(
                       dbs[i].relative_path, RSH_DB_RELATIVE_PATH_CAP
                   )) {
            rsh_failf(
                suite,
                "FAIL [%s/%s/%s/relative-path]: invalid path=\"%s\"",
                run_label, phase_label, dbs[i].role, dbs[i].relative_path
            );
        }
        basename = rsh_final_component(dbs[i].relative_path);
        if (dbs[i].kind == RSH_DB_CANDIDATE &&
            strcmp(basename, suite->target_basename) != 0) {
            rsh_failf(
                suite,
                "FAIL [%s/%s/%s/target-basename]: got=\"%s\" want=\"%s\"",
                run_label, phase_label, dbs[i].role,
                basename, suite->target_basename
            );
        }
        if (dbs[i].kind == RSH_DB_VENDOR &&
            strcmp(basename, suite->vendor_basename) != 0) {
            rsh_failf(
                suite,
                "FAIL [%s/%s/%s/vendor-basename]: got=\"%s\" want=\"%s\"",
                run_label, phase_label, dbs[i].role,
                basename, suite->vendor_basename
            );
        }
        for (j = 0; j < i; j++) {
            if (strcmp(dbs[i].role, dbs[j].role) == 0) {
                rsh_failf(suite, "FAIL [%s/%s/db-role-unique]: duplicate=\"%s\"",
                          run_label, phase_label, dbs[i].role);
            }
            if (strcmp(dbs[i].relative_path, dbs[j].relative_path) == 0) {
                rsh_failf(suite, "FAIL [%s/%s/db-path-unique]: duplicate=\"%s\"",
                          run_label, phase_label, dbs[i].relative_path);
            }
        }
    }
}

static void rsh_validate_env_profile(
    const rsh_suite_spec *suite,
    const rsh_run_spec *run,
    const char *profile_label,
    const rsh_env_assignment *assignments,
    size_t assignment_count,
    int complete_required
) {
    size_t i;
    size_t j;

    if (complete_required && (!assignments || assignment_count == 0)) {
        rsh_failf(suite, "FAIL [%s/%s-env]: complete profile is empty",
                  run->dispatch_name, profile_label);
    }
    if (!assignments && assignment_count != 0) {
        rsh_failf(suite, "FAIL [%s/%s-env]: rows=NULL count=%lu",
                  run->dispatch_name, profile_label, (unsigned long)assignment_count);
    }
    for (i = 0; i < assignment_count; i++) {
        const rsh_env_assignment *assignment = &assignments[i];
        if (!assignment->name || !assignment->name[0] ||
            strchr(assignment->name, '=') != NULL) {
            rsh_failf(suite, "FAIL [%s/%s-env-%lu]: invalid name",
                      run->dispatch_name, profile_label, (unsigned long)i);
        }
        if (assignment->value.state < RSH_ENV_UNSET ||
            assignment->value.state > RSH_ENV_VALUE) {
            rsh_failf(suite, "FAIL [%s/%s-env/%s]: invalid state=%d",
                      run->dispatch_name, profile_label, assignment->name,
                      (int)assignment->value.state);
        }
        if (assignment->value.state == RSH_ENV_VALUE &&
            (!assignment->value.value || !assignment->value.value[0])) {
            rsh_failf(suite, "FAIL [%s/%s-env/%s]: VALUE requires non-empty data",
                      run->dispatch_name, profile_label, assignment->name);
        }
        if (assignment->value.state == RSH_ENV_UNSET &&
            assignment->value.value && assignment->value.value[0]) {
            rsh_failf(suite, "FAIL [%s/%s-env/%s]: UNSET carries data",
                      run->dispatch_name, profile_label, assignment->name);
        }
        if (assignment->value.state == RSH_ENV_EMPTY &&
            assignment->value.value && assignment->value.value[0]) {
            rsh_failf(suite, "FAIL [%s/%s-env/%s]: EMPTY carries non-empty data",
                      run->dispatch_name, profile_label, assignment->name);
        }
        for (j = 0; j < i; j++) {
            if (strcmp(assignment->name, assignments[j].name) == 0) {
                rsh_failf(suite, "FAIL [%s/%s-env/%s]: duplicate assignment",
                          run->dispatch_name, profile_label, assignment->name);
            }
        }
    }
    if (complete_required) {
        for (i = 0; i < suite->controlled_env_gate_count; i++) {
            const char *gate = suite->controlled_env_gates[i];
            size_t matches = 0;
            for (j = 0; j < assignment_count; j++) {
                if (strcmp(gate, assignments[j].name) == 0) matches++;
            }
            if (matches == 0) {
                rsh_failf(suite, "FAIL [%s/%s-env/%s]: missing controlled gate",
                          run->dispatch_name, profile_label, gate);
            }
            if (matches != 1) {
                rsh_failf(suite, "FAIL [%s/%s-env/%s]: duplicate controlled gate",
                          run->dispatch_name, profile_label, gate);
            }
        }
    }
}

static void rsh_apply_env_profile(
    const rsh_suite_spec *suite,
    const rsh_run_spec *run,
    const char *profile_label,
    const rsh_env_assignment *assignments,
    size_t assignment_count,
    int complete_required
) {
    size_t i;

    rsh_validate_env_profile(
        suite, run, profile_label, assignments, assignment_count, complete_required
    );
    for (i = 0; i < assignment_count; i++) {
        const rsh_env_assignment *assignment = &assignments[i];
        int rc;
        if (assignment->value.state == RSH_ENV_UNSET) {
            rc = unsetenv(assignment->name);
        } else {
            const char *value = assignment->value.state == RSH_ENV_EMPTY
                ? "" : assignment->value.value;
            rc = setenv(assignment->name, value, 1);
        }
        if (rc != 0) {
            rsh_failf(
                suite,
                "FAIL [%s/%s-env/%s]: errno=%d error=%s",
                run->dispatch_name, profile_label, assignment->name,
                errno, strerror(errno)
            );
        }
    }
}

/* Call only in the forked child, before exec or the first SQLite call. */
static void rsh_apply_preload_env(
    const rsh_suite_spec *suite,
    const rsh_run_spec *run
) {
    rsh_apply_env_profile(
        suite, run, "preload", run->preload_env, run->preload_env_count,
        1
    );
}

static void rsh_apply_postload_env(
    const rsh_suite_spec *suite,
    const rsh_run_spec *run
) {
    rsh_apply_env_profile(
        suite, run, "postload", run->postload_env, run->postload_env_count, 0
    );
}

static rsh_apply_profile_fn rsh_require_profile(
    const rsh_suite_spec *suite,
    const char *run_label,
    const char *phase_label,
    const char *role,
    int profile
) {
    rsh_apply_profile_fn apply;

    if (profile == RSH_PROFILE_NONE) return NULL;
    apply = suite->resolve_setup_profile(profile, suite->suite_ctx);
    if (!apply) {
        rsh_failf(
            suite,
            "FAIL [%s/%s/%s/profile]: unregistered profile=%d",
            run_label, phase_label, role, profile
        );
    }
    return apply;
}

static void rsh_apply_declared_profile(
    const rsh_suite_spec *suite,
    const char *run_label,
    const char *phase_label,
    const char *role,
    sqlite3 *db,
    int profile,
    const char *stage
) {
    rsh_apply_profile_fn apply = rsh_require_profile(
        suite, run_label, phase_label, role, profile
    );
    int rc;

    if (!apply) return;
    rc = apply(db, profile, suite->suite_ctx);
    if (rc != SQLITE_OK) {
        rsh_failf(
            suite,
            "FAIL [%s/%s/%s/%s]: profile=%d rc=%d err=%s",
            run_label, phase_label, role, stage, profile, rc, sqlite3_errmsg(db)
        );
    }
}

static void rsh_open_seeded_db(
    const rsh_suite_spec *suite,
    pid_t owner_pid,
    const char *base_relative,
    const char *run_label,
    const char *phase_label,
    const rsh_db_spec *db_spec,
    rsh_db_handle *handle
) {
    char root[RSH_PATH_CAP];
    char base[RSH_PATH_CAP];
    char path[RSH_PATH_CAP];
    char error[RSH_PATH_CAP] = {0};
    sqlite3 *db = NULL;
    int rc;

    if (!rsh_root_for_pid(suite, owner_pid, root, sizeof(root))) {
        rsh_failf(suite, "FAIL [%s/%s/%s/root]: path too long",
                  run_label, phase_label, db_spec->role);
    }
    if (!rsh_ensure_directory(root, error, sizeof(error))) {
        rsh_failf(suite, "FAIL [%s/%s/%s/root]: %s",
                  run_label, phase_label, db_spec->role, error);
    }
    if (base_relative && base_relative[0]) {
        if (!rsh_relative_path_valid(base_relative, RSH_PATH_CAP) ||
            !rsh_ensure_relative_directories(root, base_relative, error, sizeof(error)) ||
            !rsh_join_path(base, sizeof(base), root, base_relative)) {
            rsh_failf(suite, "FAIL [%s/%s/%s/base-path]: %s",
                      run_label, phase_label, db_spec->role,
                      error[0] ? error : "invalid or overlong path");
        }
    } else {
        strcpy(base, root);
    }
    if (db_spec->storage == RSH_DB_MEMORY) {
        strcpy(path, ":memory:");
    } else {
        if (!rsh_ensure_parent_directories(
                base, db_spec->relative_path, error, sizeof(error)
            ) ||
            !rsh_join_path(path, sizeof(path), base, db_spec->relative_path)) {
            rsh_failf(suite, "FAIL [%s/%s/%s/path]: %s",
                      run_label, phase_label, db_spec->role,
                      error[0] ? error : "path too long");
        }
        if (!rsh_remove_db_files(path, error, sizeof(error))) {
            rsh_failf(suite, "FAIL [%s/%s/%s/stale-cleanup]: %s",
                      run_label, phase_label, db_spec->role, error);
        }
    }

    rc = suite->open(path, &db, suite->suite_ctx);
    if (rc != SQLITE_OK || !db) {
        if (db) (void)sqlite3_close(db);
        rsh_failf(
            suite,
            "FAIL [%s/%s/%s/open]: path=%s rc=%d db=%s",
            run_label, phase_label, db_spec->role, path, rc,
            db ? "non-NULL" : "NULL"
        );
    }
    rc = suite->base_seed(db, suite->suite_ctx);
    if (rc != SQLITE_OK) {
        char message[256];
        (void)snprintf(message, sizeof(message), "%s", sqlite3_errmsg(db));
        (void)sqlite3_close(db);
        rsh_failf(suite, "FAIL [%s/%s/%s/base-seed]: rc=%d err=%s",
                  run_label, phase_label, db_spec->role, rc, message);
    }
    rsh_apply_declared_profile(
        suite, run_label, phase_label, db_spec->role, db,
        db_spec->seed_profile, "seed-profile"
    );
    rsh_apply_declared_profile(
        suite, run_label, phase_label, db_spec->role, db,
        db_spec->setup_profile, "setup-profile"
    );
    handle->spec = db_spec;
    handle->db = db;
}

static const rsh_db_handle *rsh_find_db_handle(
    const rsh_db_handle *dbs,
    size_t db_count,
    const char *role
) {
    size_t i;

    if (!role) return NULL;
    for (i = 0; i < db_count; i++) {
        if (dbs[i].spec && strcmp(dbs[i].spec->role, role) == 0) return &dbs[i];
    }
    return NULL;
}

static sqlite3_stmt *rsh_prepare_checked(
    const rsh_suite_spec *suite,
    const rsh_case_spec *test_case,
    sqlite3 *db,
    const char *sql,
    const rsh_prepare_spec *prepare,
    const char *stage,
    const char **tail
) {
    sqlite3_stmt *stmt = NULL;
    const char *local_tail = NULL;
    const char **tail_out;
    int nbyte;
    int rc;

    if (!sql || !rsh_validate_prepare_spec(prepare)) {
        rsh_case_failf(suite, test_case, stage, "invalid SQL or prepare specification");
    }
    nbyte = rsh_compute_nbyte(suite, test_case, sql, prepare);
    tail_out = prepare->tail_kind == RSH_TAIL_NULL_OUT ? NULL : &local_tail;
    rc = suite->prepare(db, sql, nbyte, prepare->kind, &stmt, tail_out);
    if (rc != SQLITE_OK || !stmt) {
        if (stmt) (void)sqlite3_finalize(stmt);
        rsh_case_failf(
            suite, test_case, stage,
            "kind=%d nByte=%d rc=%d stmt=%s err=%s",
            (int)prepare->kind, nbyte, rc, stmt ? "non-NULL" : "NULL",
            sqlite3_errmsg(db)
        );
    }
    if (tail) *tail = local_tail;
    return stmt;
}

static void rsh_require_tail(
    const rsh_suite_spec *suite,
    const rsh_case_spec *test_case,
    const char *sql,
    const rsh_prepare_spec *prepare,
    const char *tail,
    const char *stage
) {
    const char *expected;

    if (prepare->tail_kind == RSH_TAIL_NULL_OUT ||
        prepare->tail_kind == RSH_TAIL_IGNORE) {
        return;
    }
    if (prepare->tail_kind == RSH_TAIL_OFFSET &&
        prepare->tail_offset > strlen(sql)) {
        rsh_case_failf(
            suite, test_case, stage,
            "declared offset=%lu exceeds SQL length=%lu",
            (unsigned long)prepare->tail_offset,
            (unsigned long)strlen(sql)
        );
    }
    expected = prepare->tail_kind == RSH_TAIL_FULL
        ? sql + strlen(sql) : sql + prepare->tail_offset;
    if (tail != expected) {
        rsh_case_failf(
            suite, test_case, stage,
            "got=%p want=%p expected_offset=%lu",
            (const void *)tail, (const void *)expected,
            (unsigned long)(expected - sql)
        );
    }
}

static void rsh_require_saved_sql(
    const rsh_suite_spec *suite,
    const rsh_case_spec *test_case,
    sqlite3_stmt *stmt,
    const char *expected,
    const char *stage
) {
    const char *actual = sqlite3_sql(stmt);

    if (!actual || !expected || strcmp(actual, expected) != 0) {
        rsh_case_failf(
            suite, test_case, stage,
            "got=\"%s\" want=\"%s\"",
            actual ? actual : "(null)", expected ? expected : "(null)"
        );
    }
}

static void rsh_finalize_checked(
    const rsh_suite_spec *suite,
    const rsh_case_spec *test_case,
    sqlite3_stmt *stmt,
    const char *stage
) {
    int rc = sqlite3_finalize(stmt);
    if (rc != SQLITE_OK) {
        rsh_case_failf(suite, test_case, stage, "rc=%d", rc);
    }
}

static void rsh_require_bind_contract(
    const rsh_suite_spec *suite,
    const rsh_case_spec *test_case,
    sqlite3_stmt *stmt,
    int check_count,
    int expected_count,
    const char *const *expected_names,
    size_t expected_name_count
) {
    int actual_count = sqlite3_bind_parameter_count(stmt);
    size_t i;

    if (check_count && actual_count != expected_count) {
        rsh_case_failf(suite, test_case, "bind-count", "got=%d want=%d",
                       actual_count, expected_count);
    }
    if (expected_name_count != 0) {
        if (!expected_names || expected_name_count != (size_t)actual_count) {
            rsh_case_failf(
                suite, test_case, "bind-names",
                "rows=%lu bind_count=%d",
                (unsigned long)expected_name_count, actual_count
            );
        }
        for (i = 0; i < expected_name_count; i++) {
            const char *actual = sqlite3_bind_parameter_name(stmt, (int)i + 1);
            if (!rsh_nullable_text_equal(actual, expected_names[i])) {
                rsh_case_failf(
                    suite, test_case, "bind-name",
                    "index=%lu got=%s want=%s",
                    (unsigned long)i + 1,
                    actual ? actual : "(null)",
                    expected_names[i] ? expected_names[i] : "(null)"
                );
            }
        }
    }
}

static void rsh_require_column_contract(
    const rsh_suite_spec *suite,
    const rsh_case_spec *test_case,
    sqlite3_stmt *stmt,
    int check_count,
    int expected_count,
    const rsh_column_expectation *expected_columns,
    size_t expected_columns_count
) {
    int actual_count = sqlite3_column_count(stmt);
    size_t i;

    if (check_count && actual_count != expected_count) {
        rsh_case_failf(suite, test_case, "column-count", "got=%d want=%d",
                       actual_count, expected_count);
    }
    if (expected_columns_count != 0) {
        if (!expected_columns || expected_columns_count != (size_t)actual_count) {
            rsh_case_failf(
                suite, test_case, "columns",
                "rows=%lu column_count=%d",
                (unsigned long)expected_columns_count, actual_count
            );
        }
        for (i = 0; i < expected_columns_count; i++) {
            const char *name = sqlite3_column_name(stmt, (int)i);
            const char *decl_type = sqlite3_column_decltype(stmt, (int)i);
#ifdef EMBY_FTS_REWRITE_DIRECT_TEST
            const char *table_name = NULL;
            const char *origin_name = NULL;
#else
            const char *table_name = sqlite3_column_table_name(stmt, (int)i);
            const char *origin_name = sqlite3_column_origin_name(stmt, (int)i);
#endif
            if (!rsh_nullable_text_equal(name, expected_columns[i].name) ||
                !rsh_nullable_text_equal(decl_type, expected_columns[i].decl_type) ||
                !rsh_nullable_text_equal(table_name, expected_columns[i].table_name) ||
                !rsh_nullable_text_equal(origin_name, expected_columns[i].origin_name)) {
                rsh_case_failf(
                    suite, test_case, "column-metadata",
                    "index=%lu got={name=%s decltype=%s table=%s origin=%s} "
                    "want={name=%s decltype=%s table=%s origin=%s}",
                    (unsigned long)i,
                    name ? name : "(null)",
                    decl_type ? decl_type : "(null)",
                    table_name ? table_name : "(null)",
                    origin_name ? origin_name : "(null)",
                    expected_columns[i].name ? expected_columns[i].name : "(null)",
                    expected_columns[i].decl_type
                        ? expected_columns[i].decl_type : "(null)",
                    expected_columns[i].table_name
                        ? expected_columns[i].table_name : "(null)",
                    expected_columns[i].origin_name
                        ? expected_columns[i].origin_name : "(null)"
                );
            }
        }
    }
}

static const rsh_db_handle *rsh_require_case_db(
    const rsh_suite_spec *suite,
    const rsh_case_spec *test_case,
    const rsh_case_context *context,
    const char *role
) {
    const rsh_db_handle *handle = rsh_find_db_handle(
        context->dbs, context->db_count, role
    );

    if (!handle || !handle->db) {
        rsh_case_failf(
            suite, test_case, "role",
            "role=\"%s\" is not open", role ? role : "(null)"
        );
    }
    return handle;
}

static void rsh_run_sql_exact(
    const rsh_suite_spec *suite,
    const rsh_case_spec *test_case,
    const rsh_case_context *context,
    const rsh_sql_exact_spec *spec
) {
    const rsh_db_handle *handle = rsh_require_case_db(
        suite, test_case, context, spec->role
    );
    const char *tail = NULL;
    sqlite3_stmt *stmt = rsh_prepare_checked(
        suite, test_case, handle->db, spec->sql, &spec->prepare, "prepare", &tail
    );

    rsh_require_saved_sql(
        suite, test_case, stmt,
        spec->expected_sql ? spec->expected_sql : spec->sql,
        "saved-sql"
    );
    rsh_require_tail(suite, test_case, spec->sql, &spec->prepare, tail, "tail");
    rsh_require_bind_contract(
        suite, test_case, stmt,
        spec->check_bind_count, spec->expected_bind_count,
        spec->expected_bind_names, spec->expected_bind_name_count
    );
    rsh_require_column_contract(
        suite, test_case, stmt,
        spec->check_column_count, spec->expected_column_count,
        spec->expected_columns, spec->expected_columns_count
    );
    rsh_finalize_checked(suite, test_case, stmt, "finalize");
}

static char *rsh_read_fixture(
    const rsh_suite_spec *suite,
    const rsh_case_spec *test_case,
    const char *path,
    int strip_final_lf
) {
    FILE *file;
    long size;
    char *buffer;

    if (!path || !path[0]) {
        rsh_case_failf(suite, test_case, "fixture", "empty path");
    }
    file = fopen(path, "rb");
    if (!file) {
        rsh_case_failf(suite, test_case, "fixture-open", "path=%s error=%s",
                       path, strerror(errno));
    }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        (void)fclose(file);
        rsh_case_failf(suite, test_case, "fixture-seek", "path=%s", path);
    }
    buffer = (char *)malloc((size_t)size + 1);
    if (!buffer) {
        (void)fclose(file);
        rsh_case_failf(suite, test_case, "fixture-alloc", "bytes=%ld", size + 1);
    }
    if (fread(buffer, 1, (size_t)size, file) != (size_t)size) {
        free(buffer);
        (void)fclose(file);
        rsh_case_failf(suite, test_case, "fixture-read", "path=%s", path);
    }
    if (fclose(file) != 0) {
        free(buffer);
        rsh_case_failf(suite, test_case, "fixture-close", "path=%s", path);
    }
    buffer[size] = '\0';
    if (strip_final_lf) {
        if (size == 0 || buffer[size - 1] != '\n') {
            free(buffer);
            rsh_case_failf(
                suite, test_case, "fixture-framing",
                "path=%s requires one final LF", path
            );
        }
        buffer[--size] = '\0';
    }
    return buffer;
}

static char *rsh_replace_once(
    const rsh_suite_spec *suite,
    const rsh_case_spec *test_case,
    const char *base,
    const char *needle,
    const char *replacement
) {
    const char *match = strstr(base, needle);
    size_t prefix;
    size_t output_len;
    char *output;

    if (!match || strstr(match + strlen(needle), needle) != NULL) {
        rsh_case_failf(
            suite, test_case, "needle-unique",
            "replacement needle=\"%s\" count=%d want=1",
            needle, rsh_count_occurrences(base, needle)
        );
    }
    prefix = (size_t)(match - base);
    output_len = strlen(base) - strlen(needle) + strlen(replacement);
    output = (char *)malloc(output_len + 1);
    if (!output) {
        rsh_case_failf(suite, test_case, "needle-unique", "allocation failed");
    }
    memcpy(output, base, prefix);
    memcpy(output + prefix, replacement, strlen(replacement));
    strcpy(output + prefix + strlen(replacement), match + strlen(needle));
    return output;
}

static char *rsh_negative_sql(
    const rsh_suite_spec *suite,
    const rsh_case_spec *test_case,
    const rsh_negative_spec *spec,
    int *owned
) {
    const char *sql;
    char *generated;

    *owned = 0;
    if (!spec->discriminating_needle || !spec->discriminating_needle[0]) {
        rsh_case_failf(suite, test_case, "needle-unique", "empty discriminator");
    }
    if (spec->source_kind == RSH_NEGATIVE_STATIC) {
        sql = spec->sql;
        if (!sql || !sql[0]) {
            rsh_case_failf(suite, test_case, "needle-unique", "empty static SQL");
        }
    } else if (spec->source_kind == RSH_NEGATIVE_FIXTURE) {
        generated = rsh_read_fixture(
            suite, test_case, spec->fixture_path, spec->strip_fixture_final_lf
        );
        sql = generated;
        *owned = 1;
    } else if (spec->source_kind == RSH_NEGATIVE_GENERATED) {
        if (!spec->base_sql || !spec->base_sql[0] ||
            !spec->replacement_needle || !spec->replacement_needle[0] ||
            !spec->replacement || !spec->replacement[0] ||
            strcmp(spec->replacement_needle, spec->replacement) == 0) {
            rsh_case_failf(
                suite, test_case, "needle-unique",
                "generated replacement data is empty or unchanged"
            );
        }
        generated = rsh_replace_once(
            suite, test_case, spec->base_sql,
            spec->replacement_needle, spec->replacement
        );
        if (strcmp(generated, spec->base_sql) == 0) {
            free(generated);
            rsh_case_failf(suite, test_case, "needle-unique", "generated SQL is unchanged");
        }
        sql = generated;
        *owned = 1;
    } else {
        rsh_case_failf(suite, test_case, "needle-unique", "invalid source kind=%d",
                       (int)spec->source_kind);
        return NULL;
    }
    if (rsh_count_occurrences(sql, spec->discriminating_needle) != 1) {
        if (*owned) free((void *)sql);
        rsh_case_failf(
            suite, test_case, "needle-unique",
            "discriminator=\"%s\" got=%d want=1",
            spec->discriminating_needle,
            rsh_count_occurrences(sql, spec->discriminating_needle)
        );
    }
    return (char *)sql;
}

static void rsh_run_negative(
    const rsh_suite_spec *suite,
    const rsh_case_spec *test_case,
    const rsh_case_context *context,
    const rsh_negative_spec *spec
) {
    int owned = 0;
    char *sql = rsh_negative_sql(suite, test_case, spec, &owned);
    const rsh_db_handle *vendor = rsh_require_case_db(
        suite, test_case, context, spec->vendor_role
    );
    const rsh_db_handle *candidate = rsh_require_case_db(
        suite, test_case, context, spec->candidate_role
    );
    const char *tail = NULL;
    const char *saved;
    sqlite3_stmt *stmt;
    size_t i;

    if (vendor->spec->kind != RSH_DB_VENDOR ||
        candidate->spec->kind != RSH_DB_CANDIDATE ||
        vendor->db == candidate->db) {
        if (owned) free(sql);
        rsh_case_failf(
            suite, test_case, "roles",
            "vendor_kind=%d candidate_kind=%d same_handle=%d",
            (int)vendor->spec->kind, (int)candidate->spec->kind,
            vendor->db == candidate->db
        );
    }

    /* Stage (c): the independent vendor floor follows construction stage (a). */
    stmt = rsh_prepare_checked(
        suite, test_case, vendor->db, sql, &spec->prepare, "vendor-prepare", &tail
    );
    saved = sqlite3_sql(stmt);
    if (!saved || strcmp(saved, sql) != 0) {
        rsh_case_failf(
            suite, test_case, "vendor-sql",
            "got=\"%s\" want=\"%s\"", saved ? saved : "(null)", sql
        );
    }
    rsh_require_tail(
        suite, test_case, sql, &spec->prepare, tail, "vendor-tail"
    );
    rsh_finalize_checked(suite, test_case, stmt, "vendor-finalize");

    /* Stage (b): the true target must prepare the original bytes unchanged. */
    tail = NULL;
    stmt = rsh_prepare_checked(
        suite, test_case, candidate->db, sql, &spec->prepare, "matcher-prepare", &tail
    );
    saved = sqlite3_sql(stmt);
    if (!saved || strcmp(saved, sql) != 0) {
        rsh_case_failf(
            suite, test_case, "matcher-miss",
            "got=\"%s\" want-original=\"%s\"", saved ? saved : "(null)", sql
        );
    }
    rsh_require_tail(
        suite, test_case, sql, &spec->prepare, tail, "matcher-tail"
    );
    rsh_require_bind_contract(
        suite, test_case, stmt,
        spec->check_bind_count, spec->expected_bind_count, NULL, 0
    );
    rsh_require_column_contract(
        suite, test_case, stmt,
        spec->check_column_count, spec->expected_column_count, NULL, 0
    );
    for (i = 0; i < spec->followup_occurrence_count; i++) {
        int actual = rsh_count_occurrences(
            saved, spec->followup_occurrences[i].needle
        );
        if (actual != spec->followup_occurrences[i].expected_count) {
            (void)sqlite3_finalize(stmt);
            if (owned) free(sql);
            rsh_case_failf(
                suite, test_case, "followup-occurrence",
                "needle=\"%s\" got=%d want=%d",
                spec->followup_occurrences[i].needle, actual,
                spec->followup_occurrences[i].expected_count
            );
        }
    }
    rsh_finalize_checked(suite, test_case, stmt, "matcher-finalize");
    if (owned) free(sql);
}

static sqlite3_stmt *rsh_parity_prepare(
    sqlite3 *db,
    const char *label,
    const char *sql,
    const char **tail
) {
    const rsh_suite_spec *suite = rsh_parity_state.suite;
    const rsh_case_spec *test_case = rsh_parity_state.test_case;
    sqlite3_stmt *stmt = NULL;
    int nbyte;
    int rc;

    (void)label;
    nbyte = rsh_compute_nbyte(
        suite, test_case, sql, &rsh_parity_state.prepare
    );
    rc = suite->prepare(
        db, sql, nbyte, rsh_parity_state.prepare.kind, &stmt, tail
    );
    if (rc != SQLITE_OK || !stmt) {
        if (stmt) (void)sqlite3_finalize(stmt);
        rsh_case_failf(
            suite, test_case, "prepare",
            "kind=%d nByte=%d rc=%d stmt=%s err=%s",
            (int)rsh_parity_state.prepare.kind, nbyte, rc,
            stmt ? "non-NULL" : "NULL", sqlite3_errmsg(db)
        );
    }
    return stmt;
}

static void rsh_run_contract_parity(
    const rsh_suite_spec *suite,
    const rsh_case_spec *test_case,
    const rsh_case_context *context,
    const rsh_contract_parity_spec *spec
) {
    const rsh_db_handle *vendor = rsh_require_case_db(
        suite, test_case, context, spec->vendor_role
    );
    const rsh_db_handle *candidate = rsh_require_case_db(
        suite, test_case, context, spec->candidate_role
    );

    if (!rsh_validate_prepare_spec(&spec->prepare) ||
        spec->prepare.tail_kind != RSH_TAIL_FULL || spec->minimum_rows < 0) {
        rsh_case_failf(
            suite, test_case, "prepare-contract",
            "parity requires a full-tail raw prepare"
        );
    }
    if (vendor->spec->kind != RSH_DB_VENDOR ||
        candidate->spec->kind != RSH_DB_CANDIDATE) {
        rsh_case_failf(suite, test_case, "roles", "parity roles are not vendor/candidate");
    }
    rsh_parity_state.suite = suite;
    rsh_parity_state.test_case = test_case;
    rsh_parity_state.prepare = spec->prepare;
    rsh_contract_suite = suite;
    contract_parity_require_min_rows_mode(
        vendor->db,
        candidate->db,
        rsh_parity_prepare,
        test_case->label,
        spec->vendor_sql,
        spec->candidate_source_sql,
        spec->expected_candidate_sql,
        spec->bind,
        spec->bind_ctx,
        spec->row_exception,
        spec->row_exception_ctx,
        spec->minimum_rows,
        1
    );
    rsh_contract_suite = NULL;
    memset(&rsh_parity_state, 0, sizeof(rsh_parity_state));
}

static void rsh_run_scalar(
    const rsh_suite_spec *suite,
    const rsh_case_spec *test_case,
    const rsh_case_context *context,
    const rsh_scalar_spec *spec
) {
    const rsh_db_handle *handle = rsh_require_case_db(
        suite, test_case, context, spec->role
    );
    const char *tail = NULL;
    sqlite3_stmt *stmt;
    int rc;
    int actual_type;
    int expected_type;

    if (spec->reset_counter) spec->reset_counter(spec->counter_ctx);
    stmt = rsh_prepare_checked(
        suite, test_case, handle->db, spec->sql, &spec->prepare, "prepare", &tail
    );
    rsh_require_saved_sql(
        suite, test_case, stmt,
        spec->expected_sql ? spec->expected_sql : spec->sql,
        "saved-sql"
    );
    rsh_require_tail(suite, test_case, spec->sql, &spec->prepare, tail, "tail");
    if (spec->bind && spec->bind(stmt, test_case->label, spec->bind_ctx) != SQLITE_OK) {
        (void)sqlite3_finalize(stmt);
        rsh_case_failf(suite, test_case, "bind", "adapter returned non-OK");
    }
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW || sqlite3_column_count(stmt) != 1) {
        (void)sqlite3_finalize(stmt);
        rsh_case_failf(suite, test_case, "step", "rc=%d columns=%d want=ROW/1",
                       rc, sqlite3_column_count(stmt));
    }
    actual_type = sqlite3_column_type(stmt, 0);
    expected_type = spec->value_kind == RSH_SCALAR_NULL ? SQLITE_NULL
        : spec->value_kind == RSH_SCALAR_INTEGER ? SQLITE_INTEGER
        : spec->value_kind == RSH_SCALAR_FLOAT ? SQLITE_FLOAT
        : spec->value_kind == RSH_SCALAR_TEXT ? SQLITE_TEXT
        : spec->value_kind == RSH_SCALAR_BLOB ? SQLITE_BLOB : -1;
    if (actual_type != expected_type) {
        (void)sqlite3_finalize(stmt);
        rsh_case_failf(suite, test_case, "type", "got=%d want=%d",
                       actual_type, expected_type);
    }
    if (spec->value_kind == RSH_SCALAR_INTEGER &&
        sqlite3_column_int64(stmt, 0) != spec->integer_value) {
        sqlite3_int64 actual = sqlite3_column_int64(stmt, 0);
        (void)sqlite3_finalize(stmt);
        rsh_case_failf(suite, test_case, "value", "got=%lld want=%lld",
                       (long long)actual, (long long)spec->integer_value);
    } else if (spec->value_kind == RSH_SCALAR_FLOAT) {
        double actual = sqlite3_column_double(stmt, 0);
        if (memcmp(&actual, &spec->float_value, sizeof(actual)) != 0) {
            (void)sqlite3_finalize(stmt);
            rsh_case_failf(suite, test_case, "value", "floating values differ");
        }
    } else if (spec->value_kind == RSH_SCALAR_TEXT ||
               spec->value_kind == RSH_SCALAR_BLOB) {
        const void *actual = spec->value_kind == RSH_SCALAR_TEXT
            ? (const void *)sqlite3_column_text(stmt, 0)
            : sqlite3_column_blob(stmt, 0);
        int actual_bytes = sqlite3_column_bytes(stmt, 0);
        if (spec->bytes_count < 0 || actual_bytes != spec->bytes_count ||
            (actual_bytes != 0 &&
             (!actual || !spec->bytes_value ||
              memcmp(actual, spec->bytes_value, (size_t)actual_bytes) != 0))) {
            (void)sqlite3_finalize(stmt);
            rsh_case_failf(suite, test_case, "value", "bytes=%d want=%d",
                           actual_bytes, spec->bytes_count);
        }
    }
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        (void)sqlite3_finalize(stmt);
        rsh_case_failf(suite, test_case, "done", "rc=%d want=%d", rc, SQLITE_DONE);
    }
    rsh_finalize_checked(suite, test_case, stmt, "finalize");
    if (spec->read_counter) {
        int actual_counter = spec->read_counter(spec->counter_ctx);
        if (actual_counter != spec->expected_counter) {
            rsh_case_failf(suite, test_case, "counter", "got=%d want=%d",
                           actual_counter, spec->expected_counter);
        }
    }
}

static void rsh_run_exact_ids(
    const rsh_suite_spec *suite,
    const rsh_case_spec *test_case,
    const rsh_case_context *context,
    const rsh_exact_ids_spec *spec
) {
    const rsh_db_handle *handle = rsh_require_case_db(
        suite, test_case, context, spec->role
    );
    const char *tail = NULL;
    sqlite3_stmt *stmt = rsh_prepare_checked(
        suite, test_case, handle->db, spec->sql, &spec->prepare, "prepare", &tail
    );
    size_t row = 0;
    int rc;

    if (spec->expected_id_count != 0 && !spec->expected_ids) {
        (void)sqlite3_finalize(stmt);
        rsh_case_failf(suite, test_case, "ids", "expected rows are NULL");
    }

    rsh_require_tail(suite, test_case, spec->sql, &spec->prepare, tail, "tail");
    if (spec->bind && spec->bind(stmt, test_case->label, spec->bind_ctx) != SQLITE_OK) {
        (void)sqlite3_finalize(stmt);
        rsh_case_failf(suite, test_case, "bind", "adapter returned non-OK");
    }
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        sqlite3_int64 actual;
        if (sqlite3_column_count(stmt) < 1 || row >= spec->expected_id_count) {
            (void)sqlite3_finalize(stmt);
            rsh_case_failf(suite, test_case, "ids", "unexpected row=%lu",
                           (unsigned long)row);
        }
        actual = sqlite3_column_int64(stmt, 0);
        if (actual != spec->expected_ids[row]) {
            (void)sqlite3_finalize(stmt);
            rsh_case_failf(suite, test_case, "ids", "row=%lu got=%lld want=%lld",
                           (unsigned long)row, (long long)actual,
                           (long long)spec->expected_ids[row]);
        }
        row++;
    }
    if (rc != SQLITE_DONE || row != spec->expected_id_count) {
        (void)sqlite3_finalize(stmt);
        rsh_case_failf(suite, test_case, "ids", "rc=%d rows=%lu want=%lu",
                       rc, (unsigned long)row,
                       (unsigned long)spec->expected_id_count);
    }
    rsh_finalize_checked(suite, test_case, stmt, "finalize");
}

static void rsh_run_fixture(
    const rsh_suite_spec *suite,
    const rsh_case_spec *test_case,
    const rsh_case_context *context,
    const rsh_fixture_spec *spec
) {
    char *source = rsh_read_fixture(
        suite, test_case, spec->source_path, spec->strip_final_lf
    );
    char *expected = spec->expected_path
        ? rsh_read_fixture(suite, test_case, spec->expected_path, spec->strip_final_lf)
        : NULL;

    if (spec->assertion_kind == RSH_FIXTURE_SQL_EXACT) {
        rsh_sql_exact_spec assertion = spec->sql_exact;
        assertion.sql = source;
        assertion.expected_sql = expected ? expected : source;
        rsh_run_sql_exact(suite, test_case, context, &assertion);
    } else if (spec->assertion_kind == RSH_FIXTURE_NEGATIVE) {
        rsh_negative_spec assertion = spec->negative;
        assertion.source_kind = RSH_NEGATIVE_STATIC;
        assertion.sql = source;
        rsh_run_negative(suite, test_case, context, &assertion);
    } else if (spec->assertion_kind == RSH_FIXTURE_CONTRACT_PARITY) {
        rsh_contract_parity_spec assertion = spec->contract_parity;
        assertion.vendor_sql = source;
        assertion.candidate_source_sql = source;
        assertion.expected_candidate_sql = expected;
        rsh_run_contract_parity(suite, test_case, context, &assertion);
    } else {
        free(expected);
        free(source);
        rsh_case_failf(suite, test_case, "fixture", "invalid assertion kind=%d",
                       (int)spec->assertion_kind);
    }
    free(expected);
    free(source);
}

static void rsh_dispatch_case(
    const rsh_suite_spec *suite,
    const rsh_run_spec *run,
    const rsh_case_spec *test_case,
    const rsh_case_context *context
) {
    if (!test_case->label || !test_case->label[0]) {
        rsh_failf(suite, "FAIL [%s/case-label]: empty label", run->dispatch_name);
    }
    if (test_case->kind < RSH_CASE_SQL_EXACT ||
        test_case->kind > RSH_CASE_CUSTOM) {
        rsh_case_failf(suite, test_case, "kind", "invalid kind=%d",
                       (int)test_case->kind);
    }
    if (test_case->kind == RSH_CASE_SQL_EXACT) {
        rsh_run_sql_exact(suite, test_case, context, &test_case->data.sql_exact);
    } else if (test_case->kind == RSH_CASE_NEGATIVE) {
        rsh_run_negative(suite, test_case, context, &test_case->data.negative);
    } else if (test_case->kind == RSH_CASE_CONTRACT_PARITY) {
        rsh_run_contract_parity(
            suite, test_case, context, &test_case->data.contract_parity
        );
    } else if (test_case->kind == RSH_CASE_SCALAR) {
        rsh_run_scalar(suite, test_case, context, &test_case->data.scalar);
    } else if (test_case->kind == RSH_CASE_EXACT_IDS) {
        rsh_run_exact_ids(suite, test_case, context, &test_case->data.exact_ids);
    } else if (test_case->kind == RSH_CASE_FIXTURE) {
        rsh_run_fixture(suite, test_case, context, &test_case->data.fixture);
    } else if (test_case->kind == RSH_CASE_PATH) {
        const rsh_path_spec *path = &test_case->data.path;
        rsh_run_sql_exact(suite, test_case, context, &path->assertion);
        if (path->assert_after_prepare &&
            path->assert_after_prepare(context, path->immutable_data) != SQLITE_OK) {
            rsh_case_failf(suite, test_case, "path-assert", "adapter returned non-OK");
        }
    } else if (test_case->kind == RSH_CASE_INDEX_PROBE) {
        const rsh_index_probe_spec *probe = &test_case->data.index_probe;
        (void)rsh_require_case_db(suite, test_case, context, probe->role);
        if (!probe->assert_probe ||
            probe->assert_probe(
                context, probe->mode_id, probe->expected_delta, probe->assert_ctx
            ) != SQLITE_OK) {
            rsh_case_failf(suite, test_case, "index-probe", "adapter returned non-OK");
        }
    } else if (test_case->kind == RSH_CASE_EXPECT_ABORT) {
        if (run->outcome != RSH_OUTCOME_EXPECT_ABORT) {
            rsh_case_failf(suite, test_case, "outcome", "run is not parent-adjudicated");
        }
        rsh_run_negative(
            suite, test_case, context, &test_case->data.expect_abort.negative
        );
    } else {
        const rsh_custom_spec *custom = &test_case->data.custom;
        if (custom->kind < RSH_CUSTOM_IDENTITY ||
            custom->kind > RSH_CUSTOM_LOG_CAPTURE || !custom->assert_custom) {
            rsh_case_failf(suite, test_case, "custom-kind", "invalid bounded custom hook");
        }
        if (custom->kind == RSH_CUSTOM_LOG_CAPTURE && !context->captured_stderr) {
            rsh_case_failf(
                suite, test_case, "custom-capture",
                "log assertions run only after the capture scope closes"
            );
        }
        if (custom->assert_custom(context, custom->immutable_data) != SQLITE_OK) {
            rsh_case_failf(suite, test_case, "custom-assert", "adapter returned non-OK");
        }
    }
}

static void rsh_dispatch_cases(
    const rsh_suite_spec *suite,
    const rsh_run_spec *run,
    const rsh_case_spec *cases,
    size_t case_count,
    const rsh_case_context *context
) {
    size_t i;

    if (!cases && case_count != 0) {
        rsh_failf(suite, "FAIL [%s/cases]: rows=NULL count=%lu",
                  run->dispatch_name, (unsigned long)case_count);
    }
    for (i = 0; i < case_count; i++) {
        if (rsh_build_enabled(cases[i].build_mask, context->active_build) &&
            (!cases[i].runtime_predicate ||
             cases[i].runtime_predicate(
                 &cases[i], context, suite->suite_ctx
             ))) {
            rsh_dispatch_case(suite, run, &cases[i], context);
        }
    }
}

static int rsh_close_db_handles(
    rsh_db_handle *handles,
    size_t handle_count,
    const char **failed_role,
    int *failed_rc
) {
    size_t i;
    int errors = 0;

    *failed_role = NULL;
    *failed_rc = SQLITE_OK;
    for (i = handle_count; i > 0; i--) {
        rsh_db_handle *handle = &handles[i - 1];
        if (handle->db) {
            int rc = sqlite3_close(handle->db);
            if (rc != SQLITE_OK) {
                if (errors == 0) {
                    *failed_role = handle->spec ? handle->spec->role : "(unknown)";
                    *failed_rc = rc;
                }
                errors++;
            } else {
                handle->db = NULL;
            }
        }
    }
    return errors;
}

static void rsh_run_phase(
    const rsh_suite_spec *suite,
    const rsh_run_spec *run,
    const rsh_phase_spec *phase,
    pid_t owner_pid,
    const char *captured_stderr,
    uint32_t active_build
) {
    rsh_db_handle *handles;
    rsh_case_context context;
    char root[RSH_PATH_CAP];
    char cleanup_error[RSH_PATH_CAP] = {0};
    const char *failed_role;
    int failed_rc;
    int close_errors;
    int cleanup_errors;
    size_t i;

    if (!phase->label || !phase->label[0]) {
        rsh_failf(suite, "FAIL [%s/phase-label]: empty label", run->dispatch_name);
    }
    rsh_validate_db_specs(
        suite, run->dispatch_name, phase->label, phase->dbs, phase->db_count
    );
    handles = (rsh_db_handle *)calloc(phase->db_count, sizeof(*handles));
    if (!handles) {
        rsh_failf(suite, "FAIL [%s/%s/alloc]: db_count=%lu",
                  run->dispatch_name, phase->label, (unsigned long)phase->db_count);
    }
    for (i = 0; i < phase->db_count; i++) {
        rsh_open_seeded_db(
            suite, owner_pid, NULL, run->dispatch_name, phase->label,
            &phase->dbs[i], &handles[i]
        );
    }
    memset(&context, 0, sizeof(context));
    context.suite_name = suite->suite_name;
    context.run_name = run->dispatch_name;
    context.phase_label = phase->label;
    context.dbs = handles;
    context.db_count = phase->db_count;
    context.captured_stderr = captured_stderr;
    context.active_build = active_build;
    rsh_dispatch_cases(
        suite, run, phase->cases, phase->case_count, &context
    );

    close_errors = rsh_close_db_handles(
        handles, phase->db_count, &failed_role, &failed_rc
    );
    if (!rsh_root_for_pid(suite, owner_pid, root, sizeof(root))) {
        free(handles);
        rsh_failf(suite, "FAIL [%s/%s/root]: path too long",
                  run->dispatch_name, phase->label);
    }
    cleanup_errors = rsh_cleanup_db_specs_at_root(
        root, phase->dbs, phase->db_count, cleanup_error, sizeof(cleanup_error)
    );
    free(handles);
    if (close_errors != 0) {
        rsh_failf(
            suite, "FAIL [%s/%s/%s/close]: rc=%d close_errors=%d cleanup_errors=%d",
            run->dispatch_name, phase->label, failed_role, failed_rc,
            close_errors, cleanup_errors
        );
    }
    if (cleanup_errors != 0) {
        rsh_failf(suite, "FAIL [%s/%s/cleanup]: errors=%d first=%s",
                  run->dispatch_name, phase->label, cleanup_errors, cleanup_error);
    }
}

static size_t rsh_matrix_product(
    const rsh_suite_spec *suite,
    const rsh_run_spec *run,
    const rsh_matrix_phase_spec *phase
) {
    size_t product = 1;
    size_t i;

    if (!phase->label || !phase->label[0] ||
        !rsh_relative_path_valid(
            phase->cell_dir_prefix, RSH_MATRIX_CELL_DIR_CAP
        )) {
        rsh_failf(suite, "FAIL [%s/matrix-spec]: invalid label or cell_dir_prefix",
                  run->dispatch_name);
    }
    if (!phase->axes || phase->axis_count == 0 ||
        phase->axis_count > RSH_MATRIX_MAX_AXES) {
        rsh_failf(suite, "FAIL [%s/%s/axes]: got=%lu cap=%d",
                  run->dispatch_name, phase->label,
                  (unsigned long)phase->axis_count, RSH_MATRIX_MAX_AXES);
    }
    for (i = 0; i < phase->axis_count; i++) {
        size_t j;
        if (!phase->axes[i].name || !phase->axes[i].name[0] ||
            !phase->axes[i].values || phase->axes[i].value_count == 0 ||
            phase->axes[i].value_count > RSH_MATRIX_MAX_AXIS_VALUES) {
            rsh_failf(suite, "FAIL [%s/%s/axis-%lu]: invalid declaration",
                      run->dispatch_name, phase->label, (unsigned long)i);
        }
        for (j = 0; j < phase->axes[i].value_count; j++) {
            size_t earlier;
            if (!phase->axes[i].values[j].label ||
                !phase->axes[i].values[j].label[0]) {
                rsh_failf(suite, "FAIL [%s/%s/axis-%lu-value-%lu]: empty label",
                          run->dispatch_name, phase->label,
                          (unsigned long)i, (unsigned long)j);
            }
            for (earlier = 0; earlier < j; earlier++) {
                if (strcmp(
                        phase->axes[i].values[j].label,
                        phase->axes[i].values[earlier].label
                    ) == 0) {
                    rsh_failf(suite, "FAIL [%s/%s/axis-%lu]: duplicate value=\"%s\"",
                              run->dispatch_name, phase->label,
                              (unsigned long)i, phase->axes[i].values[j].label);
                }
            }
        }
        for (j = 0; j < i; j++) {
            if (strcmp(phase->axes[i].name, phase->axes[j].name) == 0) {
                rsh_failf(suite, "FAIL [%s/%s/axes]: duplicate name=\"%s\"",
                          run->dispatch_name, phase->label, phase->axes[i].name);
            }
        }
        if (product > RSH_MATRIX_MAX_CELLS / phase->axes[i].value_count) {
            rsh_failf(suite, "FAIL [%s/%s/cells]: product exceeds cap=%d",
                      run->dispatch_name, phase->label, RSH_MATRIX_MAX_CELLS);
        }
        product *= phase->axes[i].value_count;
    }
    if (product > RSH_MATRIX_MAX_CELLS || phase->expected_cells != product) {
        rsh_failf(suite, "FAIL [%s/%s/cells]: product=%lu expected=%lu cap=%d",
                  run->dispatch_name, phase->label,
                  (unsigned long)product, (unsigned long)phase->expected_cells,
                  RSH_MATRIX_MAX_CELLS);
    }
    return product;
}

static size_t rsh_matrix_product_no_fail(const rsh_matrix_phase_spec *phase) {
    size_t product = 1;
    size_t i;

    if (!phase || !phase->axes || phase->axis_count == 0 ||
        phase->axis_count > RSH_MATRIX_MAX_AXES ||
        !rsh_relative_path_valid(
            phase->cell_dir_prefix, RSH_MATRIX_CELL_DIR_CAP
        )) {
        return 0;
    }
    for (i = 0; i < phase->axis_count; i++) {
        size_t values = phase->axes[i].value_count;
        if (!phase->axes[i].values || values == 0 ||
            values > RSH_MATRIX_MAX_AXIS_VALUES ||
            product > RSH_MATRIX_MAX_CELLS / values) {
            return 0;
        }
        product *= values;
    }
    return product <= RSH_MATRIX_MAX_CELLS ? product : 0;
}

static void rsh_matrix_coordinates(
    const rsh_matrix_phase_spec *phase,
    size_t linear_index,
    size_t indices[RSH_MATRIX_MAX_AXES]
) {
    size_t i = phase->axis_count;

    while (i > 0) {
        size_t axis = i - 1;
        indices[axis] = linear_index % phase->axes[axis].value_count;
        linear_index /= phase->axes[axis].value_count;
        i--;
    }
}

static int rsh_matrix_cell_component(
    const rsh_matrix_phase_spec *phase,
    const size_t indices[RSH_MATRIX_MAX_AXES],
    char *output,
    size_t output_cap
) {
    size_t used = 1;
    size_t i;

    if (output_cap < 2) return 0;
    output[0] = 'c';
    output[1] = '\0';
    for (i = 0; i < phase->axis_count; i++) {
        int rc = snprintf(output + used, output_cap - used, "-%lu",
                          (unsigned long)indices[i]);
        if (rc < 0 || (size_t)rc >= output_cap - used) return 0;
        used += (size_t)rc;
    }
    return 1;
}

static void rsh_validate_matrix_phase(
    const rsh_suite_spec *suite,
    const rsh_run_spec *run,
    const rsh_matrix_phase_spec *phase
) {
    size_t i;

    (void)rsh_matrix_product(suite, run, phase);
    rsh_validate_db_specs(
        suite, run->dispatch_name, phase->label, phase->dbs, phase->db_count
    );
    if (!phase->steps || phase->step_count == 0 ||
        phase->step_count > RSH_MATRIX_MAX_STEPS) {
        rsh_failf(suite, "FAIL [%s/%s/steps]: got=%lu cap=%d",
                  run->dispatch_name, phase->label,
                  (unsigned long)phase->step_count, RSH_MATRIX_MAX_STEPS);
    }
    for (i = 0; i < phase->step_count; i++) {
        const rsh_matrix_cell_step *step = &phase->steps[i];
        if (step->kind < RSH_CELL_SETUP_PROFILE ||
            step->kind > RSH_CELL_ASSERT) {
            rsh_failf(suite, "FAIL [%s/%s/step-%lu]: invalid kind=%d",
                      run->dispatch_name, phase->label,
                      (unsigned long)i, (int)step->kind);
        }
        if (step->kind == RSH_CELL_SETUP_PROFILE &&
            (!step->role || !step->role[0] || step->setup_profile == RSH_PROFILE_NONE)) {
            rsh_failf(suite, "FAIL [%s/%s/step-%lu]: invalid setup profile step",
                      run->dispatch_name, phase->label, (unsigned long)i);
        }
        if ((step->kind == RSH_CELL_SETUP_PROFILE ||
             (step->kind == RSH_CELL_ANALYZE_IF && step->role)) && step->role) {
            size_t db_index;
            int found = 0;
            for (db_index = 0; db_index < phase->db_count; db_index++) {
                if (strcmp(step->role, phase->dbs[db_index].role) == 0) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                rsh_failf(suite, "FAIL [%s/%s/step-%lu]: unknown role=\"%s\"",
                          run->dispatch_name, phase->label,
                          (unsigned long)i, step->role);
            }
        }
        if (step->kind == RSH_CELL_ANALYZE_IF &&
            (step->predicate.axis_index >= phase->axis_count ||
             step->predicate.value_index >=
                 phase->axes[step->predicate.axis_index].value_count)) {
            rsh_failf(suite, "FAIL [%s/%s/step-%lu]: invalid axis predicate",
                      run->dispatch_name, phase->label, (unsigned long)i);
        }
        if (step->kind == RSH_CELL_ASSERT &&
            !phase->assertion_adapter && (!step->cases || step->case_count == 0)) {
            rsh_failf(suite, "FAIL [%s/%s/step-%lu]: empty assertion step",
                      run->dispatch_name, phase->label, (unsigned long)i);
        }
    }
    if (phase->counter_count != 0 && !phase->counters) {
        rsh_failf(suite, "FAIL [%s/%s/counters]: rows=NULL count=%lu",
                  run->dispatch_name, phase->label,
                  (unsigned long)phase->counter_count);
    }
    for (i = 0; i < phase->counter_count; i++) {
        if (!phase->counters[i].name || !phase->counters[i].name[0]) {
            rsh_failf(suite, "FAIL [%s/%s/counter-%lu]: empty name",
                      run->dispatch_name, phase->label, (unsigned long)i);
        }
    }
}

static void rsh_run_analyze(
    const rsh_suite_spec *suite,
    const rsh_run_spec *run,
    const rsh_matrix_phase_spec *phase,
    const char *role,
    sqlite3 *db
) {
    char *error = NULL;
    int rc = sqlite3_exec(db, "PRAGMA analysis_limit=0; ANALYZE;", NULL, NULL, &error);

    if (rc != SQLITE_OK) {
        const char *detail = error ? error : sqlite3_errmsg(db);
        rsh_failf(suite, "FAIL [%s/%s/%s/analyze]: rc=%d err=%s",
                  run->dispatch_name, phase->label, role, rc, detail);
    }
    sqlite3_free(error);
}

static void rsh_run_matrix_step(
    const rsh_suite_spec *suite,
    const rsh_run_spec *run,
    const rsh_matrix_phase_spec *phase,
    const rsh_matrix_cell_step *step,
    rsh_db_handle *handles,
    rsh_matrix_cell *cell,
    const char *captured_stderr,
    uint32_t active_build
) {
    rsh_case_context context;

    memset(&context, 0, sizeof(context));
    context.suite_name = suite->suite_name;
    context.run_name = run->dispatch_name;
    context.phase_label = phase->label;
    context.dbs = handles;
    context.db_count = phase->db_count;
    context.matrix_cell = cell;
    context.captured_stderr = captured_stderr;
    context.active_build = active_build;

    if (step->kind == RSH_CELL_SETUP_PROFILE) {
        const rsh_db_handle *handle = rsh_find_db_handle(
            handles, phase->db_count, step->role
        );
        if (!handle) {
            rsh_failf(suite, "FAIL [%s/%s/setup-role]: missing=\"%s\"",
                      run->dispatch_name, phase->label, step->role);
        }
        rsh_apply_declared_profile(
            suite, run->dispatch_name, phase->label, step->role,
            handle->db, step->setup_profile, "cell-setup-profile"
        );
    } else if (step->kind == RSH_CELL_ANALYZE_IF) {
        if (cell->axis_indices[step->predicate.axis_index] ==
            step->predicate.value_index) {
            if (step->role) {
                const rsh_db_handle *handle = rsh_find_db_handle(
                    handles, phase->db_count, step->role
                );
                if (!handle) {
                    rsh_failf(suite, "FAIL [%s/%s/analyze-role]: missing=\"%s\"",
                              run->dispatch_name, phase->label, step->role);
                }
                rsh_run_analyze(
                    suite, run, phase, handle->spec->role, handle->db
                );
            } else {
                rsh_db_kind kind;
                for (kind = RSH_DB_VENDOR; kind <= RSH_DB_AUXILIARY; kind++) {
                    size_t i;
                    for (i = 0; i < phase->db_count; i++) {
                        if (handles[i].spec->kind == kind) {
                            rsh_run_analyze(
                                suite, run, phase,
                                handles[i].spec->role, handles[i].db
                            );
                        }
                    }
                }
            }
        }
    } else {
        rsh_dispatch_cases(
            suite, run, step->cases, step->case_count, &context
        );
        if (phase->assertion_adapter &&
            phase->assertion_adapter(&context, step, phase->assertion_ctx) != SQLITE_OK) {
            rsh_failf(suite, "FAIL [%s/%s/matrix-assert]: adapter returned non-OK",
                      run->dispatch_name, phase->label);
        }
    }
}

static void rsh_run_matrix_phase(
    const rsh_suite_spec *suite,
    const rsh_run_spec *run,
    const rsh_matrix_phase_spec *phase,
    pid_t owner_pid,
    const char *captured_stderr,
    uint32_t active_build
) {
    size_t product;
    size_t *counters;
    size_t linear;

    rsh_validate_matrix_phase(suite, run, phase);
    product = rsh_matrix_product(suite, run, phase);
    counters = phase->counter_count
        ? (size_t *)calloc(phase->counter_count, sizeof(*counters)) : NULL;
    if (phase->counter_count && !counters) {
        rsh_failf(suite, "FAIL [%s/%s/counter-alloc]: count=%lu",
                  run->dispatch_name, phase->label,
                  (unsigned long)phase->counter_count);
    }

    for (linear = 0; linear < product; linear++) {
        size_t indices[RSH_MATRIX_MAX_AXES] = {0, 0, 0, 0};
        char component[48];
        char base_relative[RSH_PATH_CAP];
        char root[RSH_PATH_CAP];
        char cell_root[RSH_PATH_CAP];
        char cleanup_error[RSH_PATH_CAP] = {0};
        rsh_db_handle *handles;
        rsh_matrix_cell cell;
        const char *failed_role;
        int failed_rc;
        int close_errors;
        int cleanup_errors;
        int path_rc;
        size_t i;

        rsh_matrix_coordinates(phase, linear, indices);
        if (!rsh_matrix_cell_component(phase, indices, component, sizeof(component))) {
            path_rc = -1;
        } else {
            path_rc = snprintf(base_relative, sizeof(base_relative), "%s/%s",
                               phase->cell_dir_prefix, component);
        }
        if (path_rc < 0 || (size_t)path_rc >= sizeof(base_relative)) {
            free(counters);
            rsh_failf(suite, "FAIL [%s/%s/cell-path]: index=%lu",
                      run->dispatch_name, phase->label, (unsigned long)linear);
        }
        handles = (rsh_db_handle *)calloc(phase->db_count, sizeof(*handles));
        if (!handles) {
            free(counters);
            rsh_failf(suite, "FAIL [%s/%s/cell-alloc]: index=%lu",
                      run->dispatch_name, phase->label, (unsigned long)linear);
        }
        for (i = 0; i < phase->db_count; i++) {
            rsh_open_seeded_db(
                suite, owner_pid, base_relative, run->dispatch_name,
                phase->label, &phase->dbs[i], &handles[i]
            );
        }
        memset(&cell, 0, sizeof(cell));
        cell.axes = phase->axes;
        cell.axis_count = phase->axis_count;
        cell.dbs = handles;
        cell.db_count = phase->db_count;
        cell.counters = counters;
        cell.counter_count = phase->counter_count;
        for (i = 0; i < phase->axis_count; i++) {
            cell.axis_indices[i] = indices[i];
            cell.axis_values[i] = &phase->axes[i].values[indices[i]];
        }
        for (i = 0; i < phase->step_count; i++) {
            rsh_run_matrix_step(
                suite, run, phase, &phase->steps[i], handles, &cell,
                captured_stderr, active_build
            );
        }
        close_errors = rsh_close_db_handles(
            handles, phase->db_count, &failed_role, &failed_rc
        );
        if (!rsh_root_for_pid(suite, owner_pid, root, sizeof(root)) ||
            !rsh_join_path(cell_root, sizeof(cell_root), root, base_relative)) {
            free(handles);
            free(counters);
            rsh_failf(suite, "FAIL [%s/%s/cell-root]: path too long",
                      run->dispatch_name, phase->label);
        }
        cleanup_errors = rsh_cleanup_db_specs_at_root(
            cell_root, phase->dbs, phase->db_count,
            cleanup_error, sizeof(cleanup_error)
        );
        if (rmdir(cell_root) != 0 && errno != ENOENT) {
            if (cleanup_errors++ == 0) {
                (void)snprintf(cleanup_error, sizeof(cleanup_error),
                               "rmdir(%s): %s", cell_root, strerror(errno));
            }
        }
        free(handles);
        if (close_errors != 0) {
            free(counters);
            rsh_failf(
                suite,
                "FAIL [%s/%s/%s/close]: rc=%d close_errors=%d cleanup_errors=%d",
                run->dispatch_name, phase->label, failed_role, failed_rc,
                close_errors, cleanup_errors
            );
        }
        if (cleanup_errors != 0) {
            free(counters);
            rsh_failf(suite, "FAIL [%s/%s/cell-cleanup]: errors=%d first=%s",
                      run->dispatch_name, phase->label, cleanup_errors, cleanup_error);
        }
    }

    for (linear = 0; linear < phase->counter_count; linear++) {
        if (counters[linear] != phase->counters[linear].expected) {
            size_t actual = counters[linear];
            free(counters);
            rsh_failf(
                suite, "FAIL [%s/%s/%s]: got=%lu want=%lu",
                run->dispatch_name, phase->label, phase->counters[linear].name,
                (unsigned long)actual,
                (unsigned long)phase->counters[linear].expected
            );
        }
    }
    free(counters);
    {
        char root[RSH_PATH_CAP];
        char cleanup_error[RSH_PATH_CAP] = {0};
        int cleanup_errors;
        if (!rsh_root_for_pid(suite, owner_pid, root, sizeof(root))) {
            rsh_failf(suite, "FAIL [%s/%s/root]: path too long",
                      run->dispatch_name, phase->label);
        }
        cleanup_errors = rsh_remove_relative_directory_chain(
            root, phase->cell_dir_prefix, cleanup_error, sizeof(cleanup_error)
        );
        if (cleanup_errors != 0) {
            rsh_failf(suite, "FAIL [%s/%s/phase-cleanup]: errors=%d first=%s",
                      run->dispatch_name, phase->label,
                      cleanup_errors, cleanup_error);
        }
    }
}

static int rsh_cleanup_db_specs_safe(
    const rsh_db_spec *dbs,
    size_t db_count
) {
    size_t i;

    if (!dbs || db_count == 0) return 0;
    for (i = 0; i < db_count; i++) {
        if (dbs[i].storage == RSH_DB_MEMORY) {
            if (strcmp(dbs[i].relative_path, ":memory:") != 0) return 0;
        } else if (dbs[i].storage != RSH_DB_RELATIVE ||
                   !rsh_relative_path_valid(
                       dbs[i].relative_path, RSH_DB_RELATIVE_PATH_CAP
                   )) {
            return 0;
        }
    }
    return 1;
}

/* Parent death-test cleanup removes only registry-declared child paths. */
static int rsh_cleanup_root_for_pid(
    const rsh_suite_spec *suite,
    const rsh_run_spec *run,
    pid_t child_pid,
    char *error,
    size_t error_cap
) {
    char root[RSH_PATH_CAP];
    struct stat root_stat;
    size_t i;
    int errors = 0;

    if (error && error_cap) error[0] = '\0';
    if (!rsh_root_for_pid(suite, child_pid, root, sizeof(root))) {
        if (error && error_cap) (void)snprintf(error, error_cap, "root path too long");
        return 1;
    }
    if (lstat(root, &root_stat) != 0) {
        if (errno == ENOENT) return 0;
        if (error && error_cap) {
            (void)snprintf(error, error_cap, "lstat(%s): %s", root, strerror(errno));
        }
        return 1;
    }
    if (!S_ISDIR(root_stat.st_mode) || S_ISLNK(root_stat.st_mode)) {
        if (error && error_cap) {
            (void)snprintf(error, error_cap, "unsafe child root type: %s", root);
        }
        return 1;
    }
    for (i = 0; i < run->phase_count; i++) {
        char local_error[RSH_PATH_CAP] = {0};
        int phase_errors;
        if (!rsh_cleanup_db_specs_safe(
                run->phases[i].dbs, run->phases[i].db_count
            )) {
            phase_errors = 1;
            (void)snprintf(local_error, sizeof(local_error),
                           "unsafe scalar cleanup descriptor");
        } else {
            phase_errors = rsh_cleanup_db_specs_at_root(
                root, run->phases[i].dbs, run->phases[i].db_count,
                local_error, sizeof(local_error)
            );
        }
        if (phase_errors != 0 && errors == 0 && error && error_cap) {
            (void)snprintf(error, error_cap, "%s", local_error);
        }
        errors += phase_errors;
    }
    for (i = 0; i < run->matrix_phase_count; i++) {
        const rsh_matrix_phase_spec *phase = &run->matrix_phases[i];
        size_t product = rsh_matrix_product_no_fail(phase);
        size_t linear;
        if (product == 0 ||
            !rsh_cleanup_db_specs_safe(phase->dbs, phase->db_count)) {
            if (errors++ == 0 && error && error_cap) {
                (void)snprintf(error, error_cap, "invalid matrix cleanup descriptor");
            }
            continue;
        }
        for (linear = 0; linear < product; linear++) {
            size_t indices[RSH_MATRIX_MAX_AXES] = {0, 0, 0, 0};
            char component[48];
            char base_relative[RSH_PATH_CAP];
            char cell_root[RSH_PATH_CAP];
            char local_error[RSH_PATH_CAP] = {0};
            int cell_errors;
            int rc;

            rsh_matrix_coordinates(phase, linear, indices);
            rc = rsh_matrix_cell_component(
                phase, indices, component, sizeof(component)
            );
            if (!rc) {
                if (errors++ == 0 && error && error_cap) {
                    (void)snprintf(error, error_cap, "matrix component too long");
                }
                continue;
            }
            rc = snprintf(base_relative, sizeof(base_relative), "%s/%s",
                          phase->cell_dir_prefix, component);
            if (rc < 0 || (size_t)rc >= sizeof(base_relative) ||
                !rsh_join_path(cell_root, sizeof(cell_root), root, base_relative)) {
                if (errors++ == 0 && error && error_cap) {
                    (void)snprintf(error, error_cap, "matrix cell path too long");
                }
                continue;
            }
            cell_errors = rsh_cleanup_db_specs_at_root(
                cell_root, phase->dbs, phase->db_count,
                local_error, sizeof(local_error)
            );
            if (cell_errors != 0 && errors == 0 && error && error_cap) {
                (void)snprintf(error, error_cap, "%s", local_error);
            }
            errors += cell_errors;
            if (rmdir(cell_root) != 0 && errno != ENOENT) {
                if (errors++ == 0 && error && error_cap) {
                    (void)snprintf(error, error_cap, "rmdir(%s): %s",
                                   cell_root, strerror(errno));
                }
            }
        }
        {
            char local_error[RSH_PATH_CAP] = {0};
            int phase_errors = rsh_remove_relative_directory_chain(
                root, phase->cell_dir_prefix, local_error, sizeof(local_error)
            );
            if (phase_errors != 0 && errors == 0 && error && error_cap) {
                (void)snprintf(error, error_cap, "%s", local_error);
            }
            errors += phase_errors;
        }
    }
    if (rmdir(root) != 0 && errno != ENOENT) {
        if (errors++ == 0 && error && error_cap) {
            (void)snprintf(error, error_cap, "rmdir(%s): %s", root, strerror(errno));
        }
    }
    return errors;
}

static FILE *rsh_new_capture_file(
    const rsh_suite_spec *suite,
    const rsh_run_spec *run
) {
    FILE *file = tmpfile();
    struct stat st;
    int fd;
    int flags;

    if (!file) {
        rsh_failf(suite, "FAIL [%s/capture-open]: error=%s",
                  run->dispatch_name, strerror(errno));
    }
    fd = fileno(file);
    if (fd < 0 || fstat(fd, &st) != 0 ||
        !S_ISREG(st.st_mode) || st.st_nlink != 0) {
        (void)fclose(file);
        rsh_failf(suite, "FAIL [%s/capture-file]: not an unlinked regular file",
                  run->dispatch_name);
    }
    flags = fcntl(fd, F_GETFD);
    if (flags < 0 || fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC) != 0) {
        (void)fclose(file);
        rsh_failf(suite, "FAIL [%s/capture-flags]: error=%s",
                  run->dispatch_name, strerror(errno));
    }
    return file;
}

static void rsh_begin_stderr_capture(
    const rsh_suite_spec *suite,
    const rsh_run_spec *run,
    rsh_capture *capture
) {
    capture->file = rsh_new_capture_file(suite, run);
    fflush(stderr);
    capture->saved_stderr = dup(STDERR_FILENO);
    if (capture->saved_stderr < 0 ||
        dup2(fileno(capture->file), STDERR_FILENO) < 0) {
        if (capture->saved_stderr >= 0) (void)close(capture->saved_stderr);
        (void)fclose(capture->file);
        capture->file = NULL;
        rsh_failf(suite, "FAIL [%s/capture-redirect]: error=%s",
                  run->dispatch_name, strerror(errno));
    }
    rsh_active_capture_file = capture->file;
    rsh_active_saved_stderr = capture->saved_stderr;
    clearerr(stderr);
}

static int rsh_read_capture_file_raw(
    FILE *file,
    char **buffer_out,
    char *error,
    size_t error_cap
) {
    long size;
    char *buffer;

    *buffer_out = NULL;
    if (!file) {
        (void)snprintf(error, error_cap, "capture file is NULL");
        return 0;
    }
    if (fflush(file) != 0 || fseek(file, 0, SEEK_END) != 0 ||
        (size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
        (void)fclose(file);
        (void)snprintf(error, error_cap, "capture seek: %s", strerror(errno));
        return 0;
    }
    buffer = (char *)malloc((size_t)size + 1);
    if (!buffer) {
        (void)fclose(file);
        (void)snprintf(error, error_cap, "capture allocation: bytes=%ld", size + 1);
        return 0;
    }
    if (fread(buffer, 1, (size_t)size, file) != (size_t)size) {
        free(buffer);
        (void)fclose(file);
        (void)snprintf(error, error_cap, "capture read: bytes=%ld", size);
        return 0;
    }
    buffer[size] = '\0';
    if (fclose(file) != 0) {
        free(buffer);
        (void)snprintf(error, error_cap, "capture close: %s", strerror(errno));
        return 0;
    }
    *buffer_out = buffer;
    return 1;
}

static char *rsh_read_capture_file(
    const rsh_suite_spec *suite,
    const rsh_run_spec *run,
    FILE *file
) {
    char *buffer = NULL;
    char error[256] = {0};

    if (!rsh_read_capture_file_raw(file, &buffer, error, sizeof(error))) {
        rsh_failf(suite, "FAIL [%s/capture-read]: %s", run->dispatch_name, error);
    }
    return buffer;
}

static char *rsh_end_stderr_capture(
    const rsh_suite_spec *suite,
    const rsh_run_spec *run,
    rsh_capture *capture
) {
    FILE *file = capture->file;

    fflush(stderr);
    if (dup2(capture->saved_stderr, STDERR_FILENO) < 0) {
        rsh_failf(suite, "FAIL [%s/capture-restore]: error=%s",
                  run->dispatch_name, strerror(errno));
    }
    (void)close(capture->saved_stderr);
    capture->saved_stderr = -1;
    capture->file = NULL;
    rsh_active_saved_stderr = -1;
    rsh_active_capture_file = NULL;
    clearerr(stderr);
    return rsh_read_capture_file(suite, run, file);
}

static void rsh_render_pass(const rsh_run_spec *run, void *suite_ctx) {
    const char *detail = run->pass_detail.literal;

    if (run->pass_detail.predicate &&
        !run->pass_detail.predicate(run, suite_ctx)) {
        detail = run->pass_detail.else_literal;
    }
    if (detail && detail[0]) {
        printf("PASS [%s]: %s\n", run->pass_label, detail);
    } else {
        printf("PASS [%s]\n", run->pass_label);
    }
    fflush(stdout);
}

static void rsh_render_skip(const rsh_run_spec *run) {
    printf("SKIP [%s]: %s\n", run->pass_label, run->skip_detail);
    fflush(stdout);
}

static int rsh_execute_run_body(
    const rsh_suite_spec *suite,
    const rsh_run_spec *run,
    pid_t owner_pid,
    uint32_t active_build
) {
    rsh_capture capture;
    char *captured_stderr = NULL;
    char cleanup_error[RSH_PATH_CAP] = {0};
    int cleanup_errors;
    size_t i;

    memset(&capture, 0, sizeof(capture));
    capture.saved_stderr = -1;
    rsh_apply_postload_env(suite, run);
    if (run->capture_scope == RSH_CAPTURE_STDERR &&
        run->outcome == RSH_OUTCOME_SUCCESS) {
        rsh_begin_stderr_capture(suite, run, &capture);
    }
    for (i = 0; i < run->phase_count; i++) {
        rsh_run_phase(
            suite, run, &run->phases[i], owner_pid,
            captured_stderr, active_build
        );
    }
    for (i = 0; i < run->matrix_phase_count; i++) {
        rsh_run_matrix_phase(
            suite, run, &run->matrix_phases[i], owner_pid,
            captured_stderr, active_build
        );
    }
    if (capture.file) {
        captured_stderr = rsh_end_stderr_capture(suite, run, &capture);
    }
    if (run->post_close_case_count != 0) {
        rsh_case_context context;
        memset(&context, 0, sizeof(context));
        context.suite_name = suite->suite_name;
        context.run_name = run->dispatch_name;
        context.phase_label = "post-close";
        context.captured_stderr = captured_stderr;
        context.active_build = active_build;
        rsh_dispatch_cases(
            suite, run, run->post_close_cases,
            run->post_close_case_count, &context
        );
    }
    cleanup_errors = rsh_cleanup_root_for_pid(
        suite, run, owner_pid, cleanup_error, sizeof(cleanup_error)
    );
    free(captured_stderr);
    if (cleanup_errors != 0) {
        rsh_failf(suite, "FAIL [%s/root-cleanup]: errors=%d first=%s",
                  run->dispatch_name, cleanup_errors, cleanup_error);
    }
    if (run->outcome == RSH_OUTCOME_SUCCESS) {
        rsh_render_pass(run, suite->suite_ctx);
    }
    return 0;
}

static const rsh_run_spec *rsh_find_run(
    const rsh_run_spec *runs,
    size_t run_count,
    const char *dispatch_name
) {
    size_t i;

    if (!dispatch_name) return NULL;
    for (i = 0; i < run_count; i++) {
        if (runs[i].dispatch_name &&
            strcmp(runs[i].dispatch_name, dispatch_name) == 0) {
            return &runs[i];
        }
    }
    return NULL;
}

static size_t rsh_validate_case_registry_rows(
    const rsh_suite_spec *suite,
    const rsh_run_spec *run,
    const rsh_case_spec *cases,
    size_t case_count
) {
    size_t abort_cases = 0;
    size_t i;

    if (!cases && case_count != 0) {
        rsh_failf(suite, "FAIL [%s/registry-cases]: rows=NULL count=%lu",
                  run->dispatch_name, (unsigned long)case_count);
    }
    for (i = 0; i < case_count; i++) {
        if (!cases[i].label || !cases[i].label[0] || cases[i].build_mask == 0 ||
            (cases[i].build_mask & ~RSH_BUILD_ALL) != 0 ||
            (cases[i].build_mask & run->build_mask) == 0 ||
            cases[i].kind < RSH_CASE_SQL_EXACT ||
            cases[i].kind > RSH_CASE_CUSTOM) {
            rsh_failf(suite, "FAIL [%s/registry-case-%lu]: invalid row",
                      run->dispatch_name, (unsigned long)i);
        }
        if (cases[i].kind == RSH_CASE_CUSTOM &&
            (cases[i].data.custom.kind < RSH_CUSTOM_IDENTITY ||
             cases[i].data.custom.kind > RSH_CUSTOM_LOG_CAPTURE)) {
            rsh_failf(suite, "FAIL [%s/%s/custom-kind]: invalid reason",
                      run->dispatch_name, cases[i].label);
        }
        if (cases[i].kind == RSH_CASE_EXPECT_ABORT) {
            if ((cases[i].build_mask & run->build_mask) != run->build_mask) {
                rsh_failf(suite, "FAIL [%s/%s/build-mask]: abort case is not always active",
                          run->dispatch_name, cases[i].label);
            }
            abort_cases++;
        }
    }
    return abort_cases;
}

static void rsh_validate_run(
    const rsh_suite_spec *suite,
    const rsh_run_spec *run
) {
    size_t abort_cases = 0;
    size_t i;

    if (!run->dispatch_name || !run->dispatch_name[0] ||
        !run->pass_label || !run->pass_label[0] || run->build_mask == 0 ||
        (run->build_mask & ~RSH_BUILD_ALL) != 0 ||
        run->process_kind < RSH_PROCESS_FORK ||
        run->process_kind > RSH_PROCESS_EXEC ||
        run->outcome < RSH_OUTCOME_SUCCESS ||
        run->outcome > RSH_OUTCOME_EXPECT_ABORT ||
        run->capture_scope < RSH_CAPTURE_NONE ||
        run->capture_scope > RSH_CAPTURE_STDERR) {
        rsh_failf(suite, "FATAL: invalid run specification");
    }
    if ((!run->phases && run->phase_count != 0) ||
        (!run->matrix_phases && run->matrix_phase_count != 0) ||
        (!run->post_close_cases && run->post_close_case_count != 0)) {
        rsh_failf(suite, "FAIL [%s/registry]: non-zero count with NULL rows",
                  run->dispatch_name);
    }
    if ((run->pass_detail.literal && !run->pass_detail.literal[0]) ||
        (run->pass_detail.else_literal && !run->pass_detail.else_literal[0]) ||
        (run->pass_detail.predicate &&
         (!run->pass_detail.literal || !run->pass_detail.else_literal)) ||
        (!run->pass_detail.predicate && run->pass_detail.else_literal)) {
        rsh_failf(suite, "FAIL [%s/pass-detail]: invalid dynamic metadata",
                  run->dispatch_name);
    }
    rsh_validate_env_profile(
        suite, run, "preload", run->preload_env, run->preload_env_count,
        1
    );
    rsh_validate_env_profile(
        suite, run, "postload", run->postload_env, run->postload_env_count, 0
    );
    for (i = 0; i < run->phase_count; i++) {
        abort_cases += rsh_validate_case_registry_rows(
            suite, run, run->phases[i].cases, run->phases[i].case_count
        );
    }
    for (i = 0; i < run->matrix_phase_count; i++) {
        size_t step;
        if (run->matrix_phases[i].step_count != 0 &&
            !run->matrix_phases[i].steps) {
            rsh_failf(suite, "FAIL [%s/registry-matrix-%lu]: steps=NULL count=%lu",
                      run->dispatch_name, (unsigned long)i,
                      (unsigned long)run->matrix_phases[i].step_count);
        }
        for (step = 0; step < run->matrix_phases[i].step_count; step++) {
            const rsh_matrix_cell_step *cell_step =
                &run->matrix_phases[i].steps[step];
            if (cell_step->kind == RSH_CELL_ASSERT) {
                abort_cases += rsh_validate_case_registry_rows(
                    suite, run, cell_step->cases, cell_step->case_count
                );
            } else if (cell_step->cases || cell_step->case_count != 0) {
                rsh_failf(
                    suite,
                    "FAIL [%s/registry-matrix-%lu-step-%lu]: "
                    "non-assertion step carries cases",
                    run->dispatch_name, (unsigned long)i, (unsigned long)step
                );
            }
        }
    }
    abort_cases += rsh_validate_case_registry_rows(
        suite, run, run->post_close_cases, run->post_close_case_count
    );
    if (run->outcome == RSH_OUTCOME_EXPECT_ABORT) {
        const rsh_abort_expectation *expectation = run->abort_expectation;
        if (abort_cases != 1 || !expectation || expectation->expected_exit != 1 ||
            !expectation->expected_stage_label ||
            strncmp(expectation->expected_stage_label, "FAIL [", 6) != 0 ||
            expectation->expected_stage_label[
                strlen(expectation->expected_stage_label) - 1
            ] != ']' ||
            (expectation->earlier_stage_label_count != 0 &&
             !expectation->earlier_stage_labels)) {
            rsh_failf(suite, "FAIL [%s/abort-contract]: invalid expectation",
                      run->dispatch_name);
        }
        for (i = 0; i < expectation->earlier_stage_label_count; i++) {
            const char *label = expectation->earlier_stage_labels[i];
            if (!label || strncmp(label, "FAIL [", 6) != 0 ||
                label[strlen(label) - 1] != ']') {
                rsh_failf(suite, "FAIL [%s/abort-contract]: invalid earlier label",
                          run->dispatch_name);
            }
        }
    } else if (run->abort_expectation || abort_cases != 0) {
        rsh_failf(suite, "FAIL [%s/abort-contract]: unexpected expectation/case",
                  run->dispatch_name);
    }
}

static int rsh_run_child(
    const rsh_suite_spec *suite,
    const rsh_run_spec *runs,
    size_t run_count,
    uint32_t active_build,
    const char *dispatch_name
) {
    const rsh_run_spec *run;

    rsh_validate_suite(suite);
    run = rsh_find_run(runs, run_count, dispatch_name);
    if (!run || !rsh_build_enabled(run->build_mask, active_build)) {
        rsh_failf(suite, "FATAL: unknown or inactive child run \"%s\"",
                  dispatch_name ? dispatch_name : "(null)");
    }
    rsh_validate_run(suite, run);
    rsh_apply_preload_env(suite, run);
    return rsh_execute_run_body(suite, run, getpid(), active_build);
}

static int rsh_count_fail_records(const char *captured) {
    const char *line = captured;
    int count = 0;

    while (line && *line) {
        const char *end = strchr(line, '\n');
        if (strncmp(line, "FAIL [", 6) == 0) count++;
        if (!end) break;
        line = end + 1;
    }
    return count;
}

static int rsh_count_fail_lines_with_token(
    const char *captured,
    const char *token
) {
    const char *line = captured;
    int count = 0;

    while (line && *line) {
        const char *end = strchr(line, '\n');
        const char *match = strstr(line, token);
        if (strncmp(line, "FAIL [", 6) == 0 && match && (!end || match < end)) count++;
        if (!end) break;
        line = end + 1;
    }
    return count;
}

static void rsh_adjudicate_abort(
    const rsh_suite_spec *suite,
    const rsh_run_spec *run,
    int wait_status,
    const char *captured,
    int cleanup_errors,
    const char *cleanup_error
) {
    const rsh_abort_expectation *expectation = run->abort_expectation;
    int exited = WIFEXITED(wait_status);
    int actual_exit = exited ? WEXITSTATUS(wait_status) : -1;
    int fail_record_count = rsh_count_fail_records(captured);
    int expected_line_count = rsh_count_fail_lines_with_token(
        captured, expectation->expected_stage_label
    );
    int expected_token_count = rsh_count_occurrences(
        captured, expectation->expected_stage_label
    );
    int earlier_count = 0;
    size_t i;

    for (i = 0; i < expectation->earlier_stage_label_count; i++) {
        earlier_count += rsh_count_occurrences(
            captured, expectation->earlier_stage_labels[i]
        );
    }
    if (!exited || actual_exit != expectation->expected_exit ||
        expected_line_count != 1 || expected_token_count != 1 ||
        fail_record_count != 1 || earlier_count != 0 || cleanup_errors != 0 ||
        strstr(captured, "FATAL") != NULL) {
        rsh_failf(
            suite,
            "FAIL [%s/expected-abort]: expected_exit=%d actual_exit=%d "
            "signaled=%d expected_label_count=%d expected_token_count=%d "
            "fail_record_count=%d earlier_stage_count=%d cleanup_errors=%d "
            "cleanup=\"%s\" stderr=\"%s\"",
            run->dispatch_name, expectation->expected_exit, actual_exit,
            WIFSIGNALED(wait_status) ? WTERMSIG(wait_status) : 0,
            expected_line_count, expected_token_count, fail_record_count,
            earlier_count, cleanup_errors,
            cleanup_error ? cleanup_error : "", captured ? captured : "(null)"
        );
    }
}

static void rsh_run_one_parent(
    const rsh_suite_spec *suite,
    const rsh_run_spec *run,
    const char *program_path,
    uint32_t active_build
) {
    FILE *abort_capture = NULL;
    pid_t child_pid;
    int wait_status;
    char cleanup_error[RSH_PATH_CAP] = {0};
    int cleanup_errors;

    if (run->outcome == RSH_OUTCOME_EXPECT_ABORT) {
        abort_capture = rsh_new_capture_file(suite, run);
    }
    fflush(stdout);
    fflush(stderr);
    child_pid = fork();
    if (child_pid < 0) {
        if (abort_capture) (void)fclose(abort_capture);
        rsh_failf(suite, "FAIL [%s/fork]: error=%s",
                  run->dispatch_name, strerror(errno));
    }
    if (child_pid == 0) {
        int rc;
        if (abort_capture) {
            int fd = fileno(abort_capture);
            fflush(stderr);
            if (dup2(fd, STDERR_FILENO) < 0) _exit(127);
            if (fd != STDERR_FILENO) (void)fclose(abort_capture);
            clearerr(stderr);
        }
        rsh_apply_preload_env(suite, run);
        if (run->process_kind == RSH_PROCESS_EXEC) {
            char *const args[] = {
                (char *)program_path,
                (char *)"--child",
                (char *)run->dispatch_name,
                NULL
            };
            if (strchr(program_path, '/')) execv(program_path, args);
            else execvp(program_path, args);
            rsh_failf(suite, "FATAL: exec child \"%s\" failed: %s",
                      run->dispatch_name, strerror(errno));
        }
        rc = rsh_execute_run_body(suite, run, getpid(), active_build);
        fflush(stdout);
        fflush(stderr);
        _exit(rc);
    }
    while (waitpid(child_pid, &wait_status, 0) < 0) {
        if (errno != EINTR) {
            if (abort_capture) (void)fclose(abort_capture);
            rsh_failf(suite, "FAIL [%s/wait]: child_pid=%ld error=%s",
                      run->dispatch_name, (long)child_pid, strerror(errno));
        }
    }
    if (run->outcome == RSH_OUTCOME_EXPECT_ABORT) {
        char *captured = NULL;
        char capture_error[256] = {0};
        int capture_ok = rsh_read_capture_file_raw(
            abort_capture, &captured, capture_error, sizeof(capture_error)
        );
        cleanup_errors = rsh_cleanup_root_for_pid(
            suite, run, child_pid, cleanup_error, sizeof(cleanup_error)
        );
        if (!capture_ok) {
            rsh_failf(
                suite,
                "FAIL [%s/expected-abort-capture]: child_pid=%ld "
                "capture=\"%s\" cleanup_errors=%d cleanup=\"%s\"",
                run->dispatch_name, (long)child_pid, capture_error,
                cleanup_errors, cleanup_error
            );
        }
        rsh_adjudicate_abort(
            suite, run, wait_status, captured, cleanup_errors, cleanup_error
        );
        free(captured);
        rsh_render_pass(run, suite->suite_ctx);
    } else {
        cleanup_errors = rsh_cleanup_root_for_pid(
            suite, run, child_pid, cleanup_error, sizeof(cleanup_error)
        );
    }
    if (run->outcome == RSH_OUTCOME_SUCCESS &&
        (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0 ||
         cleanup_errors != 0)) {
        rsh_failf(
            suite,
            "FAIL [%s/child]: child_pid=%ld exit=%d signal=%d "
            "cleanup_errors=%d cleanup=\"%s\"",
            run->dispatch_name, (long)child_pid,
            WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : -1,
            WIFSIGNALED(wait_status) ? WTERMSIG(wait_status) : 0,
            cleanup_errors, cleanup_error
        );
    }
}

static int rsh_run_all(
    const rsh_suite_spec *suite,
    const rsh_run_spec *runs,
    size_t run_count,
    const char *program_path,
    uint32_t active_build
) {
    size_t i;
    size_t j;

    rsh_validate_suite(suite);
    if (!runs || run_count == 0 || !program_path || !program_path[0] ||
        (active_build != RSH_BUILD_PLEX_LINKED &&
         active_build != RSH_BUILD_EMBY_LINKED &&
         active_build != RSH_BUILD_EMBY_DIRECT)) {
        rsh_failf(suite, "FATAL: invalid registry runner arguments");
    }
    for (i = 0; i < run_count; i++) {
        rsh_validate_run(suite, &runs[i]);
        for (j = 0; j < i; j++) {
            if (strcmp(runs[i].dispatch_name, runs[j].dispatch_name) == 0) {
                rsh_failf(suite, "FATAL: duplicate run dispatch name \"%s\"",
                          runs[i].dispatch_name);
            }
        }
    }
    for (i = 0; i < run_count; i++) {
        const rsh_run_spec *run = &runs[i];
        if (!rsh_build_enabled(run->build_mask, active_build)) continue;
        if (run->runtime_predicate &&
            !run->runtime_predicate(run, suite->suite_ctx)) {
            if (!run->skip_detail || !run->skip_detail[0]) {
                rsh_failf(suite, "FAIL [%s/skip-detail]: empty detail",
                          run->dispatch_name);
            }
            rsh_render_skip(run);
            continue;
        }
        rsh_run_one_parent(suite, run, program_path, active_build);
    }
    printf("%s\n", suite->suite_name);
    return 0;
}

#endif
