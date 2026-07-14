#include "emby_fts_rewrite.h"
#include "observability.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define OBS_SQL_CAP 4096
#define OBS_TRUNC_TAIL "...[TRUNC]"
#define OBS_LOG_LINE_CAP ((OBS_SQL_CAP * 2) * 4 + 4096)
#define OBS_SAMPLE_PERIOD 1024u
#define OBS_CLIENTDATA_KEY "sqlite3-builds-observability"
#define OBS_CORR_CLIENTDATA_KEY "sqlite3-builds-observability-corr"
#define OBS_CORR_SET_CAP 512u
#define OBS_CORR_TABLE_CAP 1024u
#define OBS_MISS_REASON_COUNT 2u

#define OBS_CORR_UNAVAILABLE_APPLIED 0x01u
#define OBS_CORR_UNAVAILABLE_STMT 0x02u

typedef struct obs_rewrite_mode_metadata {
    const char *target;
    const char *mode;
    const char *fn;
    unsigned char index_missing_eligible;
} obs_rewrite_mode_metadata;

static const obs_rewrite_mode_metadata g_obs_rewrite_modes[OBS_MODE_COUNT] = {
#define OBS_REWRITE_MODE_METADATA(suffix, target, mode, fn, eligible) \
    [OBS_MODE_##suffix] = { target, mode, fn, eligible },
    OBS_REWRITE_MODE_CATALOG(OBS_REWRITE_MODE_METADATA)
#undef OBS_REWRITE_MODE_METADATA
};

_Static_assert(
    sizeof(g_obs_rewrite_modes) / sizeof(g_obs_rewrite_modes[0]) ==
        OBS_MODE_COUNT,
    "rewrite mode metadata cardinality"
);

typedef struct obs_corr_set {
    pthread_mutex_t mutex;
    atomic_uint count;
    uint64_t keys[OBS_CORR_TABLE_CAP];
    unsigned char occupied[OBS_CORR_TABLE_CAP];
} obs_corr_set;

typedef struct obs_corr_state {
    obs_corr_set applied;
    obs_corr_set stmt;
} obs_corr_state;

typedef struct obs_connection_state {
    atomic_uint corr_unavailable_bits;
    atomic_uint_fast64_t applied_counts[OBS_MODE_COUNT];
    atomic_uint_fast64_t stmt_count;
    _Atomic(obs_corr_state *) corr_state;
} obs_connection_state;

enum obs_corr_result {
    OBS_CORR_SEEN = 0,
    OBS_CORR_NEW,
    OBS_CORR_FULL,
    OBS_CORR_UNAVAILABLE
};

typedef void (*obs_log_func)(void*, int, const char*);
typedef void (*obs_sqllog_func)(void*, sqlite3*, const char*, int);

extern int sqlite3_initialize_real(void);
extern int sqlite3_config_real(int op, ...);
extern int sqlite3_db_config_real(sqlite3 *db, int op, ...);
extern int sqlite3_open_real(const char *filename, sqlite3 **ppDb);
extern int sqlite3_open_v2_real(
    const char *filename, sqlite3 **ppDb, int flags, const char *zVfs
);
extern int sqlite3_open16_real(const void *filename, sqlite3 **ppDb);
__attribute__((visibility("hidden"))) extern void auto_extension_register_for_open(void);
__attribute__((visibility("hidden"))) extern int runtime_optimize_in_progress(void);

static pthread_once_t g_obs_once = PTHREAD_ONCE_INIT;
static atomic_int g_obs_disabled;
static atomic_int g_stmt_trace_disabled;
static atomic_int g_stmt_trace_sampling_disabled;
static atomic_int g_rewrite_applied_sql_disabled;
static pthread_mutex_t g_obs_corr_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static obs_corr_set g_miss_shape_set = {
    PTHREAD_MUTEX_INITIALIZER, 0, {0}, {0}
};
static atomic_uint_fast64_t
    g_miss_counts[OBS_MISS_REASON_COUNT][OBS_MODE_COUNT];
static atomic_uint_fast64_t
    g_index_missing_counts[OBS_MODE_COUNT];
static atomic_flag g_obs_unregistered_reported = ATOMIC_FLAG_INIT;
static __thread int s_init_depth = 0;

struct obs_name {
    int value;
    const char *name;
};

static const struct obs_name g_config_names[] = {
    { SQLITE_CONFIG_SINGLETHREAD, "SQLITE_CONFIG_SINGLETHREAD" },
    { SQLITE_CONFIG_MULTITHREAD, "SQLITE_CONFIG_MULTITHREAD" },
    { SQLITE_CONFIG_SERIALIZED, "SQLITE_CONFIG_SERIALIZED" },
    { SQLITE_CONFIG_MALLOC, "SQLITE_CONFIG_MALLOC" },
    { SQLITE_CONFIG_GETMALLOC, "SQLITE_CONFIG_GETMALLOC" },
    { SQLITE_CONFIG_SCRATCH, "SQLITE_CONFIG_SCRATCH" },
    { SQLITE_CONFIG_PAGECACHE, "SQLITE_CONFIG_PAGECACHE" },
    { SQLITE_CONFIG_HEAP, "SQLITE_CONFIG_HEAP" },
    { SQLITE_CONFIG_MEMSTATUS, "SQLITE_CONFIG_MEMSTATUS" },
    { SQLITE_CONFIG_MUTEX, "SQLITE_CONFIG_MUTEX" },
    { SQLITE_CONFIG_GETMUTEX, "SQLITE_CONFIG_GETMUTEX" },
    { SQLITE_CONFIG_LOOKASIDE, "SQLITE_CONFIG_LOOKASIDE" },
    { SQLITE_CONFIG_PCACHE, "SQLITE_CONFIG_PCACHE" },
    { SQLITE_CONFIG_GETPCACHE, "SQLITE_CONFIG_GETPCACHE" },
    { SQLITE_CONFIG_LOG, "SQLITE_CONFIG_LOG" },
    { SQLITE_CONFIG_URI, "SQLITE_CONFIG_URI" },
    { SQLITE_CONFIG_PCACHE2, "SQLITE_CONFIG_PCACHE2" },
    { SQLITE_CONFIG_GETPCACHE2, "SQLITE_CONFIG_GETPCACHE2" },
    { SQLITE_CONFIG_COVERING_INDEX_SCAN, "SQLITE_CONFIG_COVERING_INDEX_SCAN" },
    { SQLITE_CONFIG_SQLLOG, "SQLITE_CONFIG_SQLLOG" },
    { SQLITE_CONFIG_MMAP_SIZE, "SQLITE_CONFIG_MMAP_SIZE" },
    { SQLITE_CONFIG_WIN32_HEAPSIZE, "SQLITE_CONFIG_WIN32_HEAPSIZE" },
    { SQLITE_CONFIG_PCACHE_HDRSZ, "SQLITE_CONFIG_PCACHE_HDRSZ" },
    { SQLITE_CONFIG_PMASZ, "SQLITE_CONFIG_PMASZ" },
    { SQLITE_CONFIG_STMTJRNL_SPILL, "SQLITE_CONFIG_STMTJRNL_SPILL" },
    { SQLITE_CONFIG_SMALL_MALLOC, "SQLITE_CONFIG_SMALL_MALLOC" },
    { SQLITE_CONFIG_SORTERREF_SIZE, "SQLITE_CONFIG_SORTERREF_SIZE" },
    { SQLITE_CONFIG_MEMDB_MAXSIZE, "SQLITE_CONFIG_MEMDB_MAXSIZE" },
    { SQLITE_CONFIG_ROWID_IN_VIEW, "SQLITE_CONFIG_ROWID_IN_VIEW" },
};

static const struct obs_name g_dbconfig_names[] = {
    { SQLITE_DBCONFIG_MAINDBNAME, "SQLITE_DBCONFIG_MAINDBNAME" },
    { SQLITE_DBCONFIG_LOOKASIDE, "SQLITE_DBCONFIG_LOOKASIDE" },
    { SQLITE_DBCONFIG_ENABLE_FKEY, "SQLITE_DBCONFIG_ENABLE_FKEY" },
    { SQLITE_DBCONFIG_ENABLE_TRIGGER, "SQLITE_DBCONFIG_ENABLE_TRIGGER" },
    { SQLITE_DBCONFIG_ENABLE_FTS3_TOKENIZER, "SQLITE_DBCONFIG_ENABLE_FTS3_TOKENIZER" },
    { SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, "SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION" },
    { SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, "SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE" },
    { SQLITE_DBCONFIG_ENABLE_QPSG, "SQLITE_DBCONFIG_ENABLE_QPSG" },
    { SQLITE_DBCONFIG_TRIGGER_EQP, "SQLITE_DBCONFIG_TRIGGER_EQP" },
    { SQLITE_DBCONFIG_RESET_DATABASE, "SQLITE_DBCONFIG_RESET_DATABASE" },
    { SQLITE_DBCONFIG_DEFENSIVE, "SQLITE_DBCONFIG_DEFENSIVE" },
    { SQLITE_DBCONFIG_WRITABLE_SCHEMA, "SQLITE_DBCONFIG_WRITABLE_SCHEMA" },
    { SQLITE_DBCONFIG_LEGACY_ALTER_TABLE, "SQLITE_DBCONFIG_LEGACY_ALTER_TABLE" },
    { SQLITE_DBCONFIG_DQS_DML, "SQLITE_DBCONFIG_DQS_DML" },
    { SQLITE_DBCONFIG_DQS_DDL, "SQLITE_DBCONFIG_DQS_DDL" },
    { SQLITE_DBCONFIG_ENABLE_VIEW, "SQLITE_DBCONFIG_ENABLE_VIEW" },
    { SQLITE_DBCONFIG_LEGACY_FILE_FORMAT, "SQLITE_DBCONFIG_LEGACY_FILE_FORMAT" },
    { SQLITE_DBCONFIG_TRUSTED_SCHEMA, "SQLITE_DBCONFIG_TRUSTED_SCHEMA" },
    { SQLITE_DBCONFIG_STMT_SCANSTATUS, "SQLITE_DBCONFIG_STMT_SCANSTATUS" },
    { SQLITE_DBCONFIG_REVERSE_SCANORDER, "SQLITE_DBCONFIG_REVERSE_SCANORDER" },
    { SQLITE_DBCONFIG_ENABLE_ATTACH_CREATE, "SQLITE_DBCONFIG_ENABLE_ATTACH_CREATE" },
    { SQLITE_DBCONFIG_ENABLE_ATTACH_WRITE, "SQLITE_DBCONFIG_ENABLE_ATTACH_WRITE" },
    { SQLITE_DBCONFIG_ENABLE_COMMENTS, "SQLITE_DBCONFIG_ENABLE_COMMENTS" },
    { SQLITE_DBCONFIG_FP_DIGITS, "SQLITE_DBCONFIG_FP_DIGITS" },
};

static const struct obs_name g_open_flag_names[] = {
    { SQLITE_OPEN_READONLY, "SQLITE_OPEN_READONLY" },
    { SQLITE_OPEN_READWRITE, "SQLITE_OPEN_READWRITE" },
    { SQLITE_OPEN_CREATE, "SQLITE_OPEN_CREATE" },
    { SQLITE_OPEN_DELETEONCLOSE, "SQLITE_OPEN_DELETEONCLOSE" },
    { SQLITE_OPEN_EXCLUSIVE, "SQLITE_OPEN_EXCLUSIVE" },
    { SQLITE_OPEN_AUTOPROXY, "SQLITE_OPEN_AUTOPROXY" },
    { SQLITE_OPEN_URI, "SQLITE_OPEN_URI" },
    { SQLITE_OPEN_MEMORY, "SQLITE_OPEN_MEMORY" },
    { SQLITE_OPEN_MAIN_DB, "SQLITE_OPEN_MAIN_DB" },
    { SQLITE_OPEN_TEMP_DB, "SQLITE_OPEN_TEMP_DB" },
    { SQLITE_OPEN_TRANSIENT_DB, "SQLITE_OPEN_TRANSIENT_DB" },
    { SQLITE_OPEN_MAIN_JOURNAL, "SQLITE_OPEN_MAIN_JOURNAL" },
    { SQLITE_OPEN_TEMP_JOURNAL, "SQLITE_OPEN_TEMP_JOURNAL" },
    { SQLITE_OPEN_SUBJOURNAL, "SQLITE_OPEN_SUBJOURNAL" },
    { SQLITE_OPEN_SUPER_JOURNAL, "SQLITE_OPEN_SUPER_JOURNAL" },
    { SQLITE_OPEN_NOMUTEX, "SQLITE_OPEN_NOMUTEX" },
    { SQLITE_OPEN_FULLMUTEX, "SQLITE_OPEN_FULLMUTEX" },
    { SQLITE_OPEN_SHAREDCACHE, "SQLITE_OPEN_SHAREDCACHE" },
    { SQLITE_OPEN_PRIVATECACHE, "SQLITE_OPEN_PRIVATECACHE" },
    { SQLITE_OPEN_WAL, "SQLITE_OPEN_WAL" },
    { SQLITE_OPEN_NOFOLLOW, "SQLITE_OPEN_NOFOLLOW" },
    { SQLITE_OPEN_EXRESCODE, "SQLITE_OPEN_EXRESCODE" },
};

static void obs_init_once(void) {
    const char *v = getenv("SQLITE3_DISABLE_OBSERVABILITY");
    const char *stmt = getenv("SQLITE3_DISABLE_STMT_TRACE");
    const char *stmt_sampling = getenv("SQLITE3_DISABLE_STMT_TRACE_SAMPLING");
    const char *applied_sql = getenv("SQLITE3_DISABLE_REWRITE_APPLIED_SQL");
    atomic_store_explicit(
        &g_obs_disabled,
        (v && strcmp(v, "1") == 0) ? 1 : 0,
        memory_order_release
    );
    /* STMT trace is off by default; literal "0" is the explicit re-enable. */
    atomic_store_explicit(
        &g_stmt_trace_disabled,
        (stmt && strcmp(stmt, "0") == 0) ? 0 : 1,
        memory_order_release
    );
    atomic_store_explicit(
        &g_stmt_trace_sampling_disabled,
        (stmt_sampling && strcmp(stmt_sampling, "1") == 0) ? 1 : 0,
        memory_order_release
    );
    atomic_store_explicit(
        &g_rewrite_applied_sql_disabled,
        (applied_sql && strcmp(applied_sql, "1") == 0) ? 1 : 0,
        memory_order_release
    );
}

__attribute__((constructor(101)))
static void obs_init(void) {
    pthread_once(&g_obs_once, obs_init_once);
}

__attribute__((visibility("hidden"))) SQLITE_API int obs_is_disabled(void) {
    pthread_once(&g_obs_once, obs_init_once);
    return atomic_load_explicit(&g_obs_disabled, memory_order_acquire);
}

__attribute__((visibility("hidden"))) SQLITE_API int obs_stmt_trace_disabled(void) {
    pthread_once(&g_obs_once, obs_init_once);
    return atomic_load_explicit(&g_stmt_trace_disabled, memory_order_acquire);
}

static long obs_tid(void) {
#if defined(__linux__) && defined(SYS_gettid)
    return (long)syscall(SYS_gettid);
#else
    return (long)getpid();
#endif
}

static void obs_timestamp(char *buf, size_t n) {
    struct timespec ts;
    struct tm tm;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        ts.tv_sec = 0;
        ts.tv_nsec = 0;
    }
    if (gmtime_r(&ts.tv_sec, &tm) == NULL) {
        memset(&tm, 0, sizeof(tm));
    }
    snprintf(buf, n, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000L);
}

