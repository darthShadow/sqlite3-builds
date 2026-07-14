# Rewrite Engine

Part of the [Repository Architecture index](../architecture.md).

## Per-Connection PRAGMA Injection

`src/auto_extension.c` and `src/runtime_optimize.c` are compiled into both
library variants; `src/auto_extension_internal.h` is the shared internal seam
header. `src/auto_extension.c` owns open-time registration, trace-mask setup,
target filtering, and the TLS seam, and reaches runtime optimize through
`runtime_optimize_seed_path`; `src/runtime_optimize.c` owns runtime optimize
logic.

The priority-102 constructor first registers `SQLITE_CONFIG_LOG` with
`sqlite_log_to_observability`. `build/Build.sh` sets
`-DSQLITE_DEFAULT_SORTERREF_SIZE=512`, so the sorter-reference threshold is
active for code paths that do not run this constructor. The constructor
re-asserts `sqlite3_config(SQLITE_CONFIG_SORTERREF_SIZE, 512)` before
effective SQLite initialization for visibility and drift protection.
Auto-extension registration is lazy: the observability `sqlite3_open`,
`sqlite3_open_v2`, and `sqlite3_open16` wrappers call
`auto_extension_register_for_open()` immediately before the hidden real open
function. That helper calls
`sqlite3_auto_extension(autopragma_init)`, which performs the first effective
`sqlite3_initialize()` when needed; `openDatabase` then reaches
`sqlite3AutoLoadExtensions(db)` and runs the callback for the new connection.
Embedders may call startup-only `sqlite3_config()` verbs after `dlopen` and
before their first public open.

The same constructor also re-asserts `sqlite3_config(SQLITE_CONFIG_PMASZ, 8192)`
and stores the rc in `auto_extension_pmasz_cfg_rc()`, mirroring
`auto_extension_sorterref_cfg_rc()` for smoke verification and keeping the
runtime PMA target aligned with the compile default.

The callback is best-effort:

- It never makes `sqlite3_open*` fail on tuning failure.
- It logs failed PRAGMA execution with the database filename.
- It returns `SQLITE_OK` in all application-facing cases.

The callback applies tuning only when the main database filename matches one of
the supported media-server database suffixes:

| Suffix | Target |
|---|---|
| `com.plexapp.plugins.library.db` | Plex |
| `/library.db` | Emby and any same-name current target |
| `/jellyfin.db` | Legacy JF-style filename filter; deployment unsupported |

Filter details:

- Empty filenames are ignored.
- In-memory, temporary, and attached-only cases are ignored.
- A `?` query suffix is stripped defensively before matching.
- Matching is case-sensitive.
- Very long paths that truncate inside the fixed buffer fail closed as filter
  misses.

The kill switch is:

```text
SQLITE3_DISABLE_AUTOPRAGMA=1
```

Only literal `1` disables the callback. Unset values and other strings leave
the callback active.

Read-only opens are skipped before mutating PRAGMAs are run:

```text
sqlite3_db_readonly(db, "main") == 1
```

The emitted PRAGMA block is:

```sql
PRAGMA cache_size=-1048576;
PRAGMA mmap_size=34359738368;
PRAGMA temp_store=2;
PRAGMA threads=8;
PRAGMA wal_autocheckpoint=16000;
PRAGMA journal_size_limit=67108864;
PRAGMA busy_timeout=10000;
PRAGMA analysis_limit=0;
```

Per-connection injection sets `analysis_limit=0`.
`scripts/optimize_media_servers.sh` exports `SQLITE3_DISABLE_AUTOPRAGMA=1` and
re-sets `analysis_limit=0` during planned-downtime runs so maintenance
`optimize()` calls execute without the per-connection ceiling.
PMS app-side `ANALYZE` inherits this accurate-stats setting on enrolled
connections.

