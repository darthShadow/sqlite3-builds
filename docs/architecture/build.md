# Build

Part of the [Repository Architecture index](../architecture.md).

## Build Inputs

The source of truth for SQLite, CMake, and mimalloc build pins is
`pins/versions.env`; the local wrapper sources it and the workflow loads it into
`$GITHUB_ENV`.
Runtime support images live in `pins/runtime-support.tsv`. Library
compatibility groups and Plex ICU source pins live in
`pins/library-compat-groups.tsv`.

For extension procedures, use `docs/extending.md`. For the schema v3 manifest
field contract and fail-closed validator rules, use
`docs/baked-pins-schema.md`.

| Input | Current value |
|---|---|
| SQLite version | `3530300` |
| SQLite release year | `2026` |
| Amalgamation URL | `https://www.sqlite.org/2026/sqlite-amalgamation-3530300.zip` |
| Amalgamation SHA3-256 | `d45c688a8cb23f68611a894a756a12d7eb6ab6e9e2468ca70adbeab3808b5ab9` |
| Full source URL | `https://www.sqlite.org/2026/sqlite-src-3530300.zip` |
| Full source SHA3-256 | `2daecfa16e3b19e058dc2e2cb717b80ade361e0315aa5376c3619f66aa81e181` |

The current Plex library compatibility group pins ICU 69.1:

| Input | Current value |
|---|---|
| ICU tarball | `https://github.com/unicode-org/icu/releases/download/release-69-1/icu4c-69_1-src.tgz` |
| ICU SHA-512 | `d4aeb781715144ea6e3c6b98df5bbe0490bfa3175221a1d667f3e6851b7bd4a638fa4a37d4a921ccb31f02b5d15a6dded9464d98051964a86f7b1cde0ff0aab7` |
| ICU build prefix | `/opt/icu-69-plex` |
| ICU configure suffix | `--with-library-suffix=plex` |

The generic build-base image pins Kitware CMake:

| Input | Current value |
|---|---|
| CMake version | `3.31.12` |
| CMake Linux x86_64 SHA-256 | `0dc2e9a6860f06bf10bd8fadc03e35d9eeb4df46e33763a7e480e987758f385c` |
| CMake Linux aarch64 SHA-256 | `83f8fd91d2038a56556e1400390fcfe42f79602940c494f6c6f1cdae7f9e7f40` |
| Build scope | Content-addressed generic Ubuntu build base only |

The build-image, toolchain key, and generic ABI-floor pins are:

| Input | Current value |
|---|---|
| Generic Ubuntu base | `ubuntu:18.04@sha256:152dc042452c496007f07ca9127571cb9c29697f42acbfad72324b2bb2e43c98` |
| Ubuntu toolchain PPA key fingerprint | `60C317803A41BA51845E371A1E9377A2BA9EF27F` |
| LSIO Alpine base | `ghcr.io/linuxserver/baseimage-alpine:3.23` |
| Generic glibc max | `2.27` |

`BASEIMAGE_UBUNTU` and `CMAKE_*` build the content-addressed GHCR base image.
They are not per-run generic library build-stage inputs. The generic library
build consumes `BASE_IMAGE`, resolved to a GHCR digest by CI or to a local tag by
the wrapper fallback.

The library build also pins mimalloc v3.3.2:

| Input | Current value |
|---|---|
| Mimalloc version | `3.3.2` |
| Mimalloc URL | `https://github.com/microsoft/mimalloc/archive/refs/tags/v3.3.2.tar.gz` |
| Mimalloc SHA-512 | `226bbd51eca36d7737ce5e2edba7e0a3beeca448462a861bcbfb6726a0994bc077b4c684d7ff8b0805d71bf770e00df14f10ed598256ee54a154d8cc08e6a5c1` |
| Mimalloc install prefix | `/opt/mimalloc` |

The same SQLite pins must stay aligned across:

- `build/build_static_sqlite.sh`.
- `.github/workflows/sqlite-build.yml`.
- Docker build args.
- `docker-cli/Dockerfile`.
- `docker-library/Dockerfile`.
- `build/Build.sh`.

The mimalloc VERSION + URL + SHA512 tuple must stay aligned across
`build/Build.sh`, `build/build_static_sqlite.sh`, `docker-library/Dockerfile`,
and `.github/workflows/sqlite-build.yml`; `tests/check_pin_alignment.sh`
fails on any drift in the full tuple.

`CMAKE_VERSION`, `CMAKE_SHA256_*`, `BASEIMAGE_UBUNTU`, and the vendored
toolchain key inputs must stay aligned across `pins/versions.env`,
`docker-build-base/Dockerfile`, `.github/workflows/base.yml`,
`build/base_image_ref.sh`, and `tests/check_pin_alignment.sh`.

Plex ICU source VERSION and SHA512 fields are group-owned by
`pins/library-compat-groups.tsv`; the workflow passes those values as Plex
library `ICU_SOURCE_*` build args.

## Build Pipeline

### Local Wrapper

`build/build_static_sqlite.sh` is the local orchestration entrypoint.

It checks Docker access, builds the CLI and library images, passes SQLite pins
and architecture settings, extracts outputs into `release/cli`,
`release/library`, or `release/library-plex`, runs local inspection, and removes
the temporary images.

`LIBRARY_VARIANT=plex` switches the local library output directory to
`release/library-plex` and passes the extra SQLite source inputs needed for
`ext/icu/icu.c`.

For library builds, it also forwards the mimalloc VERSION + URL + SHA512 pins
into `docker-library/Dockerfile`.

For generic library builds, it resolves the content-hash base reference through
`build/base_image_ref.sh`. When `BASE_IMAGE` is not provided by the caller, the
wrapper builds `docker-build-base/` locally with Buildx `--load`, tags that local
image with the computed reference, and passes it to `docker-library/Dockerfile`
as `BASE_IMAGE`.

### Build Driver

`build/Build.sh` accepts a target and source URL.

```text
./Build.sh cli <amalgamation-url> [compressor]
./Build.sh library <amalgamation-url>
```

It also accepts `--target=cli` and `--target=library`.

For both targets, it downloads and verifies the SQLite amalgamation when
absent, extracts it, creates `dist/`, and compiles with GCC, `-O3`,
architecture tuning, LTO, and shared compile flags.

For `cli`, it builds from `shell.c sqlite3.c`, links statically, preserves
`sqlite3_orig`, strips `sqlite3`, and optionally compresses the stripped binary.

For `library`, it builds `sqlite3.c` plus `/app/auto_extension.c`,
`/app/runtime_optimize.c`, `/app/observability.c`,
`/app/slow_query_tracker.c`, `/app/fts_lex.c`,
`/app/plex_fts_rewrite.c`, and `/app/emby_fts_rewrite.c`, with
`/app/auto_extension_internal.h`, `/app/observability.h`,
`/app/rewrite_modes.h`, `/app/fts_lex.h`, `/app/plex_fts_rewrite.h`, and
`/app/emby_fts_rewrite.h` as private headers, links a shared object, applies
library-only feature defaults, and writes `dist/libsqlite3.so`.

For `library`, the link line prepends `MIMALLOC_OBJ`
(`/opt/mimalloc/lib/mimalloc.o`) and `MIMALLOC_LIB`
(`/opt/mimalloc/lib/libmimalloc.a`) so both shared-library variants interpose
mimalloc at link time. The CLI target uses the platform allocator.

The same library-only link adds
`-Wl,--version-script=/app/build/libsqlite3-version-script.ld`. The script
enumerates the pinned public `sqlite3` API exports from `sqlite3.h` plus
`auto_extension_path_is_target`, `auto_extension_sorterref_cfg_rc`,
`auto_extension_pmasz_cfg_rc`, and `sqlite3_enable_shared_cache`, then ends
with `local: *;`.

