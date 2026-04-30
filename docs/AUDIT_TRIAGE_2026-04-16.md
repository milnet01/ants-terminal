# Audit Tool Improvement Notes — 2026-04-16 (historical snapshot)

> **Historical snapshot, not current state.** This file documents
> the audit tool's noise/signal balance as of 2026-04-16 against
> `ants-audit v0.6.30 @ 074c2a6`. Many of the rules discussed here
> have shipped fixes since (see CHANGELOG between 0.6.30 and 0.7.58).
> The file is preserved because `docs/RECOMMENDED_ROUTINES.md`
> references it as a *gold-standard format example* for future
> triage runs — read it for the structure, not for the live status
> of the listed rules.

Source report: `/tmp/ants-audit-oSmejL.txt` (ants-audit v0.6.30 @ `074c2a6`).
Verifier: manual review with file:line evidence.

Purpose of this file: capture which rules are producing noise vs signal in the
current audit catalogue so that future edits to `src/auditdialog.cpp` (check
definitions, OutputFilter, parseFindings) target the categories where a small
amount of logic meaningfully reduces false-positive load.

---

## Triage summary

| Category | Total | Verified | False-Positive | FP-rate |
|---|---|---|---|---|
| Command Injection Patterns | 1 | 0 | 1 | 100 % |
| Hardcoded Secrets Scan | 2 | 0 | 2 | 100 % |
| Qt openUrl Without Scheme Check | 3 | 0 | 3 | 100 % |
| Unbounded Callback Payloads | 1 | 0 | 1 | 100 % |
| clazy (Qt AST analysis) | 55 | 2 | 53 | 96 % |
| cppcheck Static Analysis | 5 | 2 | 3 | 60 % |
| Dead Code Detection | 2 | 0 | 2 | 100 % |
| Debug / Temp Code | 2 | 0 | 2 | 100 % |
| Insecure HTTP URLs | 1 | 0 | 1 | 100 % |
| Memory Management (Qt-aware) | 30 | 0 | 30 | 100 % |
| Missing Compiler Warning Flags | 1 | 0 | 1 | 100 % |
| Overly Long Source Files | 4 | 0 | 4 | 100 % (info) |
| `bash -c` / `sh -c` Non-Literal | 1 | 0 | 1 | 100 % |
| clang-tidy Analysis | 34 | 0 | 34 | 100 % |
| **Total actionable** | **141** | **4** | **137** | **~97 %** |

---

## Verified real findings (4)

All four were fixed in the same session. Listed here for completeness.

### 1. `src/claudeallowlist.cpp:409` — clazy `range-loop-detach` — MINOR
Ranged-for on `QStringList segments` (value-captured parameter) triggers
implicit detach. Fixed by wrapping in `std::as_const(segments)`.

### 2. `src/auditdialog.cpp:3115` — clazy `container-inside-loop` — MINOR
`QStringList parts` was constructed per-finding inside `renderResults()`.
Hoisted out of the loop with a `parts.clear()` at each re-use site.

### 3. `src/elidedlabel.h:33` — cppcheck `returnByReference` — MINOR
`QString fullText() const { return m_fullText; }` → `const QString &`.

### 4. `src/sshdialog.h:32` — cppcheck `returnByReference` — MINOR
`QList<SshBookmark> bookmarks() const { return m_bookmarks; }` → `const &`.

None were bugs; all were perf/style micro-fixes. No critical or security
finding survived verification.

---

## False-positive analysis — by category

### CRITICAL-tier noise

#### `Command Injection Patterns` — 1/1 FP
- `ptyhandler.cpp:117` — `::execlp(shellCStr, argv0, nullptr)`.
- Standard `fork`/`execlp` login-shell spawn. `shellCStr` is a validated
  path; `execlp`'s first arg is a binary path (not a shell command string).
  The regex appears to fire on any `execlp` call.

**Remediation idea**: exclude `exec{l,v}{,p,e,pe}` when called with a literal
`nullptr`/`NULL` trailing arg (i.e. the `execlp(prog, argv0, nullptr)`
family). Keep the rule hot on `system()`, `popen()`, and `/bin/sh -c` with
non-literal args — those are the real injection vectors.

#### `Hardcoded Secrets Scan` — 2/2 FP
- `settingsdialog.cpp:208` — `m_aiApiKey = new QLineEdit(tab);`
- `aidialog.cpp:91` — `m_apiKey = apiKey;`
- Neither has a literal string on the RHS. The rule is matching the
  substring "apiKey" in a variable name.

**Remediation idea**: require the RHS to be a string literal of non-trivial
length, e.g. `=\s*"[^"]{16,}"`. High-entropy string detection (Shannon
entropy > 4.0) is a stronger signal but regex-scoping would eliminate these
two without any new infrastructure.

