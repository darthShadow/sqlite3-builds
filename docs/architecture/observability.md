# Observability

Part of the [Repository Architecture index](../architecture.md).

## Observability Layer

`src/observability.c` is compiled into both shared-library variants and is
excluded from the CLI target. `src/observability.h` declares the private
cross-translation-unit observability seam. `src/rewrite_modes.h` is the only
rewrite-mode catalogue. Its 14 signed `OBS_MODE_*` ids each own target, wire
mode, positional logger label, and index-missing eligibility metadata.
Applied, miss, and index-missing counters share the `OBS_MODE_COUNT` index
space. Applied counters remain per connection; miss counters and their bounded
shape set remain process-global; index-missing counters remain process-global.
Only Plex taggings, Plex On-Deck, Emby Episodes Latest, and Emby movies Latest
are index-missing eligible.

The library target patches SQLite's amalgamation so these public APIs are
implemented by wrappers in `src/observability.c` while the original SQLite
implementations remain available as hidden `*_real` symbols:

- `sqlite3_initialize`
- `sqlite3_config`
- `sqlite3_db_config`
- `sqlite3_open`
- `sqlite3_open_v2`
- `sqlite3_open16`
- `sqlite3_prepare`
- `sqlite3_prepare_v2`
- `sqlite3_prepare_v3`

The initialize/config/db-config/open wrappers call the hidden real
implementation, return the real return code unchanged, and log after the call
with numeric `rc=N` where applicable. They do not mutate config opcodes,
db-config opcodes, open flags, filenames, handles, or return codes. The public
UTF-8 prepare wrappers chain through `src/observability.c` ->
`emby_fts_rewrite_prepare()` -> `plex_fts_rewrite_prepare()` -> the matching
hidden `*_real` prepare implementation. The helpers are result-preserving for
disabled, non-target, nonmatching, drift, and failure paths, and intentionally
prepare rewritten SQL only for enabled target matches. Effective rewrites use
per-connection, per-mode applied counters plus a bounded per-connection set of
first-seen rewritten-SQL `corr` keys. The first record has
`sample=first count=1`; every 1024th per-mode record has `sample=periodic`; and
on other counts, the first occurrence of a distinct correlation has
`sample=new`. Every
emitted label carries full-span `source_corr` and rewritten `corr` plus raw
`source_sql` and rewritten `sql`, each capped at 4 KiB. Repeated correlations
between periodic samples are suppressed. When the 512-key set is full or
unavailable, logging falls back to the first/every-1024th sampler. If
per-connection sampler state cannot be allocated or registered, known-mode
`rewrite_applied` and STMT diagnostics are skipped; sampler failure never
increases output. Literal
`SQLITE3_DISABLE_REWRITE_APPLIED_SQL=1` omits both SQL text fields from every
emitted record while retaining counters and correlation keys; unset, literal
`0`, and every other value retain them. This control is cached once per
process, is subordinate to the master observability gate, and does not gate
rewriting. Plex modes are `fts+tag_type`, `guid+like-null`,
`taggings+membership`, and `ondeck`. Emby modes are `fts+membership`,
`fanout+resume`, `fanout+browse`, `fanout+favorites`, `fanout+people`,
`fanout+links_search`, `fanout+resume_simple`, `fanout+similar`,
`dashboard+episodes_latest`, and `dashboard+movies_latest`. No compatibility
alias exists for the Episodes mode. Each dashboard family logs `capture_miss`
only after its own Type discriminator; valid sibling traffic is an unlogged
clean miss. The capture-gated fan-out modes are `fanout+people`, after its
`itemPeople2` discriminator, and `fanout+links_search`, after its
`WithItemLinkItemIds` discriminator. Capture-gated fan-out and dashboard misses
log `reason=capture_miss` with `sub_reason`, `db`, `sample`, process-global
`count`, structural `shape`, exact `sql_len`, a full raw-span FNV-1a `corr`
key, and the full escaped source `sql`. The first miss per reason and mode emits
as `sample=first`, every 1024th emits as `sample=periodic`, and a first-seen
structural shape on other counts emits as `sample=new`. One process-global
512-key set observes every event, including the first. The caller computes the
shape with the shared lexer over at most 8192 bytes only after confirming
observability is enabled. Identifiers are ASCII-lowercased, numeric and string
literals are normalized, same-type literal lists collapse across cardinality,
and parameter tokens retain their exact bytes. Lexer errors and a full or
unavailable set fall back to first/periodic sampling and never increase output.
This capture
uses `pre_l1`, `select_anchor`, `ancestor_slot`, and `membership` for People;
links-search also uses `tail_anchor` and `type_slot`; and dashboard Latest uses
`prefix`, `select_anchor`, `ancestor_slot`, `from_anchor`, `projection`,
`tail_anchor`, `user_slot`, and `limit`. Positively parsed unsupported dashboard
limits and detected bind parameters instead use `reason=out_of_scope` with
`sub_reason=limit_unsupported` or `bind`. This capture
path allocates the uncapped record; allocation or record-size failure emits a
bounded fallback diagnostic and never changes prepare behavior. The key is a
diagnostic join aid, not a unique identifier or security boundary. Early clean
misses, missing-index paths, and index-probe errors do not emit capture SQL.

