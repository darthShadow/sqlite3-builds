# Repository Architecture

## Purpose

This repository builds and ships tuned SQLite artifacts for Linux media-server
containers.

It produces:

- A static SQLite CLI binary.
- A generic `libsqlite3.so` shared library for Emby.
- A Plex-specific `libsqlite3.so` shared library linked to Plex-style ICU 69.
- Release archives and SHA-256 manifests for build outputs.
- LSIO Docker mod images for Plex and Emby SQLite replacement.
- Host script support for planned-downtime database maintenance and optional
  post-start Plex database optimize triggering.

The repository is centered on replacing bundled SQLite libraries while keeping
the application-owned database semantics intact. Runtime tuning lives in the
library through SQLite's auto-extension mechanism, so it works even when the
host application loads SQLite by handle rather than by symbol interposition.

## Layout

| Path | Role |
|---|---|
| `build/Build.sh` | Container-internal build driver for CLI and library targets. |
| `build/build_static_sqlite.sh` | Local wrapper that builds Docker images and extracts artifacts into `release/`. |
| `build/base_image_ref.sh` | Computes the content-hash GHCR reference for the generic Ubuntu build base. |
| `docker-cli/Dockerfile` | Alpine-based static CLI build image. |
| `docker-build-base/Dockerfile` | Generic Ubuntu 18.04 build-base image with gcc-13, g++-13, and pinned Kitware CMake. |
| `docker-build-base/ubuntu-toolchain-r-test.asc` | Vendored `ubuntu-toolchain-r/test` signing key used by the generic build-base image. |
| `docker-library/Dockerfile` | Shared-library build image: dynamic `BASE_IMAGE` for the generic variant and LSIO Alpine/musl for the Plex variant. |
| `docs/extending.md` | Cold-start runbook for adding runtime versions, compatibility groups, pool sites, and releases. |
| `docs/baked-pins-schema.md` | Current schema v3 `baked-pins.txt` contract, validator reject list, and runtime keying rules. |
| `docs/architecture.md` | Repository architecture index and overview with links to focused architecture documents. |
| `docs/architecture/build.md` | Build inputs, build pipeline, and compile profile architecture. |
| `docs/architecture/rewrite-engine.md` | PRAGMA injection, runtime optimize, and Plex/Emby rewrite architecture. |
| `docs/architecture/observability.md` | Observability layer, slow-query tracker, and slow-query verification architecture. |
| `docs/architecture/smoke-tests.md` | Smoke-test coverage for runtime hooks, rewrites, ICU, and first-init flows. |
| `docs/architecture/lsio-mods.md` | LSIO Docker mod staging and runtime replacement architecture. |
| `docs/architecture/maintenance.md` | Planned-downtime Plex and Emby maintenance architecture. |
| `src/auto_extension.c` | Built into both library variants; owns open-time auto-extension registration, trace-mask setup, target filtering, and the TLS seam into runtime optimize. |
| `src/runtime_optimize.c` | Built into both library variants; owns runtime optimize state, cadence, eligibility, and hook helpers. |
| `src/auto_extension_internal.h` | Shared internal seam header between `src/auto_extension.c` and `src/runtime_optimize.c`. |
| `src/observability.c` | Built into both library variants; owns SQLite wrappers, observability state, record formatting, and prepare-chain entry points. |
| `src/observability.h` | Private cross-translation-unit seam for shared observability functions. |
| `src/rewrite_modes.h` | Compile-time catalogue for signed rewrite-mode identity, wire display metadata, positional logger labels, and index-missing eligibility. |
| `src/slow_query_tracker.c` | Hidden observability satellite for PROFILE-based slow-query logging and bounded per-template stats. |
| `src/fts_lex.c` | Built into both library variants; owns the shared FTS rewrite SQL token scanner. |
| `src/fts_lex.h` | Private shared lexer header consumed by the Plex and Emby FTS rewrite wrappers. |
| `src/plex_fts_rewrite.c` | Built into both library variants; owns the Plex FTS prefix-tag rewrite, opt-in GUID-LIKE NULL guard, default-on taggings-membership rewrite, and opt-in On-Deck id-list/threshold rewrites. |
| `src/plex_fts_rewrite.h` | Private prepare-wrapper seam header between the Emby helper and Plex rewrite helper. |
| `src/emby_fts_rewrite.c` | Built into both library variants; owns the default-on Emby FTS scalar and fan-out rewrites plus opt-in dashboard Latest rewrites. |
| `src/emby_fts_rewrite.h` | Private prepare-wrapper seam header between `src/observability.c` and `src/emby_fts_rewrite.c`. |
| `tests/auto_extension_smoke.c` | Runtime smoke for filter, kill switch, read-only skip, and emitted PRAGMAs. |
| `tests/runtime_optimize_smoke.c` | Runtime smoke for close and inline runtime optimize target gating, STAT1/STAT4 refresh, kill switches, skip gates, cadence, close semantics, and shutdown/reinit. |
| `tests/slow_query_smoke.c` | Runtime smoke for the slow-query tracker threshold parser, kill switches, LRU bound, truncation, and stats dump. |
| `tests/stmt_trace_smoke.c` | Runtime smoke for the STMT trace env, sampling, correlation, SQL-cap, and shared STMT/PROFILE registration contracts. |
| `tests/config_after_dlopen_smoke.c` | Runtime smoke proving startup-only config remains legal after library load and before first open. |
| `tests/shutdown_reinit_smoke.c` | Runtime smoke proving lazy auto-extension registration survives `sqlite3_shutdown()` and later reopen. |
| `tests/icu_smoke.c` | Runtime smoke for Plex ICU collation registration and comparator use. |
| `tests/plex_fts_rewrite_smoke.c` | Runtime smoke for Plex FTS, GUID-LIKE, taggings-membership, both On-Deck selectors, observability sampling/dedup, env/index gates, fail-open paths, and row parity. |
| `tests/emby_fts_rewrite_smoke.c` | Runtime smoke and direct canary for Emby FTS, fan-out, dashboard projection/scalar forms, observability sampling/dedup, fail-open gates, fixtures, and row parity. |
| `tests/emby_fts_rewrite_prepare_bench.c` | Advisory prepare-cost bench for default-on-select, enabled-nontarget-select, enabled-emby-miss, enabled-emby-large-miss, enabled-all-large-miss, enabled-emby-match, and enabled-emby-exec-miss Emby rewrite paths. |
| `tests/fixtures/emby-fts-rewrite/` | Raw Emby search statement fixtures and expected prepare-wrapper output for smoke and manual host-gate checks. |
| `tests/fixtures/plex-fts-rewrite/` | Raw Plex On-Deck statement fixtures and expected prepare-wrapper output for smoke checks. |
| `tests/render_lsio_mod_baked_pins_test.sh` | Unit tests for `tools/lsio-mod/render-lsio-mod-baked-pins.sh`: schema v3 metadata, detector, artifact, pre, Plex pool-site, unsupported rows, and malformed-input rejection. |
| `tests/cont_init_fragments_test.sh` | Static and unit checks for LSIO mod runtime fragments, phase-script shebangs, no custom env-var surface, and Plex ICU read-only posture. |
| `tests/sqlite_build_workflow_mod_only_test.sh` | Static workflow check for the `preflight` / `build-cli` / `build-generic` / `build-plex` job split and producer-owned smokes, minimal release assets, rendered release-notes wiring, and split `mod-build` / `mod-publish` jobs. |
| `tests/check_obs_counts.sh` | Pre-build lint: counts `SQLITE_CONFIG_` and `SQLITE_DBCONFIG_` decode entries in `src/observability.c` against `build/expected-sqlite-*-count.txt`. |
| `tests/check_pin_alignment.sh` | Pre-build lint: asserts mimalloc VERSION + URL + SHA512 alignment, forbids retired scalar pin keys, checks group-owned ICU source defaults and SORTERREF/PMASZ compile/runtime alignment, enforces the 14-row mode catalogue, four-mode index eligibility, exact producer map, escaped raw-literal rejection, runbook-row and ABI-staging contracts, six `Load version pins` steps, workflow concurrency, the GHCR registry build-cache contract (`type=gha` forbidden; `CACHE_EXPORT_ENABLED`-gated event-ref-only exports), and all 19 project `COPY` lines after the ICU and mimalloc dependency layers. |
| `tests/alloc_latency_bench.c` | Advisory `sqlite3_malloc` / `sqlite3_free` microbench compiled and run in library images without failing the build. |
| `tests/runtime_optimize_close_bench.c` | Advisory runtime optimize hook microbench for close-adjacent inline exits, compiled and run for the generic library variant on every build without failing the build. |
| `build/libsqlite3-version-script.ld` | Library-only linker version script: pinned public `sqlite3` API exports from `sqlite3.h` plus project-required extras, then `local: *;`. |
| `tools/lsio-mod/render-lsio-mod-baked-pins.sh` | Host-runnable renderer for per-mod `baked-pins.txt` runtime SHA data. |
| `tools/lsio-mod/stage-lsio-mod.sh` | Local and CI staging helper that assembles an ephemeral LSIO mod Docker context under `mktemp -d`. |
| `tools/ci/render-release-notes.sh` | Renders the oldest-first non-merge commit-subject section appended to the CalVer release body. |
| `tools/ci/emby-fts-rewrite-dump.c` | Manual host-gate utility that prepares an Emby fixture through the wrapper and writes `sqlite3_sql(stmt)` output; it has no CI consumer. |
| `lsio-mods/` | Source-of-truth Plex and Emby Docker mod roots, shared runtime fragments, and parent README. |
| `lsio-mods/shared/cont-init-fragments/plex-pool-patch.sh` | Args-only shared Plex pool-patch core staged into the Plex mod. |
| `scripts/optimize_media_servers.sh` | Planned-downtime maintenance helper for Plex and Emby databases, including container stop/start ownership and optional post-start Plex optimize API triggering. |
| `.github/workflows/base.yml` | Reusable workflow that resolves, builds, smokes, and publishes the generic build-base image by content hash. |
| `.github/workflows/sqlite-build.yml` | CI build, smoke, artifact upload, release archive, and SHA manifest workflow. |
| `.dockerignore` | Tight Docker context allowlist for build, Dockerfile, source, script, and test inputs. |

