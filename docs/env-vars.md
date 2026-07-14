# Runtime Environment Variables

These runtime knobs are exact-literal controls, not truthy booleans. Unset and
unrecognized values follow the source defaults below.

Shared-library controls are read at library load or first use depending on the
owner: observability and slow-query controls are cached by constructors with
`pthread_once` fallback; rewrite controls are cached once per process on
first prepare use; `SQLITE3_DISABLE_AUTOPRAGMA` is read by each auto-extension
callback; runtime optimize enablement and cadence values are read on runtime
optimize hook paths. Shared-library gates fail open: disabled, bad, unset, or
rewrite-failure cases do not break application queries.

Maintenance controls are read by `scripts/optimize_media_servers.sh` after its
Bash config is sourced. They are outside the shared-library query path.

## Core

| Env var | Type (kill-switch / numeric tunable) | Purpose | Default | Polarity (opt-in / opt-out / tunable) | Enable-or-value semantics | Other values (incl. unset) behavior | Build variant scope (generic / Plex-ICU / Emby) | Notes |
|---|---|---|---|---|---|---|---|---|
| `SQLITE3_DISABLE_AUTOPRAGMA` | kill-switch | Disable per-connection PRAGMA injection. | PRAGMA injection enabled. | opt-out | Literal `1` disables the callback. | Unset, literal `0`, and any other value keep the callback active. | generic and Plex-ICU shared libraries; current deployments are Emby and Plex. | Also disables runtime optimize hooks. The maintenance script exports `SQLITE3_DISABLE_AUTOPRAGMA=1`. |
| `SQLITE3_DISABLE_RUNTIME_OPTIMIZE` | kill-switch | Enable close and inline runtime optimize. | Runtime optimize disabled. | opt-in | Literal `0` enables runtime optimize. | Unset, literal `1`, and any other value disable runtime optimize. | generic and Plex-ICU shared libraries; current deployments are Emby and Plex. | Requires `SQLITE3_DISABLE_AUTOPRAGMA` not to be literal `1`; inline optimize also requires a fast read below `SQLITE3_SLOW_QUERY_THRESHOLD_MS`. |
| `SQLITE3_RUNTIME_OPTIMIZE_LIMITED_SECONDS` | numeric tunable | Successful LIMITED optimize cadence per target path. | `1800` seconds. | tunable | Positive unsigned decimal seconds within the source bound. | Unset, empty, `0`, signed, non-decimal, trailing junk, or overflow values use `1800`. | generic and Plex-ICU shared libraries; current deployments are Emby and Plex. | Applies only when runtime optimize is enabled. |
| `SQLITE3_RUNTIME_OPTIMIZE_FULL_SECONDS` | numeric tunable | Successful FULL optimize cadence per target path. | `86400` seconds. | tunable | Positive unsigned decimal seconds within the source bound. | Unset, empty, `0`, signed, non-decimal, trailing junk, or overflow values use `86400`. | generic and Plex-ICU shared libraries; current deployments are Emby and Plex. | Applies only when runtime optimize is enabled. A successful FULL pass also satisfies the LIMITED cadence. |
| `SQLITE3_DISABLE_OBSERVABILITY` | kill-switch | Disable wrapper log emission and per-connection trace registration. | Observability enabled. | opt-out | Literal `1` disables observability. | Unset, literal `0`, and any other value keep observability enabled. | generic and Plex-ICU shared libraries. | Master gate for statement trace, slow-query tracking, runtime optimize logs, and rewrite logs. |
| `SQLITE3_DISABLE_REWRITE_APPLIED_SQL` | kill-switch | Omit SQL text from sampled `rewrite_applied` records. | Source and rewritten SQL text included on `sample=first`, `sample=periodic`, and `sample=new` records. | opt-out | Literal `1` omits `source_sql` and rewritten `sql` while retaining counters and correlation keys. | Unset, literal `0`, and any other value keep both SQL text fields. | generic and Plex-ICU shared libraries. | Cached once per process. Subordinate to `SQLITE3_DISABLE_OBSERVABILITY=1`; it does not gate rewrites. |
| `SQLITE3_DISABLE_STMT_TRACE` | kill-switch | Enable `SQLITE_TRACE_STMT` logging. | Statement trace disabled. | opt-in | Literal `0` enables STMT trace when observability is enabled. | Unset, empty, literal `1`, and any other value keep STMT trace disabled. | generic and Plex-ICU shared libraries. | Does not disable PROFILE-side slow-query tracking. |
| `SQLITE3_DISABLE_STMT_TRACE_SAMPLING` | kill-switch | Disable sampling for explicitly enabled STMT trace. | Enabled STMT trace logs the first callback, each bounded first-seen `corr` shape, and every 1024th callback per connection. | opt-out | Literal `1` logs every enabled STMT callback. | Unset, literal `0`, and any other value retain `sample=first`, `sample=periodic`, and `sample=new` hybrid sampling. | generic and Plex-ICU shared libraries. | Cached once per process. Never enables STMT trace; `SQLITE3_DISABLE_STMT_TRACE=0` and observability enabled are still required. |
| `SQLITE3_DISABLE_SLOW_QUERY` | kill-switch | Disable PROFILE-side slow-query tracking. | Slow-query tracking enabled. | opt-out | Literal `1` disables slow-query tracking. | Unset, literal `0`, and any other value keep slow-query tracking enabled. | generic and Plex-ICU shared libraries. | Subordinate to `SQLITE3_DISABLE_OBSERVABILITY=1`; independent of `SQLITE3_DISABLE_STMT_TRACE`. |
| `SQLITE3_SLOW_QUERY_THRESHOLD_MS` | numeric tunable | Base slow-query log threshold and inline runtime-optimize fast-read cutoff. | `500` ms. | tunable | Unsigned decimal milliseconds; `0` is accepted. | Unset, empty, signed, non-decimal, trailing junk, or overflow values use `500`. | generic and Plex-ICU shared libraries. | `0` logs all nonnegative PROFILE samples and prevents inline runtime optimize because no observed elapsed time is below the threshold. |
| `SQLITE3_DISABLE_SLOW_QUERY_EXPANDED_SQL` | kill-switch | Disable expanded-SQL detail lines for slow queries. | Expanded slow-query SQL detail enabled. | opt-out | Literal `1` disables expanded SQL detail. | Unset, literal `0`, and any other value keep expanded detail enabled. | generic and Plex-ICU shared libraries. | Still requires observability and base slow-query tracking to be enabled. |
| `SQLITE3_SLOW_QUERY_EXPANDED_SQL_THRESHOLD_MS` | numeric tunable | Slow-query expanded-SQL detail threshold. | `2500` ms. | tunable | Unsigned decimal milliseconds; `0` is accepted, then clamped up to the effective base slow-query threshold. | Unset, empty, signed, non-decimal, trailing junk, or overflow values use `2500`, then clamp up to the effective base threshold when needed. | generic and Plex-ICU shared libraries. | Expanded detail only emits for target database paths and elapsed times at or above the effective expanded threshold. |

