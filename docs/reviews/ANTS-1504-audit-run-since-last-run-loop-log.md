# ANTS-1504 — `audit_run` since-last-run scope (re-scan only changed files) — review loop log

Record for [`../specs/ANTS-1504-audit-run-since-last-run.md`](../specs/ANTS-1504-audit-run-since-last-run.md) § Cold-eyes loop log.

## Cold-eyes loop log

- **Loop 6 (2026-05-25) — clean-confirm pass, fresh cold reviewer
  (briefed to verify the contract against code, not to trust this log).**
  **0 CRITICAL / 0 HIGH / 0 MEDIUM / 0 actionable LOW.** Every cited
  file:line, cross-doc quote, the two-dot git-range notation, and the
  §2.7↔INV-6 demotion-reason enum independently re-verified against live
  source. Residue: one non-actionable LOW (the §2.2 `<merge-base>` cell is
  dense but unambiguous — reviewer said no fix required) and two INFO
  code-side deferrals the spec already surfaces (the `auditrunner.h:81-82`
  / `mainwindow.cpp:4255-4257` "computes the delta" comments to correct at
  implementation; ANTS-1555 §5 header-member drift on its own pass).
  Converged → **accepted**.
- **Loop 5 (2026-05-25) — confirming pass, fresh cold reviewer.** 0
  CRITICAL / 0 HIGH material. Fixed: (bookkeeping) Status token said
  "loops folded" while the log showed only Loop 0 → recorded loops 1-5 +
  pinned the count; (MEDIUM) Source line cited the RetroDB section date
  (2026-05-17) while the ROADMAP item's `Source:` is
  `in-session-2026-05-18` → reconciled; (LOW) §2.8 empty-changeset
  envelope now lists the always-on `scope_resolved` +
  `changed_files_count:0` for INV-8 alignment. Converged.
- **Loop 4 (2026-05-25) — confirming pass, fresh cold reviewer.** 0
  CRITICAL. Fixed: (HIGH, self-inflicted by loop-2/3) INV-6's reason enum
  omitted `no_merge_base` which §2.7 had added → INV-6 now carries the
  full five-reason set with per-scope mapping; (MEDIUM) Status token
  normalised toward the specs.md form; (LOW) envelope example sha
  `a0bbe4a` (a real commit) → `<short-sha>` placeholder.
- **Loop 3 (2026-05-25) — confirming pass, fresh cold reviewer.** 0
  CRITICAL / 0 HIGH. Fixed (MEDIUM): §2.2 over-claimed ANTS-1351:52
  "already specifies" the `files` `..HEAD` form (it specifies only the
  merge-base) → softened; §7 descriptor cite `:4362-4365` straddled the
  block → `:4355-4369`; header `Pairs with: ANTS-1555` → `Blocked by:`
  (1555 declares "Blocker for: ANTS-1504"). (LOW) §2.8 added the `:1444`
  cache-gate cite.
- **Loop 2 (2026-05-25) — 2 fresh cold reviewers.** 0 CRITICAL. Fixed:
  (HIGH) §5 supersession clause quoted ANTS-1555 §4 text that loop 1 had
  already rewritten → de-quoted; (HIGH) INV-1 used the space form
  `<anchor> HEAD` against §2.2's load-bearing two-dot → unified to `..`;
  (MEDIUM) §4/§2.1 `runGit` worst case is ~3 s (1.5 s start + 1.5 s
  finish), not 1.5 s; §7 pointed at `mcp-tools.md` for a `scope`
  over-claim that lives in the descriptor; `toolArgv` call cite
  `:1325`→`:1324-1326`; ROADMAP "Update" still said scoped paths "flow to
  every tool" → corrected to cppcheck+clang-tidy; added `no_merge_base`
  demotion for `files`; pinned the empty-changeset early-return before the
  cache block.
- **Loop 1 (2026-05-25) — 2 fresh cold reviewers.** 0 CRITICAL. Fixed:
  (HIGH) §2.2 git ranges diverged from ANTS-1351:52 (missing the `files`
  merge-base + `HEAD~1`/full fallback) → aligned; (HIGH) ANTS-1555 §4 +
  ROADMAP expected ANTS-1504 to ship carry-forward + delta, which §5
  defers → reconciled across all three docs; (MEDIUM) §2.5 clazy argv
  dropped `-checks=level1` / wrong tool key → corrected; `commit`
  attribution `auditcache.cpp:163` (a read) → write at
  `auditrunner.cpp:1447`; `RunResult` field cite `h:62`→`h:84`. Surfaced
  (not auto-fixed): two code comments (`auditrunner.h:81-82`,
  `mainwindow.cpp:4255-4257`) claim ANTS-1504 "computes the delta" — now
  deferred, fix at implementation; ANTS-1555 §5 lists
  `ManifestEntry`/`cacheDirFor`/`cachePathFor` absent from the shipped
  header (sibling-spec drift, its own pass).
- **Loop 0 (2026-05-25) — grounding + self-review.** Verified every cited
  symbol against live source: `RunRequest.scope` (`auditrunner.h:36`),
  the `scope`-is-cosmetic claim (consumed only at `auditrunner.cpp:1101`
  + `:1452`; `toolArgv` narrows only via `scopedPaths` for cppcheck
  `:285` / clang-tidy `:305`), `isAuditArgSafe` (`:208`), `kExclusions`
  (`:97`), the run loop + `toolArgv` call (`:1275`-`1327`, call
  `:1324`-`1326`),
  `recordRun` call (`:1481`), `AuditCache` manifest commit read/write
  (`auditcache.cpp:137`/`:163`) + `runGit` pattern (`:39`), envelope
  builder + `tools_skipped`/`prior_run` (`mainwindow.cpp:4246`/`:4260`).
  Internal-consistency: § 2 surface ↔ § 3 INV-1..9 (anchor table ↔
  INV-1/2; language map ↔ INV-3; global-tool skip ↔ INV-4; path gate ↔
  INV-5; demotion ↔ INV-6; empty short-circuit ↔ INV-7; envelope ↔
  INV-8; pure/impure ↔ INV-9). No duplication: no existing scope narrows
  files, so this is additive. RAM cap named (§ 4). Three forks parked in
  § 8 (signed off pre-draft via brainstorming). Delta deferral made
  explicit (§ 5) rather than half-shipped. Open for a cold reviewer pass.
