# ANTS-1897 — SessionStart hook installer: review loop log

## Cold-eyes loop log

### Loop 1 — 2026-05-27 (general-purpose subagent)

**Findings:** 2 CRITICAL, 5 HIGH, 6 MEDIUM, 6 LOW (+ 3 open
questions, one of which escalated to CRITICAL on verification).

**Verified + fixed (18 of 19):**

- **CRITICAL — Architecture deviation from ROADMAP body.** The
  source roadmap entry pinned (A) to "spawn-path system-prompt
  assembly" in `src/claudeintegration.cpp`. Ants does not spawn
  `claude` (verified § 1.1), so that mechanism is infeasible.
  *Fix:* added § 1.0 acknowledging the deviation, citing the
  user's 2026-05-27 AskUserQuestion picking SessionStart, and
  scheduling the roadmap-entry-body update in the same commit
  via § 7.
- **CRITICAL — Wrong CMake target.** Spec § 2.3 placed
  `src/mcporientation.{h,cpp}` in `ants_core_lib`; the correct
  home is `ants_claude_lib` (CMakeLists.txt L342, where
  `claudeintegration.cpp` lives). *Fix:* corrected § 2.3 + noted
  the dependency direction.
- **CRITICAL (escalated from OQ-3) — `ANTS_MCP_SOCKET` is not
  exported anywhere.** The script's reachability guard depends on
  it but `grep -nE 'ANTS_MCP|env\.insert.*MCP'` returns zero hits
  in the integration source. The hook would never print. *Fix:*
  added § 2.1.5 (PTY env-var export) + INV-14 enforcing the
  contract.
- **HIGH — INV-12 byte-identity is impossible** when the script
  body embeds a runtime version. *Fix:* renamed constant to
  `kOrientationScriptTemplate`, made the script body a `%1`
  placeholder substituted at install time, and INV-12 compares
  after substitution.
- **HIGH — INV-2 forbids first-launch bootstrap.** Marker absent
  → INV-2 said "never overwrite", but absence is also the
  first-launch state. *Fix:* split INV-2 into three cases (does
  not exist / exists with marker / exists without marker).
- **HIGH — Numeric inaccuracies.** "~80+ descriptors", "~200
  hits", "~90 tools" replaced with measured values (68 / 68 /
  68 minus one non-tool).
- **HIGH — `.roadmap-counter` not bumped.** Spec allocated
  ANTS-1898 / ANTS-1899 but did not specify the counter bump.
  *Fix:* added explicit bullet in § 7.
- **MEDIUM — Stale `0.7.49` version in script example** vs
  current `0.7.92`. *Fix:* replaced with `<ANTS_VERSION>` literal
  placeholder.
- **MEDIUM — `~` expansion in hook command.** Claude Code's hook
  runner doesn't reliably expand `~`. *Fix:* schema now uses
  absolute path resolved from `QStandardPaths::AppConfigLocation`
  at install time; matches the user's pre-existing
  `UserPromptSubmit` hook style.
- **MEDIUM — Wrong socket path** `/run/user/$(id -u)/ants-
  terminal/mcp.sock` vs the actual `/tmp/ants-terminal-mcp-<pid>`
  (verified live: `/tmp/ants-terminal-mcp-4409`). *Fix:*
  swallowed by the CRITICAL ANTS_MCP_SOCKET fix (§ 2.1.5).
- **MEDIUM — INV-7 "Use " prefix exception.** One existing hint
  at L3104 starts with "Call once …". *Fix:* INV-7 now names the
  exception and requires the sweep to rewrite it.
- **MEDIUM — Cost-cue asymmetry between canonical format and
  INV-7.** *Fix:* added explicit "INV-7 scope" note saying the
  test enforces shape only; quality is deferred to ANTS-1898.
- **MEDIUM — INV-10 size budget context.** *Fix:* stated the
  current draft body is ~780 chars (~35 % headroom).
