# ANTS-1307 — `project_conventions` MCP — compact convention summary

**Status:** spec draft (2026-05-20).
**Kind:** implement.
**Source:** indie-review-2026-05-13 (roadmap bundle plan pull 37).

One new read-only MCP tool that returns the subset of project
conventions relevant to a stated task type — the spec-first rule and
feature-test pairing for a `feature`, the reproduce-before-fix rule
for a `bugfix`, and so on — instead of the session re-reading the
whole ~330-line CLAUDE.md every time. A small **curated table** in
`remotecontrol.cpp` maps each task type to a handful of
`{rule, source}` entries; every `source` is a real repo-relative
doc path, and the tool verifies each exists so the table can't
silently drift from the docs it points at.

## 1. Problem

CLAUDE.md (~330 lines, ~3 K tokens) plus the five `docs/standards/`
files are pulled into context every session, even when only a small
slice is relevant. A docs-only edit doesn't need the build-OOM
guardrails or the audit-pipeline tour; a bug fix needs the
reproduce-before-fix rule and little else. `project_conventions`
returns just the slice that matters for the task at hand, cutting the
every-session preamble cost.

It pairs with the planned ROADMAP item ANTS-1292 (the CLAUDE.md
source-of-truth split): this is the **consumer API**; ANTS-1292 is
the restructure. ANTS-1292 is not shipped (no spec doc yet — a
ROADMAP card only), so v1 carries its own curated rule text rather
than parsing CLAUDE.md (§ 6).

## 2. Goal

One tool, `{task_type, caller_cwd}` in, returning:

- **conventions** — an ordered list of `{rule, source}` entries: a
  short, stable, one-line rule plus a repo-relative path to the
  authoritative doc that states it in full. A small **common** set
  (commit format, shortest-correct/reuse) prepended to every task
  type, then the task-type-specific entries.
