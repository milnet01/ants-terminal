# ANTS-1452 — `workspace_search` opts in to gitignored / hidden files

**Status:** in flight (Bundle C follow-up, 2026-05-17).
**Kind:** feat.
**Lanes:** `remotecontrol` (`cmdWorkspaceSearch`),
`claudeintegration` (tools/list schema), `mcp_workspace_search`
feature test.
**Source:** external-cc-feedback-2026-05-17 (Vestige CC session
hit 0 matches against `compile_commands.json` after a project
relocation; falling back to `Bash grep -r` revealed 1 563 stale
`/mnt/Storage/...` prefixes).
**Pairs with:** ANTS-1248 (introduced the tool; spec extended here
with the new flags + envelope echoes).

## Problem

`workspace_search` (ANTS-1248) wraps ripgrep with rg's default
behaviour, which honours `.gitignore` / `.ignore` /
`.git/info/exclude` and skips dotfile paths. That is right for
code-review queries — the assistant should not be drowned in
build-output noise. But it makes the tool **blind** to drift in
generated artifacts that gitignore by convention:

- `build/compile_commands.json`
- `obj-*/*.d`
- `.ruff_cache/`, `.mypy_cache/`, `.semgrep_cache/`
- Lockfiles in some repos (`package-lock.json` in projects that
  gitignore it)
- Any hidden dotfile under the project root

Concrete repro from the source feedback: after a project moved
from `/mnt/Storage/...` to its new path, the caller wanted to find
stale path prefixes. `workspace_search` returned **0 matches**.
A fall-back `grep -r /mnt/Storage/ .` returned **1 563 matches** —
all inside `compile_commands.json`, which is gitignored.

The token-saving claim in the tool description
(`Bash grep -r ...` ≈ 250–4 500 tokens per query) is defeated for
any audit that needs to cover generated files — callers fall
back to Bash and pay the token tax. Equally bad, a `0 matches`
result that is filter-induced is **indistinguishable from a
genuinely clean tree** without a second tool invocation.

## Goal

Add two opt-in flags that map cleanly onto ripgrep's existing
"don't filter" surface, plus an envelope echo so a `0 matches`
caller can tell filter-induced vs. genuinely-clean apart.

1. **`respect_gitignore`** (boolean, default `true`). When `false`,
   add `--no-ignore-vcs --no-ignore` to the rg argv — bypasses
   `.gitignore`, `.ignore`, `.git/info/exclude`, and the per-user
   global ignore file. Tracked + ignored files are searched.
2. **`include_hidden`** (boolean, default `false`). When `true`,
   add `--hidden` to the rg argv — searches `.dotfile` paths
   that rg otherwise excludes. Independent of (1) — a project may
   want hidden config (`.env.example`) without disabling
   gitignore (e.g. when grepping for a key name across docs and
   dotfiles, but not bothering with build outputs).
3. **Echo filter values** in the response. The `ok:true` envelope
   gains `respect_gitignore` and `include_hidden` keys reporting
   the effective values (post server-side parse). A caller that
   got `matches:[]` can inspect them and decide whether to retry
   with the flags flipped.

Both flags default to the current behaviour, so existing callers
are unaffected. The MCP schema declares both with `default:`
values; the description names the use-case (stale-path audits in
build outputs) so Claude knows when to flip them.

## Non-goals (V1)

- **No `--no-ignore-parent` / `--no-ignore-global` granularity.**
  The three rg ignore sources (vcs/local/global) collapse under
  one `respect_gitignore=false` switch. If a project surfaces a
  case where per-source control matters, lift the schema then.
  `--no-ignore` is rg's umbrella that turns all three off; pairing
  it with `--no-ignore-vcs` is belt-and-braces (rg accepts both
  without conflict — the umbrella is the load-bearing flag, the
  vcs flag is documented in the feedback as "most ripgrep-native").
- **No `scope: "tracked" | "all"` enum.** Two booleans give the
  caller every combination; the enum form would tie hidden-file
  search to gitignore-bypass, which the two-axis model rejects.
  Echoing both booleans separately also gives the
  "is-this-filter-induced" diagnosis a clean shape.
- **No skip-count instrumentation.** Surfacing
  `ignored_files_skipped: N` accurately requires running rg with
  `--debug` (verbose stderr) or comparing against a second rg
  invocation, both at significant cost. The boolean echo is
  enough for the diagnostic use case the feedback called out
  ("0 matches: is this filter-induced?"); count-precise
  instrumentation is a follow-up if requested.