### MAJOR-tier noise

#### `Qt openUrl Without Scheme Check` — 3/3 FP
- `terminalwidget.cpp:1268` — the *previous line* (1267) is the allowlist:
  `if (ql.url.startsWith("http://") || ql.url.startsWith("https://"))`.
- `terminalwidget.cpp:2878` — `span.url` for OSC 8 is already scheme-
  validated at ingestion (`terminalgrid.cpp:616`: http/https/ftp/file/mailto).
- `terminalwidget.cpp:3798` — URL is a literal `"https://www.google.com/…"`
  with percent-encoded selection appended.

**Remediation idea**: suppress when, within a ±5-line window, either:
  (a) a `startsWith("http"|"https"|"file"|"mailto"|"ftp")` gate appears
      above the `openUrl`, or
  (b) the URL being opened is constructed from a string-literal scheme
      prefix `"https://…"` / `"http://…"` / `"file://…"` / `"mailto:…"`.
A single-file ±N-line scan is cheap and kills all three of these.

For the OSC 8 case (2878) an additional rule: findings inside
`openHyperlink()` should inherit the caller's validation context.
A one-line allowlist comment marker (`// ants-audit: scheme-validated`)
on the function signature would give the rule a target to look for.

#### `Unbounded Callback Payloads` — 1/1 FP
- `terminalgrid.cpp:795` — the rule fires on the callback call site, but
  lines 787 (`name.size() <= 128`) and 792-794
  (`decoded.truncate(kMaxUserVarBytes)`) cap both arguments.

**Remediation idea**: same ±N-line context window. If a `.truncate(`,
`.left(`, `.size() <= N`, or `constexpr int kMax…` appears on the
flagged variable within (say) 10 lines above, treat it as bounded.

### `clazy` — 53/55 FP (96 %)

Breakdown of the 55 findings:
- **~48 findings** are `qt-keywords` (`signals:`, `slots:`, `emit`). The
  project uses unqualified Qt keywords as a style convention — see
  STANDARDS.md §Plugin System Standards where the macro style is discussed.
- **~5 findings** are clazy driver diagnostics
  (`warning: unknown warning option '-Wshadow=local'`, "Processing file…"
  progress banners) being treated as findings.
- **2 real findings** — `range-loop-detach` and `container-inside-loop`
  (fixed above).

**Remediation ideas**:
1. Disable `qt-keywords` for this project: add `-Wno-clazy-qt-keywords` to
   the clazy invocation, or strip `[-Wclazy-qt-keywords]` lines in
   `OutputFilter`. Single project-level disable; 48 findings vanish.
2. Filter clazy driver noise: `warning: unknown warning option`,
   `[N/M] Processing file`, and bare `|` continuation lines should not
   surface as findings. They're diagnostic fluff from clazy itself.
3. Verify that clazy's configured check set matches the project's
   conventions. `qt-keywords`, `non-pod-global-static`, and
   `lambda-in-connect` are frequently opinion-dependent.

### `cppcheck` — 3/5 FP

- 2 FPs are `#error "This file was generated using the moc from 6.11.0"`
  — cppcheck is parsing `build*/moc_*.cpp` files.
- 1 FP is also from the same moc `#error`.

**Remediation idea**: pass `-i build/ -i build-asan/ -i build-*/` to
cppcheck's argv, not just filter in post-parse. The current
`isGeneratedFile()` skip runs *after* cppcheck has tried to parse the file
and failed. Pre-parse exclusion also saves cppcheck's time (this category
timed out at 30 s — likely spent wrestling with moc files).

### MINOR-tier noise

#### `Dead Code Detection` — 2/2 FP
Same root cause as cppcheck — moc `#error` being flagged. Same fix
(pre-parse exclusion).

#### `Debug / Temp Code` — 2/2 FP
`qDebug()` calls at `terminalwidget.cpp:1363-1364` are gated by
`if (on)` — user-triggered debug-log toggle (Ctrl+Shift+D), not forgotten
debug prints.

**Remediation idea**: require `qDebug`/`qWarning`/`qInfo`/`qCritical` to
appear *outside* an obvious debug-gate conditional in the same function.
Either: scan a small window above for `if (m_debug` / `if (on)` /
`if (.*debug)` / `if (.*verbose)`, or only flag `qDebug` calls that are
unconditional at the start of a function body.

#### `Insecure HTTP URLs` — 1/1 FP
Matched line is literally the scheme allowlist (`startsWith("http://")`
inside a routing conditional). Same remediation as the openUrl category:
context window distinguishes "calls a URL with http" from "gates on http".

