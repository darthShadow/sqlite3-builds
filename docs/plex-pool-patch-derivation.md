# Plex Pool-Patch Derivation

This document records the current derivation method and site data for the Plex
ConnectionPool pool-size patch. The runtime patch lowers each supported
ConnectionPool size immediate from 20 to 16 in `Plex Media Server` and
`Plex Media Scanner`.

## Method

Derivation starts from the RTTI name for
`__shared_ptr_emplace<ConnectionPool>`. The typeinfo reference leads to the
ConnectionPool vtable, the setup helper, and then the caller that passes the
pool size into the constructor path. The patch site is the caller-side load of
the pool-size immediate:

- amd64: `mov esi,#20`, patched to `mov esi,#16`.
- arm64: `movz w1,#20`, patched to `movz w1,#16`.

The file offset is derived from the runtime virtual address by subtracting the
binary's load delta:

| Arch | VA to file-offset delta |
|---|---:|
| `linux-x86_64-v2` / `linux-x86_64-v3` | `0x1000` |
| `linux-arm64` | `0x10000` |

Every patch tuple uses:

```text
label|offset|write_seek|original_hex|patched_hex
```

`offset` and `write_seek` are decimal file offsets. `label` is the file offset
in hex. `original_hex` and `patched_hex` are 16-byte contexts. The context must
be unique in the target binary.

## Supported Surface

The supported Plex pool-patch surface is keyed by `server_id` rows in
`pins/runtime-support.tsv`.

| Server ID | Image ref | Compat group |
|---|---|---|
| `plex-1.43.1` | `ghcr.io/linuxserver/plex:1.43.1` | `icu69` |
| `plex-1.43.2` | `ghcr.io/linuxserver/plex:1.43.2` | `icu69` |

For each supported `server_id` and arch, pristine Plex detector SHAs live in
`pins/runtime-baselines.tsv` and render into `detect` rows whose path roles end
in `:pristine` (`plex_pms:pristine` and `plex_scanner:pristine`). The runtime
pool-patch phase selects the server first, then passes the selected pristine
Plex detector SHA to the patcher.

## Patch Sites

Patch sites are server-scoped data rows in
`pins/plex-pool-patch-sites.tsv`:

```text
server_id arch binary_path baseline_sha256 label offset write_seek original_hex patched_hex
```

`baseline_sha256` must match the selected pristine Plex detector SHA for the
same `server_id`, arch, and binary path. The renderer and CI alignment tests
enforce that binding before a `pool-site` row is emitted into `baked-pins.txt`.

Human review approvals are server-scoped rows in
`pins/plex-pool-patch-reviews.tsv`:

```text
server_id arch binary_path label offset write_seek original_hex patched_hex review_ref reviewer status
```

A review row binds the full site tuple. Any change to `server_id`, `arch`,
`binary_path`, `label`, `offset`, `write_seek`, `original_hex`, or
`patched_hex` creates a different tuple and requires fresh approval.

## Exclusions

These pool-size-2 sites are for a different database and must not be patched:

| Arch | Binary | File offset | VA |
|---|---|---:|---:|
| `linux-x86_64-v2` / `linux-x86_64-v3` | `/usr/lib/plexmediaserver/Plex Media Server` | `0xaf660b` | `0xaf760b` |
| `linux-x86_64-v2` / `linux-x86_64-v3` | `/usr/lib/plexmediaserver/Plex Media Scanner` | `0x372be9` | `0x373be9` |
| `linux-arm64` | `/usr/lib/plexmediaserver/Plex Media Server` | `0x9f7868` | `0xa07868` |
| `linux-arm64` | `/usr/lib/plexmediaserver/Plex Media Scanner` | `0x32f6d0` | `0x33f6d0` |

## Runtime Mechanism

The runtime patcher receives the target binary path, selected pristine Plex
detector SHA (`detect ...:pristine`), and one or more site tuples from the Phase
04 Plex pool-patch oneshot. It reads exactly 16 bytes at each tuple offset. A
binary is patchable only when every site matches `original_hex` and the current
binary SHA matches the selected pristine Plex detector SHA. If every site
already matches `patched_hex`, the patcher logs `pool_patch
event=already-patched` and skips the binary.

For each original site, the patcher computes `write_index` as
`write_seek - offset`, selects the one output byte from `patched_hex`, writes
that byte to a same-filesystem temporary copy with `dd conv=notrunc`, and
verifies the 16-byte context. It preserves the `.bundled.bak` backup, restores
owner and mode metadata on the temp copy, and atomically replaces the target.

Unknown, mixed, mismatched, or write-failed states warn and skip without
modifying the runtime target.

## See also

[Runtime Baseline Derivation](runtime-baseline-derivation.md)
