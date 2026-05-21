# Project Standards

Four shareable template standards that govern how this project is
written, tested, documented, and committed. Each standard is v1
and self-contained; cross-references between them are explicit.
Project-local standards (dialogs, mcp-tools, status-bar, etc.)
are listed below the table.

| Standard | Covers |
|----------|--------|
| [coding.md](coding.md) | Code style, language idioms, error handling, comments, naming, security. Governs `Kind: implement / fix / refactor / audit-fix / review-fix` work. |
| [documentation.md](documentation.md) | README / CLAUDE.md / SECURITY.md structure, API contracts, screenshots, markdown style. Governs `Kind: doc / doc-fix` work. |
| [testing.md](testing.md) | TDD policy, test types, spec-first authoring, INV numbering, coverage, anti-patterns. Governs `Kind: test` work + the regression-test follow-through for `fix / audit-fix / review-fix`. |
| [commits.md](commits.md) | The `<ID>: <description>` mandate, hygiene, branching, push policy, release commits. Governs every commit plus release-orchestration work under ROADMAP bullets with `Kind: chore` or `release`. |

Sub-spec extracted from `documentation.md` for token efficiency:

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
| [audit-false-positives.md](audit-false-positives.md) | False-positive ledger (`.ants_review_falsepos.jsonl`) shared across `/audit`, `/cold-eyes`, `/indie-review`, `/test-audit` sweep skills — schema, CC write contract, MCP read contract. |
| [mcp-caches.md](mcp-caches.md) | Keying + relocation contract for every MCP cache (ANTS-1439). Invariant: a path-keyed cache may go cold/orphan on project move but must never shadow. Inventory table + "adding a new cache" checklist. |
| [mcp-error-codes.md](mcp-error-codes.md) | Canonical taxonomy for the `code` field on MCP refusal envelopes (ANTS-1353). Five categories: input validation, resource state, caller-cwd contract, I/O, dispatcher. |
| [specs.md](specs.md) | Spec-authoring standard for `docs/specs/ANTS-NNNN.md` (ANTS-1728): required structure, INV-N bullet form, grounding/RAM/security conventions, `spec_query` machine-readability contract. |
| [test-audit-resume.md](test-audit-resume.md) | Resume recipe for picking up a partially-completed `/test-audit` in a follow-up session — `partition_token` is in-process LRU, not durable; the recipe covers the explicit `session_memory` round-trip and the partition-re-run fallback (ANTS-1580). |

## Historical / superseded

| File | Notes |
|------|-------|
| [mcp-errors.md](mcp-errors.md) | Earlier (2026-05-12) draft of MCP error-code families. Superseded by `mcp-error-codes.md` (ANTS-1353) — kept as a historical breadcrumb only. Do not cite in new code or specs. |

## How they fit together

The four standards plus `ROADMAP.md` form a closed loop:

1. **ROADMAP item** declares an `[ID]`, `Kind:`, and `Source:`
   (per [roadmap-format § 3](roadmap-format.md)).
2. **Implementation** follows the standard for that Kind:
   - `implement` / `fix` / `refactor` → [coding.md](coding.md)
   - `doc` / `doc-fix` → [documentation.md](documentation.md)
   - `test` → [testing.md](testing.md)
   - `chore` / `release` → [commits.md](commits.md) §5
3. **Tests** follow [testing.md](testing.md) — TDD by default.
4. **Commit** uses `<ID>: <description>` per
   [commits.md](commits.md) §1.1.
5. **CHANGELOG** entry under `[Unreleased]` cites the ID per
   [roadmap-format § 4.2](roadmap-format.md).
6. **Release** flips the ROADMAP bullet from 🚧 to ✅, moves the
   `[Unreleased]` entry to a dated section per
   [roadmap-format § 4.3](roadmap-format.md).

Every step has a single owner and a single source of truth — no
rules buried in commit messages, no conventions inferred from
existing code, no "ask the original author".

## Adopting these standards in another project

Copy the four files in this folder verbatim into your project's
`docs/standards/` directory. They're intentionally
project-agnostic: language-specific notes are guidance rather
than mandates, and project-specific rules (specific module
boundaries, specific build commands) live in `CLAUDE.md` at the
repo root.

Any project-specific tweaks to a standard should be added as a
new section at the bottom of the relevant file, prefixed with
`## <Project> overrides`.

## Versioning

Each standard carries a v1 marker in its first-line HTML
comment:

```html
<!-- ants-coding-standards: 1 -->
```

Future revisions increment the version number. Backwards-
incompatible changes (renaming a section, removing a Kind value,
adding a required field) require a major version bump. Additive
changes stay on the current version.
