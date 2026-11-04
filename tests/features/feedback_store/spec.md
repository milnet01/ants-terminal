# The feedback corpus in the roadmap store — feature-conformance contract

Full design:
[`docs/specs/ANTS-4513-feedback-corpus-store.md`](../../../docs/specs/ANTS-4513-feedback-corpus-store.md).
This bundle holds its INV-1 to INV-10. It uses a temporary store and corpus,
never the live ones: the verbs' store sits under the bundle main's
`XDG_DATA_HOME` sandbox.

## Fixtures

- `corpus_mixed.md` — trimmed from two corpus files. It has prose before a
  first finding, compaction stubs, a closure, a mapped finding with two ids,
  an untriaged finding whose Note line names an id, an awaiting finding, a
  `###` block with no Proposed ID line, a prose `###` block, and a `###` line
  inside a fence.
- `corpus_crlf_no_eol.md` — CRLF line endings and no final newline.
- `golden_create.md` — what the file-backed `append_finding` wrote for a new
  file, made before the store change (INV-9).

## What this test asserts

- **INV-1** — import stores each fixture's bytes exactly, and the export
  writes them back exactly.
- **INV-2** — the untriaged and awaiting rows are the findings in `parse()`'s
  delta; awaiting and untagged rows match `awaiting` and `suspectedUntagged`.
  A Note line's id is not an id.
- **INV-3 and INV-5** — after each surviving write op, the rows are a rebuild
  of the stored text, and the file is that text, with its hash stored and
  mode 0444.
- **INV-4** — `feedback_query`'s delta fields equal `parse()` on the file's
  bytes.
- **INV-6** — after a hand edit, every write op refuses `export_drift` and
  changes neither the file nor the rows. `feedback_query` reports
  `export_drift`. `reimport` refuses without `confirm` and settles it with
  it.
- **INV-7** — two appends from two connections, released together, both
  land in the store and the file.
- **INV-8** — a file is imported once, in sequence and from two threads at
  once, and both concurrent calls succeed.
- **INV-9** — a first filing writes `golden_create.md` byte for byte and
  registers its reporter.
- **INV-10** — each retired op refuses `retired` and changes neither the file
  nor the store.
