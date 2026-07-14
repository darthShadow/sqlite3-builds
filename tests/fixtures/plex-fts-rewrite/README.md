# Plex FTS Rewrite Fixtures

Raw parameterized statement templates and expected prepare-wrapper outputs for
the Plex build-time smoke. Sibling `*.expected.sql` files are the exact
`sqlite3_sql(stmt)` output expected after preparing through the wrapper.
Files end with one repository text-framing LF; the smoke removes that one byte
from each payload because the production On-Deck prepare form and emitted SQL
have no terminal whitespace.

## Inventory

- `ondeck-bound-section` covers the production-representative Variant A form
  with bare `library_section_id=?`, a literal integer id list, and literal
  `account_id=42`. The expected output preserves the bare parameter token.