For `library`, it also applies `build/sqlite-amalgamation.patch` to the
extracted SQLite amalgamation with `patch -p1` before compile. The patch
renames the nine wrapped SQLite API definitions to hidden `*_real` symbols:

- `sqlite3_initialize`
- `sqlite3_config`
- `sqlite3_db_config`
- `sqlite3_open`
- `sqlite3_open_v2`
- `sqlite3_open16`
- `sqlite3_prepare`
- `sqlite3_prepare_v2`
- `sqlite3_prepare_v3`

SQLite version bumps that change any target definition shape cause `patch` to
reject hunks and fail the build with patch's native error message. The library
patch also inserts internal runtime optimize hooks into `sqlite3_step`,
`sqlite3_reset`, `sqlite3_finalize`, and the immediate-close path. Hook
movement on SQLite version bumps is therefore a patch rejection. The library
build then compiles `sqlite3.c`, `/app/auto_extension.c`,
`/app/runtime_optimize.c`, `/app/observability.c`, and
`/app/slow_query_tracker.c`, `/app/fts_lex.c`,
`/app/plex_fts_rewrite.c`, and `/app/emby_fts_rewrite.c`;
`/app/auto_extension_internal.h` is the auto-extension/runtime-optimize seam
header, `/app/observability.h` is the shared observability seam header,
`/app/rewrite_modes.h` is the header-only signed rewrite-mode identity,
display, logger, and eligibility catalogue,
`/app/fts_lex.h` is the shared FTS rewrite lexer header,
`/app/plex_fts_rewrite.h` is the Plex prepare-wrapper/rewrite seam header, and
`/app/emby_fts_rewrite.h` is the Emby prepare-wrapper/rewrite seam header.
`/app/auto_extension.c` keeps open-time registration, trace-mask setup,
target filtering, and the TLS seam, and reaches runtime optimize through
`runtime_optimize_seed_path`; `/app/runtime_optimize.c` owns the runtime
optimize logic. The CLI target does not run this patch and does not include
observability, runtime optimize, or either FTS rewrite wrapper.

`build/Build.sh:168-174` supplies `-I.` only. `/app/observability.c` includes
`/app/observability.h` with a quoted include, and that header includes
`rewrite_modes.h` with a quoted include, so lookup starts in `/app`.
`rewrite_modes.h` creates no translation unit or exported symbol; `Build.sh`
and its `sources=` list remain unchanged.

After the library compile, `build/Build.sh` checks every
`SQLITE_CONFIG_*` and `SQLITE_DBCONFIG_*` define in the pinned `sqlite3.h`
against the decode tables in `/app/observability.c`. Missing table entries are
build failures.

Adjacent to the preserved `sqlite3_*_real` and lazy-helper symbol gates,
post-link checks fail on leaked `mi_*` or malloc-family exports and require
local mimalloc implementation symbols to be present.

For the Plex variant, it additionally requires `SQLITE_SRC_URL`, verifies the
SQLite full source archive, adds `ext/icu/icu.c`, enables ICU, and links with
Plex-suffixed ICU libraries.

### CLI Docker Image

`docker-cli/Dockerfile` is Alpine based.

It installs the static-build toolchain, copies `build/Build.sh`, and invokes
the CLI target with the amalgamation URL, compressor, architecture baseline,
and SHA pin. The CLI image is only responsible for the static CLI artifact.

### Library Docker Image

`docker-library/Dockerfile` selects `base-generic` from dynamic `BASE_IMAGE` for
the generic variant and `base-plex` from the LSIO Alpine/musl base for the Plex
variant. In CI, `BASE_IMAGE` is a resolved
`ghcr.io/darthshadow/sqlite3-build-base@sha256:<digest>` reference. In fork pull
request or local fallback paths, `BASE_IMAGE` can be the local image tag loaded
by Buildx.