The same callback recognizes Plex, Emby, and the legacy JF-style filename.
JF deployment is unsupported; the filter remains a library behavior, not a
current deployment promise. PMS issues its own per-connect PRAGMAs after 2-arg
`sqlite3_open` returns with implicit `SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE`,
so the injected settings are best-effort for Plex and sticky for Emby
connections that do not run an app-side PRAGMA pass. Open-time analysis remains
absent; runtime incremental optimization runs later on eligible close and inline
safe-idle paths, and planned-downtime maintenance remains the explicit full
database rebuild lifecycle.

`synchronous` is not set by the callback. The compile profile already gives WAL
databases the desired synchronous mode, and a connection-level PRAGMA would
also affect rollback-journal databases.

`optimize` is not set by the open callback. Runtime optimize is gated to later
safe-idle boundaries so Plex's per-connection ICU/collation/tokenizer
registration can already be present on the app-owned connection.

## Runtime Optimize

`src/runtime_optimize.c` exports hidden
`auto_extension_optimize_before_close(sqlite3 *db)` and
`auto_extension_optimize_after_stmt(sqlite3 *db, sqlite3_stmt *self_stmt)` for
the library-only amalgamation patch. `src/auto_extension.c` reaches runtime
optimize through the single hidden `runtime_optimize_seed_path(sqlite3 *db,
const char *raw_fn)` seam declared in `src/auto_extension_internal.h`, while
keeping open-time registration, trace-mask setup, target filtering, and the
TLS seam on its side. The mechanism is internal, variant-agnostic C. Plex adds
no runtime branch here; `LIBRARY_VARIANT=plex` remains limited to build-time
ICU linkage.

Runtime optimize runs on safe idle boundaries only:

- Internal immediate-close path, before SQLite converts the handle to zombie.
- Public `sqlite3_step()` when it returns `SQLITE_DONE`, after a provably fast
  plain read statement.
- Public `sqlite3_reset()` when it returns `SQLITE_OK`, after a provably fast
  plain read statement.
- Public `sqlite3_finalize()` hook exists, but it passes NULL statement
  identity; directly finalized statements are close-backstop only.

The helpers never change public SQLite return codes and swallow optimize
failures after logging. NULL close, already-busy `sqlite3_close`, and busy
`sqlite3_close_v2` zombie paths retain native SQLite behavior.

Eligibility gates:

- `SQLITE3_DISABLE_AUTOPRAGMA=1` disables open-time autopragmas and all runtime
  optimize hooks.
- `SQLITE3_DISABLE_RUNTIME_OPTIMIZE=0` enables close and inline runtime
  optimize. Unset, literal `1`, and every other value disable runtime optimize.
- The main database filename must pass `auto_extension_path_is_target`.
- `sqlite3_db_readonly(db, "main") == 1` skips readonly and immutable opens.
- `sqlite3_get_autocommit(db) == 0` skips open transactions.
- Close requires `sqlite3_next_stmt(db, NULL) == NULL`.
- Inline allows prepared idle cached statements but skips when another
  statement is busy.
- Inline runs only when the triggering statement is an ordinary read-only query
  and its fresh PROFILE elapsed time is below
  `SQLITE3_SLOW_QUERY_THRESHOLD_MS` (default 500 ms). Transaction-control,
  connection-control, read-only `PRAGMA`, write, missing-statement,
  telemetry-off, and unobserved inline triggers defer to a later inline attempt
  or the close backstop.
- Operator note: `SQLITE3_SLOW_QUERY_THRESHOLD_MS=0` disables inline optimize;
  every observed elapsed time is `>= 0`, so due work runs only at close.

