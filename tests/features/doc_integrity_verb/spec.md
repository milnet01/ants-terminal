# doc_integrity_verb — the doc_integrity MCP verb (ANTS-3601)

Full design contract: `docs/specs/ANTS-3601.md` § 2.6. This test locks the
verb-layer behaviour (path→relDocs enumeration, the `kinds` filter) and the
registration wiring. The handler needs a live MainWindow, so behavioural INVs
drive the extracted pure helpers and INV-10 source-scrapes the wiring (the
`rc_get_text_byte_cap` pattern).

## Invariants

- **INV-16** — directory `path` scoping is exact (recursive `*.md` under that
  dir only), a file `path` → one doc, an omitted `path` → the `docs_dir` walk
  (not root files), a non-existent in-root `path` → empty. *Test:*
  `EnumerateScoping` over `RemoteControl::docIntegrityEnumerate`.
- **INV-18** — the `kinds` filter narrows both `findings[]` and `counts{}`.
  *Test:* `KindsFilterNarrowsCounts` over
  `RemoteControl::docIntegrityBuildResponse`.
- **INV-10** — the verb is registered with the `Required` caller_cwd contract,
  is ETag-supported, and its handler validates `path` (refusing `bad_path` on a
  root escape). *Test:* `WiringRegistered` (source-scrape of mainwindow.cpp,
  claudeintegration.cpp, remotecontrol.cpp).

## Must fail first

Before the verb + helpers exist, this test does not compile (feature-absent
RED); after implementation it passes.
