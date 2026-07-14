# Query Measurement Harness

## Purpose

`query-measure.sh` is the only copied-database query measurement and canary
engine. Query families provide derivation SQL, raw vendor/candidate SQL,
identity semantics, declared-divergence fixtures, and a case inventory. The
engine owns copy preflight, timeout policy, pre-warm, identity gating, EQP,
counterbalanced median timing, status, output, and cleanup.

Modes:

- `measure` records identity, EQP, medians, and timeout verdicts. A measured
  regression remains evidence and does not turn successful collection into a
  process failure.
- `canary` evaluates each case's declared policy after the same identity/EQP
  path and emits `STATUS PASS` or `STATUS FAIL` from cases declared `SHIPPED`
  by the family. `LATENCY` applies `CANARY_MAX_RATIO`; `WORK` applies the
  family work-counter assertion; `PLAN` applies exact EQP identity. Failures in
  `COMPARISON` cases remain informational.

## Families

| Family | Cases | Candidate kind |
|---|---|---|
| `plex-ondeck` | `id-list` and `threshold`, each with shipped plain `JOIN` and comparison `CROSS JOIN` | Structural rewrite |
| `plex-guid-like` | NULL pattern work reduction and non-NULL database-derived prefix plan identity | NULL-pattern guard |
| `plex-taggings` | Count and grouped-id taggings membership | Structural rewrite |
| `emby-search` | 9 candidates x 2 projections x 4 database-derived MATCH cells | Structural rewrite and CTE plan candidates |
| `emby-fanout` | Resume 12/24 and three captured resume-group shapes | Structural rewrite |
| `emby-dashboard` | movies-Latest and episodes-Latest | Structural rewrite plus required-index candidate prep |

`CASES` narrows families that expose it. `SEARCH_CANDIDATES`,
`SEARCH_VARIANTS`, and `SEARCH_MATCH_CASES` narrow the Emby search matrix.
Narrowing is an operator selection only; startup contract tests always validate
the complete committed arm inventory.

## Run

Copy `env.example` to ignored `env`, set host-local paths, then run:

```sh
bash query-measure.sh measure plex-ondeck
bash query-measure.sh canary plex-guid-like
bash query-measure.sh canary emby-dashboard
```

The configured source must be an offline/copy database, never the live Plex or
Emby database. The engine uses `mktemp -d` to create and own one unique run
directory, then copies the main database and an existing `-wal` sidecar before
any SQLite open. Measured SQL starts with `PRAGMA query_only=1`. Optional
candidate index creation (`PREPARE_PLEX_INDEXES` /
`PREPARE_DASHBOARD_INDEXES`) and `ANALYZE` operate only on the throwaway copy.

For the supplied runtime-matching CLI, Plex On-Deck and taggings SQL are valid.
That binary is the generic SQLite 3.53.3 build and has no ICU, so it cannot
parse `collate icu_root` and is invalid for `plex-guid-like`. The
`plex-guid-like` family requires an ICU-enabled engine matching the deployed
Plex library. It checks `ENABLE_ICU` and `icu_load_collation()` at runtime and
fails closed before derivation when either capability is absent. Do not use the
generic CLI as evidence for Plex ICU search, ranking, collation, tokenizer, or
GUID LIKE behavior.

`plex-guid-like` is not a latency canary. ICU overloads `like()`, so the
non-NULL prefix case keeps the same covering-index scan before and after the
guard. Its identity and exact EQP detail must match; its timing ratio is
informational and a value near 1.0 is expected. The NULL-pattern case asserts
statement work: candidate full-scan steps must be zero and candidate VM steps
must be at most one percent of vendor VM steps. Its canary verdict ignores the
latency ratio.

## Identity

Identity is a hard gate before EQP, work comparison, or timing. Every family
compares all result columns or its explicit grouped identity grain. Empty cells
fail unless a family declares and proves a narrower exception.

Plex On-Deck declares one exception: when multiple eligible rows share the
maximum `viewed_at`, SQLite's vendor aggregate may choose any tied bare-column
tuple while the ranked candidate chooses a deterministic tuple. The identity
gate still requires the same ids and `max(viewed_at)`, the ranked oracle tuple,
and a vendor tuple that exists among the tied maximum rows. A synthetic fixture
must report exactly one accepted and zero invalid divergences at startup.

Plex GUID LIKE declares the narrower empty-cell exception required by SQL NULL
semantics: both arms must return zero rows for the NULL pattern. A synthetic
fixture proves that exception at startup. The non-NULL prefix case remains
non-empty and compares the complete result multiset.

### Emby dashboard identity limitations

The strict Emby dashboard identity gate has two canary limitations:

- Boundary-tie false-fail: vendor and candidate order groups by maximum
  `DateCreated` with `LIMIT 12` and no deterministic group tiebreaker. When many
  groups tie across that boundary, the arms can choose different arbitrary tied
  subsets and report one missing plus one extra row. This is measurement
  nondeterminism in the vendor query, not a rewrite divergence.
- Unexercised-divergence pass: a derived cell can contain only singleton groups.
  An identity pass then does not exercise representative selection, including
  the movies MIN-to-MAX Latest semantic divergence. The authoritative gate for
  that divergence is the CI smoke fixtures under
  `tests/fixtures/emby-fts-rewrite/`, including
  `latest-movies-played-limit12.expected.sql`.

## Bound Parameters And RAW Prepare Form

Plex On-Deck keeps `library_section_id` as `?1` and `account_id` as `?2` in the
standalone raw statement. Emby search keeps the derived user as `?1` and the
derived MATCH expression as `?2`. SQLite CLI `.parameter set` binds the values
before prepare; plans are never inferred from literal substitution for those
slots.

Plex GUID LIKE keeps the pattern as `:1` and limit as `:2`. The NULL cell is the
production semantic under test. The non-NULL prefix and positive limit are
derived from the copied database, then bound before prepare.

On-Deck id lists and thresholds remain database-derived literals because those
selectors are literal grammar in the matched production raw SQL. Emby dashboard
and fan-out cells also remain derived literals where the corresponding matcher
explicitly rejects bind parameters. This distinction is recorded in
`literals.tsv`.

## Structural Coverage Contract

The harness uses contract testing plus a complete Cartesian-product case
inventory. `plex-ondeck.sh` reads the matcher selector enum and shipped join
form, then refuses startup unless both source selectors (`IDS`, `THRESHOLD`)
map to both join-form cases and each shipped-arm declaration matches the source
form. Adding a selector changes the source count and blocks every harness run
until a measurement arm is added. Changing the source join form blocks every
run until both selector declarations move with it. Other providers similarly
compare their committed case/mode inventory with source mode anchors.

`plex-guid-like.sh` reads the shipped vendor and candidate byte strings from
`src/plex_fts_rewrite.c` and refuses startup on any drift. Its two-case arm
matrix also fixes the shipped NULL case to `WORK` and the shipped non-NULL case
to `PLAN`; neither arm can silently fall back to the default latency policy.

This is established consumer-driven contract testing: the rewrite source is the
producer contract and the measurement manifest is the required consumer. It is
kept as a startup check because this task cannot modify `src/` or `tests/` to
move the contract into a compiled unit test.

## Outputs

Each run emits structured sections for:

- `manifest.tsv` - binary/source hashes, compile profile, stat state, policy.
- `literals.tsv` - database-derived cells and selection rules.
- `identity.tsv` - hard-gate verdict and mismatch counters.
- `eqp.tsv` - every EQP detail row for both arms.
- `plans.tsv` - exact-plan gates for cases whose family policy is `PLAN`.
- `work.tsv` - full-scan-step and VM-step counters plus family work assertions.
- `timings.tsv` - warm/timed iterations, timeouts, and skips.
- `medians.tsv` - warm medians, ratio, and timeout verdict.
- `verdicts.tsv` - per-case canary policy, SHIPPED/COMPARISON role, and verdict.
- `status.tsv` - mode verdict and informational comparison-arm failure count.

By default the EXIT/signal trap deletes the complete run directory. Set
`KEEP_OUTPUTS=1` only when the scratch root is uncommitted and a durable capture
is required, including a failed run; the trap still deletes the copied database
and sidecars. HUP, INT, and TERM retain conventional nonzero signal statuses.

## Guardrails

- Every SQLite invocation goes through the engine's timeout runners. A startup
  source grep blocks uncapped direct invocations and family files are forbidden
  from invoking SQLite.
- Disk preflight requires `2 * (main database + existing WAL) +
  DISK_MARGIN_BYTES`. One working copy is used because these candidates change
  SQL text; optional index/analyze changes are applied only to that copy.
- The engine pre-warms the database copy before every timing pair.
- Candidate timeouts skip remaining iterations. Identity or EQP timeout blocks
  timing. Canary latency, work, and plan policy failures emit `STATUS FAIL`.
- Queries may retain leading blank lines and `--` provenance comments. The first
  executable line must start with `SELECT` or `WITH`; prefixing a raw statement
  with `CREATE TEMP TABLE ... AS` is rejected before execution.
- Stats probes check `sqlite_schema` before reading optional `sqlite_stat1` or
  `sqlite_stat4` tables and record `ABSENT` when either table does not exist.
- HUP, INT, and TERM exit nonzero through the same bounded EXIT cleanup path.
