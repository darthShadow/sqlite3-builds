# Baked Pins Schema

This is the durable current-state contract for schema v3 `baked-pins.txt`.
Runtime code is authoritative. Scratch design notes are intent only when they
match the cited code.

## Source Files

| Surface | Authority |
|---|---|
| Runtime schema validator | `lsio-mods/shared/cont-init-fragments/manifest-parser.sh:374-853` |
| Manifest row rendering and curation gates | `tools/lsio-mod/render-lsio-mod-baked-pins.sh:181-669` |
| Runtime selector semantics | `lsio-mods/shared/cont-init-fragments/selector.sh:50-198` |
| Checked-in curation inputs | `pins/runtime-support.tsv:1-5`, `pins/library-compat-groups.tsv:1-3`, `pins/runtime-baselines.tsv:1-76`, `pins/plex-pool-patch-sites.tsv:1-19`, `pins/plex-pool-patch-reviews.tsv:1-37` |
| Retired scalar-pin guard | `tests/check_pin_alignment.sh:35-52` |

## Schema V3 Rows

`baked-pins.txt` is pipe-delimited. Field count is computed from pipe count, and
each row kind has a fixed arity in the validator switch
(`manifest-parser.sh:14-19`, `manifest-parser.sh:443-459`).

| Kind | Arity | Fields | Semantics |
|---|---:|---|---|
| `meta` | 6 | `meta`, schema version, `release_tag`, release tag, `generated_at`, generated timestamp | Exactly one schema marker. The only accepted marker shape is version `3`, key `release_tag`, and value key `generated_at` (`manifest-parser.sh:477-490`, `manifest-parser.sh:731-734`; rendered at `render-lsio-mod-baked-pins.sh:633-635`). |
| `detect` | 8 | `detect`, row version, mod, server id, arch, detector role, file path, file SHA-256 | Server identity evidence. Valid row version is `1`; mods are `plex` or `emby`; arch is one of `linux-x86_64-v2`, `linux-x86_64-v3`, `linux-arm64`; roles are the fixed per-mod detect roles (`manifest-parser.sh:59-95`, `manifest-parser.sh:493-523`). |
| `artifact` | 9 | `artifact`, row version, mod, server id, arch, compat group, artifact relpath, target path, artifact SHA-256 | Replacement library selected for a supported tuple. Valid row version is `1`; compat groups are currently hardcoded to `plex|icu69` and `emby|generic` at runtime, plus fixture-only `emby|generic2` when enabled (`manifest-parser.sh:73-85`, `manifest-parser.sh:525-574`). |
| `pre` | 10 | `pre`, row version, mod, server id, arch, path role, image ref, image digest, file path, file SHA-256 | Runtime pre-swap provenance and guards. Valid row version is `2`; roles are `target_sqlite` for both mods and the three Plex linked ICU roles (`manifest-parser.sh:98-110`, `manifest-parser.sh:576-633`; rendered at `render-lsio-mod-baked-pins.sh:590-605`). |
| `pool-site` | 11 | `pool-site`, row version, `plex`, server id, arch, binary path, label, offset, write seek, original 16-byte hex context, patched 16-byte hex context | Plex byte patch site data. Valid row version is `1`; rows are Plex-only, use decimal offsets, and must describe exactly one byte change inside the 16-byte context (`manifest-parser.sh:635-690`). |
| `unsupported` | 7 | `unsupported`, row version, mod, server id, arch, compat group, reason | Local/offline explanation for a tuple without an artifact. Valid row version is `1`; reason must start with `local-offline-`; a matching artifact row is mutually exclusive (`manifest-parser.sh:692-727`, `manifest-parser.sh:736-740`). |

Field rules:

- Empty fields are rejected: leading pipe, trailing pipe, or doubled pipe
  (`manifest-parser.sh:462-467`). Renderer input TSVs also reject empty required
  fields before rendering (`render-lsio-mod-baked-pins.sh:123-130`).
- The `-` sentinel is accepted only as field index 7 in `pre`, which is
  `pre.image_digest`; any other `-` field is rejected by the manifest validator
  (`manifest-parser.sh:469-475`). In curation input, `-` appears in non-rendered
  placeholder columns such as detector rows in `pins/runtime-baselines.tsv:2-6`.
