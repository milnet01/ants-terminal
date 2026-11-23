# ANTS-1351 — `audit_run` MCP verb: server-side audit runner — review loop log

Record for [`../specs/ANTS-1351-audit-run-mcp-verb-server.md`](../specs/ANTS-1351-audit-run-mcp-verb-server.md) § 10. Cold-eyes loop log.

## Cold-eyes loop log

**Loop 1 (2026-05-17):** v1 reviewer raised 4 H, 7 M, 5 L. v2
folded: envelope cap, blocking budget, exclusion shared config (later
reverted in v4), env policy, contract-before-cache.

**Loop 2 (2026-05-17):** three lanes (cross-spec, threat, impl).
v3 folded: namespace → PascalCase, lock primitive → InFlightRegistry
(later inlined in v4), PathValidation everywhere, scope tag
sanitise, PATH-hijack via `findExecutable`, regex hardening, lib
placement reverted to `ants_audit_lib`, QProcess+QEventLoop pattern,
test bundle named.

**Loop 3 (2026-05-17):** five lanes (performance, security,
optimisation, token-savings, final consistency). v4 folded:

- **Perf H1:** acknowledged ROADMAP.md is 763 KiB — affects
  ANTS-1397's fold-in budget, not this spec.
- **Perf H2:** pool size bumped `max(1, nproc/8)` → `max(2,
  nproc/8)` (INV-9). `retry_after_ms` hint added.
- **Perf H3:** cross-pool aggregate RAM documented in § 6.
- **Perf M5:** cold-start cost quantified in § 6.
- **Perf M6:** compiled regex cache (INV-6).
- **Perf L10:** SARIF cap `kSarifFindingsMax = 10000` (INV-13).
- **Sec H-B:** stale-slot reaper (§ 2.4 / INV-11).
- **Sec H-C:** compile_commands.json deep validation deferred (Q4);
  v1 existence-check only.
- **Opt H3:** inline gate replaces class (§ 2.4).
- **Opt M5:** exclusion list hardcoded (INV-6) — dropped new JSON
  file from v3.
- **Opt M7:** `findExecutable` 60 s TTL cache (INV-10).
- **Opt M8:** compile_commands.json no symlink-follow (INV-2).
- **Opt L12:** `formats:["sarif"]` default; HTML opt-in (§ 2.1).
- **Token H1:** § 1 framing corrected (skill load once per session,
  not per sweep).
- **Token H3:** net saving stated ~60 K (not 80 K gross).
- **Token H4:** `top_findings_count` request arg added (§ 2.1).
- **Token M4:** `token_usage` ledger cited for the 80 K figure.
- **Token M5:** `retry_after_ms` hint on `server_busy`.
- **Final consistency:** `tools_skipped` typed `QVector<ToolSkip>`
  not `QPair`; `m_auditPool` name pinned; INV-16 added for
  `cap_per_tool_seconds` validation; loop-log L2/L3 dangling refs
  rewritten as explicit cross-loop pointers.
- **Final consistency on § 2.5 QTimer capture:** snippet clarified —
  captures heap pointer not stack ref.

v4 ready for sign-off. Implementation can proceed; remaining
M/L findings either deferred to v5 (impl-time discovery), filed
as Q4/Q5 (deliberate v1 trade-offs), or non-blocking polish.

**Loop 4 (2026-07-24, ANTS-3605 edit):** two cold passes over the whole
spec + cited code, after INV-14 was relaxed and INV-21 (headless
in-process drift lanes) was added. Pass 1 findings folded (contract
surfaces only, per stay-in-lane):

- **Accuracy H1:** INV-1 aggregate cap 240 s → 900 s (code
  `kAggregateCapMs = 900'000`, ANTS-3585).
- **Accuracy H2:** INV-16 / § 2.1 / § 4 cap range `[5,60]` → `[5,300]`
  (code `kCapPerToolMax = 300`, ANTS-3585).
- **Accuracy M1:** § 2.1 `tools` set gains `clang-tidy` (10th
  `kKnownTools` entry, ANTS-1512); auto-detect basis noted.
- **Accuracy M2:** § 2.1 `scope` enum gains `full` (ANTS-2015) and
  `since-last-run` (ANTS-1870).
- **Clarity L1:** INV-21 "at full scope only" reworded to the actual
  `req.tools.isEmpty() && !scopeNarrowed` gate (default `auto` also runs
  the lanes, not only literal `full`).
