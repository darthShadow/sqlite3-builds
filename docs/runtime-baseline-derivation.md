# Runtime Baseline Derivation

## Purpose and Scope

Derive or verify the SHA-256 values recorded in `pins/runtime-baselines.tsv`
from published OCI images without a Docker daemon. Use `skopeo` to copy the
image into a local OCI dir transport, extract the exact in-image files, and
hash the extracted bytes.

This covers:

- Plex `detect` rows for `plex_pms:pristine`, `plex_pms:patched`,
  `plex_scanner:pristine`, and `plex_scanner:patched`.
- Emby `detect` rows for `emby_deps` and `emby_dll`.
- `pre target_sqlite` rows for Plex and Emby.
- Plex `pre plex_icu_linked:<soname>` rows and matching `icu-runtime` rows.
- `pins/emby-detector-evidence.tsv` rows mirroring every Emby `detect` and
  `pre target_sqlite` baseline tuple, with `image_ref` from
  `pins/runtime-support.tsv`, `file_path` and `sha256` from
  `pins/runtime-baselines.tsv`, and `source` set to
  `pins/runtime-baselines.tsv`.

Arch mapping:

- `--override-arch amd64` feeds tracked `linux-x86_64-v2` and
  `linux-x86_64-v3`.
- `--override-arch arm64` feeds tracked `linux-arm64`.

Recorded values live in `pins/runtime-baselines.tsv`. Plex pool-patch tuple
inputs live in `pins/plex-pool-patch-sites.tsv`. Site derivation, baselines,
and exclusions live in `docs/plex-pool-patch-derivation.md`.

## Prerequisites

Require:

- `bash`
- `skopeo`
- `sha256sum` or `shasum -a 256`
- `python3`
- `tar`
- `od`
- `dd`
- `awk`
- `grep`
- `mktemp`

## Extraction

Copy the published image into a fresh dir transport:

```bash
copy_dir="$(mktemp -d)"
image_ref='ghcr.io/linuxserver/plex:1.43.2'
skopeo_arch='amd64'

skopeo copy \
  --override-os linux \
  --override-arch "$skopeo_arch" \
  "docker://$image_ref" \
  "dir:$copy_dir"
```

Use these helpers for the rest of the workflow:

```bash
sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    shasum -a 256 "$1" | awk '{print $1}'
  fi
}

manifest_layers() {
  python3 - "$1" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as handle:
    data = json.load(handle)

layers = data.get("layers")
if not isinstance(layers, list):
    raise SystemExit("manifest.json missing layers array")

for layer in layers:
    digest = layer["digest"]
    print(digest.split(":", 1)[1] if ":" in digest else digest)
PY
}

blob_mode() {
  local blob_path="$1"
  local magic

  magic="$(od -An -tx1 -N2 "$blob_path" | tr -d ' \n')"
  if [ "$magic" = "1f8b" ]; then
    printf 'gzip\n'
  else
    printf 'plain\n'
  fi
}

resolve_oci_member() {
  local copy_dir="$1"
  local target_path="$2"
  local digest
  local blob_path
  local mode
  local listing_path
  local raw_member
  local winner=''

  while IFS= read -r digest; do
    blob_path="${copy_dir}/${digest}"
    mode="$(blob_mode "$blob_path")"
    listing_path="$(mktemp)"

    if [ "$mode" = "gzip" ]; then
      tar -tzf "$blob_path" >"$listing_path"
    else
      tar -tf "$blob_path" >"$listing_path"
    fi

    raw_member="$(python3 - "$listing_path" "$target_path" <<'PY'
import sys

listing_path, target_path = sys.argv[1:]
matches = []

with open(listing_path, "r", encoding="utf-8", errors="surrogateescape") as src:
    for line in src:
        member = line.rstrip("\n")
        normalized = member[2:] if member.startswith("./") else member
        if not normalized.startswith("/"):
            normalized = "/" + normalized
        if normalized == target_path:
            matches.append(member)

if matches:
    print(matches[-1])
PY
)"
    rm -f "$listing_path"

    if [ -n "$raw_member" ]; then
      winner="${blob_path}"$'\t'"${mode}"$'\t'"${raw_member}"
    fi
  done < <(manifest_layers "${copy_dir}/manifest.json")

  if [ -z "$winner" ]; then
    printf 'missing member for %s\n' "$target_path" >&2
    return 1
  fi

  printf '%s\n' "$winner"
}

extract_oci_path() {
  local copy_dir="$1"
  local target_path="$2"
  local output_path="$3"
  local resolved
  local blob_path
  local mode
  local raw_member

  resolved="$(resolve_oci_member "$copy_dir" "$target_path")" || return 1
  blob_path="$(printf '%s\n' "$resolved" | awk -F '\t' '{print $1}')"
  mode="$(printf '%s\n' "$resolved" | awk -F '\t' '{print $2}')"
  raw_member="$(printf '%s\n' "$resolved" | awk -F '\t' '{print $3}')"

  if [ "$mode" = "gzip" ]; then
    tar -xzOf "$blob_path" "$raw_member" >"$output_path"
  else
    tar -xOf "$blob_path" "$raw_member" >"$output_path"
  fi
}

lookup_runtime_sha() {
  local kind="$1"
  local mod="$2"
  local server_id="$3"
  local arch="$4"
  local path_role="$5"
  local file_path="$6"

  awk -F '\t' \
    -v kind="$kind" \
    -v mod="$mod" \
    -v server_id="$server_id" \
    -v arch="$arch" \
    -v path_role="$path_role" \
    -v file_path="$file_path" \
    '
      $1 == kind &&
      $2 == mod &&
      $3 == server_id &&
      $4 == arch &&
      $5 == path_role &&
      $8 == file_path {
        print $12
      }
    ' pins/runtime-baselines.tsv
}

lookup_icu_runtime_sha() {
  local compat_group="$1"
  local arch="$2"
  local soname="$3"

  awk -F '\t' \
    -v compat_group="$compat_group" \
    -v arch="$arch" \
    -v soname="$soname" \
    '
      $1 == "icu-runtime" &&
      $4 == arch &&
      $9 == compat_group &&
      $10 == "linked" &&
      $11 == soname {
        print $12
      }
    ' pins/runtime-baselines.tsv
}

lookup_plex_baseline_sha() {
  local server_id="$1"
  local arch="$2"
  local binary_path="$3"

  awk -F '\t' \
    -v server_id="$server_id" \
    -v arch="$arch" \
    -v binary_path="$binary_path" \
    '
      NR > 1 &&
      $1 == server_id &&
      $2 == arch &&
      $3 == binary_path {
        print $4
        exit
      }
    ' pins/plex-pool-patch-sites.tsv
}

write_plex_site_file() {
  local server_id="$1"
  local arch="$2"
  local binary_path="$3"
  local output_path="$4"

  awk -F '\t' \
    -v server_id="$server_id" \
    -v arch="$arch" \
    -v binary_path="$binary_path" \
    '
      NR > 1 &&
      $1 == server_id &&
      $2 == arch &&
      $3 == binary_path {
        printf "%s|%s|%s|%s|%s\n", $5, $6, $7, $8, $9
      }
    ' pins/plex-pool-patch-sites.tsv >"$output_path"

  grep -q . "$output_path"
}
```

Match the exact in-image path from the tracked row. Do not match fragments. The
Plex paths contain spaces, tar members may be prefixed with `./`, and overlay
layers can replace an earlier file. The last matching layer in
`manifest.json` wins.

Examples:

```bash
extract_oci_path \
  "$copy_dir" \
  '/usr/lib/plexmediaserver/Plex Media Server' \
  "$copy_dir/Plex Media Server"

extract_oci_path \
  "$copy_dir" \
  '/usr/lib/plexmediaserver/lib/libsqlite3.so' \
  "$copy_dir/libsqlite3.so"

extract_oci_path \
  "$copy_dir" \
  '/usr/lib/plexmediaserver/lib/libicuucplex.so.69' \
  "$copy_dir/libicuucplex.so.69"
```

