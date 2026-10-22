# ANTS-5502 — Check many quotations in one call: `quotation_check`

**Status:** spec, review-contract loops 1 + 2 folded, cap reached (2026-09-29). Signed off by the maintainer 2026-09-29 (message 277). Implemented 2026-09-29 on helper/ANTS-5502. Amended 2026-10-22 by ANTS-5732: an item's cited `line`, a `tolerance`, and the `hit_displaced` result (INV-11, INV-12).
**Kind:** feature.
**Source:** ROADMAP.md ANTS-5502 (claude-config joint review 2026-09-27, A1). Design answers from the maintainer session, 2026-09-28.
**Composes with:** ANTS-4547 (`src/wrapmatch.h`, the wrapped-quotation rule this verb does NOT use, § 2.6), ANTS-5506 (`ants-mcpd --call --exit-code`, which gates on this verb's top-level `findings`), ANTS-1295 (path validation), ANTS-4374 (a zero says what it looked at).

**Layman:** Review skills check that every sentence a reviewer quotes really is in the file it names. Today that is one script run per quote. This does the whole list in one call and says, for each quote, found, not found, or could not check.

## 1. Problem

Every review skill verifies a lane's quotations with `~/.claude/skills/_shared/quotation-check.sh`. The script takes each quotation as a file, so a session writes one file per quotation, plus a manifest for `--batch`, before it can check anything. A run with dozens of findings pays a tool call per quotation for those writes, and a miss says only "MISS": not where the nearest text was. The script lives in the private `~/.claude` tree, so no Ants verb and no public gate can call it.

## 2. Surface

### 2.1 The verb

`quotation_check`, project-scoped, registered in `mcp::registerProjectScopedVerbs` (`src/mcptoolregistry.cpp`), `CallerCwdContract::Required`, with its entry in `ClaudeIntegration::callerCwdContractFor`.

Arguments:

| Argument | Shape | Meaning |
|---|---|---|
| `caller_cwd` | string, required | Resolves the project root. |
| `items` | array of 1 to 500 objects, required | Each `{path, text, ref?, line?, tolerance?}`. `path` is relative to the project root, or absolute. `text` is the quotation as the lane wrote it. `ref` is an optional git revision. `line` is the 1-based line the finding cites. `tolerance` overrides the call-level one for this item. |
| `tolerance` | integer, optional | How many lines an occurrence may start from an item's `line` and still be a `hit`. Default 3 (§ 2.3). |
| `allowed` | array of non-empty strings, optional | Globs over the project-relative path. An item whose path matches none is `outside_allowed`. |
| `max_bytes` | integer, optional | The standard response cap (default 512 KiB), applied through `RemoteControl::capJsonArrayToBytes` to `results` only (§ 2.4). |

A request whose shape is wrong refuses the whole call with `bad_args`, naming the first offending index: `items` absent, not an array, empty or over 500 entries; an entry that is not an object; `path` not a non-empty string; `text` not a string; `ref` present and not a string; `line` present and not an integer of 1 or more; `tolerance`, on an item or the call, present and not an integer of 0 or more; `allowed` not an array of non-empty strings. A call-level `tolerance` names no index.

### 2.2 Normalisation: the script is the source of truth

The verb reproduces `quotation-check.sh` as of `~/.claude` commit `60fd0be`. The maintainer named `d46646e`; the script changed that same day (CR and tab as whitespace, bold and code marks dropped), and the later version is the one the skills run. Both sides, subject and quotation, pass through the script's steps in the script's order:

1. `STRIP`, on each line: `s/^[[:space:]]*>\{1,\} \?//` (POSIX basic syntax).
2. Replace each CR, tab and newline with a space.
3. Squeeze each run of spaces to one.
4. `LINK`: `s/\[([^]]*)\]\([^)]*\)/\1/g` (POSIX extended syntax): leftmost, global, non-overlapping.
5. `MARK`: delete every `**`, then every backquote.

The script's own patterns are the definition; the vectors in INV-1 check this verb against them.

The quotation is then trimmed of leading and trailing spaces. It is a hit when it occurs as a byte substring of the normalised subject, as `grep -F` finds it. Matching is case-sensitive and honours no pattern syntax.

### 2.3 Results

Each item gets exactly one `result`, in input order:

- **`hit`** — found. Where the item carries `line`, at least one occurrence also starts within `line` ± `tolerance`.
- **`hit_displaced`** — found, but the item carries `line` and no occurrence starts within `line` ± `tolerance`. It carries `line`: the line of the occurrence nearest the cited one, the earlier on a tie. An occurrence's line is the 1-based subject line holding the original byte its first normalised byte came from, so a quotation wrapped across lines takes the line it starts on. An item without `line` is never `hit_displaced`.
- **`miss`** — the search ran and the quotation is not there. It carries `matched_prefix_chars` (the length, in characters, of the longest prefix of the normalised quotation found in the normalised subject), and, when that is above zero, `line` (1-based line in the subject where that prefix's first occurrence starts) and `nearest` (the subject's original text from the start of that line, up to 200 bytes, cut on a UTF-8 boundary).
- **`not_run`** — the search did not run, so the item says nothing either way. Never folded into `miss`: that collapse is the defect the script's three-valued exit exists to stop (CFG-0163). It carries one `reason`:

| `reason` | When |
|---|---|
| `bad_path` | `PathValidation::validatePath` rejects the path for a reason other than escaping the root. |
| `not_found` | No file at the path (working tree only). |
| `not_a_file` | The path is a directory or other non-regular file. |
| `unreadable` | The file exists and could not be read. |
| `too_large` | The subject is over 16 MiB. |
| `text_too_long` | `text` is over 8 KiB in UTF-8. |
| `empty_text` | `text` is empty after § 2.2. |
| `bad_ref` | `ref` is empty, starts with `-`, or holds whitespace or a control character. Git is not run. |
| `ref_unreadable` | `git cat-file blob <ref>:./<path>` did not start or exited non-zero. It runs in the project root, so `./` makes the path project-relative; git reads a bare path from the repository's top level. `GitWrap::run` is passed a cap of 16 MiB plus one byte, and a blob over 16 MiB is `too_large`, as in the working tree. |

- **`outside_allowed`** — the path is outside the project root after symlinks are resolved, or `allowed` is given and the project-relative path matches none of its globs. The file is never opened. With a `ref`, both the root check and `allowed` use the lexical project-relative path, since a blob has no symlinks on disk.

This is a per-item result, not the whole-call `bad_path` refusal `mcp-tools.md` step 4 prescribes for a path argument, and INV-5 replaces that standard's `bad_path` test for this verb. Deliberate: one stray path must not cost a caller the verdicts on every other item (the maintainer's answer 2).

