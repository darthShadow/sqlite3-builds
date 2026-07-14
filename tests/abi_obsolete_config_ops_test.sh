#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd -P)"
image_name="${1:-sqlite3-library}"
machine="$(uname -m)"
host_os="$(uname -s)"

case "${machine}" in
  aarch64 | arm64)
    march="armv8-a"
    ;;
  *)
    march="x86-64-v2"
    ;;
esac

fatal() {
  echo "FATAL: $*" >&2
  exit 1
}

sqlite_amalg_url="${SQLITE_AMALG_URL:-}"
sqlite_amalg_sha3_256="${SQLITE_AMALG_SHA3_256:-}"

. "${repo_root}/pins/versions.env"

[ -n "${SQLITE_AMALG_URL:-}" ] || fatal "SQLITE_AMALG_URL is not set in pins/versions.env"
[ -n "${SQLITE_AMALG_SHA3_256:-}" ] || fatal "SQLITE_AMALG_SHA3_256 is not set in pins/versions.env"

sqlite_amalg_url="${sqlite_amalg_url:-${SQLITE_AMALG_URL}}"
sqlite_amalg_sha3_256="${sqlite_amalg_sha3_256:-${SQLITE_AMALG_SHA3_256}}"

tmpdir="$(mktemp -d /tmp/abi-obsolete-config-ops-test.XXXXXX 2>/dev/null || { mkdir -p /tmp/abi-obsolete-config-ops-test-$$; echo /tmp/abi-obsolete-config-ops-test-$$; })"
trap 'rm -rf "${tmpdir}"' EXIT

cat > "${tmpdir}/abi_obsolete_config_ops.c" <<'EOF'
#include <stdio.h>
#include <sqlite3.h>

enum {
    ALLOW_SQLITE_OK = 1 << 0,
    ALLOW_SQLITE_ERROR = 1 << 1,
    ALLOW_SQLITE_MISUSE = 1 << 2
};

static int check_allowed_rc(const char *label, int rc, int allowed) {
    int ok = 0;
    if (rc == SQLITE_OK && (allowed & ALLOW_SQLITE_OK)) ok = 1;
    if (rc == SQLITE_ERROR && (allowed & ALLOW_SQLITE_ERROR)) ok = 1;
    if (rc == SQLITE_MISUSE && (allowed & ALLOW_SQLITE_MISUSE)) ok = 1;

    if (!ok) {
        fprintf(stderr, "FATAL: %s returned %d (expected%s%s%s)\n",
                label,
                rc,
                (allowed & ALLOW_SQLITE_OK) ? " SQLITE_OK" : "",
                (allowed & ALLOW_SQLITE_ERROR) ? " SQLITE_ERROR" : "",
                (allowed & ALLOW_SQLITE_MISUSE) ? " SQLITE_MISUSE" : "");
        return 1;
    }
    printf("%s=%d\n", label, rc);
    return 0;
}

int main(void) {
    int failed = 0;

    failed |= check_allowed_rc("scratch_pre", sqlite3_config(SQLITE_CONFIG_SCRATCH),
                               ALLOW_SQLITE_OK | ALLOW_SQLITE_ERROR | ALLOW_SQLITE_MISUSE);
    failed |= check_allowed_rc("pagecache_pre", sqlite3_config(SQLITE_CONFIG_PAGECACHE, NULL, 0, 0),
                               ALLOW_SQLITE_OK | ALLOW_SQLITE_MISUSE);
    failed |= check_allowed_rc("heap_pre", sqlite3_config(SQLITE_CONFIG_HEAP, NULL, 0, 0),
                               ALLOW_SQLITE_OK | ALLOW_SQLITE_ERROR | ALLOW_SQLITE_MISUSE);
    failed |= check_allowed_rc("pcache_pre", sqlite3_config(SQLITE_CONFIG_PCACHE),
                               ALLOW_SQLITE_OK | ALLOW_SQLITE_ERROR | ALLOW_SQLITE_MISUSE);
    failed |= check_allowed_rc("getpcache_pre", sqlite3_config(SQLITE_CONFIG_GETPCACHE),
                               ALLOW_SQLITE_OK | ALLOW_SQLITE_ERROR | ALLOW_SQLITE_MISUSE);

    failed |= check_allowed_rc("initialize", sqlite3_initialize(),
                               ALLOW_SQLITE_OK | ALLOW_SQLITE_MISUSE);
    failed |= check_allowed_rc("pcache_post", sqlite3_config(SQLITE_CONFIG_PCACHE),
                               ALLOW_SQLITE_OK | ALLOW_SQLITE_MISUSE);
    failed |= check_allowed_rc("getpcache_post", sqlite3_config(SQLITE_CONFIG_GETPCACHE),
                               ALLOW_SQLITE_OK | ALLOW_SQLITE_MISUSE);

    sqlite3_shutdown();
    return failed ? 1 : 0;
}
EOF