Use the same helper for Emby paths such as:

- `/app/emby/system/EmbyServer.deps.json`
- `/app/emby/system/EmbyServer.dll`
- `/app/emby/lib/libsqlite3.so.3.49.2`

## Pristine Cross-Check

Hash the extracted file before trusting it:

```bash
actual_sha="$(sha256_of "$copy_dir/Plex Media Server")"
expected_sha="$(lookup_runtime_sha \
  'detect' \
  'plex' \
  'plex-1.43.2' \
  'linux-x86_64-v2' \
  'plex_pms:pristine' \
  '/usr/lib/plexmediaserver/Plex Media Server')"

if [ "$actual_sha" != "$expected_sha" ]; then
  printf 'STOP: pristine mismatch expected=%s actual=%s\n' \
    "$expected_sha" \
    "$actual_sha" >&2
  exit 1
fi
```

Rules:

- For any `pre target_sqlite` row, the extracted file SHA must equal the
  recorded `sha256`.
- For any Plex `pre plex_icu_linked:<soname>` row, the extracted file SHA must
  equal both the `pre` row and the matching `icu-runtime` row for that
  `compat_group`, `arch`, and `soname`.
- For any Plex pristine detector binary, the extracted file SHA must equal the
  matching `detect ...:pristine` row and the `baseline_sha256` recorded in
  `pins/plex-pool-patch-sites.tsv`.
- On any mismatch, STOP. Do not trust the image, do not derive a patched
  detector SHA, and do not update tracked pins from that source.

## Plex Patched Detector Derivation

Only derive a patched Plex detector SHA from a pristine binary that already
passed the cross-check above. Pull the real site tuples from
`pins/plex-pool-patch-sites.tsv`. For site derivation, baselines, exclusions,
and runtime semantics, use `docs/plex-pool-patch-derivation.md`. For the shared
runtime patch core, use
`lsio-mods/shared/cont-init-fragments/plex-pool-patch.sh`. Do not copy those
tables into this doc.

Build the tuple file for one binary:

```bash
server_id='plex-1.43.2'
arch_key='linux-x86_64-v2'
binary_path='/usr/lib/plexmediaserver/Plex Media Server'
site_file="$copy_dir/pms.sites"

write_plex_site_file "$server_id" "$arch_key" "$binary_path" "$site_file" || {
  printf 'STOP: missing pool-site tuples for %s %s\n' "$arch_key" "$binary_path" >&2
  exit 1
}
```

Apply the tuples with an independent Python byte loop:

```bash
patch_plex_detector_python() {
  local pristine_path="$1"
  local site_file="$2"
  local patched_path="$3"

  python3 - "$pristine_path" "$site_file" "$patched_path" <<'PY'
import pathlib
import sys

pristine_path = pathlib.Path(sys.argv[1])
site_file = pathlib.Path(sys.argv[2])
patched_path = pathlib.Path(sys.argv[3])

patched = bytearray(pristine_path.read_bytes())

for raw_line in site_file.read_text(encoding="utf-8").splitlines():
    if not raw_line:
        continue
    label, offset, write_seek, original_hex, patched_hex = raw_line.split("|")
    offset = int(offset)
    write_seek = int(write_seek)
    write_index = write_seek - offset
    if write_index < 0 or write_index > 15:
        raise SystemExit(f"STOP: invalid write_index for {label}: {write_index}")

    byte_start = write_index * 2
    byte_end = byte_start + 2
    if (
        original_hex[:byte_start] != patched_hex[:byte_start] or
        original_hex[byte_end:] != patched_hex[byte_end:] or
        original_hex[byte_start:byte_end] == patched_hex[byte_start:byte_end]
    ):
        raise SystemExit(f"STOP: tuple is not a single-byte patch at {label}")

    observed = bytes(patched[offset:offset + 16]).hex()
    if observed != original_hex:
        raise SystemExit(f"STOP: original_hex mismatch at {label}")

    patched[write_seek] = int(patched_hex[byte_start:byte_end], 16)
    observed_patched = bytes(patched[offset:offset + 16]).hex()
    if observed_patched != patched_hex:
        raise SystemExit(f"STOP: patched_hex mismatch at {label}")

patched_path.write_bytes(patched)
PY
}

patch_plex_detector_python \
  "$copy_dir/Plex Media Server" \
  "$site_file" \
  "$copy_dir/Plex Media Server.patched.A"

patched_sha_a="$(sha256_of "$copy_dir/Plex Media Server.patched.A")"
printf '%s\n' "$patched_sha_a"
```

