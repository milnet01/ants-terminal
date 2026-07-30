# doc_integrity_verb — the doc_integrity MCP verb (ANTS-3601)

Full design contract: `docs/specs/ANTS-3601.md` § 2.6. This test locks the
verb-layer behaviour (path→relDocs enumeration, the `kinds` filter) and the
registration wiring. Behavioural INVs drive the extracted pure helpers and
INV-10 source-scrapes the wiring (the `rc_get_text_byte_cap` pattern).

The original reason for that split — "the handler needs a live MainWindow" —
was not a property of the handler. It was ANTS-3725: `resolveRootCanonical`
dereferenced a null `MainWindow` that its own caller had already guarded, so
`RemoteControl rc(nullptr)` segfaulted rather than answering. That is fixed;
a handler-level test here is now possible, and this split is retained only
because the pure helpers are the sharper place to pin these two invariants.

## Invariants

- **INV-17** (ANTS-3737) — `docSetDigest` is content-sensitive. The verb's
  ETag is a sha256 of the response envelope, and doc_integrity /
  doc_symbols / spec_lint report FINDINGS rather than content — so a doc
  edit that changes no finding left the envelope byte-identical and
  `etag_match` answered a false 304, skipping the post-fix re-check that
  exists to catch a newly-introduced finding. The digest covers
  (relpath, size, mtime_ms) per checked doc: stable across an unchanged
  re-run (the 304 must keep working), different after a content edit, and
  different when the checked SET changes.

- **INV-16** — directory `path` scoping is exact (recursive `*.md` under that
  dir only), a file `path` → one doc, an omitted `path` → the `docs_dir` walk
  (not root files), a non-existent in-root `path` → empty. *Test:*
  `EnumerateScoping` over `RemoteControl::docIntegrityEnumerate`.
- **INV-18** — the `kinds` filter narrows both `findings[]` and `counts{}`,
  across all four kinds including `heading_sequence` (ANTS-3700). The counter
  is a `switch` over `Kind`, not an `if/else` chain whose final `else` would
  silently tally any future kind as a `toc_gap`. *Test:*
  `KindsFilterNarrowsCounts` over
  `RemoteControl::docIntegrityBuildResponse`.
- **INV-10** — the verb is registered with the `Required` caller_cwd contract,
  is ETag-supported, and its handler validates `path` (refusing `bad_path` on a
  root escape). *Test:* `WiringRegistered` (source-scrape of mainwindow.cpp,
  claudeintegration.cpp, remotecontrol.cpp).

## Known residue — markdown a doc quotes on another file's behalf

ANTS-3638(c), accepted rather than fixed. A relative link is resolved from
the **citing doc's own directory**, which is right for every ordinary link
and wrong for one narrow case: a spec that drafts text destined for a
different file. `docs/specs/ANTS-1894.md:851` writes
`` [`docs/specs/ANTS-1894.md`](docs/specs/ANTS-1894.md) `` inside its
*Cross-doc impact* section as draft prose for `CHANGELOG.md`. From the repo
root that path is correct — `CHANGELOG.md:1752` shipped the identical link —
but resolved from `docs/specs/` it probes `docs/specs/docs/specs/` and
reports a `broken_link`.

The doc is **not** the thing to fix: the draft is faithful, and rewriting it
would make the copied CHANGELOG entry wrong. The code is not worth changing
either — suppressing it needs a "this block is draft-for-another-file"
marker that no doc in the tree uses, invented for a single finding. So the
class is documented here and left in the output, where a reader can see the
path doubles and dismiss it in one glance.

If this ever grows past a handful of findings, the cheap fix is to also try
resolving a failing relative link **from the repo root** and drop the
finding when that resolves — reported as a distinct kind, never silently.

## Must fail first

Before the verb + helpers exist, this test does not compile (feature-absent
RED); after implementation it passes.