Checks run in this order, and the first that applies decides the result: `bad_path`, then `outside_allowed`, then `text_too_long` and `empty_text`, then `bad_ref`, then reading the subject (`not_found`, `not_a_file`, `unreadable`, `too_large`, `ref_unreadable`), then the match, then the cited line.

The default `tolerance` of 3 is measured. On 2026-10-22, over the lane reports of that day's readability review read at `b9fdb4c5^`, 477 quotations were found on a single line of their cited file. 88.5% sat on the cited line and 98.5% within 3 lines; the rest spread out to 170 lines. Re-run with `tools/quote-displacement-survey.py b9fdb4c5^`. The reports are gitignored, so the survey runs only where they were kept. Author's call on the cut: the curve flattens after 3, and what lies beyond is the stale citation this result exists to catch.

Glob grammar for `allowed`: `**` matches any run of characters including `/`; `*` any run without `/`; `?` one character other than `/`; everything else is literal. No braces or classes. Author's call: Qt's wildcard conversion changes meaning across the Qt versions this project builds against (6.2 upward), so the grammar is stated here and implemented by hand.

A subject is read once per call for each distinct `(ref, path)` pair, and its normalised form reused for every item naming it.

### 2.4 The envelope

```
{ok:true,
 items_checked, counts:{hit, hit_displaced, miss, not_run, outside_allowed}, files_read,
 normaliser:"quotation-check.sh 60fd0be",
 results:[{index, result}],
 findings:[{index, path, ref?, kind, reason?, matched_prefix_chars?, line?, nearest?}],
 check_errors:[{index, reason}],
 truncated?, results_dropped?}
```