That model is fixed:

- `write_index = write_seek - offset`
- `write_index` must be in `0..15`
- assert the 16-byte window equals `original_hex`
- write exactly one byte from `patched_hex`
- assert the 16-byte window equals `patched_hex`

The resulting SHA-256 is the patched detector SHA for that binary.

## Emby Detectors

Emby detector derivation has no patch step. Extract and hash these two files:

- `/app/emby/system/EmbyServer.deps.json`
- `/app/emby/system/EmbyServer.dll`

Example:

```bash
extract_oci_path \
  "$copy_dir" \
  '/app/emby/system/EmbyServer.deps.json' \
  "$copy_dir/EmbyServer.deps.json"

extract_oci_path \
  "$copy_dir" \
  '/app/emby/system/EmbyServer.dll' \
  "$copy_dir/EmbyServer.dll"

deps_sha="$(sha256_of "$copy_dir/EmbyServer.deps.json")"
dll_sha="$(sha256_of "$copy_dir/EmbyServer.dll")"
printf 'deps=%s\n' "$deps_sha"
printf 'dll=%s\n' "$dll_sha"
```

Compare those SHAs to the matching `detect` rows in `pins/runtime-baselines.tsv`.
Use the same direct extraction-and-hash check for Emby `pre target_sqlite`.
`/app/emby/lib/libsqlite3.so.*` is byte-identical across the current supported
versions, so `EmbyServer.deps.json` and `EmbyServer.dll` are the version
discriminants.

## A/B Independent Cross-Derivation

Do not sign off a new Plex patched detector SHA from one implementation alone.
Run two independent derivations and require identical output:

- Lane A: the Python byte loop above.
- Lane B: the shared shell patch core in
  `lsio-mods/shared/cont-init-fragments/plex-pool-patch.sh`.

Example Lane B:

```bash
. lsio-mods/shared/cont-init-fragments/plex-pool-patch.sh

expected_pre_sha="$(lookup_plex_baseline_sha "$server_id" "$arch_key" "$binary_path")"
mapfile -t site_strings <"$site_file"

plex_pool_patch_populate_binary_tmp \
  "$copy_dir/Plex Media Server.patched.B" \
  "$copy_dir/Plex Media Server" \
  "$expected_pre_sha" \
  "${site_strings[@]}" || {
    printf 'STOP: shell patch lane failed\n' >&2
    exit 1
  }

patched_sha_b="$(sha256_of "$copy_dir/Plex Media Server.patched.B")"

if [ "$patched_sha_a" != "$patched_sha_b" ]; then
  printf 'STOP: A/B mismatch A=%s B=%s\n' "$patched_sha_a" "$patched_sha_b" >&2
  exit 1
fi
```

Before sign-off, also require equality with the recorded
`detect ...:patched` row. If the two lanes disagree, or either lane disagrees
with the recorded value, STOP. Pool-site derivation itself is separate
human-review-gated work: the M5 pool-review.

## No-Fabrication

Never invent, approximate, or backfill a SHA from memory.

- If a required `detect`, `pre`, `icu-runtime`, or `pool-site` source value is
  missing, STOP and report the missing row.
- If the extracted file does not match the recorded pristine baseline, STOP and
  report the mismatch.
- If a patched SHA is not reproduced byte-for-byte by both derivation lanes,
  STOP and report the mismatch.

Recorded values live in `pins/runtime-baselines.tsv`,
`pins/plex-pool-patch-sites.tsv`, and
`docs/plex-pool-patch-derivation.md`.
