# sqlite3-builds invariants (evergreen)

Repo-level architectural and behavioral rules that survive across milestones.
Reference by path from charters; do not paste this content into prompts.

Milestone-specific rules (locked decisions for in-flight workstreams, current
bug states, triage decisions) live in the milestone-scoped sibling files (e.g.,
`docs/invariants/sqlite-maintenance.md`) and are cleaned up after their
milestone lands.

## 1. Build / variant

- Plex variant builds under
  `BASEIMAGE_ALPINE=ghcr.io/linuxserver/baseimage-alpine:3.23`. Generic variant
  is two-step: `BASEIMAGE_UBUNTU` builds the content-addressed GHCR generic
  base, and `docker-library/Dockerfile` consumes dynamic `BASE_IMAGE` for the
  per-run library build. Multi-stage `docker-library/Dockerfile` selects via
  `LIBRARY_VARIANT`.
- `LIBRARY_VARIANT=plex` is limited to the Plex ICU build path.
- SQLite pins must stay aligned across `build/Build.sh`,
  `build/build_static_sqlite.sh`, `.github/workflows/sqlite-build.yml`,
  `docker-cli/Dockerfile`, and `docker-library/Dockerfile`.
- ICU source VERSION/SHA512 fields live in the `icu69` row of
  `pins/library-compat-groups.tsv`. The wrapper resolves them into
  `ICU_SOURCE_VERSION` and `ICU_SOURCE_SHA512`, `docker-library/Dockerfile`
  consumes those values as build args, and the workflow passes the same
  `ICU_SOURCE_*` build args for the Plex library build.
  `tests/check_pin_alignment.sh` forbids retired scalar pin keys and asserts
  the `ICU_SOURCE_*` defaults come from the compatibility-groups TSV, and
  asserts SORTERREF/PMASZ compile defaults match their constructor-102 runtime
  config values.
- Mimalloc v3.3.2 Wire-2 link-time interposition applies to both library
  variants only; CLI stays on the platform allocator. Keep the full
  VERSION + URL + SHA512 pin tuple aligned across `build/Build.sh`,
  `build/build_static_sqlite.sh`, `docker-library/Dockerfile`, and
  `.github/workflows/sqlite-build.yml`; Plex adds `-DMI_LIBC_MUSL=ON` to
  the mimalloc CMake invocation.
- CLI variant excluded from observability; both library variants (Plex +
  generic) carry it.
- `SQLITE_THREADSAFE=2` (Multi-thread) is a `build/Build.sh` compile flag.
  `assert_emby_compile_options` in `tools/ci/lib/assertions.sh` requires
  `THREADSAFE=2`, and `tools/ci/emby-first-init-smoke.sh` invokes that gate so
  any Emby-visible drift fails CI.
- Plex variant build fails if `libsqlite3.so` carries `fcntl64` or any
  `@GLIBC_`-versioned undefined symbol (gate at
  `docker-library/Dockerfile`, conditional on `LIBRARY_VARIANT=plex`).
  Generic variant is glibc-linked and runs a bounded post-link floor gate:
  it must observe at least one `@GLIBC_` reference, and every observed
  reference must be `<= GENERIC_GLIBC_MAX` (`2.27`). This thresholded generic
  gate stays distinct from the Plex zero-GLIBC gate.
- Matrix rows are v2 / v3 / arm64 only; **no x86-64-v4 row** (v4 lib
  SIGILLs in the auto-extension constructor on non-AVX-512 Zen hosts).
  v4-capable amd64 hosts use the v3 artifact.
- `SQLITE_TEMP_STORE=3` (memory) is a `build/Build.sh` compile flag.
  `PRAGMA temp_store=2` is documentation-only under that profile (no-op).
- `SQLITE_DEFAULT_PAGE_SIZE=16384` (16 KiB) is the compile-default
  page-size pin in `build/Build.sh`.
- `SQLITE_DEFAULT_AUTOVACUUM=0` is the compile-default autovacuum pin in
  `build/Build.sh`; `scripts/optimize_media_servers.sh` passes
  `auto_vacuum=NONE` during planned-downtime `VACUUM INTO` rebuilds and gates
  the staged `PRAGMA auto_vacuum` result on `0`.
