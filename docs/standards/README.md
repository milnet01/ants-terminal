# Project Standards

**The four shared standards are owned globally at `~/.claude/standards/` and
read in place.** The files below with the same names are **deltas**: a pointer
to the global owner plus the Qt/C++/Ants-specific rules that cannot live in a
language-agnostic standard. Read the global file for the rule; read the
project file for what this project adds.

That reversed on 2026-08-12. Until then these were verbatim `/start-app`
copies kept in sync by discipline, with nothing checking them. When they were
finally reconciled all four had drifted, and three instructed behaviour the
current global standard forbids — a `git push --tags` that publishes every
local tag, a prove-your-test recipe whose green run skipped the rebuild and so
could report a false pass, and a naming rule that mandated camelCase for this
repo's Python. **A copy of a global standard does not belong here.**

| Delta file | Global owner | What the project file adds |
|----------|--------|--------|
| [coding.md](coding.md) | `~/.claude/standards/coding.md` (+ `languages/cpp.md`, `qt.md`, `python.md`) | House style (`s_` statics, K&R, `#pragma once`, signals/slots never sibling calls) and `setOwnerOnlyPerms()` in `src/secureio.h`. |
| [documentation.md](documentation.md) | `~/.claude/standards/documentation.md` | § 1.8 the `doc-examples` marker contract, § 3 ROADMAP/CHANGELOG routing, § 7 Qt accessibility, § 9 the fold-in heading. Section numbers are preserved for inbound links. |
| [testing.md](testing.md) | `~/.claude/standards/testing.md` (+ `languages/cpp.md`) | The corrected prove-a-test-is-real recipe, audit-rule fixtures, the real label set, the bundle-target trap. |
| [commits.md](commits.md) | `~/.claude/standards/commits.md` (+ `releases.md`) | This repo is public; the `ci-parity.sh` / pre-push gate; `cut-rc.sh` releases; `/bump`. |

**One file is a MIRROR, not a delta.** [`security.md`](security.md) is a
verbatim copy of `~/.claude/standards/security.md`, kept in the repo because
this repo is public and a pointer into a private home directory is useless to
an outside reader. **Do not edit it here** — corrections go upstream and are
re-copied down. Project-specific security rules go in `coding.md`.

**Two files here are NOT deltas, deliberately.** `roadmap-format.md` is
**upstream of** its global copy (CFG-0069, 2026-08-12) — the parser, the store
and the migration live here, so where the two disagree this one governs.
`specs.md` is a full standard; its § 0 records why.

Sub-spec (upstream of the global copy — see above):

| Sub-spec | Covers |
|----------|--------|
| [roadmap-format.md](roadmap-format.md) | Detailed `ROADMAP.md` and `CHANGELOG.md` format spec — file-header marker, status / theme emojis, stable IDs (`PROJ-NNNN`), insertion semantics, `Kind:` / `Source:` taxonomy, current-work signaling, fold-in subsections, anti-patterns. Read when authoring either file or any tooling that consumes them. |

Project-local standards (not part of the shareable `/start-app`
template):