A process-local per-path registry stores separate successful LIMITED and FULL
cadence stamps plus inflight and failure-backoff state. LIMITED runs at most
once per target path per `SQLITE3_RUNTIME_OPTIMIZE_LIMITED_SECONDS` successful
cadence, default `1800`. FULL runs at most once per target path per
`SQLITE3_RUNTIME_OPTIMIZE_FULL_SECONDS` successful cadence, default `86400`.
A successful FULL pass also satisfies the LIMITED cadence. Failures set only a
short backoff and never stamp either cadence, so an early missing-collation
failure can retry later on a connection that has registered the required
collation or tokenizer. A due LIMITED no-op can still spend work at a safe idle
boundary; SQLite's `PRAGMA optimize` remains the LIMITED table-staleness gate.
With runtime optimize enabled, the first qualifying fast read after a path's
FULL cadence is due can synchronously pay the FULL `ANALYZE main;` cost on the
caller connection up to `RUNTIME_OPTIMIZE_FULL_DEADLINE_NS` (15 s), bounded by
the progress-deadline interrupt. This is opt-in and defaults to about once per
target path per day.

Each enrolled target connection also carries a clientdata next-due cache for
its own path. Inline hot exits check that per-connection atomic value before
filename copy, readonly/autocommit checks, and the busy-peer scan, so a due
different target path does not force unrelated not-due target connections onto
the slower reserve path. When an enrolled connection is plausibly due but a
pre-reserve guard blocks it before shared path reservation, the hook pushes only
that connection's atomic next-due value out by 30 seconds. This re-arms the
cheap inline hot skip without touching shared per-path success cadence or
failure backoff state and bounds repeated O(statement-cache) blocked scans and
skip logs while the connection stays blocked; if per-connection state is
absent, the block keeps the old behavior and does not create re-arm state.

Execution tiers:

```sql
-- LIMITED
PRAGMA main.analysis_limit=0;
PRAGMA main.optimize=0x10002;

-- FULL
PRAGMA main.analysis_limit=0;
ANALYZE main;
```

Both tiers save and restore the prior `analysis_limit` and the caller-visible
connection error state. Inline optimize also saves and restores the
application's progress handler and uses it as a deadline guard, then suspends
that guard before restoring connection state. Inline optimize snapshots the full
busy-handler state, installs a 1 ms built-in busy timeout while the tier
statement runs, and restores the exact prior handler and timeout afterward. The
close trigger keeps the close-only 1 ms busy-timeout behavior. LIMITED runs
conditional `PRAGMA main.optimize=0x10002`; FULL runs unconditional
`ANALYZE main;` with no trailing optimize. Neither tier optimizes attached
schemas or starts a background thread or separate connection. It uses the
application's own connection, so Plex ICU/collation/tokenizer registrations are
in scope when they exist.
Runtime optimize snapshots and restores the caller-visible
`sqlite3_changes()` and `sqlite3_total_changes64()` counters across its
internal statements after computing telemetry.

On the optimize execution path, observability emits `runtime_optimize`
`event=optimize_start` before the tier statement, then
`event=optimize_done` on success or `event=optimize_failed` on failure. These
lines are gated by `SQLITE3_DISABLE_OBSERVABILITY=1` through the shared
observability sink and include `tier`, monotonic `elapsed_ms`, `stat_rows` from
the `sqlite3_total_changes64()` delta, and `since_last_ms` (`-1` when that tier
has no prior success). A due pre-reserve block on an enrolled connection emits
`event=optimize_skipped reason=readonly|open_txn|busy_peer|not_idle_read` at
the same point it re-arms the connection-local next-due value, so repeated
inline statements log at most once per 30-second re-arm interval. Not-due hot
skips emit no optimize log.

Runtime optimize's own `PRAGMA main.analysis_limit=0`,
`PRAGMA main.optimize=0x10002`, and `ANALYZE main` statements are suppressed
from `trace_stmt`, `slow_query`, and `slow_query_expanded` output. The
`runtime_optimize` start/done/failed lines remain visible through the
observability sink.

Worst-case per-connection stall budget at a safe idle boundary is approximately:

- LIMITED: 3 s.
- FULL: 15 s.

Both tiers refresh STAT1 and STAT4 at `analysis_limit=0` when their tier
statement analyzes a table. LIMITED lets SQLite's optimize staleness gate choose
the tables. FULL recomputes whole-main-schema stats every cycle, including
present-but-wrong `sqlite_stat1` rows that optimize leaves unchanged.