- `results` has one row per item and carries only its position and result. The caller already holds each item's path and text, so echoing them back would spend tokens on what it sent.
- `findings` holds every item whose result is not `hit`, with `kind` set to that result and the detail § 2.3 names. The detail appears there only, never twice. ANTS-5506's `--exit-code` gates on it (that spec's § 2.2). Author's call: `outside_allowed` counts as a finding, because a lane quoting a file outside its permitted set is itself the defect `allowed` exists to catch.
- `check_errors` lists every `not_run` item a second time, by index and reason, and is empty otherwise. ANTS-5506 § 2.2 exits `1` on a non-empty `check_errors` and `3` on findings, so a gate can tell "could not check" from "quote not there". Without it every `not_run` would exit `3`, the collapse § 2.3 forbids.
- `max_bytes` trims `results` only. **`findings` is never trimmed**, since a gate reading a trimmed `findings` array passes. At 500 items it stays bounded by the per-item fields above.
- `items_checked`, `counts` and `files_read` are always present, so an empty `findings` sits beside the numbers that make it a result (ANTS-4374).
- `hit_displaced` is a finding, not a check error, so ANTS-5506's `--exit-code` exits `3` on it. `counts.hit_displaced` is present on every call. An item without `line` gets the same result and finding it got before ANTS-5732.

### 2.5 Where it lives

- `src/quotationcheckverb.{h,cpp}` in `ants_core_lib`: everything below the resolved root — path validation, reading files and blobs (`GitWrap::run`), the per-`(ref, path)` cache, the normaliser, the glob matcher and the item loop — as free functions taking the root as a parameter, so a test drives every invariant against a temporary directory. Its own translation unit, for the reason `docs/standards/mcp-tools.md` § Tests gives.
- `RemoteControl::cmdQuotationCheck` in `src/remotecontrol_quotation_check.cpp`, a thin handler appended last in `ANTS_RC_SOURCES_REL`: resolves the root, calls the seam.
- The schema in the `tools/list` table, with a short description (`mcp-tools.md` step 11). No ETag: the reply depends on many files.

### 2.6 Alternatives