- **LOW — "## MCP tool authoring" not disambiguated.** *Fix:*
  named project CLAUDE.md explicitly in § 7.
- **LOW — Missing Layman gloss.** *Fix:* added under the header
  block.
- **LOW — `paste` shell pipeline fragility.** *Fix:* rewrote the
  audit step as two independent counts + a pointer at the
  runtime INV-6 test as the authority.

**Dismissed (3):**

- **LOW — Source line format mismatch with roadmap.** Unverified
  — the roadmap entry's Source line was truncated in my fetch;
  cannot confirm a divergence.
- **LOW — Cold-eyes log section placement vs `specs.md`
  standard.** Standard § 4 lists Cold-eyes as a recommended
  section without an ordering rule; reviewer themselves noted
  "no conflict, just note." Not a finding.
- **LOW — Test infra pattern unverified.** `QStandardPaths::
  setTestModeEnabled(true)` already in use at
  `tests/features/dialog_chrome_affordances/test_dialog_chrome_
  affordances.cpp:48` and three other sites — pattern is real.

### Loop 2 — 2026-05-27 (general-purpose subagent, cold re-read)

**Findings:** 1 CRITICAL, 3 HIGH, 5 MEDIUM, 10 LOW.

**Verified + fixed (17 of 19):**

- **CRITICAL — 68/68 numerics off by one.** Raw `["name"]` grep
  returns 70 hits; three are non-tool sites (`serverInfo` L1514,
  `props` L6494, `env`-row L7372) → 67 tools. The companion
  `selection_hint` grep returns 68 raw; one (`env`-row L7378)
  matches the same non-tool exclusion → 67 hints. Coverage is
  67/67, not 68/68. *Fix:* updated every count in § 1, § 1.1,
  § 2.2 to 67, rewrote the methodology footnote to name the
  three exclusions, and pointed at the runtime `tools/list` MCP
  response as the authoritative source the INV-6 test consumes.
- **HIGH — Script heredoc indentation bug.** The literal
  reference in § 2.1.1 was indented 2 spaces inside the markdown
  fence; a real `cat <<'EOF' … EOF` preserves leading whitespace
  and the indented closing `EOF` would never terminate the
  heredoc. *Fix:* added an explicit "script body lives at column
  0; do not indent when implementing" note above the fence;
  re-asserted column-0 EOF.
- **HIGH — § 2.1.5 PTY env-var mechanism under-specified.** The
  prior wording said "appended to the pre-fork-assembled env
  array" but `ptyhandler.cpp:165` builds a per-fork
  stack-allocated `childEnvp[]` from scratch each call, with no
  persistent array. *Fix:* renamed the mechanism to "parent-side
  `qputenv` + child-side `environ`-copy at `ptyhandler.cpp:171`"
  — no `PtyHandler` API change needed because the existing
  `environ` loop already picks up the new entry.
- **HIGH — § 2.1.5 missed the flatpak branch.** INV-14's "every
  PTY" claim would be unverifiable without scoping. *Fix:*
  explicitly scoped INV-14 to the non-flatpak branch and added
  ANTS-1900 follow-up for the flatpak path in § 4.
- **MEDIUM — quoted grep had a literal newline inside the
  bracket.** *Fix:* the rewritten methodology footnote uses a
  clean single-line grep.
- **MEDIUM — § 2.2 coverage predicate would chase false gaps.**
  *Fix:* § 2.2 coverage clause now names the three excluded
  non-tool sites explicitly.
- **MEDIUM — § 2.1 subsection numbering jumped 2.1.3 → 2.1.5.**
  *Fix:* renumbered to 2.1.1 / 2.1.2 / 2.1.3 / 2.1.4 (PTY env)
  / 2.1.5 (uninstall).
- **MEDIUM — Settings-schema path divergence under XDG / flatpak
  prefixes.** *Fix:* § 2.1.2 now states install path + `command`
  string both derive from the same `AppConfigLocation` call so
  they always agree.