It copies `build/Build.sh`,
`src/auto_extension.c`, `src/runtime_optimize.c`,
`src/auto_extension_internal.h`, `src/observability.c`, `src/observability.h`,
`src/rewrite_modes.h`, `src/slow_query_tracker.c`, `src/fts_lex.c`, `src/fts_lex.h`,
`src/plex_fts_rewrite.c`, `src/plex_fts_rewrite.h`,
`src/emby_fts_rewrite.c`, `src/emby_fts_rewrite.h`, `tests/`,
`build/sqlite-amalgamation.patch`, `build/expected-sqlite-config-count.txt`,
and `build/expected-sqlite-dbconfig-count.txt` into `/app`, with the build
files under `/app/build/`.

The generic branch starts from `BASE_IMAGE`, then builds mimalloc and SQLite for
the current run. The per-run generic branch performs no `apt` work, no PPA
registration, and no CMake installation. Those steps belong to the generic
build-base image.

The same Dockerfile builds mimalloc v3.3.2 in a dedicated layer, installs it
under `/opt/mimalloc`, writes `/opt/mimalloc/SHA512`, and exports
`MIMALLOC_OBJ` plus `MIMALLOC_LIB` into `Build.sh`. The Plex branch adds
`-DMI_LIBC_MUSL=ON`; the generic branch does not. The ICU and mimalloc
dependency layers precede every project `COPY`, so source and test edits do not
invalidate those dependency layers.

For the generic variant, it builds the shared library, requires at least one
`@GLIBC_`-versioned undefined symbol, rejects any observed `@GLIBC_` reference
above `GENERIC_GLIBC_MAX` (`2.27`), and runs the config-after-dlopen,
shutdown/reinit, auto-extension, runtime optimize, Plex rewrite, and Emby
rewrite smokes plus the Plex and Emby FTS prepare benches. The benches
report advisory p50/p95 threshold verdicts for enabled non-target passthrough
and target-DB miss prepare costs; timing verdicts do not fail the image build.
The generic variant must show the Plex rewrite helper is inert even when the
Plex env gates are enabled.

For the Plex variant, it also builds ICU 69.1 under `/opt/icu-69-plex`, exposes
`PLEX_ICU_INCLUDE` and `PLEX_ICU_LIB`, checks ICU soname and symbol shape with
`ldd` and `nm`, runs `icu_smoke`, and runs the config-after-dlopen,
shutdown/reinit, auto-extension, runtime optimize, Plex rewrite, and Emby
rewrite smokes plus the Plex and Emby FTS prepare benches with the Plex ICU
path available. The benches report advisory p50/p95 threshold verdicts for
enabled non-target passthrough and target-DB miss prepare costs; timing verdicts
do not fail the image build. The Plex branch builds under Alpine/musl so the
resulting library does not carry glibc-versioned symbols such as `fcntl64`,
which Plex's `libgcompat` shim does not provide.

For the Plex variant, the build stage fails if the produced `libsqlite3.so`
carries `fcntl64` or any other glibc-versioned symbol. This zero-GLIBC gate is
distinct from the generic variant's bounded glibc floor gate.

### Generic Base Image Workflow

`.github/workflows/base.yml` is a reusable workflow that resolves the generic
build base for callers. It loads pins, calls `build/base_image_ref.sh`, and uses
the script's content-hash tag:

```text
ghcr.io/darthshadow/sqlite3-build-base:src-<hash>
```

The hash covers `docker-build-base/Dockerfile`,
`docker-build-base/ubuntu-toolchain-r-test.asc`, `BASEIMAGE_UBUNTU`,
`CMAKE_VERSION`, `CMAKE_SHA256_X86_64`, and `CMAKE_SHA256_AARCH64`. Changing any
of those inputs changes the tag and triggers a build-if-missing path on the next
trusted CI run.

