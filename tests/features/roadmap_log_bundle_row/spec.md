# Feature: `roadmap_log op:bundle_row`

**ANTS-1691.** A write verb that appends a row to a Markdown table held
under a named section heading (the recurring "## 📊 Bundle progress"
table that RetroArch and other cross-session reporters maintain). Born
from a 7+-session recurring ask: closing a bundle meant a hand-`Edit` of
the table — the only write that did not go through an MCP verb — which
risks pipe-corruption on cells that themselves contain `|`.

This is a single-verb behavioural contract, not a multi-file design doc,
so it lives only here (no `docs/specs/ANTS-1691.md`).

## Scope

- `RemoteControl::cmdRoadmapLogBundleRow` — handler for
  `op:"bundle_row"`. m_main-independent (caller_cwd + filesystem only),
  one read + one atomic `QSaveFile` commit.
- `RemoteControl::cmdRoadmapLogBundleRowForTest` — test seam, mirrors
  `cmdRoadmapLogCreateSectionForTest`.
- `cmdRoadmapLog` dispatch ladder routes `op:"bundle_row"` to it
  (before the `m_main` guard, like create_section / append_batch).
- `bad_op_combo` error string lists `bundle_row`.

## Request

| field | req? | meaning |
|---|---|---|
| `caller_cwd` | yes | project $PWD; resolves ROADMAP.md |
| `op` | yes | `"bundle_row"` |
| `section` | yes | slug of the heading containing the table (e.g. `bundle-progress`) |
| `cells` | yes | ordered array of ≥1 strings — the row's cell values left→right |
| `header` | no | ordered array of column names; used to CREATE the table when the section has none yet |
| `position` | no | `"end"` (default) \| `"sorted"` |
| `sort_col` | no | 0-based column index for `position:"sorted"` (default 0) |

## Invariants tested

- **INV-1** dispatch — source-grep `op == "bundle_row"` routes to
  `cmdRoadmapLogBundleRow` in `cmdRoadmapLog`; the route is BEFORE the
  `m_main` guard.
- **INV-2** required fields → `missing_field` for absent `caller_cwd` /
  `section` / `cells`, and for an empty `cells` array.
- **INV-3** unknown `section` → `bad_section`; case-only mismatch →
  `bad_case` + `canonical_slug` (parity with create_section).
- **INV-4** pipe escaping — a `|` inside any cell becomes `\|` in the
  emitted row; the row's column count is unchanged (no split on the
  literal `|`).
- **INV-5** newline escaping — a `\n` inside any cell becomes `<br>`
  (Markdown table cells cannot contain a raw newline).
- **INV-6** `column_mismatch` — when the table exists, `cells.size()`
  must equal the table's column count; otherwise refuse without writing.
- **INV-7** find-or-create — section present but no table, `header`
  given → a fresh `| h1 | h2 |` header + separator + the data row are
  written; envelope `created_table:true`.
- **INV-8** no table + no `header` → `no_table` (refusal, no write).
- **INV-9** append position — with `position:"end"` (default) the new
  row is inserted immediately AFTER the table's last existing data row
  and BEFORE any following section heading.
- **INV-10** sorted insert — `position:"sorted"` inserts the row so the
  `sort_col` column stays in ascending order under numeric-aware
  comparison (`QCollator` numeric mode): `9` sorts before `78`.
- **INV-11** atomic write — source-grep: handler's only write path is
  `QSaveFile` … `.commit()`.
- **INV-12** success envelope — `{ok:true, file:"ROADMAP.md", section,
  row_index (1-based among data rows), columns, created_table,
  bytes_written>0}`.
- **INV-13** (ANTS-3432) schema wiring — source-grep
  `src/claudeintegration.cpp`: the roadmap_log inputSchema `properties`
  DECLARES `cells`, `header`, `position`, `sort_col`. Without this, the
  schema's `additionalProperties:false` makes the MCP client strip the
  bundle_row args, so `cells` reaches the handler empty and every real
  call refuses `missing_field` (the ANTS-1691 ship-vs-reality gap: the
  INV-1…12 handler seam bypassed the schema, so the wiring gap shipped
  invisibly).

- **INV-14** (ANTS-3798) `dry_run` — `dry_run:true` returns the resolved
  preview and leaves `ROADMAP.md` **byte-identical**; the envelope carries
  `dry_run:true`, `op:"bundle_row"`, the rendered `row`, and `bytes`
  (would-be) **in place of** `bytes_written`, which is the signal a caller
  keys on to tell a preview from a write. Two legs, and the second is the
  one that matters:
  - the preview cannot drift from the write — running the same request for
    real yields the identical `file` / `section` / `row_index` / `columns` /
    `created_table`, and `bytes == bytes_written`;
  - the preview resolves the **sorted** placement. `position:"sorted"` is
    the one value a caller cannot predict, since it depends on a
    numeric-aware collation against rows already in the table, and a gate
    placed *before* the placement logic would still return `ok:true` with a
    plausible envelope. Inserting `40` into a table holding `9` and `78`
    must preview `row_index:2` — not 3, where a plain append lands it, and
    not 1, where a codepoint compare would.

  bundle_row was the **last** roadmap_log write op with no preview:
  ANTS-2077 added one to `append`/`append_batch`, ANTS-2136 swept `flip`,
  `flip_batch`, `annotate`, `create_section` and `amend_body`, and this op
  was missed. It is a poor one to miss — a mismatched column count is
  refused up front, but a wrongly-sorted or wrongly-escaped cell is not, so
  a mangled Markdown table is exactly the outcome worth seeing first.

  The gap also had a documentation half, and that is how it stayed
  invisible: the schema's `dry_run` description read *"when true on
  op:append / append_batch"* long after ANTS-2136 had extended it, so a
  reader checking whether bundle_row supported the flag found a doc
  implying nothing did except append. The description now **enumerates**
  every op, so the next one added is either listed or visibly absent.

## Method

`QTemporaryDir` holds a synthetic `ROADMAP.md` per test; the test calls
`cmdRoadmapLogBundleRowForTest(req)` directly, asserts the envelope and
the resulting file bytes. Source-grep tests check dispatch + atomic-write
wiring against the remotecontrol TUs.