- **MEDIUM — INV-10 byte budget arithmetic off.** Measured with
  `wc -c` on the heredoc-printed lines: 876 bytes, not 780.
  *Fix:* INV-10 now states 876 chars / ~27 % headroom with the
  measurement command cited.
- **LOW — `~80 LOC` vs `~120 LOC` contradiction.** *Fix:* § 2.1.1
  now says "~120 LOC including the .h and .cpp combined",
  matching § 2.3.
- **LOW — Stale `kOrientationScript` reference in § 8 OQ-3.**
  *Fix:* renamed to `kOrientationScriptTemplate` to match § 2.1.1.
- **LOW — INV-2 test case name had a stray line break inside the
  identifier (`first_launch_writes_\nfresh`).** *Fix:* rejoined.
- **LOW — Layman paragraph said "~90 tools" while body uses 67.**
  *Fix:* Layman now says "dozens" — vague but correct under
  growth, and saves a future drift.
- **LOW — § 7 flipped ANTS-1897 ✅ shipped in the spec PR even
  though implementation hasn't shipped yet.** *Fix:* split § 7
  ROADMAP changes into spec-acceptance commit vs
  implementation-shipped commit.
- **LOW — § 4 OQ-3 said "escalated" but the surviving OQ-3 text
  was about list freshness.** *Fix:* renumbered: OQ-3 now holds
  the env-var escalation, OQ-4 holds the list-freshness
  question.
- **LOW — `"pre-1897"` reads awkwardly in § 5.** *Fix:* now reads
  "upgrading from a pre-ANTS-1897 build".
- **LOW — `(reaper at src/mainwindow.cpp)` missing line number.**
  *Fix:* now cites `src/mainwindow.cpp:3830-3863` (the `/tmp/
  ants-terminal-mcp-*` sweep that runs before
  `startMcpServer`).

**Dismissed (2):**

- **LOW — `sh script.sh` overrides the shebang.** Reviewer noted
  it's "fine, but a redundancy worth noting." Intentional: the
  user's pre-existing `UserPromptSubmit` hook also prefixes with
  `bash`. Consistency with surrounding hooks > shebang purity.
- **LOW — INV-7 case-sensitivity.** Reviewer asked whether the
  prefix is case-sensitive or case-insensitive. INV-7 already
  says "case-insensitive prefix `\"Use \"`" — the canonical
  example just happens to use capitalised "Use". No conflict.

### Loop 3 — 2026-05-27 (general-purpose subagent, cold re-read)

**Findings:** 1 CRITICAL, 3 HIGH, 5 MEDIUM, 8 LOW.

**Verified + fixed (16 of 17):**

- **CRITICAL — Line-number drift on `startMcpServer`.** Reviewer
  flagged `mainwindow.cpp:3865` as the cited call site; the call
  is actually at L3867 (L3865 is the `mcpSocket` definition).
  *Fix:* § 2.1.4 mechanism now reads "after the MCP server starts
  at `src/mainwindow.cpp:3867`" with a parenthetical that L3865
  is the `mcpSocket` definition. INV-14's citation updated from
  L3865 to L3867.
- **HIGH — `registerToolProvider` citation conflated definition
  with registration sites.** `claudeintegration.cpp:1253` is the
  *definition*; tools are registered at 65 call sites in
  `mainwindow.cpp`, plus 2 inline-dispatched control-plane tools
  (`get_session_info` L1710, `tool_info` L6470). *Fix:* § 1
  rewritten to name both source files and split out the 65 + 2
  decomposition.
- **HIGH — "~10 new hint strings (gaps)" contradicted verified
  67/67 coverage.** *Fix:* § 2.3 RAM budget now states "coverage
  is already 67/67 — the sweep adds zero new hints, rewrites one
  outright (L3104), tightens ~20 cost-cues; net binary growth
  ~1 KB of literal-string churn."