- **Consistency M3:** INV-21 numbering note expanded to name the
  cross-doc INV-17 collision + the un-back-ported INV-10b/18/19/20,
  pointing at ANTS-3609.
- **Deferred to ANTS-3609** (systemic pre-existing drift, not an
  ANTS-3605 concern): § 5 test-plan stale test *names* + `240'000`/`61`
  values; § 6 RAM budget "9 tools"/"240 s" + no in-process-lane budget;
  § 2.5 line "9 tools multiplex"; the INV-17 cross-doc renumber +
  back-port of INV-10b/18/19/20 into this canonical spec. These are a
  coherent ANTS-3585/feature-contract reconciliation, tracked separately
  so the ANTS-3605 change stays surgical.

The in-process-lane invariant (INV-21), the INV-14 relaxation, and the
constant corrections are verified against `src/auditrunner.cpp`,
`src/featurecoverage.h`, and `src/auditdialog.cpp`.

**Loop 4 / Pass 2** re-read the doc cold and confirmed every pass-1 fix held
(INV-14, INV-21, the `[5,300]` cap, the `900'000` aggregate, the 10-tool
`kKnownTools` set + scope enum all re-verified against code, no
resurfacing). It surfaced additional **pre-existing** drift unrelated to
ANTS-3605 — a missing `compile_commands_escape` § 4 row (ANTS-1446 ships
but INV-2/Q4 say deferred), stale § 2.3 Engine-API structs, § 2.1
`paths`/`checks` inputs, and § 2.2 `round(` vs integer-truncation — which
together with the pass-1 deferrals amount to a systematic ANTS-1351-audit-run-mcp-verb-server.md ↔
code refresh, tracked in ROADMAP ANTS-3609. The ANTS-3605 change itself
is converged: no verified finding touches what it added or edited.