- **Reuse `WrapMatch`** (the roadmap body's suggestion). Rejected: its rule treats whitespace and blockquote markers only. It renders no links and drops no marks, so it misses quotations the script hits. Widening it would change `workspace_search match_wrapped` and `roadmap_log amend_body` too. The maintainer ruled the script wins.
- **A `doc_citations` mode.** Rejected by the maintainer: its own verb.
- **Run the script from the verb.** Rejected: the script is in the private `~/.claude` tree, absent in CI and on any other machine, and a process per item is the cost this verb removes.

## 3. Invariants

- **INV-1** — For every vector in `tests/features/mcp_quotation_check/vectors/`, the verb's result equals the script's: HIT to `hit`, MISS to `miss`, ERROR to `not_run`. The vectors cover a plain hit, a plain miss, a blockquote, a `[text](url)` link, a link wrapped across lines, tabs, a CRLF file, bold and code marks, and a quotation wrapped at a line break. Three pin the order of § 2.2: a subject `a \nb` (a space before a line break) against the quotation `a b`, a HIT because line breaks become spaces before the squeeze; a subject `a ** b` against `a b`, a MISS because spaces are squeezed before marks go; and a subject `[a]**(b) x` against `a x`, a MISS because links are rendered before marks go. One pins `LINK`'s pattern: a subject `x [a [b](c) y` against `x a [b y`, a HIT. Each expected result was measured with the script. Broken by dropping or reordering any step of § 2.2. *Test:* the expected results are committed beside the vectors and checked on every run. Where the script is installed at `~/.claude/skills/_shared/quotation-check.sh`, the same test also runs it over the vectors and requires the committed expectations to match; elsewhere, that half skips and says so.
- **INV-2** — Every item gets exactly one result, in input order, and `counts` sums to `items_checked`. Broken by dropping an item that could not be read. *Test:* one item of each result kind in one call. (ANTS-5732: `hit_displaced` is one of the kinds.)
- **INV-3** — An item that could not be checked is `not_run` with its reason, never `miss`. Broken by treating a failed read as an empty subject. *Test:* one fixture per `reason` in § 2.3.
- **INV-4** — Every non-`hit` item is in `findings`, and `max_bytes` never removes one. Broken by capping the whole envelope. *Test:* a `miss`, a `not_run` and an `outside_allowed` item with `max_bytes` small enough to trim `results`: `truncated` is set, `findings` still holds three, and `check_errors` holds the one `not_run`.
- **INV-5** — Paths resolve before `allowed` is matched. Broken by matching the raw string. *Test:* a symlink inside the root pointing at a file outside it that holds the quoted text gives `outside_allowed`, not `hit`; with `allowed:["docs/**"]`, `src/a.md` gives `outside_allowed`, `docs/x/b.md` is checked, and a symlink `docs/l.md` pointing at `src/a.md` gives `outside_allowed`.
- **INV-6** — An item with `ref` reads the blob at that revision, not the working tree. Broken by ignoring `ref`. *Test:* a temporary repository commits text A, then the working tree changes it to B. Quoting A with `ref:"HEAD"` gives `hit`; without `ref`, `miss`. A `ref` of `-p` gives `not_run` / `bad_ref`.
- **INV-7** — A miss points at the nearest text. Broken by reporting the file's first line. *Test:* a quotation whose first 20 characters occur on line 7 and whose tail differs gives `matched_prefix_chars` 20, `line` 7, and `nearest` starting with line 7's text.
- **INV-8** — A subject is read once per `(ref, path)` per call. Broken by reading per item. *Test:* three items on one path give `files_read` 1.
- **INV-9** — Caps: 501 items refuses `bad_args`; a `text` of 8 KiB plus one byte gives `not_run` / `text_too_long`. Broken by an unbounded loop. *Test:* both cases.
- **INV-10** — Registered per `mcp-tools.md` § Tests: the registration call is in `mcptoolregistry.cpp`, the schema is `type:"object"` with `additionalProperties:false`, and an empty `caller_cwd` refuses `caller_cwd_required`. Broken by registering outside `registerProjectScopedVerbs` or with another contract. *Test:* those three standard asserts; the standard's `bad_path` assert is INV-5's (§ 2.3).
- **INV-11** — A quotation found far from its cited line is `hit_displaced`, and the nearest occurrence decides. Broken by testing only the first occurrence, by measuring from where an occurrence ends, or by letting the call-level `tolerance` win over an item's. *Test:* a subject holding the quotation on lines 4 and 40. `line` 41 gives `hit`. `line` 10 gives `hit_displaced` with `line` 4. `line` 10 with item `tolerance` 6 gives `hit`. `line` 10 with call `tolerance` 6 and item `tolerance` 0 gives `hit_displaced`. A quotation wrapped from line 4 onto line 5, cited at line 4 with item `tolerance` 0, gives `hit`.
- **INV-12** — `line` and `tolerance` change nothing for an item that does not cite a line, and a wrong shape refuses. Broken by defaulting a missing `line` to 1, which turns every hit that starts past line 4 into `hit_displaced`. *Test:* an item without `line` whose quotation is on line 40 gives `hit`, and `counts.hit_displaced` is 0. Each of `line` 0, `line` 1.5, `line` "3" and item `tolerance` -1 refuses `bad_args` naming its index; a call `tolerance` of -1 refuses `bad_args`.

## 4. Out of scope

- Moving the review skills onto this verb. That is claude-config's change, once this ships.
- Fuzzy or case-insensitive matching. The script matches exactly, and so does this.
- Subjects containing NUL bytes. `grep` treats them as binary; the vectors hold text only, and no claim is made either way.

## 5. Tests

- `tests/features/mcp_quotation_check/` — `spec.md`, `test_mcp_quotation_check.cpp` and `vectors/`. INV-1, INV-2, INV-3, INV-4, INV-5, INV-6, INV-7, INV-8, INV-9, INV-10, INV-11, INV-12. Drives the seam in `src/quotationcheckverb.cpp` directly, against a temporary directory; INV-6 uses a temporary git repository, INV-10 the source-text and `tools/list` asserts. `build_target_for` names its bundle once the test exists.

## 6. Reload

Nothing is held between calls, so nothing needs a stop or start control. The verb is registered in `mcp::registerProjectScopedVerbs()` and served by `ants-mcpd`. A change to it reaches a client when `ants-mcpd` is rebuilt and the client reconnects; the terminal is not relaunched. Added with ANTS-5732, which is the first change to need the statement.

## Cold-eyes loop log

The rows are in [`../reviews/ANTS-5502-quotation-check-loop-log.md`](../reviews/ANTS-5502-quotation-check-loop-log.md).
