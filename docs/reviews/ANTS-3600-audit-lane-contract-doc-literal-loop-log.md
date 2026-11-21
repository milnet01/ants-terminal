# ANTS-3600 — Audit lane: contract-doc literal-drift catcher — review loop log

Record for [`../specs/ANTS-3600-audit-lane-contract-doc-literal.md`](../specs/ANTS-3600-audit-lane-contract-doc-literal.md) § Cold-eyes loop log.

## Cold-eyes loop log

**Loop 1 (2026-07-24)** — CRITICAL 1 · HIGH 1 · MEDIUM 1 · LOW 2 (all verified, 0 unverified). Independent cold reviewer (sonnet lane), findings verified against live source, all fixed:
- **CRITICAL** — INV-8 / § 2.1 claimed the lane runs in the headless `auditrunner` path via a generic `inProcessRunner` dispatch. Verified false: `auditrunner.cpp` has zero `inProcessRunner` refs; in-process lanes are GUI-dialog-only (`auditdialog.cpp:4361`). Rescoped INV-8 to a caller-agnostic free-function contract; documented the GUI-only reach; filed the pre-existing headless gap as **ANTS-3605** and cited it in § 6.
- **HIGH** — `buildProjectSourceBlob` has a 2nd 1-arg call-site, `debtsweepengine.cpp:156` (not just `runSpecDriftCheck`). The `BlobOptions` refactor would break the build without a default arg. Added `= {}` default; named both call-sites in § 2.1 / INV-7.
- **MEDIUM** — § 2.3's filename-manifest resolves any quoted filename only if the append is decoupled from the `kExts` extension gate (the walk `continue`s on non-`kExts` before any handling). Specified the manifest records every walked path regardless of `kExts`.
- **LOW** — § 7 hedged "if `subsystems.md` enumerates lanes"; it does (`subsystems.md:68`) — stated definitely. § 4 didn't name the test bundle — named `test_audit` (`CMakeLists.txt:1723`). Citation nits corrected (`auditdialog.cpp:474`→`:476`; `featurecoverage.cpp:334`→`:335`, `:345`→`:340`).