- Planned-downtime Plex maintenance keeps `PLEX_BINARY` for ICU-sensitive
  rebuild, FTS, `REINDEX`, and staged Plex SQL. `GENERIC_SQLITE_BINARY` is the
  single host generic SQLite setting for Emby maintenance and the Plex main-DB
  STAT4 pass. For configured Plex instances, `main()` derives internal STAT4
  capability state from `GENERIC_SQLITE_BINARY` preflight. The Plex main-DB
  STAT4 pass runs only when that state is `1`. Plex STAT4 preflight or
  per-target failures warn and skip only STAT4; they do not weaken existing hard
  integrity gates.
- `SQLITE_DEFAULT_MMAP_SIZE=34359738368` (32 GiB) is the compile-default
  mmap-size pin in `build/Build.sh`; CI compile-option assertions keep it
  aligned with the runtime auto-PRAGMA target.
- `SQLITE_MAX_MMAP_SIZE=1099511627776` (1 TiB) is the `build/Build.sh`
  compile-time mmap ceiling; `assert_emby_compile_options` in
  `tools/ci/lib/assertions.sh` asserts `MAX_MMAP_SIZE=1099511627776` in CI. The
  runtime `PRAGMA mmap_size=34359738368` (32 GiB) only takes effect because this
  compile ceiling stays above 32 GiB; reducing it below 32 GiB silently caps the
  PRAGMA and breaks the Bundle-1 observability + performance objective. Changes
  to the build pin or the Emby compile-options assertion must update both.
- `SQLITE_DEFAULT_SORTERREF_SIZE=512` is the `build/Build.sh` compile
  default. SQLite's stock default is `0x7fffffff` (disabled).
  Constructor-102 re-asserts
  `sqlite3_config(SQLITE_CONFIG_SORTERREF_SIZE, 512)` in
  `src/auto_extension.c`; `tests/check_pin_alignment.sh` asserts the compile
  default and runtime value stay aligned.
- `SQLITE_SORTER_PMASZ=8192` is the `build/Build.sh` compile default;
  constructor-102 re-asserts `sqlite3_config(SQLITE_CONFIG_PMASZ, 8192)` in
  `src/auto_extension.c`. `tests/check_pin_alignment.sh` asserts the compile
  default and runtime value stay aligned. `assert_emby_compile_options` in
  `tools/ci/lib/assertions.sh`, invoked by
  `tools/ci/emby-first-init-smoke.sh`, keeps the value pinned in CI.

## 2. LSIO mods

- Plex library replacement target: exactly
  `/usr/lib/plexmediaserver/lib/libsqlite3.so`.
- Emby library replacement target comes from the selected `artifact` row
  `target_path`; current supported Emby rows use
  `/app/emby/lib/libsqlite3.so.3.49.2`.
- JF deployment is unsupported until a current design and validation plan land.
- Plex ICU runtime files MUST NOT be replaced, renamed, moved, deleted, or
  overwritten. Mod code may only read and verify `libicu*plex.so.69`.
- baked-pins.txt is the only runtime SHA source consumed by LSIO mod scripts.
- `baked-pins.txt` includes metadata, detector, artifact, runtime baseline,
  Plex pool-site, and unsupported/offline rows.
- `baked-pins.txt` row kinds:
  - `meta|3|release_tag|<tag>|generated_at|<iso8601-utc>` is the single
    metadata row.
  - `detect|1|<mod>|<server_id>|<arch>|<path_role>|<file_path>|<sha256>`
    gives server-owned detector files for per-version selection.
  - `artifact|1|<mod>|<server_id>|<arch>|<compat_group>|<artifact_relpath>|<target_path>|<artifact_sha256>`
    gives each baked `libsqlite3.so` candidate, its group-aware artifact path,
    and its runtime replacement path.
  - `pre|2|<mod>|<server_id>|<arch>|<path_role>|<image_ref>|<image_digest>|<file_path>|<sha256>`
    gives bundled runtime baselines, including Plex ICU siblings.
  - `pool-site|1|plex|<server_id>|<arch>|<binary_path>|<label>|<offset>|<write_seek>|<original_hex>|<patched_hex>`
    gives Plex pool-patch byte contexts.
  - `unsupported|1|<mod>|<server_id>|<arch>|<compat_group>|<reason>` keeps
    all-arch manifests complete when a local/offline render lacks an artifact.
- `artifact_relpath` is exactly
  `artifacts/<arch>/<compat_group>/libsqlite3.so`; published amd64 mod images
  carry both `linux-x86_64-v2` and `linux-x86_64-v3`, while arm64 mod images
  carry `linux-arm64`.
- Runtime mod scripts read no custom env vars. `MOD_INFO` is log-only and
  `uname -m` is the architecture input.