if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
  if ! docker image inspect "${image_name}" >/dev/null 2>&1; then
    docker build --rm \
      -t "${image_name}" \
      -f "${repo_root}/docker-library/Dockerfile" \
      "${repo_root}" \
      --build-arg SQLITE_AMALG_URL="${sqlite_amalg_url}" \
      --build-arg MARCH="${march}" \
      --build-arg SQLITE_AMALG_SHA3_256="${sqlite_amalg_sha3_256}" >/dev/null
  fi

  docker run --rm \
    -v "${tmpdir}:/work" \
    "${image_name}" \
    bash -lc '
      set -euo pipefail
      LIB_DIR=$(dirname "$(ls /app/sqlite-amalgamation-*/dist/libsqlite3.so)")
      AMALG_DIR=$(dirname "$LIB_DIR")
      gcc -O0 -o /work/abi_obsolete_config_ops /work/abi_obsolete_config_ops.c \
        -I"$AMALG_DIR" -L"$LIB_DIR" -lsqlite3 -ldl -lpthread
      LD_LIBRARY_PATH="$LIB_DIR" /work/abi_obsolete_config_ops
    '
else
  command -v curl >/dev/null 2>&1 || fatal "curl not found"
  command -v unzip >/dev/null 2>&1 || fatal "unzip not found"
  command -v patch >/dev/null 2>&1 || fatal "patch not found"
  command -v cc >/dev/null 2>&1 || fatal "cc not found"
  command -v openssl >/dev/null 2>&1 || fatal "openssl not found"

  amalg_zip="${tmpdir}/sqlite-amalgamation.zip"
  curl -fsSL "${sqlite_amalg_url}" -o "${amalg_zip}"
  actual_sha3="$(openssl dgst -sha3-256 "${amalg_zip}" | awk '{print $2}')"
  [[ "${actual_sha3}" == "${sqlite_amalg_sha3_256}" ]] || fatal "source SHA3-256 mismatch: expected ${sqlite_amalg_sha3_256}, got ${actual_sha3}"

  unzip -q "${amalg_zip}" -d "${tmpdir}"
  amalg_dir="$(find "${tmpdir}" -maxdepth 1 -type d -name 'sqlite-amalgamation-*' | head -n 1)"
  [[ -n "${amalg_dir}" ]] || fatal "sqlite amalgamation directory not found after unzip"

  cp "${repo_root}/src/auto_extension.c" "${tmpdir}/auto_extension.c"
  cp "${repo_root}/src/runtime_optimize.c" "${tmpdir}/runtime_optimize.c"
  cp "${repo_root}/src/auto_extension_internal.h" "${tmpdir}/auto_extension_internal.h"
  cp "${repo_root}/src/observability.c" "${tmpdir}/observability.c"
  cp "${repo_root}/src/observability.h" "${tmpdir}/observability.h"
  cp "${repo_root}/src/rewrite_modes.h" "${tmpdir}/rewrite_modes.h"
  cp "${repo_root}/src/fts_lex.c" "${tmpdir}/fts_lex.c"
  cp "${repo_root}/src/fts_lex.h" "${tmpdir}/fts_lex.h"
  cp "${repo_root}/src/plex_fts_rewrite.c" "${tmpdir}/plex_fts_rewrite.c"
  cp "${repo_root}/src/plex_fts_rewrite.h" "${tmpdir}/plex_fts_rewrite.h"
  cp "${repo_root}/src/emby_fts_rewrite.c" "${tmpdir}/emby_fts_rewrite.c"
  cp "${repo_root}/src/emby_fts_rewrite.h" "${tmpdir}/emby_fts_rewrite.h"
  (
    cd "${amalg_dir}"
    patch -p1 < "${repo_root}/build/sqlite-amalgamation.patch" >/dev/null
  )

  case "${host_os}" in
    Darwin)
      lib_path="${tmpdir}/libsqlite3.dylib"
      shared_flags="-dynamiclib"
      runtime_var="DYLD_LIBRARY_PATH"
      extra_ldflags=""
      ;;
    *)
      lib_path="${tmpdir}/libsqlite3.so"
      shared_flags="-shared -fPIC"
      runtime_var="LD_LIBRARY_PATH"
      extra_ldflags="-ldl"
      ;;
  esac

  # shared_flags is a platform-specific compiler flag vector.
  # shellcheck disable=SC2086
  cc -O0 ${shared_flags} \
    -I"${amalg_dir}" \
    -DSQLITE_CORE \
    -DSQLITE_ENABLE_LOAD_EXTENSION \
    -DSQLITE_ENABLE_NORMALIZE \
    -DSQLITE_ENABLE_UNLOCK_NOTIFY \
    -DSQLITE_DISABLE_PAGECACHE_OVERFLOW_STATS \
    -DSQLITE_DEFAULT_PCACHE_INITSZ=256 \
    -DSQLITE_OMIT_SHARED_CACHE \
    -DSQLITE_THREADSAFE=2 \
    -DSQLITE_USE_URI=1 \
    "${amalg_dir}/sqlite3.c" \
    "${tmpdir}/auto_extension.c" \
    "${tmpdir}/runtime_optimize.c" \
    "${tmpdir}/observability.c" \
    "${repo_root}/src/slow_query_tracker.c" \
    "${tmpdir}/fts_lex.c" \
    "${tmpdir}/plex_fts_rewrite.c" \
    "${tmpdir}/emby_fts_rewrite.c" \
    -lpthread -lm ${extra_ldflags} \
    -o "${lib_path}"

  cc -O0 -o "${tmpdir}/abi_obsolete_config_ops" "${tmpdir}/abi_obsolete_config_ops.c" \
    -I"${amalg_dir}" -L"${tmpdir}" -lsqlite3 -lpthread ${extra_ldflags}
  env "${runtime_var}=${tmpdir}" "${tmpdir}/abi_obsolete_config_ops"
fi

echo "abi obsolete config ops test passed"