- **HIGH — 876-byte measurement only correct after stripping
  markdown indentation.** INV-10 risk: a reader thinks "876"
  refers to the as-rendered fence content. *Fix:* INV-10 now
  states the un-indent caveat explicitly and points back at the
  "do not indent" note in § 2.1.1.
- **MEDIUM — INV-6 was framed as a gap-closing claim when
  coverage is already 67/67.** *Fix:* added "regression guard,
  not a gap-closing claim" sentence under INV-6.
- **MEDIUM — Audit-step block had stale 68/68 numbers.** *Fix:*
  rewritten to "70 `["name"]` raw, 68 `["selection_hint"]` raw,
  subtract 3 + 1 exclusions → 67/67 net".
- **MEDIUM — `%1` substitution mismatch.** Template marker line
  showed literal `<ANTS_VERSION>` but the spec called for
  `QString::arg`. *Fix:* template now uses `%1` directly (the
  Qt-idiomatic `arg` placeholder); rationale paragraph below the
  fence already correctly describes the substitution.
- **MEDIUM — CLAUDE.md ↔ mcp-tools.md duplication.** Project
  CLAUDE.md already delegates the MCP authoring checklist to
  `docs/standards/mcp-tools.md`; duplicating the selection_hint
  rule in both invites drift. *Fix:* dropped the CLAUDE.md
  bullet in § 7.
- **MEDIUM — INV-14 test surface mismatch.** PTY test can't run
  under `setTestModeEnabled` + `QTemporaryDir`; it needs
  `/dev/ptmx`. *Fix:* split INV-14 into a separate test dir
  `tests/features/mcp_pty_env_carries_socket/` and updated § 6.
  *(That split was planned and not built — see § 6's ANTS-4135
  amendment for where INV-14 actually landed.)*
- **LOW — Settings page name unverified.** Reviewer asked whether
  "Claude integration" is the real page name; verified live —
  the tab is `"AI Assistant"` (`src/settingsdialog.cpp:61`).
  *Fix:* every "Settings → Claude integration" reference
  renamed to "Settings → AI Assistant" (script comment, toast
  text, settings-dialog bullet, CHANGELOG copy).
  **Post-loop correction (discovered during implementation
  2026-05-27):** loops 3–5 misread the placement. The "AI
  Assistant" tab hosts the legacy Ollama-style chat-completion
  controls (`m_aiEnabled`, `m_aiEndpoint`, etc.) at L424; the
  existing Claude toggles (`m_claudeTabStatusIndicator`,
  `m_claudeAutoModelSwitch`) live in **General** at L237.
  Implementation lands the MCP-orientation checkbox in General
  next to the autoswitch toggle. Spec text + script template +
  CHANGELOG copy updated in the same implementation commit.
- **LOW — `claude.mcp_orientation_*` namespace collision risk
  unverified.** *Fix:* verified live — the only existing
  `claude.*` keys are the 4 `claude.auto_model_*` entries from
  ANTS-1735; no collision.
- **LOW — INV-12 read-mode ambiguity.** A `QTextStream` read
  could normalise line endings and break the byte-equality test.
  *Fix:* INV-12 now states the read mode (binary, no
  QTextStream).
- **LOW — OQ-1 cited an unverified vendor-behaviour claim**
  ("Claude Code de-dupes cached context"). *Fix:* dropped that
  sentence from the rationale.
- **LOW — Loop-2 mentioned `~80 LOC` once, contradicted by
  `~120 LOC` elsewhere.** Already fixed in loop-2 fold; no
  surviving instance.
- **LOW — Indented reference body inside fenced code block** —
  reviewer suggested showing the body un-indented in the fence.
  Kept indented + the "do not indent" warning above — the
  indent matches the surrounding spec's bullet-nesting visual
  rhythm and the test (INV-12) catches a misimplementation.
- **LOW — OQ-3 escalation wording mismatch** — already
  renumbered in loop-2 fold (OQ-3 now holds env-var
  escalation, OQ-4 holds list-freshness).