| Standard | Covers |
|----------|--------|
| [dialogs.md](dialogs.md) | Dialog convention for every `QDialog` — theme conformance via `DialogChrome`, user-resizable, size persisted to `Config`, always re-centered over the terminal window on open (D1–D4). |
| [mcp-tools.md](mcp-tools.md) | Ordered authoring checklist for adding a tool to the Ants MCP surface — registration, caller-cwd contract, path validation, response wrap, refusal codes, ETag / `fields=` opt-ins, cache contract, required tests. Umbrella over mcp-error-codes.md + mcp-caches.md. |
| [status-bar.md](status-bar.md) | Status-bar widget convention specific to this codebase's `MainWindow` + `ClaudeStatusBarController` architecture. |
| [audit-false-positives.md](audit-false-positives.md) | False-positive ledger (`.ants_review_falsepos.jsonl`) shared across the `/audit`, `review-contract`, `/code-quality-review` and `/test-audit` sweep skills — schema, CC write contract, MCP read contract. |
| [mcp-caches.md](mcp-caches.md) | Keying + relocation contract for every MCP cache (ANTS-1439). Invariant: a path-keyed cache may go cold/orphan on project move but must never shadow. Inventory table + "adding a new cache" checklist. |
| [mcp-error-codes.md](mcp-error-codes.md) | Canonical taxonomy for the `code` field on MCP refusal envelopes (ANTS-1353). Five categories: input validation, resource state, caller-cwd contract, I/O, dispatcher. |
| [specs.md](specs.md) | Spec-authoring standard for `docs/specs/ANTS-NNNN.md` (ANTS-1728): required structure, INV-N bullet form, grounding/RAM/security conventions, `spec_query` machine-readability contract. |
| [test-audit-resume.md](test-audit-resume.md) | Resume recipe for picking up a partially-completed `/test-audit` in a follow-up session — `partition_token` is in-process LRU, not durable; the recipe covers the explicit `session_memory` round-trip and the partition-re-run fallback (ANTS-1580). |
| [mcp-feedback-files.md](mcp-feedback-files.md) | Format spec for the cross-session `*_Ants_MCP_Feedback.md` files that other CC sessions use to report MCP issues — contributor/maintainer roles, the maintainer-block watermark anchor, and the un-triaged-delta parser contract the `feedback_query` verb (ANTS-1961) consumes. |

## Historical / superseded

| File | Notes |
|------|-------|
| [mcp-errors.md](mcp-errors.md) | Earlier (2026-05-12) draft of MCP error-code families. Superseded by `mcp-error-codes.md` (ANTS-1353) — kept as a historical breadcrumb only. Do not cite in new code or specs. |

## How they fit together

The standards plus `ROADMAP.md` form a closed loop (§-numbers below are the
**global** files' unless the link points into this folder):

1. **ROADMAP item** declares an `[ID]`, `Kind:`, and `Source:`
   (per [roadmap-format § 3](roadmap-format.md)).
2. **Implementation** follows the standard for that Kind:
   - `implement` / `fix` / `refactor` → [coding.md](coding.md)
   - `doc` / `doc-fix` → [documentation.md](documentation.md)
   - `test` → [testing.md](testing.md)
   - `chore` / `release` → global `releases.md` + [commits.md](commits.md)
3. **Tests** follow [testing.md](testing.md) — TDD by default.
4. **Commit** uses `<ID>: <description>` per global `commits.md` § 1.1.
5. **CHANGELOG** entry under `[Unreleased]` cites the ID per
   [roadmap-format § 4.2](roadmap-format.md).
6. **Release** flips the ROADMAP bullet from 🚧 to ✅, moves the
   `[Unreleased]` entry to a dated section per
   [roadmap-format § 4.3](roadmap-format.md).

Every step has a single owner and a single source of truth — no
rules buried in commit messages, no conventions inferred from
existing code, no "ask the original author".

## Adopting these standards in another project

**Don't copy them.** Read `~/.claude/standards/` in place; that is what it is
for. This folder previously told you to copy the four files verbatim, and this
project is the worked example of why that fails: the copies drifted for three
months, nothing noticed, and three of them ended up instructing behaviour the
owner forbids.

If a project genuinely needs to differ, write a **delta** like the four above —
a pointer to the global owner plus only what is specific to that project — and
say in its header why the difference exists. A delta that grows back into a
restatement of its owner has become a second standard.

## Versioning

Each standard carries a version marker in its first-line HTML
comment:

```html
<!-- ants-coding-standards: 2 -->
```

Future revisions increment the version number. Backwards-
incompatible changes (renaming a section, removing a Kind value,
adding a required field) require a major version bump. Additive
changes stay on the current version.

**The four delta files went to `: 2` on 2026-08-12** — the cut from
full copy to delta renames and removes sections, which is
backwards-incompatible by the rule above.

**Exception — sub-specs.** `roadmap-format.md` uses
`ants-roadmap-format-spec: 1.1` rather than the standard
`ants-<name>-standards: N` pattern. The `spec` suffix signals
it tracks the *data-file format* (what conforming `ROADMAP.md`
files must look like), not an authoring guideline for
practitioners. The `1.1` subversion marks one additive revision
(the `Layman:` field). This is the only sub-spec exception;
all other files in this directory follow the `-standards: N`
form.