**Loop 2 (2026-07-24, cold — reviewer un-briefed on loop-1 fixes)** — CRITICAL 1 · HIGH 0 · MEDIUM 1 · LOW 3 (all verified, 0 unverified). Loop-1 fixes all held (none resurfaced; reviewer verified them sound). New findings, all fixed:
- **CRITICAL** — the loop-1 manifest fix decoupled the append only from the `kExts` gate, but the walk's *earlier* name-based `continue`s (`spec.md` `:335`, `ROADMAP.md`/`CHANGELOG.md` `:340`) fire first, so those files' paths never reached the manifest — and `` `ROADMAP.md` `` / `` `CHANGELOG.md` `` / `` `spec.md` `` are cited in backticks dozens of times across `docs/specs/` (this spec's own § 7 cites `` `CHANGELOG.md` ``), so every one would false-flag day one. Rewrote § 2.3 to a two-phase walk (unconditional manifest append **before all** gates); INV-2 now tests the name-excluded + extensionless literals.
- **MEDIUM** — the path-widened charset (`:` + `/`) captures whole URLs; added a scheme-prefix URL guard to `extractDocLiteralTokens` (§ 2.4).
- **LOW** — added a latency budget (§ 5, dim 9); citation precision (`featurecoverage.cpp:276`→ list-start, `"*.md"` at `:285`; `CMakeLists.txt` bundle `:1723` vs source-line `:1738`).

**Loop 3 (2026-07-24, cold)** — CRITICAL 0 · HIGH 0 · MEDIUM 1 · LOW 4 (all verified, 0 unverified). Loop-2 fixes all held. Convergence trend 2C/3H → 1C/1H → 0C/0H:
- **MEDIUM** — § 1 framed both ANTS-3598 + ANTS-3599 as the motivating class but only case 2 is caught; a §1-skimming reader could over-trust. Added an explicit scope caveat to § 1 (case 2 resolved, case 1 human-only).
- **LOW** — `subsystems.md:68`→`:67` (bullet start); stated the allowlist match semantics (exact case-sensitive); INV-9 now covers the partial-dir case (only one of `docs/standards`/`docs/specs` present); documented the manifest substring-collision false-negative as a § 6 limitation (narrow widening of the existing `existsInSource` containment tradeoff).

**Loop 4 (2026-07-24, cold)** — CRITICAL 1 · HIGH 1 · MEDIUM 1 · LOW 1 · INFO 1 (all verified). Deeper cold read; loop-3 fixes held but two were themselves incomplete:
- **CRITICAL** — the allowlist file `.ants_doc_drift_allow.txt` is a hidden `*.txt` at root, so the `QDir::Hidden` walk with `"*.txt"` in `kExts` would read its **content** into the blob → `existsInSource` self-resolves any token it mentions (incl. `#` comments), masking real drift and defeating the tested allowlist mechanism. Added the file to the content-exclusion name check (§ 2.6 / § 2.3); its path may still appear in the manifest.
- **HIGH** — the lane would flag its **own** spec's illustrative tokens (INV examples) and every spec's **forward-references to not-yet-built code** on first run. Added § 2.7 (illustrative + forward-reference tokens are expected findings; allowlist + fencing mitigation the ROADMAP anticipated; `Severity::Minor` + human-triage + non-blocking; rollout seed) and surfaced the **`docs/standards`-only vs both-dirs scope** as an Open Question for sign-off.
- **MEDIUM** — divergence from ANTS-3601 on the `.ants/project.json` `docs_dir`/`specs_dir` override left unstated → § 2.7 documents the intentional fixed-path design (an Ants-Terminal audit lane, like `spec_code_drift`'s fixed `tests/features/`).
- **LOW** — URL scheme-guard matched case-insensitively (`HTTPS://…`). **INFO** — INV-8's GUI-dispatch clause isn't directly tested (matches `spec_code_drift` precedent; left).

**Loop 5 (2026-07-24, cold, neutral prompt)** — CRITICAL 0 · HIGH 0 · MEDIUM 1 · LOW 1 · INFO 1 (all verified). Loop-4 fixes all held. No CRITICAL/HIGH:
- **MEDIUM** — § 6 called the manifest false-negative "narrow," but the manifest records **every** walked path (~1,500 files), so the real collision surface is whole-tree path text. Reworded § 6 to state the true confidence level for a human triaging "no finding."
- **LOW** — pointed the `Blocked by` line + § 2.4 at ANTS-3601 § 2.2 for the `markdownscan.h` interface (no separate ANTS-3603 doc exists). **INFO** — the § 2.7 rollout seed step has no acceptance test (deliberately a manual gate; left).

**Loop 6 (2026-07-24, cold, confirmation)** — CRITICAL 0 · HIGH 0 · MEDIUM 1 · LOW 3 (all verified). No CRITICAL/HIGH:
- **MEDIUM** — the § 2.5 runner called `loadAllowlist` but § 2.1 named only three additions and gave it no signature or parsing contract. Named it as the **fourth** addition (`QSet<QString> loadAllowlist(const QString&)`) and pinned the file-format edges (trim, CRLF, whole-line-only `#` comments) in § 2.6 + INV-6.
- **LOW** — `BlobOptions` call sites now use **designated initializers** (`{.includeMarkdownContents=false, .appendPathManifest=true}`) to avoid a silent field-swap (global CLAUDE.md § 5 idiom); corrected § 5's "a few KB" manifest estimate to ~tens of KB (~1,500 files); split a dense § 6 sentence.

**Loop 7 (2026-07-24, cold, final)** — CRITICAL 0 · HIGH 1 · MEDIUM 0 · LOW 0 (verified). One integration gap:
- **HIGH** — the `AuditCheck` registration left `OutputFilter::maxLines` at its **100** default. The `inProcessRunner` output flows through the same `handleCheckOutput` → `applyFilter` path as shell lanes (`auditdialog.cpp:4361`), which silently `break`s past 100 findings — so on the FP-heavy ~200-spec corpus (§ 2.7) the seed pass would see only the first ~100 *alphabetically-sorted* docs and mask drift in the rest, forever. Set `c.filter.maxLines = 0` (uncapped; `applyFilter`'s `f.maxLines > 0` guard) + reasoning in § 2.2 + INV-10 (a registration source-grep, since the feature test bypasses `applyFilter`).
