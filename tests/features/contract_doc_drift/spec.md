# contract_doc_drift — contract-doc ↔ code literal-drift lane (ANTS-3600)

Full design contract: `docs/specs/ANTS-3600.md`. This test locks the runtime
behaviour of `FeatureCoverage::runContractDocDriftCheck` and the registration
of the `contract_doc_drift` audit check.

The lane extracts back-ticked literals from `docs/standards/` + `docs/specs/`
`*.md` files and reports the ones that appear in no non-`.md` source file and
match no real file path in the tree — the drift that escaped review in
ANTS-3598/3599.

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
- **INV-9** — silent no-op with neither docs dir; scans whichever one exists.
  *Test:* `NoOpAndPartialDir`.
- **INV-10** — the registration leaves the output filter uncapped
  (`maxLines = 0`). *Test:* `RegistrationUncapped` (source-grep).
- **INV-11** (ANTS-3849) — a `path:line` or `path:line-range` citation
  (`remotecontrol.cpp:2540`, `src/vault.py:39-49`) is not extracted; the bare
  path still flags (INV-5) and a non-path `head:N` stays in scope. *Test:*
  `PathLineCitationsSkipped`.

INV-7 (blob refactor behaviour-preserving) and INV-8 (caller-agnostic
free function) are covered by the existing `feature_coverage` +
`debt_sweep_engine` suites staying green and by this test driving the runner
headlessly.

## Must fail first

Before `runContractDocDriftCheck` / `extractDocLiteralTokens` / `loadAllowlist`
/ `BlobOptions` exist, this test does not compile (feature-absent RED); after
implementation it passes. Verified by a deliberate sabotage of the
closer-resolution path before restore.