**Dismissed (1):**

- **LOW — Stale-env-var UX edge case** (Ants crash → restart →
  shells from before the crash hold the old `ANTS_MCP_SOCKET`
  → SessionStart hook silently exits 0 even though new MCP is
  healthy). Real edge case but matches the documented
  cleanup contract in § 2.1.4. Documented behaviour, not a
  bug. The user opens a new tab — same recovery as for
  pre-MCP-start PTYs.

### Loop 4 — 2026-05-27 (general-purpose subagent, cold re-read)

**Findings:** 1 CRITICAL, 2 HIGH, 4 MEDIUM, 0 LOW.

**Verified + fixed (7 of 7):**

- **CRITICAL — Descriptor framing.** § 1's "67 = 65 + 2 inline-
  dispatched" wording conflated *registration* with
  *descriptor-bearing*. All 67 tools have descriptors in the
  `tools/list` builder; the 65/2 split is about handler
  dispatch routing, not about descriptor coverage. *Fix:*
  § 1 rewritten as "67 tools via `tools/list`; routing-wise,
  65 via registry + 2 inline-dispatched". INV-6 unchanged
  (it already targeted `tools/list`).
- **HIGH — Settings.json write disclosure missing from user copy.**
  Ants writing to `~/.claude/settings.json` is a significant
  side-effect that the toast + CHANGELOG entry did not mention.
  *Fix:* toast text now reads "Ants now installs a SessionStart
  hook in `~/.claude/settings.json` so every Claude Code session
  opens with a short Ants-MCP cheat-sheet. You can mute this in
  Settings → General." CHANGELOG entry rewritten to name
  the merge + the marker-substring preservation contract +
  the toggle for opt-out.
- **HIGH — Early-PTY race not grep-grounded.** Reviewer asked
  whether any PTY spawns before `startMcpServer` at L3867.
  *Fix:* verified ordering via grep — MainWindow ctor at L240
  calls `setupStatusBarChrome()` at L609 (→ `setupClaudeMcp
  Providers()` → `startMcpServer` at L3867) **before** the
  first `newTab()` at L612. Added an "Ordering verified"
  bullet to § 2.1.4. The qputenv lands before any child
  shell forks.
- **MEDIUM — Marker definition imprecise.** Was it a prefix
  match or a full-line match? *Fix:* § 2.1.1 now opens with a
  "Marker definition" bullet stating the marker is the
  **prefix** `# ants-orientation-version:` followed by a
  parsed version token. The case-2 wording now reads "compares
  the parsed version to the running ANTS_VERSION".
- **MEDIUM — Substring collision risk in § 2.1.2.** The
  `ants-terminal/hooks/mcp-orientation.sh` substring is the
  marker; a future Ants component using a different
  SessionStart hook would collide. *Fix:* added a "Collision
  posture" sentence stating the substring is specific enough
  to be safe; future Ants components must use a different
  filename to avoid the marker collapse.
- **MEDIUM — INV-12 vs case-2 self-contradiction.** INV-12
  asserted byte-equality "at all times"; case 2 skipped the
  write when the marker version matched, so user-edits to the
  body survived → INV-12 would fail. *Fix:* INV-12 now scopes
  to "after Ants writes or refreshes" (case 1 or case-2
  version-bump). § 2.1.1 case 2 explicitly documents the
  "user edit survives until next version bump" trade-off,
  with a corresponding header-comment warning in the script
  body.
  *Superseded 2026-06-05 (ANTS-2038): the version-only gate is now
  content-parity — a marker-present body is rewritten on any byte
  difference, so a user edit no longer survives until the next bump.
  INV-12 holds for every marker-present file post-install. See the
  top-of-file amendment note.*