RCA: sampled `sqlite_stat1` created by `analysis_limit=1024` can record a
limit+1 fan-out artifact such as `1025` and mis-drive the Plex user prefix
tag-search into a tight `fts4_tag_titles_icu_segdir` per-row loop. The spin was
observed on SQLite 3.53.2; the current 3.53.3 pin does not fix it. Accurate
`analysis_limit=0` stats flip the plan to the FTS-driver shape.

## Plex FTS Prefix Rewrite

`src/plex_fts_rewrite.c` is compiled into both shared-library variants and is
active only in the Plex-capable build where `SQLITE_ENABLE_ICU` is set. It is
default-on. Literal `SQLITE3_DISABLE_PLEX_FTS_REWRITE=1` disables the rewrite;
unset, literal `0`, and every other value enable it. The env result is cached
once per process.

The rewrite runs from the public UTF-8 prepare wrappers:

- `sqlite3_prepare`
- `sqlite3_prepare_v2`
- `sqlite3_prepare_v3`

`sqlite3_prepare16`, `sqlite3_prepare16_v2`, and `sqlite3_prepare16_v3` are a
documented non-goal because the observed PMS query is ASCII/UTF-8.

Runtime eligibility requires the main database filename to pass
`auto_extension_path_is_target()` and have exact basename
`com.plexapp.plugins.library.db`. This excludes generic `/tmp/library.db`,
`/tmp/jellyfin.db`, in-memory, temporary, and non-target database handles even
when the env gate is enabled.

The matcher is structural, not byte-exact. It token-scans outside
strings/comments, tolerates whitespace variation, and matches a minimal
anchor set for the current Plex target: a `fts4_tag_titles_icu` reference with
a `MATCH` on that table, plus exactly one actual unqualified
`tag_type=<value>` equality in the same syntactic `SELECT` block. The
`tag_type` equality must be in predicate context reached through `WHERE` or
`ON`, including `AND`/`OR` continuations and parenthesized predicate
expressions. Non-predicate `tag_type` mentions in projections or ordering do not
count as anchors and do not permit a rewrite. The `tag_type` value token may be
a bare integer, a quoted string, or a bare `?` bind, and the next significant
token must be EOF, `)`, `AND`, `OR`, or a clause boundary keyword. RHS
continuations such as `tag_type=6+1`, `tag_type=?+1`, and partial literals such
as `tag_type=0x1f` are misses. It does not require `DISTINCT`, the
`metadata_items.library_section_id` or `metadata_items.metadata_type`
predicates, `GROUP BY tags.id`, `ORDER BY count(*)`, or `LIMIT 100`.
Unparseable values, duplicate or ambiguous `tag_type=<value>` equality anchors,
semicolons, embedded NULs, missing anchors, or any scan ambiguity are misses.

`src/plex_fts_rewrite.c` keeps the Plex-specific FTS table name, target
predicate column, and database basename together as Plex identifier isolation.
`src/emby_fts_rewrite.c` owns the separate Emby rewrite path, and both rewrite
paths consume the shared `src/fts_lex.c` scanner.

`src/rewrite_modes.h` owns every rewrite-mode identity and wire label. Plex and
Emby producers pass exact signed `OBS_MODE_*` ids through candidates and helper
calls; no engine-owned free-form mode string remains. The central observability
skip helper derives target, mode, and positional logger label from the same
catalogue row. Producer selection, matcher gates, SQL construction, index
probes, fail-open fallback, and applied-after-success ordering remain
engine-owned and unchanged.

Rationale: `unlikely(tag_type=X)` preserves affinity by wrapping the boolean result; `+tag_type`
strips it (`unlikely(tag_type='6')` matched 614279 rows, `+tag_type='6'` matched 0). PMS inlines
varying literals, so minimal anchors wrap any accepted value instead of a brittle byte-exact/full skeleton.
MATCH and `tag_type=` share one SELECT block, with `tag_type=` in predicate context, to exclude CTE/subquery/projection/ORDER BY mentions.
Drift, NOMEM, or prepare denial falls back to original SQL; the rewrite is performance-only.