The workflow first inspects the content-hash tag with `docker buildx imagetools`.
If the manifest already exists, it emits the resolved
`ghcr.io/darthshadow/sqlite3-build-base@sha256:<digest>` reference. If the tag
is missing on a trusted push, native `ubuntu-24.04` amd64 and
`ubuntu-24.04-arm` arm64 lanes build the base without QEMU, push each
architecture image by digest to GHCR, smoke the built image, and use
`imagetools create` to publish the multi-arch manifest. The manifest is then
inspected and emitted as the resolved digest output.

Base-image smoke checks cover gcc, g++, cc, c++ major version 13, the pinned
CMake version, glibc 2.27, the direct `ubuntu-toolchain-r/test` bionic source,
OpenSSL SHA3 support, image labels, and the expected linux/amd64 plus
linux/arm64 manifest platforms.

Fork pull requests and other events without package write authority do not push
to GHCR. When the content-hash base is missing, the caller builds
`docker-build-base/` locally with `docker buildx build --load`, uses the computed
local reference as `BASE_IMAGE`, and continues in the same artifact job;
`build-generic` and `build-plex` each carry the fallback step.

### Workflow Matrix

`.github/workflows/sqlite-build.yml` builds on pushes to `main`,
digit-prefixed tag pushes matching `[0-9]*`, pull requests to `main`, and
manual dispatch. The workflow jobs are `base`, `preflight`, `build-cli`,
`build-generic`, `build-plex`, `mod-static-tests`, `release`, `mod-build`, and
`mod-publish`. The `base` job resolves the generic `BASE_IMAGE`. `preflight`
loads the pins and compatibility-group artifact stems, then runs pin alignment
and Build.sh prechecks once. The three artifact jobs wait for `preflight`;
`build-generic` and `build-plex` also wait for `base`. A tag-gated `release`
waits for all three artifact jobs and the `mod-static-tests` suite.

A workflow-level `concurrency` group keys pull requests by PR number and other
runs by `github.ref`, and cancels superseded in-progress runs. Cancellation is
disabled for `refs/tags/`, so release runs are not cancelled. The three artifact
jobs hold `packages: write` for the GHCR build-cache package.

Each artifact job uses the same native architecture matrix:

| Runner | `MARCH` | Suffix |
|---|---|---|
| `ubuntu-24.04` | `x86-64-v2` | `linux-x86_64-v2` |
| `ubuntu-24.04` | `x86-64-v3` | `linux-x86_64-v3` |
| `ubuntu-24.04-arm` | `armv8-a` | `linux-arm64` |

Each artifact job builds one Docker image family for each native matrix row.
Per-run builds use Buildx with `--load` and import two public registry
refs per family/native target: `<scope>-<event-name>` first and
`<scope>-baseline` second. Every non-pull-request event, including tag pushes
and manual dispatch, resolves `<event-name>` to `baseline`; pull requests
resolve it to `pr-<number>`, so a baseline import never reads a PR-writable ref.
On canonical `main` pushes and same-repository pull requests,
separate best-effort export steps write only the event ref with `mode=max`,
`oci-mediatypes=true`, `image-manifest=true`, and
`compression=zstd`. Fork pull requests skip GHCR login and every export step,
and import anonymously from the public cache package, including the warm
baseline ref. The CLI Dockerfile's
`RUN --mount=type=cache,target=/ccache` mount is local to that build and is
not exported through the registry cache.

x86-64-v4 is intentionally absent. GitHub `ubuntu-24.04` runners commonly use
AMD Zen 3 hosts without AVX-512, and a v4 library SIGILLs in the
auto-extension constructor. v4-capable amd64 hosts receive the v3 artifact.

Each build output is extracted with the pinned Docker-extract action and
uploaded as a workflow artifact. `build-cli` builds, checks, and uploads the
CLI; `build-generic` builds, smokes, and uploads the generic library; and
`build-plex` does the same for the Plex library. Each artifact job sets
`fail-fast: false` and uses Buildx `--load`. `build-plex` runs the Plex
support-window smokes; `build-generic` runs the Emby support-window,
slow-query, observability-count, and ABI checks; `build-cli` runs the STAT4 EQP
reproduction.