- **MEDIUM — `#!/bin/sh` + UTF-8 bullets portability.** The
  bullet `•` and en-dash `→` characters need a UTF-8-aware
  shell; busybox/Alpine `sh` may not handle them. *Fix:*
  shebang now `#!/bin/bash` (matches the user's existing
  `UserPromptSubmit` hook style). The `"command"` schema
  invokes the script directly (no `sh ...` prefix) so the
  shebang governs interpreter selection — bash is then
  guaranteed to handle UTF-8 with default locales.

### Loop 5 — 2026-05-27 (general-purpose subagent, cold re-read)

**Findings:** 0 CRITICAL, 3 HIGH, 4 MEDIUM, 5 LOW/INFO.

**Verified + fixed (7 of 7 actionable):**

- **HIGH — Invocation-style contradiction.** Loop-4 fixed the
  schema to invoke the script directly (no `bash` prefix) but
  kept the "consistent with the user's UserPromptSubmit hook"
  justification. The user's actual hook is
  `bash /home/ants/.claude/hooks/git-context-on-keyword.sh`
  (verified live). *Fix:* schema now reads `"command": "bash
  /home/<user>/.config/ants-terminal/hooks/mcp-orientation.sh"`
  — matches the user's hook style for `settings.json` visual
  consistency + deterministic interpreter (no shebang
  ambiguity if the executable bit is later stripped).
- **HIGH — Reaper line range too narrow.** `mainwindow.cpp:
  3853-3862` lands mid-loop on the `kill(pid, 0)`
  continue-guard, not at the sweep block top. *Fix:* widened
  citation to `src/mainwindow.cpp:3830-3863` in § 2.1.4
  cleanup bullet (and the cold-eyes log entry).
- **HIGH — INV-12 brittle on `%N` Qt arg semantics.** Future
  `printf "%s"` or similar shell-side addition would silently
  consume `arg()` arguments. *Fix:* INV-12 now carries a
  "Placeholder contract" sub-clause: template MUST contain
  exactly one `%1` and no other `%N`; literal `%` chars MUST
  be doubled `%%`. Includes a grep recipe to enforce.
- **MEDIUM — `%1` vs `<ANTS_VERSION>` inconsistency.** The
  cold-eyes log Loop-1 entry still showed
  `# ants-orientation-version: <ANTS_VERSION>` while the
  current code-side marker is `%1`. *Fix:* INV-1 narrative
  uses prose `<ANTS_VERSION>` as shorthand for "the resolved
  value"; § 2.1.1 reference script + INV-12 use the
  code-accurate `%1`. The two conventions are now explicitly
  distinguished.
- **MEDIUM — INV-10 missing exact measurement command.** *Fix:*
  INV-10 now includes the precise `wc -c` recipe (against
  either a live-socket run or a direct heredoc measurement)
  so the test author measures identically.
- **MEDIUM — § 5 missing config-key names.** *Fix:* § 5
  Migration block now lists `claude.mcp_orientation_enabled`
  and `claude.mcp_orientation_nudge_shown` with defaults so a
  maintainer can find them without grepping.
- **MEDIUM — § 7 didn't state `.roadmap-counter` pre-bump
  value.** *Fix:* § 7 counter bullet now reads "current value
  is `1897` at spec time; bumped to `1900`" + warns future
  spec authors to bump from the live counter.

**Open questions left for review (OQ-A / OQ-B from loop 5):**

- **OQ-A** — Already resolved above (HIGH-1 fix). The schema
  now uses `bash <path>`, matching the user's hook style.
- **OQ-B** — `restoreSessions()` is called at L615 (after the
  L612 first-newTab). The newTab at L612 creates the first
  PTY tab; `restoreSessions()` may add more tabs but they all
  come after the qputenv lands. Confirmed: no PTY is forked
  between MainWindow ctor entry and L609 setupStatusBarChrome
  call (grep of `new TerminalWidget` / `PtyHandler::start*` in
  the ctor body between L240 and L609 returns zero).
  Documented in § 2.1.4 ordering bullet.