static int obs_logf_full(
    const char *ts,
    const char *fn,
    const char *fmt,
    size_t prefix_len,
    size_t payload_len,
    va_list ap
) {
    char *line;
    size_t total;
    size_t used;
    int has_payload = fmt && fmt[0];
    int rc;

    if (has_payload) {
        if (prefix_len > SIZE_MAX - payload_len ||
            prefix_len + payload_len > SIZE_MAX - 3u) {
            return 0;
        }
        total = prefix_len + payload_len + 3u;
    } else {
        if (prefix_len > SIZE_MAX - 2u) return 0;
        total = prefix_len + 2u;
    }
    if ((sqlite3_uint64)total != total) return 0;
    line = (char *)sqlite3_malloc64((sqlite3_uint64)total);
    if (!line) return 0;
    rc = snprintf(
        line, total, "[sqlite3-builds-obs] %s %ld %ld %s",
        ts, (long)getpid(), obs_tid(), fn ? fn : ""
    );
    if (rc < 0 || (size_t)rc != prefix_len) {
        sqlite3_free(line);
        return 0;
    }
    used = prefix_len;
    if (has_payload) {
        line[used++] = ' ';
        rc = vsnprintf(line + used, total - used, fmt, ap);
        if (rc < 0 || (size_t)rc != payload_len) {
            sqlite3_free(line);
            return 0;
        }
        used += payload_len;
    }
    line[used++] = '\n';
    line[used] = 0;
    flockfile(stderr);
    (void)fwrite(line, 1, used, stderr);
    funlockfile(stderr);
    sqlite3_free(line);
    return 1;
}