- SHA-256 fields are exactly 64 hex characters
  (`manifest-parser.sh:21-23`, `manifest-parser.sh:505-507`,
  `manifest-parser.sh:538-540`, `manifest-parser.sh:594-596`).
- Pool-site hex contexts are exactly 32 hex characters, representing 16 bytes
  (`manifest-parser.sh:25-27`, `manifest-parser.sh:669-671`).
- Pool-site `offset` and `write_seek` are unsigned decimal strings with no
  leading zero when multi-digit; `write_seek - offset` must be in `0..15`
  (`manifest-parser.sh:650-668`).
- `original_hex` and `patched_hex` must differ at exactly the byte selected by
  `write_seek - offset` (`manifest-parser.sh:41-57`,
  `manifest-parser.sh:673-675`).

## Validator Reject List

`validate_baked_pins_schema` fails closed on the following conditions.

| Group | Reject condition | Evidence |
|---|---|---|
| Manifest presence | Manifest path is unset or missing. | `manifest-parser.sh:374-380` |
| Legacy rows | Legacy `current`, `pool-pre`, schema-v2 `version|2`, and `managed_window` rows are rejected. | `manifest-parser.sh:419-441` |
| Kind and arity | Row kind is not one of the six schema v3 kinds; row field count differs from the kind arity. | `manifest-parser.sh:443-459` |
| Field shape | Any empty field is rejected. | `manifest-parser.sh:462-467` |
| Sentinel scope | `-` outside `pre.image_digest` is rejected. | `manifest-parser.sh:469-475` |
| Meta | The meta row is not exactly schema `3` with `release_tag` and `generated_at`; meta is duplicated; or no meta row exists. | `manifest-parser.sh:477-490`, `manifest-parser.sh:731-734` |
| Detect fields | Detect row version, mod, arch, or role is invalid. | `manifest-parser.sh:87-95`, `manifest-parser.sh:493-504` |
| Detect SHA | Detect SHA is not 64 hex. | `manifest-parser.sh:505-507` |
| Detect key | Duplicate detect row key for `(mod, server_id, arch, role)`. | `manifest-parser.sh:509-514` |
| Artifact fields | Artifact row version, mod, arch, or compat group is invalid. | `manifest-parser.sh:73-85`, `manifest-parser.sh:525-537` |
| Artifact SHA | Artifact SHA is not 64 hex. | `manifest-parser.sh:538-540` |
| Plex artifact target | Plex artifact target path is not exactly `/usr/lib/plexmediaserver/lib/libsqlite3.so`. | `manifest-parser.sh:542-545` |
| Emby artifact target | Emby artifact target is `/app/emby/lib/libsqlite3.so` or does not match the concrete versioned-file regex. | `manifest-parser.sh:112-114`, `manifest-parser.sh:546-555` |
| Artifact key | Duplicate artifact row key for `(mod, server_id, arch, compat_group)`. | `manifest-parser.sh:556-561` |
| Artifact coverage group | One `(mod, server_id, arch)` tuple names more than one compat group across artifact or unsupported rows. | `manifest-parser.sh:567-572`, `manifest-parser.sh:721-725` |
| Pre fields | Pre row version, mod, arch, or path role is invalid. | `manifest-parser.sh:98-110`, `manifest-parser.sh:576-589` |
| Pre image data | `image_ref` or `image_digest` is empty. | `manifest-parser.sh:590-593` |
| Pre SHA | Pre SHA is not 64 hex. | `manifest-parser.sh:594-596` |
| Pre target path | Plex `target_sqlite` path is not the Plex target; Emby `target_sqlite` is the unversioned alias or does not match the concrete versioned-file regex. | `manifest-parser.sh:598-612` |
| Plex ICU pre path | Plex ICU `pre` row file path is not `/usr/lib/plexmediaserver/lib/<required-soname>`. | `manifest-parser.sh:614-620` |
| Pre key | Duplicate pre row key for `(mod, server_id, arch, role)`. | `manifest-parser.sh:622-627` |
| Pool-site fields | Pool-site row version is not `1`, mod is not `plex`, or arch is invalid. | `manifest-parser.sh:635-649` |
| Pool-site offsets | Offset or write seek is not decimal; either has a leading zero; write seek is before offset; or `write_seek - offset` is outside `0..15`. | `manifest-parser.sh:650-668` |
| Pool-site hex | Original or patched context is not 32 hex. | `manifest-parser.sh:669-671` |
| Pool-site byte diff | Patched context does not differ from original at exactly the write byte. | `manifest-parser.sh:41-57`, `manifest-parser.sh:673-675` |
| Pool-site key | Duplicate pool-site key for `(server_id, arch, binary_path, label, offset)`. | `manifest-parser.sh:677-682` |
| Unsupported fields | Unsupported row version, mod, arch, or compat group is invalid. | `manifest-parser.sh:692-702` |
| Unsupported reason | Unsupported reason does not start with `local-offline-`. | `manifest-parser.sh:703-709` |
| Unsupported key | Duplicate unsupported row key for `(mod, server_id, arch, compat_group)`. | `manifest-parser.sh:710-715` |
| Unsupported/artifact exclusion | A tuple has both `unsupported` and `artifact` rows for the same compat group. | `manifest-parser.sh:736-740` |
| Emby detector coverage | Any Emby tuple lacks exactly one `emby_deps` and one `emby_dll` detect row. | `manifest-parser.sh:743-752` |
| Plex detector coverage | Any Plex tuple lacks exactly one `plex_pms:pristine`, `plex_pms:patched`, `plex_scanner:pristine`, and `plex_scanner:patched` detect row. | `manifest-parser.sh:753-760` |
| Plex detector path conflict | Plex pristine and patched rows for the same logical detector use different file paths. | `manifest-parser.sh:761-768` |
| Tuple mod guard | A collected tuple has an invalid mod while post-loop detector coverage is checked. | `manifest-parser.sh:771-774` |
| Duplicate canonical detector set | Two server ids for the same mod and arch have the same complete detector SHA set. | `manifest-parser.sh:776-782` |
| Missing artifact or unsupported row | A tuple has no compat group, or lacks both the expected artifact row and matching unsupported row. | `manifest-parser.sh:785-799` |
| Artifact file | Artifact relpath is not `artifacts/<arch>/<compat_group>/libsqlite3.so`, the file is absent, or its SHA mismatches the row. | `manifest-parser.sh:802-814` |
| Missing pre target | Artifact target path has no matching `pre target_sqlite` row. | `manifest-parser.sh:815-818` |
| Missing Plex ICU pre | Plex tuple lacks one of the three linked ICU `pre` rows. | `manifest-parser.sh:821-832` |
| Missing Plex pool sites | Plex tuple lacks pool-site rows for either pristine PMS or pristine Scanner detector path. | `manifest-parser.sh:834-839` |
| Pool-site detector binding | Pool-site binary path is not one of the tuple's pristine Plex detector paths. | `manifest-parser.sh:842-849` |