On a match, the helper inserts only `unlikely(` immediately before `tag_type`
and `)` immediately after the value token, preserving every other SQL byte.
The wrapper prepares the rewritten SQL with the matching hidden `_real`
prepare function and accepts only the end-of-input SQL surface: a successful
rewrite must prepare through the rewritten statement end. If `pzTail` is
requested, the helper returns the corresponding end pointer in the caller's
original SQL buffer and never returns a tail pointer into the temporary rewrite
buffer. Semicolon and multi-statement tail remapping are unsupported misses. On
any rewrite-path error, it finalizes any partial statement, resets `*ppStmt`,
and retries the original SQL once. Misses and disabled paths call the original
SQL directly.

Enablement gates that query the real Plex FTS table require a Plex-equivalent
binary/context with STAT4, the PMS `icu_root` collation, and the PMS
`collating` FTS tokenizer registered. The generic static CLI cannot satisfy
those Plex-table gates. The committed `tests/plex_fts_stat4_eqp_repro_test.sh`
is a synthetic version-bump signal against the workflow-produced `cli/sqlite3`
STAT4/FTS path. It requires `ENABLE_STAT4` and FTS3/FTS4 and asserts the
original `tag_type`-first plan flips to an FTS-first `unlikely()` plan.

## Plex GUID LIKE NULL Guard Rewrite

The Plex GUID LIKE rewrite is opt-in:
`SQLITE3_DISABLE_PLEX_GUID_LIKE_REWRITE=0` enables it; unset, literal `1`, and
every other value disable it. It is independent of
`SQLITE3_DISABLE_PLEX_FTS_REWRITE` and `SQLITE3_DISABLE_AUTOPRAGMA`.

The matcher compares raw `zSql` bytes and accepts only this complete statement:

```sql
SELECT mt.`id` FROM metadata_items mt WHERE (mt.`guid` LIKE :1) LIMIT :2
```

Case, whitespace, quoting, aliases, bind tokens, parentheses, projection,
clauses, semicolons, embedded NULs, and multi-statement tails must match
exactly. Every other shape prepares the original SQL.

On a match, the helper prepares:

```sql
SELECT mt.`id` FROM metadata_items mt WHERE :1 IS NOT NULL AND (mt.`guid` LIKE :1) LIMIT :2
```

The repeated named `:1` token retains one bind slot, so bind count and names
remain `:1` and `:2`. The projection and `LIMIT` stay unchanged. The prepared
rewrite must expose two binds and one result column; mismatch, allocation
failure, rewritten-prepare failure, or tail mismatch finalizes any rewritten
statement and prepares the original SQL. Successful preparation maps `pzTail`
to the end of the caller's original SQL buffer.

For a NULL pattern, `LIKE` is NULL and cannot satisfy `WHERE`; the added guard
allows SQLite to stop before scanning. For a non-NULL pattern, the guard is
true and the original ICU `LIKE` expression determines every match with its
existing Unicode case folding, BLOB handling, and collation behavior. The
non-NULL plan remains a covering-index scan because the ICU-overloaded
`like()` disqualifies SQLite's LIKE range optimization. This family does not
add a range predicate or an index seek.

## Plex Taggings Membership Rewrite

The Plex taggings rewrite is opt-out and default-on:
`SQLITE3_DISABLE_PLEX_TAGGINGS_REWRITE=1` disables it; unset, literal `0`, and
every other value enable it. It is independent of
`SQLITE3_DISABLE_PLEX_FTS_REWRITE` and `SQLITE3_DISABLE_AUTOPRAGMA`.