__attribute__((visibility("hidden"))) SQLITE_API void obs_logf(const char *fn, const char *fmt, ...) {
    char ts[32];
    char line[OBS_LOG_LINE_CAP];
    size_t prefix_len;
    size_t payload_len = 0;
    size_t used;
    size_t tail_len = sizeof(OBS_TRUNC_TAIL) - 1;
    int rc;
    int truncated = 0;
    va_list ap;

    if (obs_is_disabled()) return;

    obs_timestamp(ts, sizeof(ts));
    rc = snprintf(
        line, sizeof(line), "[sqlite3-builds-obs] %s %ld %ld %s",
        ts, (long)getpid(), obs_tid(), fn ? fn : ""
    );
    if (rc < 0) return;
    prefix_len = (size_t)rc;
    if ((size_t)rc >= sizeof(line)) {
        used = sizeof(line) - 1;
        truncated = 1;
    } else {
        used = (size_t)rc;
    }
    if (truncated) {
        if (fmt && fmt[0]) {
            va_start(ap, fmt);
            rc = vsnprintf(NULL, 0, fmt, ap);
            va_end(ap);
            if (rc < 0) return;
            payload_len = (size_t)rc;
        }
        va_start(ap, fmt);
        if (obs_logf_full(
                ts, fn, fmt, prefix_len, payload_len, ap
            )) {
            va_end(ap);
            return;
        }
        va_end(ap);
    } else if (fmt && fmt[0]) {
        if (used + 1 >= sizeof(line)) {
            va_start(ap, fmt);
            rc = vsnprintf(NULL, 0, fmt, ap);
            va_end(ap);
            if (rc < 0) return;
            payload_len = (size_t)rc;
            truncated = 1;
            va_start(ap, fmt);
            if (obs_logf_full(
                    ts, fn, fmt, prefix_len, payload_len, ap
                )) {
                va_end(ap);
                return;
            }
            va_end(ap);
        } else {
            line[used++] = ' ';
            va_start(ap, fmt);
            rc = vsnprintf(line + used, sizeof(line) - used, fmt, ap);
            va_end(ap);
            if (rc < 0) return;
            if ((size_t)rc >= sizeof(line) - used) {
                payload_len = (size_t)rc;
                used = sizeof(line) - 1;
                truncated = 1;
                va_start(ap, fmt);
                if (obs_logf_full(
                        ts, fn, fmt, prefix_len, payload_len, ap
                    )) {
                    va_end(ap);
                    return;
                }
                va_end(ap);
            } else {
                used += (size_t)rc;
            }
        }
    }
    if (truncated) {
        if (used + tail_len + 1 >= sizeof(line)) {
            used = sizeof(line) - tail_len - 2;
        }
        memcpy(line + used, OBS_TRUNC_TAIL, tail_len);
        used += tail_len;
    }
    line[used++] = '\n';
    flockfile(stderr);
    (void)fwrite(line, 1, used, stderr);
    funlockfile(stderr);
}

static const char *obs_lookup_name(
    const struct obs_name *names, size_t count, int value
) {
    size_t i;
    for (i = 0; i < count; i++) {
        if (names[i].value == value) return names[i].name;
    }
    return NULL;
}

static const char *obs_config_name(int op, char *buf, size_t n) {
    const char *name = obs_lookup_name(
        g_config_names, sizeof(g_config_names) / sizeof(g_config_names[0]), op
    );
    if (name) return name;
    snprintf(buf, n, "UNKNOWN(%d)", op);
    return buf;
}

static const char *obs_dbconfig_name(int op, char *buf, size_t n) {
    const char *name = obs_lookup_name(
        g_dbconfig_names,
        sizeof(g_dbconfig_names) / sizeof(g_dbconfig_names[0]),
        op
    );
    if (name) return name;
    snprintf(buf, n, "UNKNOWN(%d)", op);
    return buf;
}

static void obs_escape_span(
    const char *src,
    size_t src_len,
    char *dst,
    size_t dst_n,
    size_t cap,
    int *truncated
) {
    size_t i = 0;
    size_t o = 0;
    size_t limit = src_len < cap ? src_len : cap;
    *truncated = 0;
    if (dst_n == 0) return;
    if (!src) {
        snprintf(dst, dst_n, "(null)");
        return;
    }
    while (i < limit && o + 5 < dst_n) {
        unsigned char c = (unsigned char)src[i++];
        switch (c) {
            case '\\':
                dst[o++] = '\\';
                dst[o++] = '\\';
                break;
            case '"':
                dst[o++] = '\\';
                dst[o++] = '"';
                break;
            case '\n':
                dst[o++] = '\\';
                dst[o++] = 'n';
                break;
            case '\r':
                dst[o++] = '\\';
                dst[o++] = 'r';
                break;
            case '\t':
                dst[o++] = '\\';
                dst[o++] = 't';
                break;
            default:
                if (c < 0x20) {
                    static const char hex[] = "0123456789ABCDEF";
                    dst[o++] = '\\';
                    dst[o++] = 'x';
                    dst[o++] = hex[c >> 4];
                    dst[o++] = hex[c & 0x0f];
                } else {
                    dst[o++] = (char)c;
                }
                break;
        }
    }
    if (i < src_len) *truncated = 1;
    dst[o] = 0;
}

