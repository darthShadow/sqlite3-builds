#ifndef SQLITE3_BUILDS_OBSERVABILITY_H
#define SQLITE3_BUILDS_OBSERVABILITY_H

#include "rewrite_modes.h"
#include "sqlite3.h"

#include <stddef.h>
#include <stdint.h>

typedef enum obs_miss_reason {
    OBS_MISS_CAPTURE = 0,
    OBS_MISS_OUT_OF_SCOPE = 1
} obs_miss_reason;

__attribute__((visibility("hidden"))) SQLITE_API int obs_is_disabled(void);
__attribute__((visibility("hidden"))) SQLITE_API int obs_stmt_trace_disabled(void);
__attribute__((visibility("hidden"))) SQLITE_API int obs_trace_cb(
    unsigned trace,
    void *ctx,
    void *p,
    void *x
);
__attribute__((visibility("hidden"))) SQLITE_API int obs_trace_stmt_cb(
    unsigned trace,
    void *ctx,
    void *p,
    void *x
);
__attribute__((visibility("hidden"))) SQLITE_API void obs_logf(
    const char *fn,
    const char *fmt,
    ...
);
__attribute__((visibility("hidden"))) SQLITE_API void obs_log_rewrite_miss(
    obs_rewrite_mode mode,
    obs_miss_reason reason,
    const char *sub_reason,
    sqlite3 *db,
    const char *sql,
    size_t len,
    uint64_t shape
);
__attribute__((visibility("hidden"))) SQLITE_API void obs_log_index_missing(
    sqlite3 *db,
    obs_rewrite_mode mode
);
__attribute__((visibility("hidden"))) SQLITE_API void obs_log_rewrite_applied(
    obs_rewrite_mode mode,
    sqlite3 *db,
    const char *source,
    size_t source_len,
    const char *rewritten,
    size_t rewritten_len
);
__attribute__((visibility("hidden"))) SQLITE_API void obs_log_rewrite_skipped(
    sqlite3 *db,
    const char *reason,
    obs_rewrite_mode mode
);

#endif