**Loop 5 (2026-07-24, ANTS-3609 code reconciliation).** Folded the
loop-4 deferrals: § 5 test-plan rewritten to the 30 real
`TEST(mcp_audit_run, …)` names; § 6 tool-count/cap/in-process-lane
budget; § 2.5 tool count; § 2.3 structs refreshed to `auditrunner.h`;
§ 2.1 `paths`/`checks`; § 2.2 integer-truncation; § 4
`compile_commands_escape` row; INV-2 include-validation; back-ported
INV-18/19/20 (ANTS-2185/2182/2183) + INV-22 secret-redaction (ANTS-2188,
the feature contract's INV-17) with the cross-doc numbering note; INV-10b
fold-in. Then a fresh 3-lane cold read (surface / invariants /
tests+budget) **verified all of the above against code** and surfaced
**deeper pre-existing drift** now also folded:

- **INV-9 was fiction** — the `m_auditPool` pool / `server_busy` code /
  `max(2, nproc/8)` sizing were never built (aspirational v2,
  ROADMAP.md:6512). Rewrote INV-9 + § 2.5 + § 4 (`server_busy` →
  `too_many_jobs`) + § 6 + § 8 to the shipped per-call-`QThread` +
  in-flight-gate + `kAuditJobsMax = 16` model. Code follow-up (build the
  v2 pool / bound multi-root RAM): **ANTS-3612**.
- **§ 2.4 reaper** described a non-existent per-slot QTimer; rewrote to
  the lazy sweep-on-acquire + the two shipped helpers
  (`verbInFlightTryAcquire`/`verbInFlightRelease`). Also documented that
  `kVerbInFlightReapMs`/`kAuditJobReapMs` are a stale `270'000` (240+30)
  not tracking the 900 s cap — a real INV-11 bug, code fix **ANTS-3611**.
- **INV-1** `tools.count` → *resolved-tool count* (`toolAbsPath.size()`).
- **§ 2.1 `checks`** refusal clarified (only for an explicitly-named
  non-clang-tidy tool; auto-detect runs unfiltered, no refusal).
- **§ 5** Required-contract sentence corrected (`Inv3RequiredContract` in
  this file, not `mcp_caller_cwd_contracts`).
- **§ 7** skill fast-path marked PROPOSED — the live `audit/SKILL.md` has
  no `audit_run` block; apply it: **ANTS-3613**.

Code-side items surfaced but not fixed here (doc-review lane): ANTS-3610
(`auditrunner.h` header comment lists 9 tools, missing clang-tidy),
ANTS-3611, ANTS-3612, ANTS-3613. Every doc claim in this loop was verified
against `auditrunner.{h,cpp}`, `claudeintegration.{h,cpp}`,
`mainwindow.cpp`, `auditdialog.cpp`, `featurecoverage.h`, the shipped test
file, and the feature contract `spec.md`.

**Loop 5 / Pass 2** re-read the doc cold and re-verified pass-1 held (INV-9,
§ 2.4, INV-1…22 all clean), then corrected finer pre-existing drift the
first pass hadn't reached: § 2.3 CMake citation (`:315` → `:489`) + the
already-shipped `ants_audit_dialog_lib` split (closing Q5); the § 2.4
`verbInFlightTryAcquire` snippet (returns `qint64`/`-1`, not `bool`; the
`already_running` envelope carries `running_since_ms` + `retry_after_ms`);
the § 2.5 `QProcess` ownership (`std::shared_ptr` map, not a "scope-guard
struct"); the § 2.1 `async` request field (ANTS-3396) it had omitted; the
§ 1 net-saving arithmetic (20 KiB envelope ≈ 5 K tokens, not 20 K → ~70–75 K
net); one leftover "pool slot / server_busy" line in § 6; the § 5
INV-7/8/11/12 test-coverage claim (they are unasserted, not
structurally-locked); and the header's ANTS-1397 "worker model" cross-ref.
No new code-side items; all doc-only.

**Pass 3** widened the read-set to `src/auditcache.cpp` (absent from the
pass-1/2 set) and caught the INV-11/INV-12 cluster: INV-12 described a
`/tmp/audit-<sid>-<seq>.sarif` primary path + a `MainWindow::aboutToQuit`
cleanup — both wrong. Rewrote INV-12 (+ the § 2.2 example/table) to the
shipped `<root>/.audit_cache/audit-<iso>-<sha>.sarif` primary
(`AuditCache::sarifPathFor`, ANTS-1555), the `/tmp`/XDG `-<rand>` fallback,
and the real manifest-driven cache reaper (there is no `aboutToQuit`
hook); the un-reaped `/tmp` fallback is a minor leak → **ANTS-3614**.
INV-11's "aggregate-cap + 30 s" reaper phrasing was reconciled with § 2.4's
correct fixed-270 s / ANTS-3611 explanation. The `not_runnable` per-tool
status (§ 2.2 example + table, § 2.3, INV-2) was dropped — the shipped
state machine only sets `ok`/`timed_out`/`crashed` (unresolved tools →
`tools_skipped[]`); the stale `not_runnable`/`scope` header comments in
`auditrunner.h` were folded into **ANTS-3610**. Plus the § 6 tool-side sum
rounding (~1.8 GiB). All doc-only.

**Pass 4** deep-verified the v1 invariants prior passes had only scanned
holistically (INV-3/4/5/6/7/8/15) and caught the same v1-contract-vs-
shipped-headless-engine gap class again in the *suppression* surface:
INV-6's second half (custom-regex hardening + compiled-regex cache) is
wired only into the GUI `AuditEngine::applyFilter`, not `runAudit`; INV-7's
`.audit_suppress`→SARIF-`result.suppressions[]` marking is likewise
GUI-only, and the `suppressions` request field / `suppressionsMode` is
**never read** by the headless engine (a silent no-op) — the only shipped
headless suppression is the ANTS-1820 learned-FP ledger (drop-from-count,
no SARIF marking). Rewrote INV-6, INV-7, § 2.1's `suppressions` row, and
INV-2 to the shipped reality; wire-or-drop code decision → **ANTS-3615**.
Also softened INV-4 (`samples[0]` crash excerpt is best-effort, not
guaranteed) and corrected INV-8's SARIF property name
(`toolExecutionNotifications`). INV-3/5/6-exclusion-half/15 verified clean.
All doc-only.

**Pass 5** (final convergence pass) re-verified all five pass-4 rewrites
against code — INV-6/7 GUI-only, `suppressionsMode` no-op, INV-8
`toolExecutionNotifications`, INV-4 best-effort — all confirmed accurate,
with two refinements of the pass-4 edits: (a) INV-7 said a learned FP is
dropped from "raw/after-filter counts", but code drops it from
`after_filter_count` *only* (`rawCount` keeps the raw total — a suppressed
FP still counts toward `noise_rate_pct`'s denominator; auditrunner.cpp:910);
(b) § 2.2's `crashed`/`samples[0]` table row was softened to match INV-4's
best-effort language (intra-doc consistency). One code-side stale comment
(`auditrunner.h:118-119`) folded into ANTS-3610. Every INV-1…22 and every
section is now deep-verified against code across the five passes.

### Loop 6 (2026-07-25) — post-implementation reconciliation

Passes 6-9 below all ran on 2026-07-25 against code the ANTS-3610…3615 +
ANTS-1713 sweep had just changed. They continue the pass numbering above;
Loops 1-5 used a nested "Pass N" scheme, so read these as top-level
passes of this loop.

**Pass 6** ran after the ANTS-3610…3615 + ANTS-1713 code
sweep landed, and is the first pass to review the spec against *changed*
code rather than to reconcile the spec to static code. Three lanes
(§§ 1–4, §§ 5–10, and a cross-doc lane over ANTS-1397 + the error-code
taxonomy) each read cold. Verified findings, all fixed in this pass:

- **§ 2.4 asserted a live code bug that pass 5 had already fixed** — the
  "`kVerbInFlightReapMs` is a fixed `270'000`" paragraph survived the
  INV-11 rewrite, so the document simultaneously described the reap
  window as broken (§ 2.4) and as derived (INV-11). Rewritten to the
  derived form. The lesson: fixing an invariant is not the same as
  fixing every passage that discusses it.
- **§ 8's `m_auditPool` decision line** still called the aggregate cap
  unbuilt, contradicting § 4 and § 6 of the same document. Rewritten to
  record what ANTS-3612 actually decided: cap, not pool, and the pool is
  no longer planned.
- **§ 8's "stderr unredacted"** predated ANTS-2188's `SecretRedact::scrub`
  (INV-22). Narrowed to what it now means.
- **§ 2.1 / INV-2 credited `PathValidation::validatePath`**, which this
  verb never calls — it hand-rolls canonicalise + `isDir` in two places.
- **`bad_cwd` was missing from § 4** and is what an MCP caller actually
  receives for a bad `caller_cwd`; the engine's `bad_path` branch is
  unreachable through the verb.
- **`top_findings[]` was documented with a shape and a byte bound it does
  not have** — it is sample-shaped, and is built *after* the INV-13
  cascade from a measure that excludes it, so it sits outside the
  18/20 KiB budget rather than inside it.
- **§ 5 was one test behind** (`Inv9bReapWindowsDeriveFromAggregateCap`)
  and still claimed the stale-slot reaper was untested; its window
  derivation is now locked, only the runtime behaviour is not.
- Smaller: § 7's "at top" placement, an unlabelled fence that broke the
  heading run, the `max(2, nproc/8)` → flat-2 rationale, `kExclusions()`
  being a file-local function not a constant, the INV-9/INV-9b cross-doc
  numbering map, `mutable` on the gate members, the combined
  audit + build ceiling (≈ 6.6 GiB, not ~5 GiB), and a `Source:` line
  number that had drifted in an append-only log.

One finding was raised against **code**, not this doc, and was
roadmapped rather than silently changed: the `tools` schema string claims
auto-detect excludes clang-tidy when `kAutoDetectTools()` excludes only
mypy (ANTS-3622). A second cross-doc finding — ANTS-1397's `Optional`
caller_cwd claim, contradicted by all five `test_audit_*` verbs
registering `Required` — was a *doc* defect and was corrected in
`docs/specs/ANTS-1397-test-audit-mcp-trio-skill.md` itself, not roadmapped.

**Pass 7** (2026-07-25, same day as pass 6). Ran cold against the
pass-6 edits. It found that fixing a claim in one section reliably
leaves its restatements elsewhere untouched — the same failure mode
pass 6 named, now demonstrated on pass 6's own output. Verified and
fixed:

- **INV-2 still carried a pre-ANTS-3615 parenthetical** saying the
  engine "never reads `suppressionsMode`, so there is no such path to
  validate" — contradicting § 2.1, INV-7 and § 4, all of which pass 6
  had already corrected, and contradicting the code. Both §§ 1–4 and
  §§ 5–10 lanes raised it independently.
- **INV-13 claimed SARIF-side overflow reporting via
  `notifications[]`** — no such mechanism exists; the cap is a bare
  `break` and overflow surfaces as the envelope's `findings_truncated`.
- **`samples_truncated` was documented as "any cap fires"** — it is set
  only by the cascade; a 256 B per-message truncation cannot set it, so
  `false` is not proof messages are intact.
- **Three "present iff" response rows were wrong in the empty case** —
  `top_findings[]`, `html_path` and `cache_path` are *omitted*, not
  emitted empty.
- **§ 2.1's `scope` row omitted the working-tree union** on `files` /
  `since-tag` / `since-last-run`, which an implementer would use to size
  the changed-file set.
- **§ 6 restated "envelope ≤ 20 KiB"** after INV-13 had been corrected
  to exclude `top_findings[]` from that budget.
- **§ 5's coverage paragraph** listed INV-7/8/12 as untested when each
  has a sibling feature dir (INV-7's is the 9-test suite added the same
  day); and pointed at the wrong duplicate-coverage suite for INV-3.
- **Pass 6's own log entry misreported** ANTS-1397's contract finding as
  roadmapped when it was fixed in place.
- Smaller: the `tools_skipped` example used a `reason` string the engine
  never emits; `compile_commands_escape`'s row omitted the size/entry
  caps; INV-10 asserted signal-mask/umask behaviour nothing implements
  or tests; `§ 2.4`'s snippet had the wrong parameter name; the INV-21
  note counted a fold-in as a collision; and INV-2's "without
  symlink-follow" described a guard that was never built.

One genuine **code** gap surfaced and was roadmapped, not patched:
`validateCompileCommandsImpl` probes two compile-DB locations while the
tools resolve `-p` from seven, so a `build-fast/` DB is used unvalidated
(**ANTS-3624**). Two stale code comments were corrected in place — the
in-flight gate's consumer list, and an ANTS-1820 comment classifying
`shellcheck` as line-based when `isJsonFindingTool` treats it as JSON.
That last one had propagated into `tests/features/audit_run_allowlist/spec.md`
(written earlier the same day) and was fixed there too.

**Pass 8** (2026-07-25). Converging — no CRITICAL in either lane, and
both lanes independently verified large clean areas (all 31 test names,
the § 6 arithmetic end-to-end, every § 8 decision, every § 4 code).
Fixed:

- **§ 2.1's `paths` row named two tools; the code appends scoped
  positionals for eight** (all the file-scoped tools; only gitleaks and
  trivy ignore them), and the two-tool claim contradicted this spec's own
  INV-18.
- **§ 9 Q4 called the compile-DB guard "Resolved"** while § 10 recorded
  the same gap as open ANTS-3624 — the doc contradicting itself about a
  security guard. Now "partially resolved", with the covered scope named.
- **§ 6 restated two things already corrected elsewhere**: a
  "PathValidation cost" heading (this verb hand-rolls it) and an FD budget
  citing 10 pidfds (no pidfd path exists).
- **INV-13 vs § 2.2 flatly disagreed on `samples_truncated`** — INV-13
  said "any cap fires", § 2.2 said the 256 B message cap does not set it.
  Code agrees with § 2.2; INV-13 now also records that the flag is raised
  before anything is removed, so `true` does not prove a trim occurred.
- **INV-10 claimed the tool-resolve cache invalidates on PATH change** —
  it is keyed on tool name with a TTL only.
- The § 2.2 example's `noise_rate_pct` was 83 where the documented integer
  formula gives 84; `full` was called "git-independent" when it shells
  `git ls-files src/`; the source-scrape bucket claimed `.cpp`-only when
  two locks scrape a header and one asserts a constant directly.

Also completed a sweep pass 7 had claimed as done: the stale
"shellcheck is line-based" comment had a second instance in
`auditrunner.cpp` that the first sweep missed. "Corrected in place"
should mean every instance — logging that here rather than quietly
fixing it.

**Pass 9** (2026-07-25). §§ 5-10 came back with **zero CRITICAL and zero
HIGH** — the first lane in this loop to do so — and independently
re-verified all 31 test names, every § 8 decision, § 9's Q4/Q5 status and
every § 10 fix-claim. §§ 1-4 returned one HIGH. Fixed:

- **§ 2.2 contradicted INV-13 on `samples_truncated`** — pass 8 corrected
  INV-13 to say the flag is raised *before* anything is removed, and left
  § 2.2's "iff the cascade trimmed" standing. Same drift class, now on
  pass 8's own output. Both sides now say: raised when the soft cap is
  exceeded, proof of neither removal nor intactness.
- **§ 6's build-RAM arithmetic was wrong on the host it names.** It
  budgeted a concurrent build at ~3 GiB by assuming the `max(2, nproc/4)`
  floor, but CMakeLists sizes that pool explicitly for a 12-core box — 3
  cc1plus, ~4.5 GiB. Common case ≈ 6.3 GiB, worst case ≈ 8.1 GiB. This is
  the one number in § 6 a reader acts on (may I run `/audit` during a
  build?), and it was under-stated.
- **§ 2.4 named ANTS-1397 as the gate's second consumer** — it never
  wired one; the real second is `indie_review_dispatch` (ANTS-1352). The
  matching stale comment in `mainwindow.cpp` was corrected too.
- **§ 4's `bad_scope` promised a scope-enum validator that does not
  exist.** An unrecognised `scope` is not refused — it degrades silently
  to a full scan, a materially costlier outcome than a refusal.
- **`formats` is unvalidated** — an unrecognised value suppresses the
  `["sarif"]` default and produces no artifact at all while returning
  `ok:true`, the same silent-no-op class ANTS-3615 closed for
  `suppressions`. Documented and roadmapped as **ANTS-3626**.
- Smaller: INV-13 called `toolExecutionNotifications[]` unconditional
  (it is guarded on non-empty output — pass 8's wording); `top_findings`
  selection walks a `QHash` so its membership is unordered, not
  "most severe"; the § 5 coverage sentence stated its claim twice with
  the pre-pass-7 "of any kind" wording still in it; the sibling-dir list
  omitted `mcp_audit_run_async/`; the ~60 KSLOC anchor behind the ~45 s
  median is ~130 KSLOC today; HTML artifact size was given as ~80 KiB in
  § 6 and ~50 KiB in § 2.1; and § 10's own numbering had unheaded passes,
  now gathered under this Loop 6 heading.

Two code-side gaps were roadmapped rather than patched under a docs
skill: **ANTS-3626** (`formats` validation) and, from the ANTS-1397 lane,
**ANTS-3627** and **ANTS-3628**.

**Pass 10** (2026-07-25). **Both lanes returned zero CRITICAL; §§ 1-4
returned zero HIGH as well** — the convergence point for this document.
§§ 5-10's single HIGH was pass 9's own restatement drift: INV-9 still
cross-referenced "§ 6's ~5 GiB audit ceiling" after pass 9 had retired
that number in § 6 itself. Fixed, along with:

- **§ 6 claimed CMakeLists "sizes the pool explicitly for a 12-core
  box".** It does not — the cap is `max(2, nproc/4)` computed at
  configure time from `ProcessorCount`; the 12-core wording is a tuning
  anecdote in a comment. The 3-job / ~4.5 GiB / 6.3 / 8.1 figures are
  right *for this host*, but reading as a hardcode would send a 32-core
  reader to the wrong number (8 jobs ≈ 12 GiB). Now stated as a formula.
- **INV-13 fused two different `kSarifFindingsMax` ceilings.** The
  per-tool one sets `findings_truncated`; the cross-tool cumulative one
  in `writeSarif` is a bare `break` that signals nothing, so a multi-tool
  sweep can lose thousands of SARIF results while the envelope reports
  `false`. Silent data loss in the artifact the verb exists to produce —
  documented here, roadmapped as **ANTS-3629**.
- **§ 2.3's struct comments contradicted INV-2 and INV-16** — "canonical
  absolute (validated)" (the field carries the *raw* cwd) and "clamped"
  (out-of-range refuses, never clamps). Both were mirrored in
  `auditrunner.h` and corrected there too; the "clamped" one would have
  had an implementer clamp where three other places say refuse.
- Smaller: § 4's `bad_args` row omitted the ANTS-3615 `suppressions`
  cause; INV-22 and the feature contract both said trivy runs
  `--scanners secret` (it is `vuln,secret`); INV-1's "tools that resolved
  on PATH" is really resolved-minus-skipped; the INV-21 note counted one
  fold-in where it lists two; INV-12's `<sid>` is built on first fallback
  use, not at process start; and § 10 had two blocks both labelled
  "Pass 2", now disambiguated by loop.

**Process note.** Two of pass 9's recorded fixes had not actually landed
— a multi-edit batch aborted on one failed match and silently discarded
the rest of its writes, and the log recorded intent rather than verified
outcome. Both are fixed here. The lesson is the one this log keeps
re-learning at a different level: "corrected in place" has to mean
*verified in the file*, not *attempted*.
- **impl (2026-11-23)** — ANTS-6199 amendment to INV-6: kExclusions() now starts from AuditEngine::excludedDirGlobs() and adds the headless-only names. No reviewer was dispatched (rule 14: an amendment gets no automatic review).