static void obs_escape_unlimited(const char *src, char *dst, size_t dst_n) {
    int truncated = 0;
    size_t src_len = src ? strlen(src) : 0;
    obs_escape_span(src, src_len, dst, dst_n, SIZE_MAX, &truncated);
}

static int obs_escaped_span_len(
    const char *src,
    size_t src_len,
    size_t *escaped_len
) {
    size_t i;
    size_t total = 0;

    if (!src) {
        *escaped_len = sizeof("(null)") - 1;
        return 1;
    }
    for (i = 0; i < src_len; i++) {
        unsigned char c = (unsigned char)src[i];
        size_t add = c < 0x20 ? 4u : 1u;
        if (c == '\\' || c == '"' || c == '\n' || c == '\r' || c == '\t') {
            add = 2u;
        }
        if (total > SIZE_MAX - add) return 0;
        total += add;
    }
    *escaped_len = total;
    return 1;
}

static size_t obs_escape_span_exact(
    const char *src,
    size_t src_len,
    char *dst
) {
    static const char hex[] = "0123456789ABCDEF";
    size_t i;
    size_t o = 0;

    if (!src) {
        memcpy(dst, "(null)", sizeof("(null)") - 1);
        return sizeof("(null)") - 1;
    }
    for (i = 0; i < src_len; i++) {
        unsigned char c = (unsigned char)src[i];
        switch (c) {
            case '\\':
                dst[o++] = '\\';
                dst[o++] = '\\';
                break;
            case '"':
                dst[o++] = '\\';
                dst[o++] = '"';
                break;
            case '\n':
                dst[o++] = '\\';
                dst[o++] = 'n';
                break;
            case '\r':
                dst[o++] = '\\';
                dst[o++] = 'r';
                break;
            case '\t':
                dst[o++] = '\\';
                dst[o++] = 't';
                break;
            default:
                if (c < 0x20) {
                    dst[o++] = '\\';
                    dst[o++] = 'x';
                    dst[o++] = hex[c >> 4];
                    dst[o++] = hex[c & 0x0f];
                } else {
                    dst[o++] = (char)c;
                }
                break;
        }
    }
    return o;
}

static void obs_escape_sql_bounded(
    const char *src,
    size_t src_len,
    char *dst,
    size_t dst_n,
    size_t cap
) {
    int truncated = 0;
    size_t tail_len = strlen(OBS_TRUNC_TAIL);
    obs_escape_span(src, src_len, dst, dst_n, cap, &truncated);
    if (truncated) {
        size_t len = strlen(dst);
        if (dst_n > tail_len + 1) {
            if (len + tail_len >= dst_n) {
                len = dst_n - tail_len - 1;
            }
            memcpy(dst + len, OBS_TRUNC_TAIL, tail_len + 1);
        }
    }
}

__attribute__((visibility("hidden"))) SQLITE_API void obs_escape_sql(
    const char *src,
    char *dst,
    size_t dst_n
) {
    obs_escape_sql_bounded(src, src ? strlen(src) : 0, dst, dst_n, OBS_SQL_CAP);
}