## Documents

- [Build](architecture/build.md) - Build inputs, build pipeline, and compile profile.
- [Rewrite Engine](architecture/rewrite-engine.md) - PRAGMA injection, runtime optimize, and Plex/Emby rewrite behavior.
- [Observability](architecture/observability.md) - Observability layer and slow-query tracker behavior.
- [Smoke Tests](architecture/smoke-tests.md) - Runtime, ICU, and first-init smoke-test coverage.
- [LSIO Mods](architecture/lsio-mods.md) - Docker mod staging and runtime replacement architecture.
- [Maintenance](architecture/maintenance.md) - Planned-downtime Plex and Emby maintenance flow.

## Artifact Set

| Artifact | Source path | Output shape | Consumer |
|---|---|---|---|
| Static CLI | `docker-cli/Dockerfile` + `build/Build.sh cli` | `sqlite3`, `sqlite3_orig`, CLI archive | Host-side diagnostics and maintenance |
| Generic library | `docker-library/Dockerfile` + `build/Build.sh library` | `libsqlite3.so`, library archive | Emby |
| Plex library | `docker-library/Dockerfile` + `LIBRARY_VARIANT=plex` | `libsqlite3.so`, Plex library archive | Plex |
| `SHA256SUMS` | Release job | Archive SHA plus extracted `.so` SHA for library archives | Release users and mod-build verification |
| Plex LSIO mod image | `lsio-mods/plex` staged by `tools/lsio-mod/stage-lsio-mod.sh` | GHCR image `linuxserver-mod-sqlite3-plex:<tag>` | LSIO Plex containers |
| Emby LSIO mod image | `lsio-mods/emby` staged by `tools/lsio-mod/stage-lsio-mod.sh` | GHCR image `linuxserver-mod-sqlite3-emby:<tag>` | LSIO Emby containers |