- Phase scripts run as native s6-rc oneshots under
  `/etc/s6-overlay/s6-rc.d/init-mod-sqlite3-*/run` and use
  `#!/usr/bin/with-contenv bash`.
- SQLite mod oneshots are chained after `init-mods` and before
  `init-mods-end`: preflight -> verify -> swap -> config (Emby) or
  poolpatch (Plex). `init-mods-end` depends on the final SQLite oneshot, so
  the chain completes before `init-services` and `svc-*` startup.
- LSIO runtime command surface:
  | Scope | Commands |
  |---|---|
  | Common phases | `awk chmod chown cp grep mkdir mktemp mv rm sed sha256sum stat tr uname` |
  | Plex pool patch | `dd od printf` |
- LSIO runtime has no dependency on `curl`, `tar`, `gunzip`, Python, `jq`,
  package managers, or network access.
- Phase posture: malformed or missing `baked-pins.txt`, baked artifact
  mismatch, and selected baked artifact absence are fatal. Unsupported arch,
  missing target path, runtime Plex ICU mismatch, unknown current target SHA,
  backup failure, temp-copy failure, and pool-patch drift warn and exit 0.
- Phase 03 swap skips an unrecognized current target even when a
  `.bundled.bak` exists; it restores a verified backup only after a failed
  install attempt.
- Pool patch is args-only: callers pass every path, SHA, and site tuple.
  The staged `plex-pool-patch.sh` fragment reads no environment variables and
  carries no SHA constants.
- Pool-patch site tuples are
  `label|offset|write_seek|original_hex|patched_hex`. The writer derives one
  byte from `patched_hex` at `write_seek - offset`, applies it to a same-fs
  temp copy with `dd conv=notrunc`, restores target owner/mode metadata, and
  atomically replaces the target; original and patched contexts remain
  per-binary and per-site coupled.
- Pool-site to pristine detector baseline-SHA agreement is a render/CI gate.
  Runtime pool-site rows bind to pristine detector rows by binary path, not by
  an embedded pool-site baseline SHA.
- Pool patch skips a binary when any site is mixed or unknown. It patches only
  when all sites are original and the binary SHA matches the selected pristine
  detector SHA; it skips when all sites are already patched.
- Plex amd64 and arm64 swap SQLite and patch PMS / Scanner pool size.

## 3. Source

- `build/sqlite-amalgamation.patch` is the M7 invariant: source-level
  amalgamation patching via a checked-in unified diff.
- Nine wrapper-target functions (`sqlite3_initialize`, `sqlite3_config`,
  `sqlite3_db_config`, `sqlite3_open`, `sqlite3_open_v2`,
  `sqlite3_open16`, `sqlite3_prepare`, `sqlite3_prepare_v2`,
  `sqlite3_prepare_v3`) renamed to `_real` with hidden visibility. Build fails
  on rename-drift.
- Runtime optimize is ABI/export neutral. It does not add public SQLite API
  exports, does not alter the version-script public allowlist, and keeps
  `auto_extension_register_for_open`,
  `auto_extension_optimize_before_close`,
  `auto_extension_optimize_after_stmt`,
  `auto_extension_progress_handler_push`, and
  `auto_extension_progress_handler_pop` absent from `.dynsym`. The Plex and
  Emby FTS rewrite helpers `plex_fts_rewrite_prepare` and
  `emby_fts_rewrite_prepare`, plus the shared lexer helpers `fts_lex_init`,
  `fts_lex_token_text_eq`, `fts_lex_next_token`,
  `fts_lex_shape_key`,
  `fts_lex_match_rhs_is_complete`, and
  `fts_rewrite_db_basename_matches`, are also hidden and absent from
  `.dynsym`.
- `sqlite3_enable_shared_cache` no-op stub at `src/auto_extension.c:7-16`
  is load-bearing for the Plex variant. **KEEP byte-for-byte intact.**
- `build/libsqlite3-version-script.ld` is load-bearing for symbol-export
  discipline: exact list from the pinned `sqlite3.h` plus
  `auto_extension_path_is_target`, `auto_extension_sorterref_cfg_rc`,
  `auto_extension_pmasz_cfg_rc`, and `sqlite3_enable_shared_cache`, then
  `local: *;` to deny everything else including `mi_*`. The post-link
  hidden-symbol deny gate in `build/Build.sh:265-288` (including
  `plex_fts_rewrite_prepare`, `emby_fts_rewrite_prepare`, `fts_lex_init`,
  `fts_lex_token_text_eq`, `fts_lex_next_token`,
  `fts_lex_shape_key`, `fts_lex_match_rhs_is_complete`, and
  `fts_rewrite_db_basename_matches`) stays adjacent to the preserved
  `build/Build.sh:255-264`
  `_real` gates as belt-and-braces.