Mod runtime SHA data is rendered by
`tools/lsio-mod/render-lsio-mod-baked-pins.sh` from same-run build artifacts,
same-run runtime baseline extraction, and the Plex pool-patch pins
(`pins/plex-pool-patch-sites.tsv` and `pins/plex-pool-patch-reviews.tsv`).

The `mod-static-tests` job runs the repo-only LSIO mod, negative, maintenance,
workflow, release-note rendering, and shellcheck suites, including
`tests/render_release_notes_test.sh` and
`tests/check_multi_version_pin_alignment_negative_test.sh`. Mandatory
multi-version gates include `tests/check_multi_version_pin_alignment.sh` in
`preflight`, the parser and selector suites, support-image digest artifacts,
and the skopeo OCI static check in
`tools/ci/skopeo-mod-image-oci-check.sh`.

### Release Job

The release job runs only for CalVer tags matching `YYYY.MM.DD-rN`. It checks
out full history, selects the greatest reachable earlier CalVer tag in Git
version order, and appends oldest-first non-merge commit subjects to the
existing compatibility body. A first CalVer release and a same-commit revision
receive explicit text. It then downloads same-run sqlite artifacts, creates the
public CLI and library tarballs, writes `SHA256SUMS`, and publishes only:

- `sqlite-<tag>-*.tar.gz`
- `SHA256SUMS`

It reads prior tags and commit subjects only for release notes; it does not
fetch prior release assets, assemble runtime manifests, or publish runtime mod
metadata.

### LSIO Mod Jobs

`mod-build` runs on every push, pull request, and tag; depends on
`build-generic` and `build-plex`, not `release`. It uses a four-lane matrix:

| Mod | Runner | Arch suffix |
|---|---|---|
| Plex | `ubuntu-24.04` | `amd64` |
| Plex | `ubuntu-24.04-arm` | `arm64` |
| Emby | `ubuntu-24.04` | `amd64` |
| Emby | `ubuntu-24.04-arm` | `arm64` |

Each lane renders `baked-pins.txt`, stages an ephemeral Docker context under
`mktemp -d`, builds a run-scoped temporary image tag, and smokes by building a
throwaway image `FROM` the pinned LSIO base with the staged `root-fs/` copied
in, then running it so the native s6-rc `init-mod-sqlite3-*` oneshots execute
in the real LSIO environment before the app service starts. `mod-build` pushes
nothing and uploads each mod image as a workflow artifact.

`mod-publish` is tag-gated; depends on `release` and `mod-build`. It pushes
stable per-arch tags and the final multi-arch manifests. It never pushes
`latest`.

## Compile Profile

The compile profile is tuned by category rather than by one-off per-app forks.

Feature categories include search/indexing, app compatibility, operational
tooling, planner and sorter tuning, file and memory behavior, high-capacity
limits, and library safety posture. The profile avoids per-app forks except for
Plex ICU linkage.

The CLI target adds interactive diagnostics such as database-stat and bytecode
virtual tables. The library target keeps those out of application processes.

The shared compile flags pin `-DSQLITE_SORTER_PMASZ=8192` and
`-DSQLITE_DEFAULT_SORTERREF_SIZE=512` in `build/Build.sh`, so both apply to all
artifacts including the static CLI. At the 16 KiB compile-default page size,
PMASZ yields a 128 MiB PMA cap per active sort worker; with `PRAGMA threads=8`, a
connection may drive up to eight concurrent sort workers at that cap. The
SORTERREF compile default activates the 512 B threshold even on code paths that
do not run the runtime `sqlite3_config` re-assert.

The library profile pins `-DSQLITE_DEFAULT_AUTOVACUUM=0`.
`scripts/optimize_media_servers.sh` sets `PRAGMA auto_vacuum=NONE` explicitly
during planned-downtime database rebuilds, matching the compile default.