Every mode-bearing API checks `SQLITE3_DISABLE_OBSERVABILITY` before mode
lookup. Applied logging then acquires connection state, increments its mode
counter, observes correlation, selects the schedule, and emits. Miss logging
validates the miss reason before mode lookup, then increments its process-global
counter, observes shape, selects the schedule, and emits. Index-missing logging
looks up the mode, rejects registered ineligible rows before counter mutation,
then selects its process-global schedule. Skip logging looks up the mode and
emits without sampling. Invalid numeric ids return before clientdata access,
allocation, counter mutation, or correlation observation and suppress the
requested record. When observability is enabled, one shared atomic flag permits
at most one invalid-id diagnostic per process. The diagnostic prints the signed
`mode_id` and one of `rewrite_applied`, `rewrite_miss`, `index_missing`, or
`rewrite_skipped` as `site`.

The invalid-mode record uses positional logger label `observability` followed
by payload
`event=obs_mode_unregistered mode_id=<signed-decimal> site=<site>`; it does not
emit a `fn=` field. The negative-first applied example contains
` observability event=obs_mode_unregistered mode_id=-1 site=rewrite_applied`.
With the base Plex On-Deck rewrite enabled, both the id-list and
`viewed_at > <literal>` selector arms run. After the exact On-Deck head matches,
parse drift emits `capture_miss` with first-failure `sub_reason` from `section`,
`selector`, `id_list`, `threshold`, `post_id`, `account`, `tail`, or `trailing`.
The threshold arm has no separate gate or disabled-silence state; a threshold
parse failure emits `sub_reason=threshold`. The recognized per-GUID selector
form emits `reason=out_of_scope sub_reason=ondeck_per_guid`; recognizer drift
falls back to `capture_miss sub_reason=selector`.
Missing required Emby dashboard or Plex
taggings/On-Deck indexes use process-global per-mode counters. The first event
emits `sample=first count=1`, every 1024th emits `sample=periodic count=N`, and
all other events are suppressed across database handles. The emitted `db=` is
the exemplar handle while `count=` is process-wide. Probe errors remain
unsuppressed. This path has no clientdata allocation dependency.
Other fail-open rewrite reasons are `scalar_unavailable` for Emby FTS,
`index_probe_error`, `build_failed`, `rewritten_prepare_failed`,
`tail_mismatch`, `bind_count_mismatch`, and `column_count_mismatch` for the
verified Plex GUID-LIKE and On-Deck rewrites. They do not carry capture SQL.
`SQLITE3_DISABLE_OBSERVABILITY` gates helper logs independently of
`SQLITE3_DISABLE_STMT_TRACE`; pure passthrough and clean-miss paths do not log.

The `sqlite3_open`, `sqlite3_open_v2`, and `sqlite3_open16` wrappers call
`auto_extension_register_for_open()` after observability initialization and
immediately before their hidden `_real` open implementation. The helper is
per-open and may trigger effective SQLite initialization before `openDatabase`
enters its own initialization path.

The observability kill switch is:

```text
SQLITE3_DISABLE_OBSERVABILITY=1
```

Only literal `1` disables observability. The value is cached once during the
priority-101 observability constructor, with per-wrapper `pthread_once`
fallback. Disabled mode skips wrapper log emission and skips per-connection
`sqlite3_trace_v2` registration.

The statement-trace env knob is:

```text
SQLITE3_DISABLE_STMT_TRACE=0
```