- Initialize/config/db-config/open `SQLITE_API` wrappers in
  `src/observability.c` MUST chain to `*_real` and return its result
  UNMODIFIED. Prepare wrappers in `src/observability.c` MUST call
  `emby_fts_rewrite_prepare`; that helper MUST chain to
  `plex_fts_rewrite_prepare`, then the matching `*_real` prepare
  implementation. Disabled, non-target, nonmatching, drift, and failure paths
  must prepare the original SQL unchanged, while enabled Emby matches
  intentionally prepare scalar-plus-membership, fan-out, or dashboard Latest
  rewrites and enabled Plex matches intentionally prepare the
  `unlikely(tag_type=<value>)`, GUID-LIKE NULL-guard, taggings-membership
  conjunct, or On-Deck ranked-subquery rewrite. Rewrite-success logging is
  emitted only after the rewritten statement is the returned statement.
  Applied records combine
  per-connection/per-mode first-and-every-1024th sampling with a bounded
  per-connection first-seen-`corr` set. Full emitted records use
  `sample=first`, `sample=periodic`, or `sample=new`; a full or unavailable set
  falls back to the periodic sampler. If per-connection sampler state cannot be
  allocated or registered, known-mode `rewrite_applied` and STMT diagnostics
  are skipped; sampler failure never increases output. Capture-gated misses
  and positive `out_of_scope` exclusions use process-global per-reason/per-mode
  first/every-1024th counters plus one bounded process-global first-seen
  structural-shape set. The caller gates and computes the lexer shape; this
  file gains no lexer dependency. Every event, including count 1, is observed
  before schedule selection. `src/rewrite_modes.h` is the only rewrite-mode
  catalogue. Producers pass signed `OBS_MODE_*` ids. Applied, miss, and
  index-missing counters share the `OBS_MODE_COUNT` index space while applied
  counters remain per connection and miss plus index-missing counters remain
  process-global. Invalid numeric mode ids suppress the requested mode-bearing
  record. When observability is enabled, the first invalid id across all
  mode-bearing APIs emits one process-wide record with positional logger label
  `observability` followed by payload
  `event=obs_mode_unregistered mode_id=<signed-decimal> site=<site>`; later
  invalid ids suppress silently. The negative-first applied example contains
  the exact substring
  ` observability event=obs_mode_unregistered mode_id=-1 site=rewrite_applied`.
  Registered index-ineligible ids suppress index-missing records without
  emitting that diagnostic. Emitted miss records
  allocate and carry full uncapped raw source SQL, first-failure `sub_reason`,
  `sample`, process-global `count`, structural `shape`, exact length, and
  full-span correlation; allocation or record-size failure emits a bounded
  fallback without changing prepare behavior. Clean misses and
  fallback-to-original paths do not emit applied records.
- Every `obs_forward_config` / `obs_forward_db_config` case-arm
  preserves the exact arity, types, and argument pass-through to
  `sqlite3_config_real` / `sqlite3_db_config_real`. New SQLite opcodes
  require an explicit case with verified arity; consolidating,
  reordering, or dropping any existing case-arm corrupts the va_list
  dispatch for the affected opcodes. `tests/check_obs_counts.sh` plus
  expected-count files (`build/expected-sqlite-config-count.txt`,
  `build/expected-sqlite-dbconfig-count.txt`) are the drift detector.
- Observability constructor priority: 101.
- Auto-extension callback must NEVER make `sqlite3_open*` fail.
- Runtime optimize hooks in `sqlite3_close`, `sqlite3_close_v2`,
  `sqlite3_step`, `sqlite3_reset`, and `sqlite3_finalize` must NEVER change
  the public return code. Optimize failures are logged/swallowed; native
  close, zombie, busy, step, reset, and finalize semantics stay authoritative.
- Runtime optimize execution logs use `obs_logf` only on the execution path:
  `event=optimize_start` before the tier SQL statement, `event=optimize_done`
  on success, and `event=optimize_failed` on failure. Each line carries `tier`,
  `elapsed_ms`, `stat_rows`, and `since_last_ms`; not-due hot skips emit no
  optimize log.