The matcher accepts one `tags.id=<integer>` predicate in the same syntactic
SELECT block as canonical `taggings.metadata_item_id=metadata_items.id` and
`taggings.tag_id=tags.id` joins. It rejects bound, named, string, duplicate, or
ambiguous tag ids, existing taggings-membership conjuncts, semicolon tails,
non-Plex basenames, and FTS shape 09 containing `fts4_metadata_titles_icu`.
Every matched prepare probes `sqlite_master` for
`idx_dshadow_taggings_tag_id_metadata_item_id`; absence fails open with
`reason=index_missing`, sampled process-wide for this mode on the first and
every 1024th occurrence.

On a match, the helper appends
`AND metadata_items.id IN (SELECT metadata_item_id FROM taggings WHERE tag_id=<N>)`
after the validated `tags.id=<N>` predicate and preserves all other SQL bytes.
Build failure, rewritten-prepare failure, tail mismatch, or index-probe failure
prepares the original SQL.

## Plex On-Deck Rewrite

The Plex On-Deck rewrite is opt-in:
`SQLITE3_DISABLE_PLEX_ONDECK_REWRITE=0` enables it; unset, literal `1`, and
every other value disable it. It is independent of
`SQLITE3_DISABLE_PLEX_FTS_REWRITE` and `SQLITE3_DISABLE_AUTOPRAGMA`.
Both Variant A id-list matching and Variant B threshold matching run whenever
the base On-Deck rewrite is enabled.

The matcher consumes raw `zSql` and is exact-shape for the Plex On-Deck
statement and accepts exactly one selector after `library_section_id`. Variant
A accepts a nonempty `grandparents.id IN (...)` list of inlined decimal
integers. Variant B accepts one unsigned decimal literal after `viewed_at >`;
binds, signs, decimals, expressions, overflow beyond signed 64-bit range, and
an id-list-plus-threshold cross-product fail open. `library_section_id` and
`account_id` accept an inlined decimal integer or a positional `?`/canonical
`?N` in both variants. Each of those two scalar slots is parsed sequentially
with SQLite numbering: bare `?` takes the next maximum index, explicit `?N` may
create holes or be reused, and the rewrite copies each captured token verbatim,
so bare `?` remains anonymous and explicit `?N` retains its name. Only those two
scalar slots can hold a variable. Named variables,
id-list variables, and noncanonical or out-of-limit indices fail open. The
prepared rewrite's bind count is the two-slot maximum, verified against
`sqlite3_bind_parameter_count` after prepare (`bind_count_mismatch` fails open).
The matcher also rejects slot drift, missing required settings joins, left
joins, projection drift, semicolon tails, and non-Plex basenames. Every matched
prepare probes `sqlite_master` for
`idx_dshadow_metadata_item_views_account_grandparent_guid`; absence fails open
with `reason=index_missing`, sampled process-wide for this mode on the first and
every 1024th occurrence.

An exact-head miss is silent. After the exact head matches, parse drift emits
`reason=capture_miss mode=ondeck` with first-failure `sub_reason` from
`section`, `selector`, `id_list`, `threshold`, `post_id`, `account`, `tail`, or
`trailing`. The recognized `true` plus per-GUID selector form instead
emits `reason=out_of_scope sub_reason=ondeck_per_guid`; any recognizer mismatch
falls back to `capture_miss sub_reason=selector`. Both supported selector arms
use the capture diagnostic path whenever the base On-Deck rewrite is enabled.
Capture and out-of-scope records use process-global first/new/every-1024th
sampling keyed by lexer-normalized structural shape while retaining full,
uncapped source SQL on emitted records.

On a match, the helper emits a ranked subquery with planner-selectable inner
join order, strips the vendor `INDEXED BY` hints by replacing the whole
statement, and copies either
the Variant A `grandparents.id IN (...)` predicate or the Variant B
`metadata_item_views.viewed_at > <literal>` predicate into the inner `WHERE`.
It returns one row per `grandparents.id` with a deterministic `row_number()`
tie-breaker ordered by `metadata_item_views.viewed_at`,
`metadata_item_views.id`, `grandparentsSettings.id`, and
`metadata_item_settings.id` descending. Result column 4 uses the unary-plus
expression `+viewed_at AS "max(viewed_at)"`, so its name remains
`max(viewed_at)` while declared type, table, and origin metadata remain NULL
like the vendor aggregate. The prepared rewrite must expose
the template's seven result columns. Build failure, rewritten-prepare failure,
tail mismatch, bind-count mismatch, column-count mismatch, or index-probe failure
finalizes any rewritten statement and prepares the original SQL.