## Artifact and Compatibility Keying

Runtime artifact rows include `mod`, `server_id`, `arch`, and `compat_group`, but
the staged replacement library file is keyed by `(compat_group, arch)`, with the
mod implied by the compat group. The renderer accepts artifacts as
`ARCH:COMPAT_GROUP:NAME:PATH:TARGET_PATH`, stores artifact SHA by `arch|compat`,
and emits relpaths under `artifacts/<arch>/<compat_group>/libsqlite3.so`
(`render-lsio-mod-baked-pins.sh:235-260`,
`render-lsio-mod-baked-pins.sh:301-321`,
`render-lsio-mod-baked-pins.sh:654-655`). The stager enforces one source
artifact per `(compat_group, arch)` and rejects relpaths outside that shape
(`stage-lsio-mod.sh:46-54`, `stage-lsio-mod.sh:62-83`).

Rationale: server versions with the same compat group share one built library for
each arch. Detector and `pre` rows stay per server version; artifact files stay
per compat group and arch. Current compat groups are data rows in
`pins/library-compat-groups.tsv:1-3`, but the runtime validator still hardcodes
the accepted mod/group pairs (`manifest-parser.sh:73-85`). The workflow also
derives the build stems from the first compat group per mod, so adding a second
same-mod group is a code and workflow change in the current implementation
(`.github/workflows/sqlite-build.yml`, step `Load library artifact stems`;
`tests/check_multi_version_pin_alignment.sh`, exact Emby/Plex upload-artifact
`require_line` assertions).