## Runtime Targets

| Target | SQLite binding | Library file | Variant | State in this cycle |
|---|---|---|---|---|
| Plex | Native process using bundled `libsqlite3.so` | `/usr/lib/plexmediaserver/lib/libsqlite3.so` | `plex` | Active |
| Emby | .NET P/Invoke into bundled SQLite | Selected artifact-row `target_path` (current rows use `/app/emby/lib/libsqlite3.so.3.49.2`) | `generic` | Active |
| JF | .NET SQLitePCLRaw name lookup | `/usr/lib/jellyfin/bin/libe_sqlite3.so` | `generic` | Unsupported / out of scope |

See §Out of Scope.

## Library Variants

Two shared-library variants are built from the same source tree.

| Variant | Build selector | Added source or linkage | Runtime purpose |
|---|---|---|---|
| `generic` | default `LIBRARY_VARIANT=generic` | Amalgamation plus the shared library source set: `src/auto_extension.c`, `src/runtime_optimize.c`, `src/observability.c`, `src/slow_query_tracker.c`, `src/fts_lex.c`, `src/plex_fts_rewrite.c`, `src/emby_fts_rewrite.c`, and private seam headers | Emby |
| `plex` | `LIBRARY_VARIANT=plex` | Generic source plus SQLite `ext/icu/icu.c`, `SQLITE_ENABLE_ICU`, and Plex-style ICU libs | Plex |