The deterministic tie-break resolves SQLite's undefined bare-column
representative for `max()`/`GROUP BY` ties; it does not introduce the potential
divergence. A tied Plex group can expose different row values, including NULL
`originally_available_at`, and an Emby group can expose different nullable
dashboard values than the vendor statement happens to choose. The rewrites
deliberately add no NULL hardening because consumers must already tolerate any
representative the vendor query can return. Measured ties are rare: one real
library had 11 tied On-Deck groups, 6 with a divergent representative.

## Emby FTS Search Rewrite

`src/emby_fts_rewrite.c` is compiled into both shared-library variants. The
FTS search rewrite is opt-out and default-on: literal
`SQLITE3_DISABLE_EMBY_FTS_REWRITE=1` disables the rewrite; unset, literal `0`,
and every other value enable it. The env result is cached once per process.

Runtime eligibility requires the main database filename to have exact basename
`library.db`. This excludes Plex, Jellyfin, in-memory, temporary, and
non-target database handles even when the env gate is enabled.

The matcher validates a single statement, exactly one `fts_search9 MATCH`
parameter site, no embedded NUL in the scanned SQL, and the current Emby
membership-materialization byte shape with numeric list slots. It rejects
duplicate anchors, semicolons, multi-statement inputs, over-cap slots,
non-numeric slots, and SQL drift.

On a match, the helper performs one atomic rewrite:

- Wrap the MATCH right-hand side with `dshadow_emby_fts_rewrite(...)`.
- Replace the inline membership `IN` branches with correlated `EXISTS` arms.

The scalar function registers per connection when no same-name function already
exists. It is deterministic, direct-only, and guarded by an ownership canary.
The scalar rewrites only flat uppercase `OR` chains of quoted trailing-prefix
atoms into `AND`; unsupported values, NULL, non-text values, and parser doubt
return the original value unchanged.

Misses and disabled paths call the downstream Plex rewrite helper unchanged.
Scalar collision, authorizer denial, ownership mismatch, allocation failure,
rewrite-build failure, rewritten-prepare failure, and tail mismatch all fail
open to the original SQL. Effective rewrites log the shared hybrid
first/every-1024th/new `rewrite_applied` schema with
`target=emby mode=fts+membership`; `reason=scalar_unavailable`,
`reason=build_failed`, `reason=rewritten_prepare_failed`, and
`reason=tail_mismatch` paths log
`event=rewrite_skipped target=emby reason=... mode=fts+membership`. L1/L2 list
mismatches log `event=slot_mismatch target=emby slot=L1_L2` while still
applying the validated rewrite.

## Emby Fan-Out Membership Rewrites

The Emby fan-out rewrite family is opt-out and default-on:
`SQLITE3_DISABLE_EMBY_FANOUT_REWRITE=1` disables it; unset, literal `0`, and
every other value enable it. It covers RES-A/RES-D complex resume,
resume-simple, Similar-items, Browse-by-name, Favorites-first, People, Studios,
Type-29, and links-search fan-out statements. It is independent of
`SQLITE3_DISABLE_EMBY_FTS_REWRITE` and
`SQLITE3_DISABLE_AUTOPRAGMA`.

The dispatcher first requires a `WithAncestors` byte pre-gate, then validates a
single statement before trying the family matchers. The rewrites are
projection-agnostic membership replacements: `A.Id IN <membership CTE>` becomes
production-style correlated `EXISTS` arms, while projection, grouping,
ordering, collation text, `RANDOM()`, and LIMIT stay unchanged. Browse keeps
`ORDER BY A.SortName collate NATURALSORT ASC` byte-exact.
Complex resume emits the RES-A ancestor `EXISTS` splice and the RES-D implied
watched/progress conjunct in one candidate. Resume-simple and Similar-items emit
the ancestor `EXISTS` splice.