For Plex ICU runtime SHAs, enforcement is per `(compat_group, arch, soname)`, not
group-global. Renderer input accepts `icu-runtime` baseline rows, ignores rows
whose ICU role is not `linked`, and stores linked runtime SHAs by
`compat|arch|soname` (`render-lsio-mod-baked-pins.sh:380-390`). Each Plex tuple
then requires its three linked ICU `pre` rows to match that per-arch runtime SHA
set (`render-lsio-mod-baked-pins.sh:576-582`). Current pins show the linked rows
for all three runtime arch classes (`pins/runtime-baselines.tsv:50-58`).

The replacement library links only `libicuucplex.so.69`,
`libicui18nplex.so.69`, and `libicudataplex.so.69`
(`build/Build.sh:41-49`, `pins/library-compat-groups.tsv:2`). A sibling such as
`libicuioplex.so.69`, when captured from an image for provenance, is not part of
the linked closure and does not become a required `pre` row unless the library's
DT_NEEDED set changes; non-`linked` `icu-runtime` rows are ignored by the
renderer (`render-lsio-mod-baked-pins.sh:380-390`).

## Target Paths

Plex replacement targets only `/usr/lib/plexmediaserver/lib/libsqlite3.so`.
Both artifact rows and `pre target_sqlite` rows are rejected if they name any
other Plex path (`manifest-parser.sh:542-545`, `manifest-parser.sh:598-602`).
The CI bake-smoke target resolver enforces the same Plex baseline path before
rendering (`tools/ci/mod-bake-smoke.sh:62-87`).

Emby replacement targets must be concrete versioned files matching
`^/app/emby/lib/libsqlite3.so.<major>.<minor>.<patch>$`
(`manifest-parser.sh:112-114`, `render-lsio-mod-baked-pins.sh:119-121`).
The unversioned alias `/app/emby/lib/libsqlite3.so` is explicitly rejected;
other aliases such as `/app/emby/lib/libsqlite3.so.0` fail the concrete-file
regex (`manifest-parser.sh:546-555`, `manifest-parser.sh:603-611`;
tests cover the `.so.0` rejection at `tests/manifest_parser_test.sh:294-296`).
Alias symlinks are provenance-only and are never artifact or `pre target_sqlite`
targets.

A target path may differ between server versions inside one compat group and arch
only when the same artifact SHA safely replaces each path. The renderer emits the
per-server baseline target path, warns on divergent target paths, and fails if
the divergent target path is paired with a different artifact SHA
(`render-lsio-mod-baked-pins.sh:560-574`). CI bake-smoke records target paths per
`compat_group|arch` and warns on divergence while passing the selected target to
the renderer (`tools/ci/mod-bake-smoke.sh:150-157`,
`tools/ci/mod-bake-smoke.sh:168-187`).

## Pool-Site Review Binding

The renderer binds reviews to the exact review-row site tuple:
`server_id`, `arch`, `binary_path`, `label`, `offset`, `write_seek`,
`original_hex`, and `patched_hex`. It hashes that tuple from the site rows and
requires review rows to reproduce the same tuple (`render-lsio-mod-baked-pins.sh:429-481`).
Any change to one of those fields requires new matching approved review rows.

`baseline_sha256` is present in `pins/plex-pool-patch-sites.tsv`, but review
rows do not carry it. The implemented binding validates `baseline_sha256`
separately against the selected pristine detector SHA before accepting the site
(`render-lsio-mod-baked-pins.sh:484-524`). This is the code reality: review
approval binds the exact review tuple above, while baseline SHA drift is guarded
by pristine-detector binding.

Each exact site requires at least two distinct approved reviewers. Renderer
review counting deduplicates by reviewer and requires `review_count >= 2`
(`render-lsio-mod-baked-pins.sh:475-480`,
`render-lsio-mod-baked-pins.sh:522-524`). The multi-version alignment test
independently enforces approved status and fewer-than-two review rejection
(`tests/check_multi_version_pin_alignment.sh:286-297`,
`tests/check_multi_version_pin_alignment.sh:327-330`).

## Retired Scalar Pins