- **No `.git/` path bypass.** Even with both flags `true`, rg
  still excludes `.git/` itself (rg's hardcoded behaviour —
  `--hidden` does not flip it). Callers needing to grep inside
  `.git/` should use Bash directly. Document the carve-out in
  the tool description.

## Design

### Argv extension

`remotecontrol.cpp::cmdWorkspaceSearch` (the existing body at
roughly lines 2599–2818). New parse step after the existing
`isRegex` / `caseMode` reads:

```cpp
const bool respectGitignore = req.value("respect_gitignore").toBool(true);
const bool includeHidden    = req.value("include_hidden").toBool(false);
```

Argv extension right before the `--` separator that closes the
flag block:

```cpp
if (!respectGitignore) {
    argv << QStringLiteral("--no-ignore-vcs")
         << QStringLiteral("--no-ignore");
}
if (includeHidden) {
    argv << QStringLiteral("--hidden");
}
```

Both flags are literal switches with no argument values — no
new escape / quoting surface, no new TOCTOU window. Argv is
still shell-less (INV-3 unchanged). `--max-columns 500`, the
`--threads` cap, and `-L` exclusion are untouched, so the DoS /
symlink-loop story is unchanged. (The thread count itself moved
1 → 4 in ANTS-3732; it was never the DoS lever — `--max-columns`
and the wall budget are.)

### Envelope echo

The `ok:true` response gains two keys, placed between
`truncated` and `elapsed_ms` to keep the existing field order
stable for log parsers:

```jsonc
{
  "ok": true,
  "pattern": "/mnt/Storage/",
  "matches": [...],
  "truncated": false,
  "respect_gitignore": true,
  "include_hidden": false,
  "elapsed_ms": 12
}
```

Both echoes reflect the **effective** (post-parse, post-default)
values, not the raw request. A caller that omitted both reads
`true` / `false` back.

The error branch (`ok:false`) does **not** add the echoes — error
responses are pre-search refusals; the filter values would not
have shaped the absent result.

### Schema extension

`claudeintegration.cpp` tools/list registration. Two new
properties added to `inputSchema.properties`, alongside the
existing seven:

```cpp
QJsonObject respectGitignoreProp;
respectGitignoreProp["type"]    = "boolean";
respectGitignoreProp["default"] = true;
respectGitignoreProp["description"] = QStringLiteral(
    "When false, bypass .gitignore/.ignore (--no-ignore-vcs "
    "--no-ignore). Use for audits across generated files like "
    "compile_commands.json, build/, or cache dirs.");
QJsonObject includeHiddenProp;
includeHiddenProp["type"]    = "boolean";
includeHiddenProp["default"] = false;
includeHiddenProp["description"] = QStringLiteral(
    "When true, search dotfile paths (--hidden). Excludes .git/ "
    "itself regardless of this flag.");
props["respect_gitignore"] = respectGitignoreProp;
props["include_hidden"]    = includeHiddenProp;
```

Tool description gains one sentence pointing at the use case so
Claude reaches for the flag without re-reading the schema:

> Stale-path audits or grep over generated/cache files: pass
> `respect_gitignore=false` (and `include_hidden=true` if the
> target is dotfile-cased).

### No new error codes

Both flags are `toBool()`-coerced. Non-boolean JSON values
(string, null, array) coerce to the default per Qt's permissive
`QJsonValue::toBool(defaultValue)` semantics. This matches the
existing `regex` flag's parse and avoids adding a new validation
branch. Misuse (`respect_gitignore: "false"` as a string) is
silently treated as the default `true` — documented in the
schema, surfaced via the envelope echo.

## Invariants

- **INV-1 / argv adds `--no-ignore-vcs --no-ignore` only when
  `respect_gitignore=false`.** Source-grep against
  `remotecontrol.cpp` confirms both flag strings appear under
  the gating branch. Default-path argv (omitted or `true`) does
  not include either flag.
- **INV-2 / argv adds `--hidden` only when `include_hidden=true`.**
  Default-path argv does not include `--hidden`.
- **INV-3 / shell-less argv preserved.** No `bash -c`, no
  `/bin/sh`, no `system(`, no `popen(`. Both new flags are
  appended to the existing argv list, never spliced into a
  single string. ANTS-1248-INV-3 holds.