All fan-out families fail open. Applied logs use the shared
first/every-1024th/new schema with `target=emby mode=fanout+<family>`.
Capture-gated fan-out structural drift logs the shared full raw-source record with
`event=rewrite_skipped target=emby reason=capture_miss mode=fanout+...`, a
first-failure `sub_reason`, length, and correlation, then prepares the original
SQL.

## Emby Dashboard Latest Rewrites

The dashboard rewrites are opt-in: literal
`SQLITE3_DISABLE_EMBY_DASHBOARD_REWRITE=0` enables both; unset, literal `1`, and
every other value disable both. They are independent of FTS, FANOUT, and
AUTOPRAGMA. Both reject bare, numbered, and named bind tokens across the whole
statement before readiness probes.

Both matchers accept exactly one ancestor CTE grammar: the list form
`AncestorId in (<integers>) )select` or the scalar form
`AncestorId=<integer> )select`. The scalar form has one closing parenthesis;
the list form retains its list-closing plus CTE-closing pair. Mixed, duplicate,
bound, nonnumeric, or mismatched anchor pairs fail open. A positively detected
bind emits `reason=out_of_scope sub_reason=bind`; a parsed limit outside
12/16/20 emits `reason=out_of_scope sub_reason=limit_unsupported`. Limit parse
failure remains `reason=capture_miss sub_reason=limit`. Generated SQL renders
either validated source form as `AncestorId IN (<captured slot>)`.

`emby_match_episodes_latest` first requires exact Type=8. It emits mode
`dashboard+episodes_latest` and K1: `keys(gk)` enumerates ancestor-visible,
unplayed groups, while the existing picked/ranked tail and
`idx_dshadow_emby_latest_gk_dc` readiness contract remain. The readiness gate
is advisory: absence or probe failure falls open, while an unexpected same-name
index shape is a performance-only risk because K1 contains no `INDEXED BY`.
It rejects `over`, `LastWatchedEpisodes`, series-browse ordering/collation, and
unsafe projections before copying the validated projection verbatim into the
owned outer SELECT.

`emby_match_movies_latest` first requires exact Type=5 and the guarded played
tail. Before the capture boundary it silently rejects `over` and
`LastWatchedEpisodes`, matching the Episodes guard posture. It captures the
outer projection structurally instead of requiring one fixed column list:
empty or over-cap spans, `WithAncestors`, parentheses, aggregate/window
identifiers, and bare `*` fail open; accepted projection bytes are copied
verbatim into the owned outer SELECT. It emits mode
`dashboard+movies_latest` and C2 using
`idx_dshadow_emby_latest_movies_dcn_puk` plus
`idx_dshadow_emby_latest_movies_puk_dc_cover`; it has no native-index
dependency. The anti-join keeps the maximum non-NULL `DateCreated` per
`PresentationUniqueKey`, with the lower `Id` as the deterministic tie-break for
equal dates; NULL wins only when every eligible date in the group is NULL. This
deliberately replaces the vendor query's unspecified bare-column representative,
so projected row identity and ordering can differ for multi-row groups. A
no-guard statement is matcher-non-applying: Type=5 passes, the
guarded tail misses, `capture_miss` logs when observability is enabled, and the
original statement prepares.

Sibling Type traffic is an unlogged clean miss, so neither family produces a
cross-family capture miss. Missing indexes, probe errors, build/allocation
failure, candidate prepare failure, and tail mismatch fail open to original
SQL. Missing-index records use process-global first/every-1024th counters per
dashboard mode; `db=` identifies the emitted exemplar rather than every handle.
Probe errors remain unsuppressed. The two exact Type=5 indexes are required only to
prepare C2; DateCreated storage class is not an enablement condition because C2
uses only native comparison and ordering with no cast, arithmetic, or coercion.