The single-version scalar pin model is retired. The complete retired-key list is:

- `PLEX_IMAGE_TAG`
- `EMBY_IMAGE_TAG`
- `ICU_VERSION`
- `ICU_SHA512`

`tests/check_pin_alignment.sh` constructs that list and recursively rejects those
keys under `pins/`, `.github/workflows/`, `build/`, `docker-library/`, `tools/`,
and `scripts/` (`tests/check_pin_alignment.sh:35-52`). No environment variable
or script path re-enables the old single-pin behavior in the current checked
surfaces; reintroducing one of those keys in the scanned trees fails the guard.

## Moving Image Aliases and Digest Reproducibility

`pins/runtime-support.tsv` stores curated `image_ref` values. Current rows use
LSIO tag aliases, not checked-in immutable digests (`pins/runtime-support.tsv:1-5`).
Runtime safety does not rely on `image_ref` immutability: detector SHAs, target
`pre` SHAs, linked ICU SHAs, artifact SHAs, and pool-site byte contexts are the
fail-closed gates (`manifest-parser.sh:505-507`,
`manifest-parser.sh:594-620`, `manifest-parser.sh:802-849`).

`pre.image_digest` is provenance. The validator only requires it to be non-empty
and confines the `-` sentinel to that field; it does not validate digest syntax
(`manifest-parser.sh:469-475`, `manifest-parser.sh:590-596`). CI records resolved
repo digests for supported images in a `support-image-digests-<mod>-<arch_suffix>`
artifact by pulling each `image_ref` and writing `image-digests.tsv`
(`.github/workflows/sqlite-build.yml`, step `Record support image digests`).
Bake-smoke also pulls the image,
extracts `RepoDigests`, and injects that digest into pre fragments used for
rendering (`tools/ci/mod-bake-smoke.sh:131-161`).

## Detector Selection

Selector role normalization collapses Plex pristine and patched detector rows to
logical roles `plex_pms` and `plex_scanner`; Emby uses `emby_deps` and
`emby_dll` (`selector.sh:3-22`). The validator requires exactly four Plex detect
rows per tuple and exactly two Emby detect rows per tuple
(`manifest-parser.sh:743-760`). The selector hashes each detector file path once,
then accepts a server id only when every required logical role exists and the
live SHA matches one of that role's allowed SHA values (`selector.sh:81-172`).

For Plex, this means a host is selectable when each logical detector matches
either its pristine SHA or its known-patched SHA. The selector is stateless: it
reads only manifest `detect` rows and live detector files, returns a server id
for exactly one complete match, and logs/warn-skips with exit 0 on zero or
ambiguous complete matches (`selector.sh:50-58`, `selector.sh:81-122`,
`selector.sh:174-197`). A cross-product case where different detector files match
different server ids yields zero complete matches and logs the partial-match path,
not a selection (`selector.sh:140-172`, `selector.sh:179-191`). The SQLite target
and `.bundled.bak` backup are not selector identity inputs; target SHA checks
occur only after a server id is selected (`selector.sh:237-278`).

## Curation Status

`pins/runtime-support.tsv` status values are closed to:

- `pending_pool_review`
- `supported`
- `unsupported`

The renderer accepts only those values (`render-lsio-mod-baked-pins.sh:81-85`).
Only `supported` rows are rendered into the mod support window; all other status
values are ignored by the renderer (`render-lsio-mod-baked-pins.sh:203-232`).
The workflow digest-recording path also filters to supported rows only
(`.github/workflows/sqlite-build.yml`, step `Record support image digests`).

Operational meanings:

- `pending_pool_review`: curation row is not eligible for rendering yet because
  required pool-site review evidence is incomplete. Code effect: ignored until
  changed to `supported`.
- `supported`: static evidence, artifact coverage, and required review records
  must exist; renderer emits the row and fail-closed guards apply
  (`render-lsio-mod-baked-pins.sh:531-588`,
  `render-lsio-mod-baked-pins.sh:637-665`).
- `unsupported`: retained curation/evidence row that is not rendered as a
  supported runtime target. Runtime `unsupported` manifest rows are a separate
  local/offline artifact-missing mechanism and require a `local-offline-*` reason
  (`manifest-parser.sh:692-727`).