__attribute__((visibility("hidden"))) SQLITE_API uint64_t obs_sql_corr_key(
    const char *sql,
    size_t len
) {
    uint64_t hash = UINT64_C(14695981039346656037);
    size_t i;

    if (!sql) return hash;
    for (i = 0; i < len; i++) {
        hash ^= (unsigned char)sql[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void obs_connection_state_free(void *arg) {
    sqlite3_free(arg);
}

static int obs_corr_set_init(obs_corr_set *set) {
    memset(set, 0, sizeof(*set));
    atomic_init(&set->count, 0u);
    return pthread_mutex_init(&set->mutex, NULL) == 0;
}

static void obs_corr_state_free(void *arg) {
    obs_corr_state *state = (obs_corr_state *)arg;
    if (!state) return;
    (void)pthread_mutex_destroy(&state->applied.mutex);
    (void)pthread_mutex_destroy(&state->stmt.mutex);
    sqlite3_free(state);
}

static obs_connection_state *obs_connection_state_get(sqlite3 *db, int create) {
    obs_connection_state *state;
    int rc;
    size_t i;

    if (!db) return NULL;
    state = (obs_connection_state *)sqlite3_get_clientdata(db, OBS_CLIENTDATA_KEY);
    if (state || !create) return state;
    if (pthread_mutex_lock(&g_obs_corr_state_mutex) != 0) return NULL;
    state = (obs_connection_state *)sqlite3_get_clientdata(db, OBS_CLIENTDATA_KEY);
    if (state) {
        (void)pthread_mutex_unlock(&g_obs_corr_state_mutex);
        return state;
    }
    state = (obs_connection_state *)sqlite3_malloc64(sizeof(*state));
    if (!state) {
        (void)pthread_mutex_unlock(&g_obs_corr_state_mutex);
        return NULL;
    }
    atomic_init(&state->corr_unavailable_bits, 0u);
    for (i = 0; i < OBS_MODE_COUNT; i++) {
        atomic_init(&state->applied_counts[i], 0u);
    }
    atomic_init(&state->stmt_count, 0u);
    atomic_init(&state->corr_state, NULL);
    rc = sqlite3_set_clientdata(
        db, OBS_CLIENTDATA_KEY, state, obs_connection_state_free
    );
    if (rc != SQLITE_OK) {
        sqlite3_free(state);
        (void)pthread_mutex_unlock(&g_obs_corr_state_mutex);
        return NULL;
    }
    (void)pthread_mutex_unlock(&g_obs_corr_state_mutex);
    return state;
}

static obs_corr_state *obs_corr_state_get(
    sqlite3 *db,
    obs_connection_state *connection,
    int create
) {
    obs_corr_state *state;
    int rc;

    if (!db || !connection) return NULL;
    state = atomic_load_explicit(&connection->corr_state, memory_order_acquire);
    if (state || !create) return state;
    if (pthread_mutex_lock(&g_obs_corr_state_mutex) != 0) return NULL;
    state = atomic_load_explicit(&connection->corr_state, memory_order_acquire);
    if (state) {
        (void)pthread_mutex_unlock(&g_obs_corr_state_mutex);
        return state;
    }
    state = (obs_corr_state *)sqlite3_get_clientdata(db, OBS_CORR_CLIENTDATA_KEY);
    if (state) {
        atomic_store_explicit(
            &connection->corr_state, state, memory_order_release
        );
        (void)pthread_mutex_unlock(&g_obs_corr_state_mutex);
        return state;
    }
    state = (obs_corr_state *)sqlite3_malloc64(sizeof(*state));
    if (!state) {
        (void)pthread_mutex_unlock(&g_obs_corr_state_mutex);
        return NULL;
    }
    if (!obs_corr_set_init(&state->applied)) {
        sqlite3_free(state);
        (void)pthread_mutex_unlock(&g_obs_corr_state_mutex);
        return NULL;
    }
    if (!obs_corr_set_init(&state->stmt)) {
        (void)pthread_mutex_destroy(&state->applied.mutex);
        sqlite3_free(state);
        (void)pthread_mutex_unlock(&g_obs_corr_state_mutex);
        return NULL;
    }
    rc = sqlite3_set_clientdata(
        db, OBS_CORR_CLIENTDATA_KEY, state, obs_corr_state_free
    );
    if (rc != SQLITE_OK) {
        obs_corr_state_free(state);
        (void)pthread_mutex_unlock(&g_obs_corr_state_mutex);
        return NULL;
    }
    atomic_store_explicit(&connection->corr_state, state, memory_order_release);
    (void)pthread_mutex_unlock(&g_obs_corr_state_mutex);
    return state;
}

static size_t obs_corr_bucket(uint64_t corr) {
    corr ^= corr >> 33;
    corr *= UINT64_C(0xff51afd7ed558ccd);
    corr ^= corr >> 33;
    corr *= UINT64_C(0xc4ceb9fe1a85ec53);
    corr ^= corr >> 33;
    return (size_t)corr & (OBS_CORR_TABLE_CAP - 1u);
}

static enum obs_corr_result obs_corr_set_observe(
    obs_corr_set *set,
    uint64_t corr
) {
    size_t bucket;
    size_t i;
    unsigned int count;

    if (!set || pthread_mutex_lock(&set->mutex) != 0) {
        return OBS_CORR_UNAVAILABLE;
    }
    count = atomic_load_explicit(&set->count, memory_order_relaxed);
    if (count >= OBS_CORR_SET_CAP) {
        (void)pthread_mutex_unlock(&set->mutex);
        return OBS_CORR_FULL;
    }
    bucket = obs_corr_bucket(corr);
    for (i = 0; i < OBS_CORR_TABLE_CAP; i++) {
        size_t slot = (bucket + i) & (OBS_CORR_TABLE_CAP - 1u);
        if (!set->occupied[slot]) {
            set->keys[slot] = corr;
            set->occupied[slot] = 1u;
            atomic_store_explicit(&set->count, count + 1u, memory_order_release);
            (void)pthread_mutex_unlock(&set->mutex);
            return OBS_CORR_NEW;
        }
        if (set->keys[slot] == corr) {
            (void)pthread_mutex_unlock(&set->mutex);
            return OBS_CORR_SEEN;
        }
    }
    (void)pthread_mutex_unlock(&set->mutex);
    return OBS_CORR_FULL;
}

static obs_corr_set *obs_corr_set_for_stream(
    sqlite3 *db,
    obs_connection_state *connection,
    unsigned int unavailable_bit,
    int applied
) {
    obs_corr_state *state;
    obs_corr_set *set;
    unsigned int unavailable;

    if (!connection) return NULL;
    unavailable = atomic_load_explicit(
        &connection->corr_unavailable_bits, memory_order_acquire
    );
    if (unavailable & unavailable_bit) return NULL;
    state = obs_corr_state_get(db, connection, 1);
    if (!state) {
        (void)atomic_fetch_or_explicit(
            &connection->corr_unavailable_bits,
            unavailable_bit,
            memory_order_release
        );
        return NULL;
    }
    set = applied ? &state->applied : &state->stmt;
    if (atomic_load_explicit(&set->count, memory_order_acquire) >=
        OBS_CORR_SET_CAP) {
        return NULL;
    }
    return set;
}

static const char *obs_sample_label(
    uint64_t count,
    enum obs_corr_result observed
) {
    if (count == 1u) return "first";
    if (observed == OBS_CORR_NEW) return "new";
    if (count % OBS_SAMPLE_PERIOD == 0u) return "periodic";
    return NULL;
}

static const obs_rewrite_mode_metadata *obs_rewrite_mode_lookup(
    obs_rewrite_mode mode,
    const char *site
) {
    int32_t mode_id = mode;

    if (mode_id > OBS_MODE_NONE && mode_id < OBS_MODE_COUNT) {
        return &g_obs_rewrite_modes[(size_t)mode_id];
    }
    if (!atomic_flag_test_and_set_explicit(
            &g_obs_unregistered_reported, memory_order_relaxed
        )) {
        obs_logf("observability",
            "event=obs_mode_unregistered mode_id=%" PRId32 " site=%s",
            mode_id, site
        );
    }
    return NULL;
}

__attribute__((visibility("hidden"))) SQLITE_API void obs_log_index_missing(
    sqlite3 *db,
    obs_rewrite_mode mode
) {
    const obs_rewrite_mode_metadata *metadata;
    uint64_t count;
    const char *sample_label;
    size_t mode_index;

    if (obs_is_disabled()) return;
    metadata = obs_rewrite_mode_lookup(mode, "index_missing");
    if (!metadata) return;
    if (metadata->index_missing_eligible == 0) return;
    mode_index = (size_t)(int32_t)mode;
    count = atomic_fetch_add_explicit(
        &g_index_missing_counts[mode_index], 1u, memory_order_relaxed
    ) + 1u;
    sample_label = obs_sample_label(count, OBS_CORR_UNAVAILABLE);
    if (!sample_label) return;
    obs_logf(
        metadata->fn,
        "event=rewrite_skipped target=%s reason=index_missing mode=%s "
        "db=%p sample=%s count=%" PRIu64,
        metadata->target, metadata->mode, (void *)db,
        sample_label, count
    );
}

static uint64_t obs_mix_string(uint64_t hash, const char *value) {
    const unsigned char *p = (const unsigned char *)(value ? value : "");

    while (*p) {
        hash ^= *p++;
        hash *= UINT64_C(1099511628211);
    }
    return hash * UINT64_C(1099511628211);
}

static uint64_t obs_mix(
    uint64_t shape,
    const obs_rewrite_mode_metadata *metadata,
    const char *reason,
    const char *sub_reason
) {
    uint64_t hash = shape;

    hash = obs_mix_string(hash, metadata->target);
    hash = obs_mix_string(hash, reason);
    hash = obs_mix_string(hash, metadata->mode);
    return obs_mix_string(hash, sub_reason);
}

static const char *obs_miss_reason_name(obs_miss_reason reason) {
    if (reason == OBS_MISS_CAPTURE) return "capture_miss";
    if (reason == OBS_MISS_OUT_OF_SCOPE) return "out_of_scope";
    return NULL;
}

static int obs_log_capture_miss_full(
    const obs_rewrite_mode_metadata *metadata,
    const char *reason,
    const char *sub_reason,
    sqlite3 *db,
    const char *sql,
    size_t len,
    const char *sample,
    uint64_t count,
    uint64_t shape,
    uint64_t corr
) {
    char ts[32];
    char *line;
    size_t escaped_len;
    size_t prefix_len;
    size_t total;
    size_t used;
    int rc;

    if (!obs_escaped_span_len(sql, len, &escaped_len)) return 0;
    obs_timestamp(ts, sizeof(ts));
    rc = snprintf(
        NULL, 0,
        "[sqlite3-builds-obs] %s %ld %ld %s "
        "event=rewrite_skipped target=%s reason=%s mode=%s "
        "sub_reason=%s db=%p sample=%s count=%" PRIu64
        " shape=%016" PRIx64 " sql_len=%zu corr=%016" PRIx64 " sql=\"",
        ts, (long)getpid(), obs_tid(), metadata->fn,
        metadata->target, reason, metadata->mode,
        sub_reason ? sub_reason : "", (void *)db, sample, count, shape, len, corr
    );
    if (rc < 0) return 0;
    prefix_len = (size_t)rc;
    if (prefix_len > SIZE_MAX - escaped_len ||
        prefix_len + escaped_len > SIZE_MAX - 3u) {
        return 0;
    }
    total = prefix_len + escaped_len + 3u;
    line = (char *)sqlite3_malloc64((sqlite3_uint64)total);
    if (!line) return 0;
    rc = snprintf(
        line, total,
        "[sqlite3-builds-obs] %s %ld %ld %s "
        "event=rewrite_skipped target=%s reason=%s mode=%s "
        "sub_reason=%s db=%p sample=%s count=%" PRIu64
        " shape=%016" PRIx64 " sql_len=%zu corr=%016" PRIx64 " sql=\"",
        ts, (long)getpid(), obs_tid(), metadata->fn,
        metadata->target, reason, metadata->mode,
        sub_reason ? sub_reason : "", (void *)db, sample, count, shape, len, corr
    );
    if (rc < 0 || (size_t)rc != prefix_len) {
        sqlite3_free(line);
        return 0;
    }
    used = prefix_len;
    used += obs_escape_span_exact(sql, len, line + used);
    line[used++] = '"';
    line[used++] = '\n';
    line[used] = 0;
    flockfile(stderr);
    (void)fwrite(line, 1, used, stderr);
    funlockfile(stderr);
    sqlite3_free(line);
    return 1;
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
    const obs_rewrite_mode_metadata *metadata;
    char sqlbuf[OBS_SQL_CAP * 4 + sizeof(OBS_TRUNC_TAIL)];
    enum obs_corr_result observed;
    const char *reason_name;
    const char *sample_label;
    uint64_t count;
    uint64_t corr;
    uint64_t key;
    size_t mode_index;

    if (obs_is_disabled()) return;
    reason_name = obs_miss_reason_name(reason);
    if (!reason_name) return;
    metadata = obs_rewrite_mode_lookup(mode, "rewrite_miss");
    if (!metadata) return;
    mode_index = (size_t)(int32_t)mode;
    count = atomic_fetch_add_explicit(
        &g_miss_counts[(unsigned int)reason][mode_index],
        1u,
        memory_order_relaxed
    ) + 1u;
    key = obs_mix(shape, metadata, reason_name, sub_reason);
    observed = shape != 0u
        ? obs_corr_set_observe(&g_miss_shape_set, key)
        : OBS_CORR_UNAVAILABLE;
    sample_label = obs_sample_label(count, observed);
    if (!sample_label) return;
    corr = obs_sql_corr_key(sql, len);
    if (obs_log_capture_miss_full(
            metadata, reason_name, sub_reason, db, sql, len,
            sample_label, count, shape, corr
        )) {
        return;
    }
    obs_escape_sql_bounded(sql, len, sqlbuf, sizeof(sqlbuf), OBS_SQL_CAP);
    obs_logf(
        metadata->fn,
        "event=rewrite_skipped target=%s reason=%s mode=%s "
        "sub_reason=%s db=%p sample=%s count=%" PRIu64
        " shape=%016" PRIx64 " sql_len=%zu corr=%016" PRIx64 " sql=\"%s\"",
        metadata->target, reason_name, metadata->mode,
        sub_reason ? sub_reason : "", (void *)db, sample_label, count, shape,
        len, corr, sqlbuf
    );
}

static void obs_emit_rewrite_applied(
    const obs_rewrite_mode_metadata *metadata,
    sqlite3 *db,
    const char *sample,
    uint64_t count,
    const char *source,
    size_t source_len,
    const char *rewritten,
    size_t rewritten_len,
    uint64_t source_corr,
    uint64_t corr
) {
    if (atomic_load_explicit(
            &g_rewrite_applied_sql_disabled, memory_order_acquire
        )) {
        obs_logf(
            metadata->fn,
            "event=rewrite_applied target=%s mode=%s db=%p sample=%s "
            "count=%" PRIu64 " source_corr=%016" PRIx64 " corr=%016" PRIx64,
            metadata->target, metadata->mode, (void *)db, sample,
            count, source_corr, corr
        );
        return;
    }
    {
        char sourcebuf[OBS_SQL_CAP * 4 + sizeof(OBS_TRUNC_TAIL)];
        char sqlbuf[OBS_SQL_CAP * 4 + sizeof(OBS_TRUNC_TAIL)];
        obs_escape_sql_bounded(
            source, source_len, sourcebuf, sizeof(sourcebuf), OBS_SQL_CAP
        );
        obs_escape_sql_bounded(
            rewritten, rewritten_len, sqlbuf, sizeof(sqlbuf), OBS_SQL_CAP
        );
        obs_logf(
            metadata->fn,
            "event=rewrite_applied target=%s mode=%s db=%p sample=%s "
            "count=%" PRIu64 " source_corr=%016" PRIx64
            " corr=%016" PRIx64 " source_sql=\"%s\" sql=\"%s\"",
            metadata->target, metadata->mode, (void *)db, sample,
            count, source_corr, corr, sourcebuf, sqlbuf
        );
    }
}

__attribute__((visibility("hidden"))) SQLITE_API void obs_log_rewrite_applied(
    obs_rewrite_mode mode,
    sqlite3 *db,
    const char *source,
    size_t source_len,
    const char *rewritten,
    size_t rewritten_len
) {
    const obs_rewrite_mode_metadata *metadata;
    obs_connection_state *state;
    obs_corr_set *corr_set = NULL;
    enum obs_corr_result corr_result = OBS_CORR_UNAVAILABLE;
    uint64_t count;
    uint64_t source_corr;
    uint64_t corr;
    const char *sample_label;
    size_t mode_index;

    if (obs_is_disabled()) return;
    metadata = obs_rewrite_mode_lookup(mode, "rewrite_applied");
    if (!metadata) return;
    mode_index = (size_t)(int32_t)mode;
    state = obs_connection_state_get(db, 1);
    if (!state) return;
    count = atomic_fetch_add_explicit(
        &state->applied_counts[mode_index], 1u, memory_order_relaxed
    ) + 1u;
    corr_set = obs_corr_set_for_stream(
        db, state, OBS_CORR_UNAVAILABLE_APPLIED, 1
    );
    corr = obs_sql_corr_key(rewritten, rewritten_len);
    if (corr_set) {
        corr_result = obs_corr_set_observe(corr_set, corr);
        if (corr_result == OBS_CORR_UNAVAILABLE) {
            (void)atomic_fetch_or_explicit(
                &state->corr_unavailable_bits,
                OBS_CORR_UNAVAILABLE_APPLIED,
                memory_order_release
            );
        }
    }
    sample_label = obs_sample_label(count, corr_result);
    if (!sample_label) return;
    source_corr = obs_sql_corr_key(source, source_len);
    obs_emit_rewrite_applied(
        metadata, db, sample_label, count,
        source, source_len, rewritten, rewritten_len, source_corr, corr
    );
}

__attribute__((visibility("hidden"))) SQLITE_API void obs_log_rewrite_skipped(
    sqlite3 *db,
    const char *reason,
    obs_rewrite_mode mode
) {
    const obs_rewrite_mode_metadata *metadata;

    if (obs_is_disabled()) return;
    metadata = obs_rewrite_mode_lookup(mode, "rewrite_skipped");
    if (!metadata) return;
    obs_logf(
        metadata->fn,
        "event=rewrite_skipped target=%s reason=%s mode=%s db=%p",
        metadata->target, reason, metadata->mode, (void *)db
    );
}

static void obs_decode_open_flags(int flags, char *buf, size_t n) {
    unsigned int remaining = (unsigned int)flags;
    size_t i;
    size_t used = 0;
    int wrote = 0;

    if (n == 0) return;
    buf[0] = 0;
    for (i = 0; i < sizeof(g_open_flag_names) / sizeof(g_open_flag_names[0]); i++) {
        unsigned int bit = (unsigned int)g_open_flag_names[i].value;
        if ((remaining & bit) == bit) {
            int rc = snprintf(buf + used, n - used, "%s%s",
                              wrote ? "|" : "", g_open_flag_names[i].name);
            if (rc < 0 || (size_t)rc >= n - used) {
                buf[n - 1] = 0;
                return;
            }
            used += (size_t)rc;
            wrote = 1;
            remaining &= ~bit;
        }
    }
    if (remaining || !wrote) {
        int rc = snprintf(buf + used, n - used, "%s0x%x",
                          wrote ? "|" : "", remaining);
        if (rc < 0 || (size_t)rc >= n - used) {
            buf[n - 1] = 0;
        }
    }
}

static int obs_forward_config(int op, va_list *ap) {
    switch (op) {
        case SQLITE_CONFIG_SINGLETHREAD:
        case SQLITE_CONFIG_MULTITHREAD:
        case SQLITE_CONFIG_SERIALIZED:
            return sqlite3_config_real(op);
        case SQLITE_CONFIG_MALLOC:
        case SQLITE_CONFIG_GETMALLOC: {
            sqlite3_mem_methods *p = va_arg(*ap, sqlite3_mem_methods*);
            return sqlite3_config_real(op, p);
        }
        case SQLITE_CONFIG_SCRATCH:
            return sqlite3_config_real(op);
        case SQLITE_CONFIG_PAGECACHE:
        case SQLITE_CONFIG_HEAP: {
            void *p = va_arg(*ap, void*);
            int a = va_arg(*ap, int);
            int b = va_arg(*ap, int);
            return sqlite3_config_real(op, p, a, b);
        }
        case SQLITE_CONFIG_MEMSTATUS:
        case SQLITE_CONFIG_URI:
        case SQLITE_CONFIG_COVERING_INDEX_SCAN:
        case SQLITE_CONFIG_WIN32_HEAPSIZE:
        case SQLITE_CONFIG_STMTJRNL_SPILL:
        case SQLITE_CONFIG_SMALL_MALLOC:
        case SQLITE_CONFIG_SORTERREF_SIZE: {
            int v = va_arg(*ap, int);
            return sqlite3_config_real(op, v);
        }
        case SQLITE_CONFIG_MUTEX:
        case SQLITE_CONFIG_GETMUTEX: {
            sqlite3_mutex_methods *p = va_arg(*ap, sqlite3_mutex_methods*);
            return sqlite3_config_real(op, p);
        }
        case SQLITE_CONFIG_LOOKASIDE: {
            int a = va_arg(*ap, int);
            int b = va_arg(*ap, int);
            return sqlite3_config_real(op, a, b);
        }
        case SQLITE_CONFIG_PCACHE:
        case SQLITE_CONFIG_GETPCACHE:
            return sqlite3_config_real(op);
        case SQLITE_CONFIG_LOG: {
            obs_log_func x = va_arg(*ap, obs_log_func);
            void *p = va_arg(*ap, void*);
            return sqlite3_config_real(op, x, p);
        }
        case SQLITE_CONFIG_PCACHE2:
        case SQLITE_CONFIG_GETPCACHE2: {
            sqlite3_pcache_methods2 *p = va_arg(*ap, sqlite3_pcache_methods2*);
            return sqlite3_config_real(op, p);
        }
        case SQLITE_CONFIG_SQLLOG: {
            obs_sqllog_func x = va_arg(*ap, obs_sqllog_func);
            void *p = va_arg(*ap, void*);
            return sqlite3_config_real(op, x, p);
        }
        case SQLITE_CONFIG_MMAP_SIZE: {
            sqlite3_int64 a = va_arg(*ap, sqlite3_int64);
            sqlite3_int64 b = va_arg(*ap, sqlite3_int64);
            return sqlite3_config_real(op, a, b);
        }
        case SQLITE_CONFIG_PCACHE_HDRSZ:
        case SQLITE_CONFIG_ROWID_IN_VIEW: {
            int *p = va_arg(*ap, int*);
            return sqlite3_config_real(op, p);
        }
        case SQLITE_CONFIG_PMASZ: {
            unsigned int v = va_arg(*ap, unsigned int);
            return sqlite3_config_real(op, v);
        }
        case SQLITE_CONFIG_MEMDB_MAXSIZE: {
            sqlite3_int64 v = va_arg(*ap, sqlite3_int64);
            return sqlite3_config_real(op, v);
        }
        default:
            return sqlite3_config_real(op);
    }
}

static int obs_forward_db_config(sqlite3 *db, int op, va_list *ap) {
    switch (op) {
        case SQLITE_DBCONFIG_MAINDBNAME: {
            char *z = va_arg(*ap, char*);
            return sqlite3_db_config_real(db, op, z);
        }
        case SQLITE_DBCONFIG_LOOKASIDE: {
            void *p = va_arg(*ap, void*);
            int a = va_arg(*ap, int);
            int b = va_arg(*ap, int);
            return sqlite3_db_config_real(db, op, p, a, b);
        }
        case SQLITE_DBCONFIG_FP_DIGITS: {
            int v = va_arg(*ap, int);
            int *p = va_arg(*ap, int*);
            return sqlite3_db_config_real(db, op, v, p);
        }
        case SQLITE_DBCONFIG_ENABLE_FKEY:
        case SQLITE_DBCONFIG_ENABLE_TRIGGER:
        case SQLITE_DBCONFIG_ENABLE_FTS3_TOKENIZER:
        case SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION:
        case SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE:
        case SQLITE_DBCONFIG_ENABLE_QPSG:
        case SQLITE_DBCONFIG_TRIGGER_EQP:
        case SQLITE_DBCONFIG_RESET_DATABASE:
        case SQLITE_DBCONFIG_DEFENSIVE:
        case SQLITE_DBCONFIG_WRITABLE_SCHEMA:
        case SQLITE_DBCONFIG_LEGACY_ALTER_TABLE:
        case SQLITE_DBCONFIG_DQS_DML:
        case SQLITE_DBCONFIG_DQS_DDL:
        case SQLITE_DBCONFIG_ENABLE_VIEW:
        case SQLITE_DBCONFIG_LEGACY_FILE_FORMAT:
        case SQLITE_DBCONFIG_TRUSTED_SCHEMA:
        case SQLITE_DBCONFIG_STMT_SCANSTATUS:
        case SQLITE_DBCONFIG_REVERSE_SCANORDER:
        case SQLITE_DBCONFIG_ENABLE_ATTACH_CREATE:
        case SQLITE_DBCONFIG_ENABLE_ATTACH_WRITE:
        case SQLITE_DBCONFIG_ENABLE_COMMENTS: {
            int onoff = va_arg(*ap, int);
            int *p = va_arg(*ap, int*);
            return sqlite3_db_config_real(db, op, onoff, p);
        }
        default:
            return sqlite3_db_config_real(db, op);
    }
}

SQLITE_API int sqlite3_initialize(void) {
    int outer;
    int rc;
    pthread_once(&g_obs_once, obs_init_once);
    outer = (s_init_depth++ == 0);
    rc = sqlite3_initialize_real();
    s_init_depth--;
    if (outer && rc != SQLITE_OK &&
        !atomic_load_explicit(&g_obs_disabled, memory_order_acquire)) {
        obs_logf("sqlite3_initialize", "rc=%d", rc);
    }
    return rc;
}

SQLITE_API int sqlite3_config(int op, ...) {
    va_list ap;
    int rc;
    char unknown[32];
    const char *name;

    pthread_once(&g_obs_once, obs_init_once);
    va_start(ap, op);
    rc = obs_forward_config(op, &ap);
    va_end(ap);

    if (!atomic_load_explicit(&g_obs_disabled, memory_order_acquire)) {
        name = obs_config_name(op, unknown, sizeof(unknown));
        obs_logf("sqlite3_config", "op=%s rc=%d", name, rc);
    }
    return rc;
}

SQLITE_API int sqlite3_db_config(sqlite3 *db, int op, ...) {
    va_list ap;
    int rc;
    char unknown[32];
    const char *name;

    pthread_once(&g_obs_once, obs_init_once);
    va_start(ap, op);
    rc = obs_forward_db_config(db, op, &ap);
    va_end(ap);

    if (!atomic_load_explicit(&g_obs_disabled, memory_order_acquire)) {
        name = obs_dbconfig_name(op, unknown, sizeof(unknown));
        obs_logf("sqlite3_db_config", "db=%p op=%s rc=%d", (void*)db, name, rc);
    }
    return rc;
}

SQLITE_API int sqlite3_open(const char *filename, sqlite3 **ppDb) {
    int rc;
    char file[4096];

    pthread_once(&g_obs_once, obs_init_once);
    auto_extension_register_for_open();
    rc = sqlite3_open_real(filename, ppDb);
    if (!atomic_load_explicit(&g_obs_disabled, memory_order_acquire)) {
        obs_escape_unlimited(filename, file, sizeof(file));
        if (rc == SQLITE_OK) {
            obs_logf("sqlite3_open",
                     "file=\"%s\" flags=SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE db=%p rc=%d",
                     file, ppDb ? (void*)*ppDb : NULL, rc);
        } else {
            obs_logf("sqlite3_open", "file=\"%s\" db=%p rc=%d",
                     file, ppDb ? (void*)*ppDb : NULL, rc);
        }
    }
    return rc;
}

SQLITE_API int sqlite3_open_v2(
    const char *filename, sqlite3 **ppDb, int flags, const char *zVfs
) {
    int rc;
    char file[4096];
    char vfs[512];
    char flagbuf[1024];

    pthread_once(&g_obs_once, obs_init_once);
    auto_extension_register_for_open();
    rc = sqlite3_open_v2_real(filename, ppDb, flags, zVfs);
    if (!atomic_load_explicit(&g_obs_disabled, memory_order_acquire)) {
        obs_escape_unlimited(filename, file, sizeof(file));
        obs_escape_unlimited(zVfs, vfs, sizeof(vfs));
        obs_decode_open_flags(flags, flagbuf, sizeof(flagbuf));
        obs_logf("sqlite3_open_v2", "file=\"%s\" flags=%s vfs=\"%s\" db=%p rc=%d",
                 file, flagbuf, vfs, ppDb ? (void*)*ppDb : NULL, rc);
    }
    return rc;
}

SQLITE_API int sqlite3_open16(const void *filename, sqlite3 **ppDb) {
    int rc;

    pthread_once(&g_obs_once, obs_init_once);
    auto_extension_register_for_open();
    rc = sqlite3_open16_real(filename, ppDb);
    if (!atomic_load_explicit(&g_obs_disabled, memory_order_acquire)) {
        if (rc == SQLITE_OK) {
            obs_logf("sqlite3_open16",
                     "filename16=%p flags=SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE db=%p rc=%d",
                     filename, ppDb ? (void*)*ppDb : NULL, rc);
        } else {
            obs_logf("sqlite3_open16", "filename16=%p db=%p rc=%d",
                     filename, ppDb ? (void*)*ppDb : NULL, rc);
        }
    }
    return rc;
}

SQLITE_API int sqlite3_prepare(
    sqlite3 *db,
    const char *zSql,
    int nByte,
    sqlite3_stmt **ppStmt,
    const char **pzTail
) {
    return emby_fts_rewrite_prepare(
        db, zSql, nByte, 0, ppStmt, pzTail, FTS_REWRITE_PREPARE_LEGACY
    );
}

SQLITE_API int sqlite3_prepare_v2(
    sqlite3 *db,
    const char *zSql,
    int nByte,
    sqlite3_stmt **ppStmt,
    const char **pzTail
) {
    return emby_fts_rewrite_prepare(
        db, zSql, nByte, 0, ppStmt, pzTail, FTS_REWRITE_PREPARE_V2
    );
}

SQLITE_API int sqlite3_prepare_v3(
    sqlite3 *db,
    const char *zSql,
    int nByte,
    unsigned int prepFlags,
    sqlite3_stmt **ppStmt,
    const char **pzTail
) {
    return emby_fts_rewrite_prepare(
        db, zSql, nByte, prepFlags, ppStmt, pzTail, FTS_REWRITE_PREPARE_V3
    );
}

__attribute__((visibility("hidden"))) SQLITE_API int obs_trace_stmt_cb(unsigned trace, void *ctx, void *p, void *x) {
    sqlite3_stmt *stmt;
    sqlite3 *db;
    const char *sql;
    const char *file_name;
    const char *sample_label;
    uint64_t count;
    uint64_t corr;
    size_t sql_len;
    char file[4096];
    char sqlbuf[OBS_SQL_CAP + 256];
    obs_connection_state *state;
    obs_corr_set *corr_set = NULL;
    enum obs_corr_result corr_result = OBS_CORR_UNAVAILABLE;
    int scheduled = 1;

    if (runtime_optimize_in_progress()) return 0;
    (void)ctx;
    if (trace != SQLITE_TRACE_STMT || obs_is_disabled()) return 0;

    /* STMT trace stays template-only: sensitive expanded SQL capture is
     * restricted to the gated PROFILE slow path because STMT has no elapsed
     * threshold and can fire for every statement. */
    stmt = (sqlite3_stmt*)p;
    db = stmt ? sqlite3_db_handle(stmt) : NULL;
    state = obs_connection_state_get(db, 1);
    if (state) {
        count = atomic_fetch_add_explicit(
            &state->stmt_count, 1u, memory_order_relaxed
        ) + 1u;
        if (atomic_load_explicit(
            &g_stmt_trace_sampling_disabled, memory_order_acquire
            )) {
            sample_label = "full";
        } else {
            sample_label = obs_sample_label(count, OBS_CORR_UNAVAILABLE);
            scheduled = sample_label != NULL;
        }
        if (!atomic_load_explicit(
                &g_stmt_trace_sampling_disabled, memory_order_acquire
            )) {
            corr_set = obs_corr_set_for_stream(
                db, state, OBS_CORR_UNAVAILABLE_STMT, 0
            );
            if (!scheduled && !corr_set) return 0;
        }
    } else {
        return 0;
    }
    sql = x ? (const char*)x : (stmt ? sqlite3_sql(stmt) : NULL);
    sql_len = sql ? strlen(sql) : 0;
    corr = obs_sql_corr_key(sql, sql_len);
    if (corr_set) {
        corr_result = obs_corr_set_observe(corr_set, corr);
        if (corr_result == OBS_CORR_UNAVAILABLE) {
            (void)atomic_fetch_or_explicit(
                &state->corr_unavailable_bits,
                OBS_CORR_UNAVAILABLE_STMT,
                memory_order_release
            );
        }
    }
    if (!scheduled) {
        sample_label = obs_sample_label(count, corr_result);
        if (!sample_label) return 0;
    }
    file_name = db ? sqlite3_db_filename(db, "main") : NULL;
    obs_escape_unlimited(file_name, file, sizeof(file));
    obs_escape_sql(sql, sqlbuf, sizeof(sqlbuf));
    obs_logf(
        "trace_stmt",
        "event=SQLITE_TRACE_STMT db=%p stmt=%p sample=%s count=%" PRIu64
        " file=\"%s\" sql_len=%zu corr=%016" PRIx64 " sql=\"%s\"",
        (void*)db, p, sample_label, count, file, sql_len, corr, sqlbuf
    );
    return 0;
}