Statement trace logging is off by default. Only literal `0` enables it; unset,
empty, `1`, and every other value disable it. The value is cached by the same
observability initialization path as the master kill switch. Disabled/default
mode omits `SQLITE_TRACE_STMT` from the per-connection trace mask; PROFILE-side
slow-query tracking is unaffected. When the resulting trace mask is `0`,
`sqlite3_trace_v2` is not called.

Enabled STMT logging uses a per-connection hybrid sampler. The first callback
emits as `sample=first`; every 1024th callback emits as `sample=periodic`; and
on other counts, the first occurrence of a distinct `corr` shape emits once as
`sample=new`. The first-seen set is bounded at 512 keys; a full or
unavailable set falls back to the first/every-1024th sampler. Literal
`SQLITE3_DISABLE_STMT_TRACE_SAMPLING=1` restores full enabled-STMT logging with
`sample=full count=N`; unset, literal `0`, and every other value retain
hybrid sampling. The sampling control never enables STMT trace by itself.
Unscheduled callbacks with an available first-seen set retrieve and hash SQL;
already-seen shapes return before filename lookup, escaping, or logging.

Log lines are written to stderr:

```text
[sqlite3-builds-obs] <ISO-8601-UTC-ms> <pid> <kernel-tid> <fn> key=value ...
```

Kernel thread IDs come from `syscall(SYS_gettid)`. `obs_logf` formats inline
before locking stderr; records that exceed the inline buffer use an allocated
full-record path. Both paths perform exactly one `fwrite()` while holding the
stream lock. Every record has one terminal newline; allocation or record-size
failure receives a bounded `...[TRUNC]` fallback. `OBS_SQL_CAP=4096` governs
STMT SQL and both the source and
rewritten SQL fields in `rewrite_applied` records. `capture_miss` instead uses
its separately allocated full-record path described above. STMT records include
exact `sql_len` and a full-span `corr` key. For an applied rewrite, the STMT key
describes the rewritten SQL; join source SQL through the applied record's
`source_corr`, not the STMT key.

`sqlite3_initialize` uses a thread-local depth counter so nested initialization
still calls the real implementation but only the outer 0-to-1 transition logs
failures where `rc != SQLITE_OK`. This absorbs helper-triggered initialization
followed by `openDatabase`'s initialization check without false failure logs.

`autopragma_init()` registers a unified trace callback before the AUTOPRAGMA
gate, with STMT added only when `SQLITE3_DISABLE_STMT_TRACE=0` and PROFILE
conditionally added when slow-query tracking is enabled. Observability is
orthogonal to PRAGMA injection: target filtering, read-only skips, and the
autopragma kill switch do not suppress observability logs. Trace registration
failure logs and continues; it must never make `sqlite3_open*` fail.

### Slow-Query Tracker

The same per-connection trace registration uses one unified callback with
`SQLITE_TRACE_STMT` added only when statement tracing is explicitly enabled and
`SQLITE_TRACE_PROFILE` added only when slow-query tracking is enabled. SQLite
allows one trace callback per connection, so the tracker shares the existing
registration site that runs before the connection escapes to application code.

The kill-switch hierarchy is:

```text
SQLITE3_DISABLE_OBSERVABILITY=1 -> no observability or tracker work
SQLITE3_DISABLE_STMT_TRACE unset or !=0 -> STMT observability off, PROFILE tracker unaffected
SQLITE3_DISABLE_STMT_TRACE=0 -> sampled STMT observability on when observability is enabled
SQLITE3_DISABLE_STMT_TRACE_SAMPLING=1 -> every enabled STMT callback logs
SQLITE3_DISABLE_REWRITE_APPLIED_SQL=1 -> all emitted applied records omit SQL text only
SQLITE3_DISABLE_SLOW_QUERY=1 -> PROFILE tracker off; STMT follows SQLITE3_DISABLE_STMT_TRACE
```

The helper unconditionally calls `sqlite3_auto_extension` regardless of the
`SQLITE3_DISABLE_AUTOPRAGMA=1` state. This keeps
`sqlite3_trace_v2(SQLITE_TRACE_STMT)` registration in `autopragma_init`
independent of AUTOPRAGMA enablement and preserves orthogonality between the two
kill switches.

