#ifndef SQLITE3_BUILDS_CONTRACT_PARITY_H
#define SQLITE3_BUILDS_CONTRACT_PARITY_H

/*
 * Differential testing is the established practice for behavior-preserving
 * SQL rewrites. This header-only helper keeps the standalone smoke binaries
 * independent while giving every rewrite family one statement contract.
 */
typedef void (*contract_parity_bind_fn)(
    sqlite3_stmt *stmt,
    const char *side,
    const char *label,
    void *ctx
);

typedef sqlite3_stmt *(*contract_parity_prepare_fn)(
    sqlite3 *db,
    const char *label,
    const char *sql,
    const char **tail
);

typedef int (*contract_parity_row_exception_fn)(
    const char *label,
    int row,
    sqlite3_stmt *vendor,
    sqlite3_stmt *candidate,
    void *ctx
);

static int contract_parity_nullable_text_equal(const char *left, const char *right) {
    if (!left || !right) return left == right;
    return strcmp(left, right) == 0;
}

static int contract_parity_cell_equal(sqlite3_stmt *left, sqlite3_stmt *right, int column) {
    int left_type = sqlite3_column_type(left, column);
    int right_type = sqlite3_column_type(right, column);

    if (left_type != right_type) return 0;
    if (left_type == SQLITE_NULL) return 1;
    if (left_type == SQLITE_INTEGER) {
        return sqlite3_column_int64(left, column) == sqlite3_column_int64(right, column);
    }
    if (left_type == SQLITE_FLOAT) {
        double left_value = sqlite3_column_double(left, column);
        double right_value = sqlite3_column_double(right, column);
        return memcmp(&left_value, &right_value, sizeof(left_value)) == 0;
    }
    {
        const void *left_value;
        const void *right_value;
        int left_bytes = sqlite3_column_bytes(left, column);
        int right_bytes = sqlite3_column_bytes(right, column);

        if (left_bytes != right_bytes) return 0;
        left_value = left_type == SQLITE_TEXT
            ? (const void *)sqlite3_column_text(left, column)
            : sqlite3_column_blob(left, column);
        right_value = right_type == SQLITE_TEXT
            ? (const void *)sqlite3_column_text(right, column)
            : sqlite3_column_blob(right, column);
        return left_bytes == 0 || memcmp(left_value, right_value, (size_t)left_bytes) == 0;
    }
}