#### `Memory Management (Qt-aware)` — 30/30 FP (biggest single noise source)
All 30 are `new QWidget(parent)` patterns where a Qt parent is passed
(→ Qt parent-child ownership — no leak possible). Per STANDARDS.md
§Memory Management, this is the project's prescribed idiom.

**Remediation idea**: the rule appears to match `new \w+\(` unconditionally.
Require absence of a parent arg inside the parens — flag only `new X()`,
`new X(nullptr)`, or `new X(NULL)`. Presence of any `this`, `&dlg`,
`parent`, or similar keyword/identifier inside the parens = Qt-owned,
suppress.

Given this category alone is 21 % of total findings (30/141), fixing it
would meaningfully improve the tool's signal-to-noise.

#### `Missing Compiler Warning Flags` — 1/1 FP
`-Wconversion` recommendation. Fine as an advisory but should be downgraded
to INFO severity, not MINOR. Currently it pollutes the MINOR tier.

#### `Overly Long Source Files` — 4 INFO (not noise, but worth noting)
Advisory; correctly tagged. No action.

#### `bash -c` / `sh -c` with Non-Literal — 1/1 FP
`mainwindow.cpp:3065` — `actionValue` sourced from `auto_profile_rules`
(user-configured, same trust model as plugin manifests; see
`PLUGINS.md`).

**Remediation idea**: suppress when the non-literal originates from
`m_config.*()` or another trusted-config source. The Config class is the
project's declared trust boundary (STANDARDS.md §Security).

#### `clang-tidy Analysis` — 34/34 FP
All 34 are `'QString' file not found`, `'QDialog' file not found`, etc. —
clang-tidy cannot resolve Qt6 system headers. Driver-level misconfiguration
(missing `-isystem /usr/include/qt6`) or stale/empty `compile_commands.json`
for the run.

**Remediation idea**: on any `file not found` error in clang-tidy output,
emit a single banner ("clang-tidy headers not resolved — regenerate
`compile_commands.json`") and suppress all subsequent per-file repeats.
Currently this one issue creates 34 near-identical lines.

---

## Priority order for audit-tool improvements

By FP-count reduction per unit of work:

1. **Qt parent-child aware Memory Management rule** (30 FP eliminated)
2. **clazy `qt-keywords` project-disable** (~48 FP eliminated)
3. **clang-tidy "file not found" collapse** (34 FP → 1 banner)
4. **Pre-parse generated-file exclusion for cppcheck** (3-5 FP, plus fixes
   the 30-s timeout on Compiler Warnings)
5. **Context-window gate detection** (kills ~5 MAJOR FPs: openUrl x3,
   callback x1, http-url x1 — highest severity-weighted impact)
6. **Hardcoded-secrets RHS literal constraint** (2 CRITICAL FPs)
7. **`execlp`/`execv*` exception for `command-injection`** (1 CRITICAL FP)
8. **Debug-gate awareness for `qDebug` rule** (2 FP)
9. **Config-sourced trust for `sh -c`** (1 FP)

Items 1-3 alone would reduce the finding count from 141 to ~29 — a drop
from ~97 % FP rate to ~86 %. With items 4-9 added, the actionable rate
climbs toward ~40-60 %.

---

## Lessons for the audit philosophy

- **Regex-only checks struggle with context.** Half of the MAJOR findings
  here would be correct on a snippet shown out of context but are wrong
  when the surrounding 5 lines are visible. A cheap "look for a gate N
  lines above" heuristic would fix most of them without needing AST.
- **External tools need curation.** cppcheck + clang-tidy + clazy all
  emit driver-level diagnostic lines (file-not-found errors, progress
  banners, unknown-warning warnings) that currently flow through as
  findings. `OutputFilter` needs a pass to strip these — external tools
  produce noise around their findings, not just findings themselves.
- **Project idioms need per-project allowlists.** Qt parent-child `new`,
  `signals:`/`slots:`/`emit`, and Config-sourced trust are all things the
  codebase does by design. The rule framework should support a project-
  level disable list for checks that encode different idioms. Currently
  suppressing each instance individually is the only recourse.
- **Severity tiers leak.** `Missing Compiler Warning Flags` and
  `Overly Long Source Files` are advisories; classifying them as MINOR
  (same tier as real bugs) makes triage harder. A dedicated INFO tier
  for "advisories that aren't bugs" would keep MINOR for actual smells.

---

## Appendix — what was NOT in the report

No CVE or dependency findings appeared (the project has no lockfile
dependencies beyond Qt6 system libraries). No `TODO`/`FIXME` scans
were triggered. No test-coverage findings.