- **sources** — the de-duplicated set of `source` paths with an
  `exists` flag each (resolved against the caller's project root) —
  the drift guard and the "go read the full thing" pointer list.

`task_type ∈ {feature, bugfix, refactor, docs, test}`.

## 3. Surface

### 3.1 No new lib

Implemented as
`RemoteControl::cmdProjectConventions(const QJsonObject&)` in
`remotecontrol.cpp`, declared public in `remotecontrol.h` next to
`cmdTaskPriors` (ANTS-1306). It reuses `resolveRootCanonical(m_main,
req)` for root resolution and `QFileInfo` for the existence check. No
new `.cpp` file, no IPC verb. MCP-only, like `spec_query`.

### 3.2 The curated table

A process-static mapping, defined once in an anonymous namespace in
`remotecontrol.cpp`. Two layers:

Each rule's text paraphrases a rule that is **actually stated in the
cited `source` doc** (verified against the doc body, not assumed). The
parenthetical `§ N` points at the section that states it.

**Common (prepended to every task type):**

| rule | source |
|------|--------|
| Commit messages use the `<ID>: <description>` mandate — imperative subject, one concern per commit. | `docs/standards/commits.md` (§ 1.1, § 2.1) |
| Shortest correct implementation; reuse before rewriting (Rule of Three on the third call-site). | `docs/standards/coding.md` (§ 1.1, § 1.3) |

**`feature`:**

| rule | source |
|------|--------|
| Write `spec.md` first as a human-readable contract and get sign-off before writing the test/code. | `docs/standards/testing.md` (§ 2.3) |
| Pair the feature with a `tests/features/<name>/` conformance test (spec.md + GUI-free `test_*.cpp` added to a subsystem bundle's SOURCES, label `features;fast`). | `tests/features/README.md` |
| Validate at boundaries, not internally; don't write error paths for scenarios that can't happen at the call site. | `docs/standards/coding.md` (§ 2) |

**`bugfix`:**

| rule | source |
|------|--------|
| TDD: write a failing test asserting the bug doesn't recur, confirm it fails on current code, then write the fix (test first, code second). | `docs/standards/testing.md` (§ 1) |
| Verify the test fails on pre-fix code (revert the fix, run, must FAIL) before locking it in. | `docs/standards/testing.md` (§ 2.2) |
| Fix the root cause; no warning-silencing, `try/except: pass`, `--no-verify`, or disabling checks as a default. | `docs/standards/coding.md` (§ 1.2) |

**`refactor`:**

| rule | source |
|------|--------|
| Pure refactors keep the existing tests passing — no behaviour change, no new test required. | `docs/standards/testing.md` (§ 1, § 7) |
| Reuse before rewriting (call it / refactor-and-call / only then write new); extract a helper on the third call-site. | `docs/standards/coding.md` (§ 1.3) |

**`docs`:**

| rule | source |
|------|--------|
| Keep `CMakeLists.txt` VERSION, `CHANGELOG.md`, and the `README.md` version line in lockstep (use the `/bump` recipe); completed ROADMAP items migrate to the matching CHANGELOG section. | `CLAUDE.md` (Versioning & release files) |
| Stable `[PROJ-NNNN]` IDs are append-only — never renumber, never reuse. | `docs/standards/roadmap-format.md` (§ 3.5.1) |
| Render the labels you want spoken — Qt strips HTML `aria`/`alt`/`role`, so set `accessibleName`/`Description` on QWidget subclasses and emit inline text beside decorative emoji. | `docs/standards/documentation.md` (§ 7) |

**`test`:**

| rule | source |
|------|--------|
| Feature-conformance tests pair `spec.md` with a GUI-free C++ test under `tests/features/<name>/`, registered into a subsystem bundle (not a fresh `add_executable` unless process isolation is needed). | `tests/features/README.md` |
| Tests test the contract, not the implementation; anchor to external signals (spec/RFC/CVE), not source structure. | `docs/standards/testing.md` (§ 2.1) |
| Audit-rule fixtures are count-based against fixture dirs, not line-number-based. | `docs/standards/testing.md` (§ 3.5) |

Rule text is intentionally short and stable; the `source` doc is the
authoritative full statement. Every `source` above is a real
repo-relative path **whose body states the paraphrased rule** (the
common + per-type rules were each grep-verified against the cited doc
during spec authoring). In a `/start-app`-scaffolded project the
`docs/standards/` bundle is identical; `CLAUDE.md` is project-specific,
so its two rows (`feature` has none; `docs` cites it once) resolve
against the caller's own `CLAUDE.md`.

### 3.3 Existence check & `sources`

After assembling `conventions` for the task type, collect the
de-duplicated ordered set of `source` values. For each, resolve
`rootCanonical + "/" + source` and emit `{path, exists}` where
`exists = QFileInfo(...).exists() && isFile()`. The `conventions`
array is returned **regardless** of `exists` (the rule text stands
on its own); `sources[].exists` is the drift signal. The
ANTS-1307 feature test asserts every emitted `source` exists in this
repo (so renaming `coding.md` without updating the table fails CI).

### 3.4 Envelope

```json
{ "ok": true,
  "task_type": "feature",
  "conventions": [
    {"rule":"Commit messages use the `<ID>: <description>` mandate — imperative subject, one concern per commit.",
     "source":"docs/standards/commits.md"},
    {"rule":"Shortest correct implementation; reuse before rewriting (Rule of Three on the third call-site).",
     "source":"docs/standards/coding.md"},
    {"rule":"Write `spec.md` first as a human-readable contract and get sign-off before writing the test/code.",
     "source":"docs/standards/testing.md"},
    {"rule":"Pair the feature with a `tests/features/<name>/` conformance test (spec.md + GUI-free `test_*.cpp` added to a subsystem bundle's SOURCES, label `features;fast`).",
     "source":"tests/features/README.md"},
    {"rule":"Validate at boundaries, not internally; don't write error paths for scenarios that can't happen at the call site.",
     "source":"docs/standards/coding.md"} ],
  "conventions_count": 5,
  "sources": [
    {"path":"docs/standards/commits.md","exists":true},
    {"path":"docs/standards/coding.md","exists":true},
    {"path":"docs/standards/testing.md","exists":true},
    {"path":"tests/features/README.md","exists":true} ],
  "sources_count": 4 }
```

`conventions_count == conventions.size()`;
`sources_count == sources.size()`.

### 3.5 Refusal codes

| code | when |
|------|------|
| `bad_args` | `task_type` missing, empty, or not one of `feature`/`bugfix`/`refactor`/`docs`/`test` (case-sensitive lower-case). The error message lists the accepted values. (`mcp-error-codes.md` § 1.) |
| `no_project` | root unresolved (no `caller_cwd`, no focused tab). (`mcp-error-codes.md` § 2.) |

(`caller_cwd_required` is emitted upstream by the dispatcher per
ANTS-1404 before the handler runs, when `caller_cwd` is absent.)

### 3.6 Edge cases

| Input | Behaviour |
|-------|-----------|
| `task_type:"Feature"` (wrong case) | `bad_args` — the enum is lower-case; the message lists the accepted values. |
| A `source` doc missing in the caller's project | `conventions` still returned in full; that `sources[]` entry carries `exists:false`. |
| Caller's project is not a `/start-app` project | the curated rule text is still returned (it is the shared v1-standards essence); `sources[].exists` flags which docs that project happens to lack. |

## 4. Invariants

| # | Statement |
|---|-----------|
| 1 | `cmdProjectConventions(const QJsonObject&)` declared public in `remotecontrol.h` (returns `QJsonDocument`); defined in `remotecontrol.cpp` with an `ANTS-1307` anchor comment. No new lib, no new `.cpp`. |
| 2 | `task_type` is validated against exactly `{feature, bugfix, refactor, docs, test}` (case-sensitive); anything else → `bad_args` with a message listing the accepted values. |
| 3 | Root resolution is `resolveRootCanonical(m_main, req)`; empty → `no_project`. |
| 4 | The curated table is process-static, defined once; it always prepends the two **common** entries before the task-type-specific entries, in the order shown in § 3.2. |
| 5 | Every `source` in the curated table is a repo-relative path that exists in this repository (enforced by the ANTS-1307 feature test). |
| 6 | `sources[]` is the de-duplicated ordered set of `source` values from the assembled `conventions`, each `{path, exists}` with `exists` resolved via `QFileInfo` against `rootCanonical`. |
| 7 | `conventions` is returned in full regardless of any `exists:false`; only `bad_args` / `no_project` make the call non-ok. |
| 8 | `conventions_count == conventions.size()` and `sources_count == sources.size()`. |
| 9 | `MainWindow::setupClaudeMcpProviders` registers `project_conventions` via `registerToolProvider` with `CallerCwdContract::Required`, delegating to `cmdProjectConventions`, with the `kRcUnavailable` fallback. MCP-only — not in the IPC `dispatch()` switch. |
| 10 | `callerCwdContractFor` classifies `project_conventions` as `Required` (explicit branch). |
| 11 | `kindForName` classifies `project_conventions` as `"convention"` (a new bucket branch before the fall-through default, so not `"other"` — enforced by `tests/features/mcp_tool_prefix_tags/` INV-3). |
| 12 | The token-cost ledger in `claudeintegration.cpp` carries `{project_conventions,{400,1500}}` (typical, cap). |
| 13 | The `claudeintegration.cpp` tools/list block registers `project_conventions` with `task_type` (required, `enum`), `caller_cwd` (required), `additionalProperties:false`, and a `selection_hint`. |

## 5. Memory & cost budget

- Per call: O(1) — index into a small static table (the 2 common
  entries + the task type's 2–3 entries = ≤ 5 conventions), then
  ≤ 5 `QFileInfo::exists` stats (one per deduped source). No
  file-body reads, no process spawns, no cache.
- Worst-case heap: the assembled `conventions` (≤ 5 entries) +
  `sources` (≤ 5) ≈ a few KiB. Trivial.
- Time: sub-millisecond plus the stat syscalls.

## 6. Out of scope

- Parsing CLAUDE.md / the `docs/standards/` bodies to derive the rule
  text live. v1 is a curated table; the planned ROADMAP item ANTS-1292
  (CLAUDE.md split) is the restructure that would let a future v2 read
  the slice straight from a machine-readable source instead of
  duplicating one-liners here.
  The § 3.3 existence check is the v1 drift guard until then.
- Returning full doc bodies. The tool returns one-line rules +
  pointers; the caller `Read`s a `source` when it needs the detail.
- Per-project custom convention tables. v1 ships the shared
  /start-app v1-standards essence for every project; a project that
  diverges sees its divergence via `sources[].exists`, not via
  per-project rule text.
- Additional task types (e.g. `release`, `security`). v1 covers the
  five highest-frequency types; widening the enum is a later edit to
  the same table.

## 7. Doc-drift fold-in (apply in the same commit)

- Flip `ROADMAP.md` ANTS-1307 to ✅.
- Add a CHANGELOG entry to the unreleased section.
- `CLAUDE.md` `remotecontrol` module map: `project_conventions` is
  MCP-only, so no IPC verb-list edit (matches `spec_query` /
  `invariant_check` / `task_priors`).
- No new error codes for `docs/standards/mcp-error-codes.md`: reuses
  `bad_args` (§ 1) and `no_project` (§ 2).