## Plex

| Env var | Type (kill-switch / numeric tunable) | Purpose | Default | Polarity (opt-in / opt-out / tunable) | Enable-or-value semantics | Other values (incl. unset) behavior | Build variant scope (generic / Plex-ICU / Emby) | Notes |
|---|---|---|---|---|---|---|---|---|
| `SQLITE3_DISABLE_PLEX_FTS_REWRITE` | kill-switch | Control the Plex FTS prefix-tag rewrite. | Plex FTS rewrite enabled in the Plex-ICU build. | opt-out | Literal `1` disables the rewrite. | Unset, literal `0`, and any other value enable the rewrite. | Plex-ICU only. | Cached once per process. Non-Plex-ICU builds pass through original SQL. |
| `SQLITE3_DISABLE_PLEX_GUID_LIKE_REWRITE` | kill-switch | Control the Plex GUID LIKE NULL-pattern guard rewrite. | GUID LIKE rewrite disabled. | opt-in | Literal `0` enables the rewrite. | Unset, literal `1`, and any other value disable the rewrite. | Plex-ICU only. | Cached once per process. Independent of FTS and `SQLITE3_DISABLE_AUTOPRAGMA`; build or verification failure prepares original SQL. |
| `SQLITE3_DISABLE_PLEX_TAGGINGS_REWRITE` | kill-switch | Control the Plex taggings-membership rewrite. | Taggings rewrite enabled in the Plex-ICU build. | opt-out | Literal `1` disables the rewrite. | Unset, literal `0`, and any other value enable the rewrite. | Plex-ICU only. | Cached once per process. Independent of FTS and `SQLITE3_DISABLE_AUTOPRAGMA`; missing taggings index or rewrite failure prepares original SQL. |
| `SQLITE3_DISABLE_PLEX_ONDECK_REWRITE` | kill-switch | Control the Plex On-Deck ranked-subquery rewrite. | On-Deck rewrite disabled. | opt-in | Literal `0` enables both the id-list and `viewed_at > <literal>` selector arms. | Unset, literal `1`, and any other value disable the rewrite. | Plex-ICU only. | Cached once per process. Independent of FTS and `SQLITE3_DISABLE_AUTOPRAGMA`; missing g2 index or rewrite failure prepares original SQL. |

## Emby