The intentional variant delta is ICU support for Plex.

Both library variants also link mimalloc v3.3.2 by link-time interposition;
the static CLI remains on the platform allocator.

Everything else is shared: the SQLite amalgamation pin, `src/auto_extension.c`,
`src/runtime_optimize.c`, `src/observability.c`, `src/slow_query_tracker.c`,
`src/fts_lex.c`, `src/plex_fts_rewrite.c`, `src/emby_fts_rewrite.c`, seam
headers `src/auto_extension_internal.h`, `src/fts_lex.h`,
`src/observability.h`, `src/rewrite_modes.h`, `src/plex_fts_rewrite.h`, and
`src/emby_fts_rewrite.h`, feature families,
tuning defaults, high-capacity limits, shared-cache omission, and page-cache
overflow stat posture.

Plex needs a separate variant because its runtime uses ICU 69 libraries built
with a `plex` library suffix. Those files expose suffixed C symbols and Plex
also depends on them directly. The `plex` variant links against:

```text
libicuucplex.so.69
libicui18nplex.so.69
libicudataplex.so.69
```

The Plex variant's `src/auto_extension.c` exports a no-op
`sqlite3_enable_shared_cache(int)` returning `SQLITE_OK` because PMS's bundled
`libpython27.so` dynamically links that symbol at load time. The stub does
NOT touch `sqlite3GlobalConfig.sharedCacheEnabled`, and
`SQLITE_OMIT_SHARED_CACHE` keeps the shared-cache consumer compiled out, so
process-level `sqlite3_enable_shared_cache` calls, the
`SQLITE_OPEN_SHAREDCACHE` open flag, and the `cache=shared` URI parameter
are all inert -- connections stay private regardless of caller.

Plex `icu.c` is compiled with `-DU_HAVE_LIB_SUFFIX=1` and
`-DU_LIB_SUFFIX_C_NAME=_plex`. Those defines select ICU's suffix-aware public
names so ICU calls resolve to the `_69_plex`-suffixed symbols exported by the
suffix-built ICU libraries.

No supported LSIO target in this repository requires Plex-renamed ICU except
Plex. Emby uses the generic library. JF deployment is out of scope until a
current design and validation plan land.

## Hard Constraints

Plex ICU:

- Plex's renamed ICU 69 files are runtime dependencies.
- LSIO mod code must not replace, delete, move, rename, or overwrite
  `libicu*plex.so.69`; it may only read and verify those files.
- The Plex variant may link to those files but only replaces `libsqlite3.so`.
- The Plex variant links `-licuucplex -licui18nplex -licudataplex` inside
  `-Wl,--push-state,--no-as-needed` and `-Wl,--pop-state` so modern binutils
  default `--as-needed` does not drop the ICU `DT_NEEDED` entries.
- The Plex variant carries `-Wl,-z,defs` so unresolved ICU symbols fail at link
  time rather than at runtime.

Variant gating:

- `LIBRARY_VARIANT` defaults to `generic`.
- `LIBRARY_VARIANT=plex` requires the SQLite full source URL and SHA.
- The Plex variant is the only build path that downloads SQLite full source.
- The generic variant does not link ICU.

SHA pins:

- SQLite amalgamation pins drive CLI and both library variants.
- SQLite full-source pins drive only the Plex variant.
- ICU pins drive only the Plex variant.
- Workflow, local wrapper, Dockerfiles, and `Build.sh` must stay aligned.

Release artifacts:

- Release archive names must match workflow release output.
- Library archives must have extracted-`.so` SHA entries in `SHA256SUMS`.
- LSIO mod runtime verification uses `baked-pins.txt`, not release archive
  basename rules.

Plex target path:

- Plex library replacement target is exactly
  `/usr/lib/plexmediaserver/lib/libsqlite3.so`.
- Plex ICU files in the same directory are not deploy targets.

LSIO tool surface:

- Perform no runtime archive download or extraction.
- Common phases use `awk`, `chmod`, `chown`, `cp`, `grep`, `mkdir`, `mktemp`,
  `mv`, `rm`, `sed`, `sha256sum`, `stat`, `tr`, and `uname`.
- Plex pool patch additionally uses `dd`, `od`, and `printf`.
- Do not depend on `curl`, `tar`, `gunzip`, Python, `jq`, package managers, or
  network access at runtime.

Auto-extension:

- Tuning must never block an application database open.
- The kill switch must remain literal `SQLITE3_DISABLE_AUTOPRAGMA=1`.
- Read-only opens must stay quiet.
- Maintenance must set the kill switch.

Observability:

- Initialize, config, db-config, and open observability wrappers must chain to
  the hidden real SQLite implementation and return its result unchanged.
- Prepare wrappers must chain through the Emby helper, then the Plex helper,
  then the hidden real SQLite implementation. Disabled, non-target,
  nonmatching, drift, and failure paths must prepare the original SQL unchanged;
  enabled Emby matches intentionally prepare the scalar-plus-membership,
  fan-out, or dashboard Latest rewrite; enabled Plex matches intentionally
  prepare the `unlikely(tag_type=<value>)`, taggings-membership conjunct, or
  On-Deck ranked-subquery rewrite. Rewrite helpers log only after the rewritten
  statement is effectively returned or after a target-shape rewrite-path
  failure falls back. Applied records combine a per-connection/per-mode first
  and every 1024th sampler with a bounded per-connection first-seen-`corr` set;
  emitted labels are `sample=first`, `sample=periodic`, and `sample=new`, and
  every emitted record carries the full bounded SQL fields unless their text is
  disabled. Capture misses allocate and log the full uncapped source SQL on the
  low-volume miss path with first-failure `sub_reason`, length, and correlation
  fields; allocation or record-size failure falls back to a bounded diagnostic
  without affecting prepare behavior.
- Observability logging must not inject config, db-config, open-flag,
  filename, handle, or return-code behavior.
- Trace registration failure must log and continue; it must never fail
  `sqlite3_open*`.
- `obs_logf` must emit each bounded record with one `fwrite()` under the stderr
  stream lock. Missing-index logs use process-global per-mode first/every-1024th
  sampling; index probe errors are not deduplicated.
- Observability log emission is independent of target filtering, read-only
  filtering, and `SQLITE3_DISABLE_AUTOPRAGMA`.
- The observability kill switch must remain literal
  `SQLITE3_DISABLE_OBSERVABILITY=1`.
- CLI builds must not carry the observability wrapper layer.
- CLI builds must not carry runtime optimize.

M-series invariants:

- M7: Source-level amalgamation patching via a checked-in unified-diff `.patch`
  file applied with `patch -p1` after unzip; SQLite version bumps that change
  any of the nine wrapped API target signatures or the internal runtime optimize
  hook hunks cause `patch` to reject hunks and fail the build with patch's
  native error message. `build/sqlite-amalgamation.patch` is the single locus of
  amalgamation modifications.

Baked pin manifest:

- `baked-pins.txt` uses schema v3 with metadata row
  `meta|3|release_tag|<tag>|generated_at|<iso8601-utc>`.
- It contains `detect`, `artifact`, `pre`, `pool-site`, and `unsupported` rows.
- `artifact` rows use
  `artifacts/<arch>/<compat_group>/libsqlite3.so`.
- Per-version detector selection chooses the server id before target-specific
  verification or mutation.
- Missing image digest evidence for a supported runtime image is a CI failure.
- Runtime SHA mismatch has no bypass environment variable.
- LSIO mod images are the authoritative SQLite replacement artifact for a
  release tag.

## Out of Scope

JF deployment is unsupported until a current design and validation plan land. JF
maintenance is absent; adding it requires design and validation before use.