**Convergence:** loop-5 returned no CRITICAL, 3 HIGH all
mechanical / text-level. Convergence is asymptotic. Going to
loop 6 (one over the `--max-loops` default of 5) only if the
user approves; the spec is implementer-ready as written and
further loops will diminishing-return on text polish.

### Loop 6 — gated on user decision

User asked at the `--max-loops` cap whether to continue or
accept the spec as ready-to-implement.

### Loop 7 — 2026-05-27 (in-the-field discovery, ANTS-1902 fold-in)

Two implementation bugs surfaced after the spec shipped — both
made it through cold-eyes loops 1–5 because the test seam
(`installAt(homeOverride)`) hardcodes the lowercase `ants-terminal`
config dir and so masked the substring-vs-`applicationName`
mismatch. Field discovery: a fresh Claude Code session emitted
`SessionStart:startup hook error: bash: /home/ants/.config/Ants:
No such file or directory` on every launch; user reported 5 prior
sessions of "fixed → broke again" loops.

- **HIGH — Marker substring did not match the production path.**
  `kMarkerSubstring = "ants-terminal/hooks/mcp-orientation.sh"`
  (lowercase-hyphenated) failed the case-sensitive
  `QString::contains` check against the actual production path
  `.../Ants Terminal/hooks/mcp-orientation.sh` (Qt's
  `QStandardPaths::AppConfigLocation` uses `applicationName`
  verbatim → capital + space). `findAntsEntry` therefore returned
  `{-1, -1}` on every launch and `mergeSettings` appended a fresh
  outer container instead of updating in place — duplicates
  accumulated linearly with Ants Terminal launches. *Fix:* shrunk
  marker to suffix-only (`/hooks/mcp-orientation.sh`), updated
  INV-3 wording to require sweep-all-then-append on every install
  (idempotent + self-healing for users whose settings.json had
  already filled with stale duplicates).
- **HIGH — `hookCommand` did not quote the script path.** Built
  `"bash " + scriptPath` raw via `QStringLiteral`; once the path
  contained a space (production: "Ants Terminal"), the hook
  runner tokenised at the space and bash reported "No such file
  or directory". *Fix:* added `bashQuote(s)` (wrap in single
  quotes; escape embedded `'` as `'\''`), updated INV-3 to
  require the `command` value to bash-single-quote the path.

**Verified (3):**

- Reproducer (failing tests `SweepsExistingDuplicates` +
  `CommandRunnableWithSpacesInPath`) FAILS pre-fix, PASSES
  post-fix.
- Existing INV-3 idempotent test continues to pass on the
  test-seam path.
- End-to-end check on the user's real `~/.claude/settings.json`:
  seeded a broken unquoted duplicate (the exact form that bit
  the user for 5 sessions) → ran the rebuilt
  `ants-terminal` in offscreen mode → settings.json swept to
  a single canonical quoted entry; subsequent fresh-launch
  attempts converged to the same state.

**Dismissed (0).**

Shipped as ANTS-1902.

---

### ANTS-2038 amendment — 2026-06-05

Content-parity rewrite gate. Two lanes per loop (the spec amendment +
the paired `mcp-tools.md` §6a / `mcp-error-codes.md` doc edits), run
cold (no fix-briefing between loops). Converged at loop 4 (zero
verified actionable findings); 1 INFO-only residue surfaced + folded.

**Loop 1 — verified 7, fixed 7.**
- HIGH (spec): §2.1.1 fenced body is illustrative; under content-parity
  INV-12's byte source must be unambiguous. *Fix:* added an
  "illustrative fence / canonical `kOrientationScriptBody`" note.
- HIGH (spec): case-2 prose quote vs the script header comment differed.
  *Fix:* aligned the quote to the verbatim comment (with marker name).
- MEDIUM (spec): loop-5 historical trade-off read as a live
  contradiction. *Fix:* inline "Superseded 2026-06-05" marker.