- Runtime optimize due-but-blocked logs use `obs_logf` only after an enrolled
  connection has passed the not-due hot skip and a pre-reserve guard blocks
  work. The hook atomically re-arms only that connection's `next_due_ns` and
  emits one throttled
  `event=optimize_skipped reason=readonly|open_txn|busy_peer|not_idle_read`
  per 30-second re-arm interval. It must not acquire `g_runtime_optimize_mu` or
  modify per-path success cadence / failure backoff state on this path.
- Runtime optimize internal statements MUST NOT emit `trace_stmt`,
  `slow_query`, or `slow_query_expanded` lines. `runtime_optimize` start, done,
  failed, and skipped logs remain the public optimize telemetry.
- Read-only opens stay quiet for PRAGMA emission; observability is
  independent of read-only state.
- `sqlite3_initialize` log gate:
  `if (outer && rc != SQLITE_OK && !disabled)` -- failure-only.
- Open-wrapper observability annotations of IMPLICIT default flags
  (the wrappers without a caller-supplied flags argument: `sqlite3_open`
  and `sqlite3_open16`) MUST gate the implicit-flags log line on
  `rc == SQLITE_OK` to avoid misleading NOMEM-path logs. Applies to
  both wrappers symmetrically; `sqlite3_open_v2` logs caller-supplied
  flags and is not subject to this gate.
- Word-bounded `bad_signal_re` MUST stay.
- Constructor priorities: 101 = observability cache init (`obs_init` in
  `src/observability.c`); 102 = `SQLITE_CONFIG_LOG` process-wide
  callback registration routed into the observability sink, then the
  `SQLITE_CONFIG_SORTERREF_SIZE` and `SQLITE_CONFIG_PMASZ` runtime
  re-asserts of the `build/Build.sh` compile defaults (no
  `sqlite3_auto_extension` call -- registration is lazy per the open-wrapper
  helper); 103 =
  slow-query tracker `pthread_once` priming + `atexit` registration. The
  101->102->103 order ensures observability env caches, including
  `SQLITE3_DISABLE_OBSERVABILITY` and `SQLITE3_DISABLE_STMT_TRACE`, are
  populated before any open-wrapper-triggered callback fires. Do NOT move
  slow-query registration onto the PROFILE hot path.
- `SLOW_QUERY_SQL_CAP=2048` (tracker key + display) and `OBS_SQL_CAP=4096`
  (observability STMT plus rewrite-applied source/rewritten SQL) are separate
  constants. Capture-miss source SQL instead uses an allocated, uncapped
  full-record path with bounded fallback on allocation or record-size failure.
  The tracker uses its own cap to prevent collision;
  `tests/check_obs_counts.sh` references only `OBS_SQL_CAP`.
- `obs_logf` formats outside the stderr stream lock, allocates a full record
  when formatted output exceeds its inline buffer, and emits one `fwrite()`
  plus one terminal newline per record. Allocation or record-size failure uses
  a bounded fallback. `index_missing` uses process-global per-mode counters for
  Plex taggings/On-Deck and Emby Episodes/movies Latest. Count 1 and every
  1024th occurrence emit; all others suppress across database handles. Probe
  errors stay unsuppressed. Missing-index logging has no clientdata allocation
  dependency. Only those four catalogue rows are index-missing eligible.
- Full `capture_miss` and `out_of_scope` records allocate and format outside the stderr stream
  lock, then emit one `fwrite()` plus one terminal newline; failure to build the
  full record falls back to `obs_logf` and never affects prepare behavior.
- `SLOW_QUERY_MAX_ENTRIES=4096` is the tracker LRU entry cap.
- `auto_extension_pmasz_cfg_rc()` mirrors
  `auto_extension_sorterref_cfg_rc()` in `src/auto_extension.c:28-34`;
  both remain default-visibility C getters consumed by
  `tests/auto_extension_smoke.c:538-567`, linked through `-lsqlite3` by
  `docker-library/Dockerfile:252-254`, and both MUST stay in the version-script
  `global:` allowlist.
- Tracker registration via `sqlite3_trace_v2` MUST stay in the
  per-connection init path (`autopragma_init` in `src/auto_extension.c`)
  that runs BEFORE the connection escapes to application code. Do not move
  to a deferred / lazy / on-first-query path. When STMT trace and
  slow-query PROFILE are both disabled, the trace mask is `0` and
  `sqlite3_trace_v2` is not called.
