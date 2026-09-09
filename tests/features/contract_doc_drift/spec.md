# contract_doc_drift — contract-doc ↔ code literal-drift lane (ANTS-3600)

Full design contract: `docs/specs/ANTS-3600.md`. This test locks the runtime
behaviour of `FeatureCoverage`'s two drift lanes and the registration of the
`contract_doc_drift_standards` and `contract_doc_drift_specs` audit checks.

Each lane extracts back-ticked literals from the `*.md` files under ONE
contract-doc directory and reports the ones that appear in no non-`.md` source
file and match no real file path in the tree — the drift that escaped review in
ANTS-3598/3599.

**One lane per directory is ANTS-3849's fix and is load-bearing.** A single
runner over both directories put ~1,100 specs findings and ~64 standards
findings in one category, which was over 80% of the audit report; nobody read
it, so anything genuine inside it was invisible. The split suppresses nothing
— both lanes report — it makes the small half legible on its own.

## Invariants (mapped to `docs/specs/ANTS-3600.md`)

- **INV-1** — an unknown back-ticked symbol flags. *Test:* `DriftAndResolve`.
- **INV-2** — a literal naming a real file (incl. name-excluded ROADMAP.md /
  CHANGELOG.md and an extensionless LICENSE) does not flag; deleting the files
  makes all three flag. *Test:* `RealFilenamesResolveViaManifest`.
- **INV-3** — a literal living only in another doc's prose flags
  (`.md` bodies are excluded from the blob). *Test:* `DriftAndResolve`.
- **INV-4** — a literal inside a fenced code block is not extracted; inline is.
  *Test:* `FencedTokensSkipped`.
- **INV-5** — a slash-path literal absent from sources flags (path-widened
  charset). *Test:* `DriftAndResolve`.
- **INV-6** — an allowlisted token never flags; `#`-comment / blank lines are
  ignored; padding is trimmed; deleting the entry re-flags. *Test:*
  `AllowlistSuppresses`.
- **INV-9** (ANTS-3849 form) — each lane is a silent no-op when **its own**
  directory is absent, and reports when it is present. A specs-only tree leaves
  the standards lane empty. *Test:* `NoOpAndPartialDir`.
- **INV-10** — **both** registrations leave the output filter uncapped
  (`maxLines = 0`). The grep needles are the quoted ids, not the shared
  `contract_doc_drift` prefix, which would match the first block twice and
  never inspect the second. *Test:* `RegistrationUncapped` (source-grep).
- **INV-11** (ANTS-3849) — a `<head>:<line-or-json-literal>` citation
  (`remotecontrol.cpp:2540`, `src/vault.py:39-49`, `applyTheme:3118`,
  `sections_checked:false`) is not extracted; the bare path still flags
  (INV-5) and a head carrying a colon (`Gone::method`) is not a citation.
  *Test:* `PathLineCitationsSkipped`.
- **INV-12** (ANTS-3849) — the two lanes **partition** the corpus: a drifting
  token in each directory is reported by its own lane and by neither the other.
  Asserted in both directions, because a lane that still scanned both dirs
  would pass a one-directional check. *Test:* `LanesPartitionByDirectory`.

INV-7 (blob refactor behaviour-preserving) and INV-8 (caller-agnostic
free function) are covered by the existing `feature_coverage` +
`debt_sweep_engine` suites staying green and by this test driving the runner
headlessly.

## Must fail first

Before the lane runners / `extractDocLiteralTokens` / `loadAllowlist` /
`BlobOptions` exist, this test does not compile (feature-absent RED); after
implementation it passes. Verified by a deliberate sabotage of the
closer-resolution path before restore.

For INV-12 and the ANTS-3849 form of INV-9 the RED is behavioural, not a
compile failure: point both lane runners back at a both-directories scan and
`LanesPartitionByDirectory` fails on the cross-directory legs while
`NoOpAndPartialDir`'s standards-empty assertion fails on the specs-only tree.