- MEDIUM (spec): INV-1 framed version-bump as a co-equal trigger.
  *Fix:* reworded to "byte-difference is the sole gate".
- MEDIUM (std): §6a "→" phrasing implied `no_changelog`/`bullet_not_found`
  were retired. *Fix:* explicit "not retired — generic codes stay live"
  clause.
- MEDIUM (std): `no_changelog`/`bullet_not_found` absent from the
  taxonomy. *Fix:* added both rows to `mcp-error-codes.md` §2.
- MEDIUM (std): `format_mismatch` envelope-shape credited only ANTS-2031.
  *Fix:* co-credited ANTS-2040.
- Dismissed: spec M3 (`idempotent_rewrite` "misnomer") — the parenthetical
  already clarifies it tests a skip; the gtest `IdempotentRewrite`
  installs the real template first, so it stays byte-identical.

**Loop 2 — verified 4, fixed 4.**
- HIGH (spec): the promised ANTS-2038 loop log was absent → this section.
- MEDIUM (spec): active text named a non-existent `kOrientationScriptTemplate`
  constant (code ships `kOrientationScriptBody` + `orientationScriptTemplate()`).
  *Fix:* reconciled all active citations to the real symbols (historical
  loop-log entries left as point-in-time records).
- MEDIUM (std): `format_mismatch` carries a `path` field, omitted from
  both docs. *Fix:* added `path` to §6a and the error-codes row.
- LOW (spec): "version-only" vs "version-bump gate" phrasing. *Fix:* aligned.
- Dismissed: spec H2 ("spec ahead of code / header pending marker") — the
  ANTS-2038 code was implemented in the same work item, closing the lag.

**Loop 3 — verified 6, fixed 6.**
- HIGH (spec): the §2.1.1 "Marker definition" bullet still said the version
  token is "compared to the running ANTS_VERSION". *Fix:* reworded to
  presence-only detection.
- MEDIUM (spec): INV-12's whole-file `grep '%[0-9]'` recipe returns spurious
  hits from `.arg()` warning strings. *Fix:* scoped the check to the
  `kOrientationScriptBody` literal.
- MEDIUM (std): `bullet_ambiguous` emitted but undocumented. *Fix:* added a row.
- MEDIUM (std): §6a `hint` described as a bare Edit fallback. *Fix:* required
  a format-specific hint.
- LOW (spec): "~120 LOC" understated the as-built ~515. *Fix:* corrected.
- LOW (std): `no_changelog` row omitted the bad-cwd branch. *Fix:* aligned
  with `no_roadmap`.

**Loop 4 — verified 1 (code-side), fixed 1; clean on docs.**
- Both lanes returned zero verified actionable doc findings. One code-side
  drift: `mcporientation.cpp:36`'s marker comment still said "parsed and
  compared". *Fix:* reworded to presence-only (matches INV-1). Two INFO
  polish items folded (`unrecognised_format` vs `format_mismatch`
  distinction in §6a; literal `format:` tokens in the error-codes
  examples). Spec M1 ("test cases may not exist") dismissed — verified
  false: `McpOrientation_Inv1.{IdempotentRewrite,InVersionBodyRefresh}`
  exist and pass.

Shipped as ANTS-2038.

### impl — ANTS-6471 (2026-11-15)

Not a review loop; no reviewer was dispatched. Implementation amendment: § 2.1.2 gains "Other writers" (the merge holds ConfigWriteLock across read, merge and write, and backs off when settings.json changed before its rename), and INV-15 states it. Origin: ANTS-6471 (review-code 2026-11-06, lane 17-mcp-infra). Both INV-15 tests ran red against the pre-fix source.

### impl (2026-11-16)

ANTS-6786 doc-fix, not a review; no reviewer was dispatched. Line citations replaced by symbols; the dated amendment passages and the ANTS-5730 framing moved verbatim to docs/history/ANTS-1897-ants-mcp-discoverability-sessionstart.md; the ANTS-5730 migration rules moved into § 5.