| Env var | Type (kill-switch / numeric tunable) | Purpose | Default | Polarity (opt-in / opt-out / tunable) | Enable-or-value semantics | Other values (incl. unset) behavior | Build variant scope (generic / Plex-ICU / Emby) | Notes |
|---|---|---|---|---|---|---|---|---|
| `SQLITE3_DISABLE_EMBY_FTS_REWRITE` | kill-switch | Control the Emby FTS scalar plus membership rewrite. | Emby FTS rewrite enabled. | opt-out | Literal `1` disables the rewrite. | Unset, literal `0`, and any other value enable the rewrite. | Both variants; runtime-gated by target DB basename. | Cached once per process. Independent of `SQLITE3_DISABLE_AUTOPRAGMA`. |
| `SQLITE3_DISABLE_EMBY_FANOUT_REWRITE` | kill-switch | Control Emby fan-out rewrites: Browse-by-name, Favorites-first, RES-A/RES-D, resume-simple, Similar-items, People, Studios, Type-29, and links-search. | Fan-out rewrites enabled. | opt-out | Literal `1` disables the fan-out rewrites. | Unset, literal `0`, and any other value enable the fan-out rewrites. | Both variants; runtime-gated by target DB basename. | Cached once per process. Independent of FTS, dashboard, and `SQLITE3_DISABLE_AUTOPRAGMA`. |
| `SQLITE3_DISABLE_EMBY_DASHBOARD_REWRITE` | kill-switch | Control the Emby Episodes-Latest and movies-Latest dashboard rewrites. | Dashboard rewrites disabled. | opt-in | Literal `0` enables both dashboard rewrites. | Unset, literal `1`, and any other value disable both dashboard rewrites. | Both variants; runtime-gated by target DB basename. | Cached once per process. Independent of FTS, fan-out, and `SQLITE3_DISABLE_AUTOPRAGMA`; modes are `dashboard+episodes_latest` and `dashboard+movies_latest`. |

Rewrite names follow `SQLITE3_DISABLE_<ENGINE>_<PURPOSE>_REWRITE`.

## Maintenance

| Env var | Type (kill-switch / numeric tunable) | Purpose | Default | Polarity (opt-in / opt-out / tunable) | Enable-or-value semantics | Other values (incl. unset) behavior | Build variant scope (generic / Plex-ICU / Emby) | Notes |
|---|---|---|---|---|---|---|---|---|
| `PLEX_OPTIMIZE_API` | kill-switch | Control the optional post-start Plex `PUT /library/optimize?async=1` trigger. | `0` in the config template; unset behaves as `0`. | opt-in | Literal `1` enables the trigger after successful Plex maintenance and restart. | Unset, literal `0`, and any other value skip the API trigger. | Maintenance script. | Sourced from the Bash config file (`scripts/optimize_media_servers.conf` by default), not via `getenv`; trigger failures warn and affect the script exit only when the API path was enabled and no Plex instance reached an accepted, already-running, or completed state. |
| `PLEX_PROCESS_BLOB_DB` | kill-switch | Control the optional Plex blob database rebuild pass. | `0` in the config template; unset behaves as `0`. | opt-in | Literal `1` enables the blob database rebuild pass. | Unset, literal `0`, and any other value skip the blob database pass. | Maintenance script. | Sourced from the Bash config file (`scripts/optimize_media_servers.conf` by default), not via `getenv`. |
| `STATS_BANDWIDTH_RETAIN_DAYS` | numeric tunable | Retention window for Plex `statistics_bandwidth` deflate on the staged main database. | `90` in the config template. | tunable | Decimal digits are accepted and used as days. | Empty, unset, or any non-digit value skips the bandwidth deflate with a warning. | Maintenance script. | Sourced from the Bash config file (`scripts/optimize_media_servers.conf` by default), not via `getenv`; DELETE or VACUUM failures warn and continue to the post-deflate integrity gate; post-deflate integrity failure aborts before swap. |

The maintenance config also owns instance names, binary paths, and backup paths.
Those are deployment configuration, not shared-library kill-switches or numeric
runtime tunables.

## Non-Env Logging Limits

No environment variable changes capture-source logging, the statement-trace and
rewrite-applied SQL cap, slow-query template cap, expanded-SQL cap, or slow-query
LRU size. `capture_miss` allocates its low-volume record to include the full,
uncapped source SQL; allocation or record-size failure emits a bounded fallback
diagnostic. In source, the fixed limits are: STMT trace plus rewrite-applied
source and rewritten SQL cap `4096` bytes, slow-query template cap `2048` bytes,
expanded-SQL source cap `65536` bytes, and slow-query LRU size `4096` entries.
Formatted records that exceed the inline observability buffer use an allocated
full-record path, so the escaped 65536-byte expanded-SQL field is emitted in
full; allocation or record-size failure emits a bounded fallback.