`SQLITE3_SLOW_QUERY_THRESHOLD_MS` defaults to `500`. The parser accepts only
unsigned decimal milliseconds, allows `0` for debug-only all-sample logging,
and falls back to `500` for unset, empty, signed, non-digit, trailing-junk,
ERANGE, or pre-conversion overflow values. PROFILE elapsed values come from
SQLite's millisecond-quantized timing source and are scaled to nanoseconds by
SQLite before the callback receives them.

The tracker keeps a process-global 4096-entry LRU keyed by parameterized
`sqlite3_sql()` text. It stores and displays at most 2048 SQL bytes per
template, uses the full SQL FNV-1a hash as the probe accelerator, and uses that
full hash as the equality proof for templates longer than 2048 bytes. Existing
STMT and rewrite-applied source/rewritten SQL logging keeps its separate 4 KiB
`OBS_SQL_CAP`; capture-miss source SQL is uncapped on its allocated full-record
path. `SLOW_QUERY_SQL_CAP` changes neither behavior, and config/db-config decode
tables stay in `src/observability.c`.

LRU evictions tombstone the bucket entry and trigger an amortized O(1) bucket
rebuild when tombstones reach 25% of entries_used.

Stats are retained as count, sum, sum-of-squares, min, and max. Supported
compilers use `unsigned __int128` accumulators; fallback compilers use
`uint64_t` with a per-template sample cap of 1048576 observations and one
warning per capped template. On that fallback, `sum_ns` and `sum_sq_ns`
saturate at `UINT64_MAX` rather than wrapping. Saturated fields are capped
floors, not exact figures, and emitted snapshots mark them as
`saturated=sum_ns`, `saturated=sum_sq_ns`, or
`saturated=sum_ns,sum_sq_ns`. A saturated `sum_ns` always qualifies for stats
emission and does not consume the 20-entry total-only budget.
The dump path heap-allocates value snapshots, copies under the tracker mutex,
sorts by descending total elapsed time, and emits `slow_query_stats` lines
after releasing the mutex when `count >= 5` and either the mean reaches the
threshold or `total_ns / 20 >= threshold_ns`. The total-only arm emits at most
the first 20 templates per dump in the existing descending-total order;
mean-qualified templates are not budgeted. Stats fields `total_ms`, `mean_ms`,
`stddev_ms`, `min_ms`, and `max_ms` render with six digits after the decimal
point.

Dumps occur at normal process exit and at most every five minutes during
PROFILE activity. The atexit path sets an atomic in-exit flag first, so later
PROFILE callbacks return before touching SQLite pointers or the mutex, and it
uses `pthread_mutex_trylock`; on contention it emits a one-line skip diagnostic
instead of stalling process shutdown.

The tracker is an observability satellite that depends on the hidden
`obs_logf` / `obs_is_disabled` ABI. No sink-abstraction layer exists.

### Slow-Query Verification

Docker build compiles + runs `slow_query_smoke`, `slow_query_atexit_smoke`,
`slow_query_concurrency_smoke`. CI re-runs positive, disabled-mode, atexit,
and concurrency checks, and it compiles + runs `stmt_trace_smoke` against the
same registration path to enforce STMT enablement, sampling override,
hybrid first/periodic/new correlation sampling, and full capture-miss SQL with
bounded allocation-failure fallback. It also covers rewrite-miss structural
sampling, GUID-literal and id-list cardinality collapse, exact parameter-token
shapes, set exhaustion, lexer-error fallback, and the master gate. Rewrite
smokes cover full
first/periodic/new applied records, SQL-text suppression, capture-miss source
records, `out_of_scope` classification, and process-global missing-index
sampling. Bench binaries
(`slow_query_select1_bench`,
`slow_query_metadata_bench`, `config_after_dlopen_concurrent_bench`,
`alloc_latency_bench`, `runtime_optimize_close_bench`,
`plex_fts_rewrite_prepare_bench`, and `emby_fts_rewrite_prepare_bench`) compile
unconditionally. The Dockerfile runs `slow_query_select1_bench`,
`slow_query_metadata_bench`, `config_after_dlopen_concurrent_bench`, and
`alloc_latency_bench` as advisory steps on every library build: nonzero exits
print an `ADVISORY FAIL` line and continue. The generic library image also runs
`runtime_optimize_close_bench` as advisory output; it prints p50, p95, p99, max,
and pass/fail verdicts, then exits 0 so benchmark drift never fails CI. The Plex
and Emby FTS prepare benches follow the same advisory posture: their timing
verdicts report prepare-cost drift but do not fail CI.