- Atexit safety: `g_in_atexit` acquire-load MUST be the FIRST executable
  instruction in the PROFILE-side dispatch (in `obs_trace_cb` PROFILE
  branch AND in `slow_query_trace_profile`), BEFORE any `sqlite3_*`
  accessor (`sqlite3_db_handle`, `sqlite3_sql`) or `*x` deref. Reordering
  risks use-after-teardown on PROFILE callbacks arriving during process
  exit.
- Tracker hot path (`slow_query_record_sql`) MUST use
  `slow_query_is_disabled_cached()` (atomic-load only). The
  `slow_query_disabled()` helper (with `pthread_once`) is reserved for
  non-hot test/registration paths; constructor(103) primes `pthread_once`
  at dlopen so the cached load sees the resolved value.
- Slow-query stats dump gating lives outside the record hot path. A
  `slow_query_stats` line requires `count >= 5` and either
  `mean_ns >= threshold_ns` or the overflow-safe total arm
  `sum_ns / 20 >= threshold_ns`. Total-only emissions are capped at the first
  20 entries in the existing descending-total snapshot; mean-qualified entries
  are uncapped. `total_ms`, `mean_ms`, `stddev_ms`, `min_ms`, and `max_ms`
  render with `%.6f` precision. On the no-`__int128` fallback, count retains
  the existing 1048576-observation per-template cap; `sum_ns` and `sum_sq_ns`
  saturate at `UINT64_MAX` rather than wrapping and emit an explicit
  `saturated=` field naming the capped floor. A saturated `sum_ns` always
  qualifies and is not charged to the total-only budget.
- Slow-query stats key identity: full-SQL `memcmp` for templates <=2048
  bytes; 64-bit FNV-1a hash equality for templates >2048 bytes (collision
  probability negligible at <=4096 templates). FNV-1a MUST NEVER be used
  as equality for <=2048-byte templates.
- Slow-query LRU eviction tombstones the evicted bucket slot
  (`SLOW_QUERY_NIL = UINT16_MAX`) and increments `g_slow.tombstones`.
  Bucket rebuild triggers only when `tombstones >= entries_used / 4`
  (25%-rehash gate). New inserts do NOT reuse tombstones; tombstones
  cleaned on rebuild. Steady-state eviction is amortized O(1).

## 4. Runtime

- `SQLITE3_DISABLE_OBSERVABILITY=1` is the master kill switch.
- `SQLITE3_DISABLE_AUTOPRAGMA=1` disables PRAGMA emission (orthogonal to
  the observability kill switch).
- `SQLITE3_DISABLE_STMT_TRACE=0` enables STMT trace registration. Unset or
  any other value disables STMT trace registration; PROFILE-side slow-query
  tracking is unaffected.
- `SQLITE3_DISABLE_SLOW_QUERY=1` disables PROFILE-side slow-query
  tracking; subordinate to `SQLITE3_DISABLE_OBSERVABILITY=1` (master) and
  orthogonal to `SQLITE3_DISABLE_AUTOPRAGMA=1` (PRAGMA emission).
- Runtime optimize is opt-in for eligible target write connections. Literal
  `SQLITE3_DISABLE_RUNTIME_OPTIMIZE=0` enables close and inline runtime
  optimize; unset, literal `1`, and every other value disable runtime
  optimize. Literal `SQLITE3_DISABLE_AUTOPRAGMA=1` disables open-time PRAGMAs
  and runtime optimize.
- Plex FTS rewrite is opt-out in the Plex/ICU build: literal
  `SQLITE3_DISABLE_PLEX_FTS_REWRITE=1` disables; unset, literal `0`, and every
  other value enable. The Plex GUID-LIKE rewrite is opt-in:
  `SQLITE3_DISABLE_PLEX_GUID_LIKE_REWRITE=0` enables; unset, literal `1`, and
  every other value disable. The Plex taggings rewrite is opt-out and
  default-on: `SQLITE3_DISABLE_PLEX_TAGGINGS_REWRITE=1` disables; unset,
  literal `0`, and every other value enable. The Plex On-Deck rewrite is
  opt-in: `SQLITE3_DISABLE_PLEX_ONDECK_REWRITE=0` enables; unset, literal `1`,
  and every other value disable. These Plex rewrites fail open and are
  independent of `SQLITE3_DISABLE_AUTOPRAGMA` and
  `SQLITE3_DISABLE_PLEX_FTS_REWRITE`.
  The On-Deck id-list and threshold arms both run whenever the base On-Deck
  rewrite is enabled.