static void contract_parity_require_min_rows(
    sqlite3 *vendor_db,
    sqlite3 *candidate_db,
    contract_parity_prepare_fn prepare,
    const char *label,
    const char *vendor_sql,
    const char *candidate_source_sql,
    const char *expected_candidate_sql,
    contract_parity_bind_fn bind,
    void *bind_ctx,
    contract_parity_row_exception_fn row_exception,
    void *exception_ctx,
    int minimum_rows
) {
    sqlite3_stmt *vendor = NULL;
    sqlite3_stmt *candidate = NULL;
    const char *vendor_tail = NULL;
    const char *candidate_tail = NULL;
    const char *saved_vendor_sql;
    const char *saved_candidate_sql;
    int columns;
    int binds;
    int row = 0;
    int i;
    int rc;

    vendor = prepare(vendor_db, label, vendor_sql, &vendor_tail);
    candidate = prepare(candidate_db, label, candidate_source_sql, &candidate_tail);
    saved_vendor_sql = sqlite3_sql(vendor);
    saved_candidate_sql = sqlite3_sql(candidate);
    if (!saved_vendor_sql || strcmp(saved_vendor_sql, vendor_sql) != 0) {
        failf("FAIL [%s/vendor-sql]: got=\"%s\" want=\"%s\"", label,
              saved_vendor_sql ? saved_vendor_sql : "(null)", vendor_sql);
    }
    /* C6: a silent matcher miss must never turn parity into vendor-vs-vendor. */
    if (!saved_candidate_sql || strcmp(saved_candidate_sql, candidate_source_sql) == 0) {
        failf("FAIL [%s/rewrite-fired]: candidate SQL did not change: \"%s\"", label,
              saved_candidate_sql ? saved_candidate_sql : "(null)");
    }
    if (expected_candidate_sql && strcmp(saved_candidate_sql, expected_candidate_sql) != 0) {
        failf("FAIL [%s/candidate-sql]: got=\"%s\" want=\"%s\"", label,
              saved_candidate_sql, expected_candidate_sql);
    }
    if (vendor_tail != vendor_sql + strlen(vendor_sql) ||
        candidate_tail != candidate_source_sql + strlen(candidate_source_sql)) {
        failf("FAIL [%s/tail]: vendor=%ld/%ld candidate=%ld/%ld", label,
              (long)(vendor_tail - vendor_sql), (long)strlen(vendor_sql),
              (long)(candidate_tail - candidate_source_sql),
              (long)strlen(candidate_source_sql));
    }

    columns = sqlite3_column_count(vendor);
    if (sqlite3_column_count(candidate) != columns) {
        failf("FAIL [%s/column-count]: vendor=%d candidate=%d", label, columns,
              sqlite3_column_count(candidate));
    }
    for (i = 0; i < columns; i++) {
        const char *vendor_name = sqlite3_column_name(vendor, i);
        const char *candidate_name = sqlite3_column_name(candidate, i);
        const char *vendor_decltype = sqlite3_column_decltype(vendor, i);
        const char *candidate_decltype = sqlite3_column_decltype(candidate, i);
#ifdef EMBY_FTS_REWRITE_DIRECT_TEST
        /* The pristine direct-test amalgamation omits SQLITE_ENABLE_COLUMN_METADATA. */
        const char *vendor_table = NULL;
        const char *candidate_table = NULL;
        const char *vendor_origin = NULL;
        const char *candidate_origin = NULL;
#else
        const char *vendor_table = sqlite3_column_table_name(vendor, i);
        const char *candidate_table = sqlite3_column_table_name(candidate, i);
        const char *vendor_origin = sqlite3_column_origin_name(vendor, i);
        const char *candidate_origin = sqlite3_column_origin_name(candidate, i);
#endif

        if (!contract_parity_nullable_text_equal(vendor_name, candidate_name) ||
            !contract_parity_nullable_text_equal(vendor_decltype, candidate_decltype) ||
            !contract_parity_nullable_text_equal(vendor_table, candidate_table) ||
            !contract_parity_nullable_text_equal(vendor_origin, candidate_origin)) {
            failf("FAIL [%s/column-metadata-%d]: "
                  "vendor={name=%s decltype=%s table=%s origin=%s} "
                  "candidate={name=%s decltype=%s table=%s origin=%s}",
                  label, i,
                  vendor_name ? vendor_name : "(null)",
                  vendor_decltype ? vendor_decltype : "(null)",
                  vendor_table ? vendor_table : "(null)",
                  vendor_origin ? vendor_origin : "(null)",
                  candidate_name ? candidate_name : "(null)",
                  candidate_decltype ? candidate_decltype : "(null)",
                  candidate_table ? candidate_table : "(null)",
                  candidate_origin ? candidate_origin : "(null)");
        }
    }

    binds = sqlite3_bind_parameter_count(vendor);
    if (sqlite3_bind_parameter_count(candidate) != binds) {
        failf("FAIL [%s/bind-count]: vendor=%d candidate=%d", label, binds,
              sqlite3_bind_parameter_count(candidate));
    }
    for (i = 1; i <= binds; i++) {
        const char *vendor_name = sqlite3_bind_parameter_name(vendor, i);
        const char *candidate_name = sqlite3_bind_parameter_name(candidate, i);
        if (!contract_parity_nullable_text_equal(vendor_name, candidate_name)) {
            failf("FAIL [%s/bind-name-%d]: vendor=%s candidate=%s", label, i,
                  vendor_name ? vendor_name : "(null)",
                  candidate_name ? candidate_name : "(null)");
        }
    }
    if (bind) {
        bind(vendor, "vendor", label, bind_ctx);
        bind(candidate, "candidate", label, bind_ctx);
    }

    for (;;) {
        int vendor_rc = sqlite3_step(vendor);
        int candidate_rc = sqlite3_step(candidate);
        int row_differs = 0;

        if (vendor_rc != candidate_rc) {
            failf("FAIL [%s/step-row-%d]: vendor_rc=%d candidate_rc=%d", label,
                  row, vendor_rc, candidate_rc);
        }
        if (vendor_rc == SQLITE_DONE) break;
        if (vendor_rc != SQLITE_ROW) {
            failf("FAIL [%s/step-row-%d]: rc=%d vendor_err=%s candidate_err=%s",
                  label, row, vendor_rc, sqlite3_errmsg(vendor_db),
                  sqlite3_errmsg(candidate_db));
        }
        for (i = 0; i < columns; i++) {
            if (!contract_parity_cell_equal(vendor, candidate, i)) {
                row_differs = 1;
                break;
            }
        }
        if (row_differs) {
            if (!row_exception ||
                !row_exception(label, row, vendor, candidate, exception_ctx)) {
                failf("FAIL [%s/cell-row-%d-column-%d]: vendor_type=%d "
                      "candidate_type=%d vendor_bytes=%d candidate_bytes=%d",
                      label, row, i, sqlite3_column_type(vendor, i),
                      sqlite3_column_type(candidate, i),
                      sqlite3_column_bytes(vendor, i),
                      sqlite3_column_bytes(candidate, i));
            }
        }
        row++;
    }
    if (row < minimum_rows) {
        failf("FAIL [%s/rows]: got=%d want>=%d", label, row, minimum_rows);
    }
    rc = sqlite3_finalize(vendor);
    if (rc != SQLITE_OK) failf("FAIL [%s/vendor-finalize]: rc=%d", label, rc);
    rc = sqlite3_finalize(candidate);
    if (rc != SQLITE_OK) failf("FAIL [%s/candidate-finalize]: rc=%d", label, rc);
}

static void contract_parity_require(
    sqlite3 *vendor_db,
    sqlite3 *candidate_db,
    contract_parity_prepare_fn prepare,
    const char *label,
    const char *vendor_sql,
    const char *candidate_source_sql,
    const char *expected_candidate_sql,
    contract_parity_bind_fn bind,
    void *bind_ctx,
    contract_parity_row_exception_fn row_exception,
    void *exception_ctx
) {
    contract_parity_require_min_rows(
        vendor_db, candidate_db, prepare, label, vendor_sql,
        candidate_source_sql, expected_candidate_sql, bind, bind_ctx,
        row_exception, exception_ctx, 1
    );
}

#endif