- **INV-4 / response echoes both filter values on success.**
  `ok:true` envelopes from `cmdWorkspaceSearch` contain
  `respect_gitignore: <bool>` and `include_hidden: <bool>`,
  reflecting the effective values. Both keys absent on
  `ok:false` envelopes.
- **INV-5 / schema declares both new props with defaults.**
  `tools/list` for `workspace_search` includes
  `respect_gitignore` (type:boolean, default:true) and
  `include_hidden` (type:boolean, default:false) in
  `inputSchema.properties`. Neither is added to `required`.
- **INV-6 / defaults match pre-1452 behaviour.** A request
  omitting both flags produces argv with no `--no-ignore*` and
  no `--hidden` entries — byte-identical to the ANTS-1248 baseline
  argv. Existing callers see no change.
- **INV-7 / `.git/` carve-out documented.** Tool description
  in `claudeintegration.cpp` names the `.git/` exclusion that
  survives `include_hidden=true`. This is documentation-only;
  no source check.

## Tests

Extend `tests/features/mcp_workspace_search/test_mcp_workspace_search.cpp`
with INVs 1–6 above as source-grep checks. The existing 10
invariants stay locked; the new ones append after them.

Test additions (all source-scrape, no rg fork):

- **INV-1a / INV-1b:** `remotecontrol.cpp` contains the literal
  strings `"--no-ignore-vcs"` and `"--no-ignore"`, and both sit
  under a branch keyed on `respect_gitignore`.
- **INV-2:** `remotecontrol.cpp` contains the literal `"--hidden"`
  under a branch keyed on `include_hidden`.
- **INV-3:** ANTS-1248-INV-3 source-grep is re-run unchanged
  (the new flags ride existing argv plumbing, but the regression
  guard stays explicit).
- **INV-4:** `remotecontrol.cpp` sets `out["respect_gitignore"]`
  and `out["include_hidden"]` in the `ok:true` builder.
- **INV-5:** `claudeintegration.cpp` registers both property
  names as JSON keys; tool description mentions
  "respect_gitignore" or "gitignore".
- **INV-6:** parse-side reads use `.toBool(true)` /
  `.toBool(false)` — the default-preserving overload, not the
  zero-arg form.

End-to-end runtime test deferred: the existing feature test
locks the wiring contract, and the unit-level rg-fork test
requires an integration harness this project doesn't have. The
runtime behaviour is testable manually with a fixture project
that contains a `.gitignore`'d artifact — the spec includes the
recipe in the test journal.

## RAM / build cost

`cmdWorkspaceSearch` gains two `bool` locals and four argv
entries in the worst case (`--no-ignore-vcs --no-ignore --hidden`).
Wire-cost change: response envelope grows by ~40 bytes
(`,"respect_gitignore":true,"include_hidden":false`); negligible
against the 1–50 KiB typical body. No new heap allocations.
Schema bytes: ~250 B added to `tools/list` for the two new
property entries — emitted once per session at the MCP handshake.

LoC: ~12 in `remotecontrol.cpp`, ~18 in `claudeintegration.cpp`,
~40 in the feature test. ~70 LoC total.

## Migration

None. Existing callers omit both flags and get byte-identical
argv. The envelope echo is additive — log parsers that key on
`matches` / `truncated` / `elapsed_ms` ignore unknown fields.
Future callers needing the audit lane pass `respect_gitignore=false`
explicitly.

## Step → verify plan

1. Write feature test additions (TDD-red against current source).
   → verify: `ctest --test-dir build -R mcp_workspace_search`
   fails on the 6 new invariants.
2. Extend `cmdWorkspaceSearch` parse + argv (12 LoC) and ok:true
   envelope build (2 LoC). → verify: same ctest passes.
3. Extend schema in `claudeintegration.cpp`. → verify: same
   ctest passes; manual `tools/list` smoke shows both props.
4. Update tool description with the stale-path-audit sentence.
   → verify: source-grep test for INV-5 still green.
5. ROADMAP / CHANGELOG entries. → verify: `grep ANTS-1452 ROADMAP.md CHANGELOG.md` returns both lines.

## Pairs / cross-refs

- **ANTS-1248** — the parent spec. The 10 original invariants are
  preserved verbatim; this spec adds 6 more.
- **External feedback note** —
  `external-cc-feedback-2026-05-17` (the Vestige CC session that
  hit 0 matches against `compile_commands.json`).