- Literal `SQLITE3_DISABLE_REWRITE_APPLIED_SQL=1` omits source and rewritten
  SQL text from every emitted applied record; unset, literal `0`, and every
  other value retain it on `sample=first`, `sample=periodic`, and
  `sample=new` records. Literal
  `SQLITE3_DISABLE_STMT_TRACE_SAMPLING=1` logs every enabled STMT callback;
  unset, literal `0`, and every other value retain hybrid first/periodic/new
  sampling. The sampling override never enables STMT trace.
- Emby FTS and fan-out rewrites are opt-out and default-on: literal `1` in
  `SQLITE3_DISABLE_EMBY_FTS_REWRITE` or
  `SQLITE3_DISABLE_EMBY_FANOUT_REWRITE` disables that family; unset, literal
  `0`, and every other value enable. The Emby dashboard rewrite is opt-in:
  `SQLITE3_DISABLE_EMBY_DASHBOARD_REWRITE=0` enables; unset, literal `1`, and
  every other value disable. These Emby rewrites fail open and are independent
  of `SQLITE3_DISABLE_AUTOPRAGMA`.
- Runtime optimize has two successful per-path cadences: LIMITED defaults to
  1800 seconds, sets `PRAGMA main.analysis_limit=0`, and runs
  `PRAGMA main.optimize=0x10002` with a 3-second deadline; FULL defaults to
  86400 seconds, sets `PRAGMA main.analysis_limit=0`, and runs
  `ANALYZE main;` with a 15-second deadline and no trailing optimize. A
  successful FULL pass also satisfies the LIMITED cadence.
- Inline runtime optimize runs only after a provably fast plain read statement:
  `sqlite3_stmt_readonly(self_stmt)` is true, the SQL is not transaction or
  connection control, and the fresh PROFILE elapsed time is below
  `SQLITE3_SLOW_QUERY_THRESHOLD_MS`. Missing statement identity, telemetry-off,
  write, transaction-control, slow, and unobserved inline triggers defer without
  stamping success; close remains the unconditional due-work backstop.
- Runtime optimize pre-reserve `readonly`, `open_txn`, `busy_peer`, and
  `not_idle_read` blocks re-arm only the enrolled connection-local next-due
  cache for 30 seconds. They do not stamp LIMITED/FULL success or set shared
  failure backoff.
- Runtime optimize saves and restores the prior `analysis_limit`,
  caller-visible error state, caller-visible `sqlite3_changes()` /
  `sqlite3_total_changes64()` counters, and the application's progress handler
  around optimize work. Inline optimize also saves and restores `busy_timeout`.
  The close path sets `sqlite3_busy_timeout(db, 1)` and does not restore it
  because the connection is closing. The progress deadline is suspended before
  restoring connection state after an interrupted tier statement.
- `scripts/optimize_media_servers.sh` owns Plex and Emby container stop/start
  during planned-downtime maintenance. `PLEX_OPTIMIZE_API=1` enables the
  inline post-start Plex database optimize trigger only after the per-instance
  start gate passes. The trigger resolves reachability only from
  `docker inspect`, waits for `GET /identity` HTTP 200, reads
  `PlexOnlineToken` from the `/opt/<instance>/...` Preferences.xml path, sends
  only `PUT /library/optimize?async=1` with `X-Plex-Token`, never retries the
  PUT, and tracks completion only through debounced
  `type="general.db.optimize"` absence in `/activities`.
- PMS uses `sqlite3_open` (2-arg) exclusively; implicit flags
  `READWRITE|CREATE`.
- Emby uses `sqlite3_open_v2` exclusively with
  `READWRITE|CREATE|NOMUTEX|PRIVATECACHE`.
- Emby's `PRAGMA locking_mode=EXCLUSIVE` in WAL mode does NOT block
  other-process readers. Any SQLite-related claim MUST qualify
  rollback-journal-mode vs WAL-mode semantics.
- Auto-PRAGMA runtime `mmap_size` is 32 GiB (`34359738368`) for supported
  media-server database connections; compile default and runtime target stay
  aligned at 32 GiB.
- Mimalloc replaces the default glibc/musl allocator for all
  SQLite-internal allocations in both library variants; the CLI continues
  using the platform allocator. `MIMALLOC_ALLOW_THP=1` is the v3.3.2
  default and fits the deploy environment's THP=always + defer+madvise
  posture.

## 5. Workflow

- Matrix `fail-fast: false` in every
  `.github/workflows/sqlite-build.yml` strategy block.
- Smoke-step positive-mode marker assertions opt in with
  `SQLITE3_DISABLE_STMT_TRACE=0`: aggregate count guard
  `sqlite3_open(_v2|16)?` AND `SQLITE_TRACE_STMT`. Do NOT assert
  `sqlite3_initialize` marker emission.
- `release` publishes `sqlite-<tag>-*.tar.gz` and `SHA256SUMS`.
- `release` is tag-gated and depends on `build-cli`, `build-generic`,
  `build-plex`, and `mod-static-tests`.
- Release-note wiring checks out full history with `fetch-depth: 0`, invokes
  `bash tools/ci/render-release-notes.sh "${GITHUB_REF_NAME}" "${RELEASE_NOTES_DELIMITER}"`,
  writes the `changes` output through `printf 'changes<<%s\n'` with delimiter
  `EOF_RELEASE_NOTES_CHANGES`, and places the compatibility content in inline
  `body:`; `body_path:` is forbidden.
- Buildx layer cache uses GHCR `type=registry` only; `type=gha` is forbidden.
  Imports read `<scope>-<event>` and then `<scope>-baseline`. Exports write the
  event ref only and use `continue-on-error: true`. `CACHE_EXPORT_ENABLED`
  enables exports only for canonical `main` pushes
  (`github.ref == 'refs/heads/main'` and
  `github.repository == 'darthshadow/sqlite3-builds'`) and same-repository pull
  requests. `CACHE_EVENT_NAME` maps pull requests to `pr-<number>` and every
  non-pull-request event to `baseline`.
- `docker-library/Dockerfile` keeps all 19 project `COPY` lines after the ICU
  and mimalloc dependency layers, after
  `ENV MIMALLOC_LIB=/opt/mimalloc/lib/libmimalloc.a`.
- Workflow `concurrency` keys pull requests by PR number and other runs by
  `github.ref`, and cancels in-progress runs except `refs/tags/`.
- `mod-build` builds and smokes run-scoped temporary tags only.
- `mod-publish` is tag-gated, depends on `release` and `mod-build`, and is
  the only job that pushes stable mod tags or multi-arch manifests.
- Native arm64 mod lanes use `ubuntu-24.04-arm`; no native arm64 lane uses
  `--platform`.

## 6. Scope

- User's "build scripts" scope = `build/` directory ONLY. Does NOT
  include `scripts/` (maintenance helper only) or `tools/` (amalgamation
  patch + version script + expected-count helpers + LSIO mod tooling).
- `AGENTS.md` at project root is a symlink to `CLAUDE.md`. Do not
  modify unless explicitly asked.

## 7. Performance non-regression

- New mechanisms in the observability / optimization layers
  (`src/observability.c`, `src/auto_extension.c`, tracker /
  profiler modules) MUST NOT regress runtime performance to a
  user-noticeable degree.
- **Threshold**: nanosecond-scale per-query overhead is acceptable;
  microsecond-scale is marginal and must be justified by the gain it
  enables; millisecond-scale overhead is disqualifying.
- Hot-path cycle accounting is part of every design proposal in these
  modules; numbers without ranges or contention-path costs are
  insufficient.
- Defensive guards that add per-query branches must justify their cycle
  cost rather than be added reflexively.
- Concurrency benchmarks under N>=4 threads with KPTI on are gating
  acceptance criteria, not advisory; uncontended single-thread numbers
  do not validate the contended path.
- Kill-switch fast-paths (early-return before any work) preserve this
  invariant under disabled workloads and should be the default for any
  new feature with a runtime kill switch.

## 8. Versioning

- Release identity uses CalVer tags matching
  `^[0-9]{4}\.[0-9]{2}\.[0-9]{2}-r[1-9][0-9]*$`; no `v` prefix; always
  include `-rN`.
- LSIO mod tags mirror repository CalVer release tags one-to-one.
- No LSIO mod publishes `latest`.
- `baked-pins.txt` metadata row shape is
  `meta|3|release_tag|<tag>|generated_at|<iso8601-utc>`.
- `baked-pins.txt` contains no managed-window fields and no `post` rows.
- Plex runtime ICU `pre` rows live in `baked-pins.txt`.
- `tests/check_pin_alignment.sh` enforces mimalloc VERSION + URL + SHA512
  tuple alignment, forbids retired scalar pin keys, and asserts
  `ICU_SOURCE_VERSION` / `ICU_SOURCE_SHA512` defaults come from
  `pins/library-compat-groups.tsv` for `icu69`. It also asserts SORTERREF and
  PMASZ compile defaults match their constructor runtime config values.
