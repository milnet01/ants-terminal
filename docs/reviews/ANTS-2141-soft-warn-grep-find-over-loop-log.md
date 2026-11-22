# ANTS-2141 — soft-warn the `grep`/`find`-over-source veto into a non-blocking MCP-index nudge — review loop log

Record for [`../specs/ANTS-2141-soft-warn-grep-find-over.md`](../specs/ANTS-2141-soft-warn-grep-find-over.md) § Cold-eyes loop log.

## Cold-eyes loop log

### Loop 1 (2026-06-16) — 2 independent reviewers, all verified findings folded

- **Design (B-M4/M5, verified via claude-code-guide re-fetch):** dropped
  `permissionDecision:"allow"` from the warn envelope — `additionalContext`
  injects on PreToolUse with no decision, so the pure-advisory form avoids any
  permission-escalation and matches the cited precedent (§2.1).
- **Match algorithm (B-H1/H2/H3):** replaced the blanket `^/` exemption with an
  explicit non-source-root list (absolute paths *into* the repo now warn);
  defined recursive-intent eligibility so `rg`/pathless `grep -rn` are covered;
  added the before-first-pipe `pre` rule for compound commands and a `$PPID`
  fallback; added a worked-case table (§2.2).
- **Throttle/cache (B-C1, A-M2, open-Qs):** specified `mkdir -p` + **fail-open**
  semantics (broken cache → warn, never silence); pinned a single cache tree
  `grep-nudge/{<PPID>.stamp,count.jsonl}` (was stated three inconsistent ways);
  defined PPID-missing behaviour (§2.3/§2.5).
- **Caps (A-M1/B-M1, open-Q1):** reconciled — warn `additionalContext` ≤ 400 B
  (INV-3); block branches keep ANTS-1252 INV-4's ≤ 200 B `reason`; ANTS-1252
  INV-4 to be split-annotated, not superseded (§2/§2.6/INV-3).
- **Tests strengthened (B-M2/M3):** INV-7 adds an after-window expiry assertion;
  INV-8 tightened to ≤ 251 post-truncate + concurrency noted as accepted-lossy.
- **Invariant collision (A-H1):** all cross-spec cites qualified `ANTS-1252
  INV-N`; self-cites bare; added a header note.
- **Ordering (A-H2):** added INV-11 — bookkeeping runs only after the
  per-project gate.
- **README (A-M3):** reframed as closing a pre-existing dangling reference
  (3 call sites), not a nice-to-have.
- **Helper contracts (B-L1):** §2.6 now gives one-line contracts for all three
  helpers.
- **Doc hygiene (A-L2, B-L2/L3):** added a Contents/TOC; named the amended
  assertions; `.log` exemption stated as a suffix match; claude-code-guide cite
  reframed (mechanism doc-verified, smoke authoritative).
- **Accepted blind spots documented** (§2.2, §5): `find -exec` search and
  `&&`-tail search — favour fewer false positives.

### Loop 2 (2026-06-16) — 2 independent reviewers, all verified findings folded

- **Doc-vs-code (A-H1):** §2 said the block branch routes to `git_state`; the
  live veto reason (`ants-bash-veto.sh:40,43`) says `get_git_status` /
  `roadmap_query`. Quoted the actual reason strings; clarified "unchanged" =
  literal strings preserved.
- **Parsing strategy (B-OQ1, the biggest gap):** named it explicitly — a
  parsed-token predicate (`pre="${cmd%%|*}"`, parameter-expansion strips,
  `[[ =~ ]]` scans on the decoded value), not glob `case`; documented the
  first-`|`-regardless-of-quoting approximation as benign (§2.2).
- **EXEMPT scope (B-M4):** non-source-root + flag scans run on `pre` (only
  `# ants-bypass` on the whole `$cmd`), so a source search that pipes into
  `/var/log` still warns (§2.2).
- **Find-destructive locus (B-H1):** clarified `-delete`/`-exec` are rejected by
  the PROJECT_FIND guard, not the EXEMPT token-scan; reconciled the "exempt"
  wording in the blind-spots note (§2.2).
- **Token cost (B-H2):** added a §4 cost line — `additionalContext` is a new
  cost class (≤400 B ≈ ~100 tokens, throttled) absent from ANTS-1252 §0's
  block-only table.
- **Test determinism (B-M1/M2/M3):** INV-7 pins `ANTS_GREP_NUDGE_THROTTLE_SEC=2`
  + `touch -d`; INV-8 asserts `== 251` (not ≤); INV-9 fixes the canonical
  fail-open fixture to a regular-file `grep-nudge` (root-independent).
- **jq fix (B-L1):** INV-1's assertion rewritten as one valid jq program.
- **README ref (A-M1/B-L2):** corrected — `ants-bash-veto.sh:24` + spec.md:31
  say `hooks/README.md`, ANTS-1252 INV-12 says `hooks/README`; normalise the
  latter when creating the file (§2.6).
- **Test numbering (A-M3):** policy stated — append new asserts, do not renumber
  7-13 (spec.md prose cites "assertion 12") (§2.6).
- **`$PPID` stability (A-M4/B-OQ2):** added a second-grep throttle check to the
  manual smoke + a follow-up note (key on session id) if PPID isn't
  session-stable (§6).
- **Tool vocabulary (A-L3/B-I1):** enumerated the `count.jsonl` `tool` values
  (§2.4); added the ≤251-transient off-by-one clause; cross-ref'd ANTS-1252 §1.4
  for the `~/.cache/ants-terminal` base; cut the unspecified stamp-reaper (§4).

### Loop 3 (2026-06-16) — 2 independent reviewers, all verified findings folded

- **Line cite (both, A-H1/B-M1):** the `hooks/README.md` reference is at
  `ants-bash-veto.sh:24` (re-read to confirm — both reviewers guessed wrong at
  :25/:26, and the prior draft's :23 was also off); corrected throughout.
- **INV-4 mislabel (B-M2):** destructive `find -delete` was listed under
  "Exempt"; reworded to mark it **non-eligible** (fails PROJECT_FIND, not the
  EXEMPT scan) — the exact confusion §2.2 warns against.
- **EXEMPT substring blind spot (B-H1):** added blind spot (c) — a search whose
  *pattern* contains a non-source-root substring or ends in `.log` is wrongly
  exempted (errs toward fewer warns, accepted) (§2.2).
- **Counter ceiling (A-M2):** truncate trigger `> 500` → `≥ 500` so the
  steady-state cap is genuinely 500 (was off-by-one to 501); §2.4/§4/cache-tree
  wording reconciled.
- **INV-8 determinism (B-H2):** test runs single-threaded with the truncate as a
  hard step (the `|| true` is production resilience, not a test escape).
- **stat mechanism (B-M4):** named `stat -c %Y` (integer seconds, GNU) for
  `now - mtime` (§2.3), per the CI-parity portability concern.
- **Gate-exit citation (A-H2):** §2.5 now cites the success path `return 0`
  (`_common.sh:24`) and the three exit lines (`:19`/`:28`/`:31`).
- **Test numbering (A/B-M3):** clarified the harness uses descriptive `pass`/
  `fail` labels (no numbers); the 1-13 numbering is `spec.md` prose only; flagged
  the stale `test_hooks.sh:2,6` "12 assertions" header for refresh (§2.6).
- **Cross-doc drift (A-L3):** ANTS-1252 §1.2 (`:98`) still says git→`git_state`;
  added its fix to §2.6/§7 since this spec already touches ANTS-1252 INV-4.
- **Polish:** INV-9 fixture note (mkdir vs write branch); `ack-grep`→`ack`
  bucket; `grep-vs-index.sh` skips malformed lines; single-`cd`-only strip note;
  §2.1 body 237 B headroom under the 400 B cap.

### Loop 4 (2026-06-16) — 2 independent reviewers; findings down to line-cites + one clarity gap

- **Old block branch removal (A-H1):** stated explicitly that the old
  `grep…src/`/`find src…-name` → block case (`ants-bash-veto.sh:35-38`) is
  *removed* and superseded by the warn branch — after this change no grep/find
  blocks; only git/roadmap do (§2).
- **Line cites corrected (B-H1/H2, both verified by re-read):** the "12
  assertions" header is `test_hooks.sh:3` (cite was :2,6 → now :2-3); "assertion
  12" prose is `spec.md:61` with the numbered item at `:48` (cite was :60).
- **EXEMPT determinism (B-M1):** pinned the non-source-root/`.log` checks as
  **substring** tests on `pre` (the reading blind spot (c) relies on) (§2.2).
- **Counter rule de-duplicated (B-M2):** §2.4 now states the single pre-append
  truncate rule once (≥ 500 → last 250 → append), dropping the redundant
  "steady-state" prose that invited a second check.
- **Fail-open discipline (B-M3):** every filesystem op in **both** helpers is
  `|| true`-guarded, so the helpers are safe even though the veto runs `set -u`
  only (§2.3).
- **Invariant-immutability (A-M3):** dropped the instruction to reword ANTS-1252
  INV-12's shipped text; creating `hooks/README.md` satisfies its `hooks/README`
  reference without editing the invariant (§2.6, specs.md §5.5).
- **Byte count measured (B-L1):** §2.1 body measured at exactly 237 B (was the
  unverified "~290"); INV-3 + loop log updated.
- **Polish:** INV-7 notes the test proves the *mechanism* given a stable key
  (not that CC supplies one); INV-8 notes any seed ≥ 500 yields == 251; §4
  latency acknowledges the three added per-match filesystem ops; §2.4 `find`
  bucket only records post-eligibility.

### Loop 5 (2026-06-16) — 2 independent reviewers; both verified all 17 cross-file cites exact, only polish surviving → accepted

- **Predicate control flow (B-H1):** `ants_is_source_search` must `return 0/1`,
  never `exit` — the cited `ants_validate_session_id` mirror is the *table-test
  style*, not the control flow (that validator `exit`s, so it needs a subshell;
  ours must stay in-process callable) (§2.2).
- **Cap measurement target (B-M2):** INV-3's test extracts
  `.hookSpecificOutput.additionalContext` via `jq -r` then measures — the
  ≤400 B / 237 B cap is on the field value, not the ~320 B whole-stdout envelope.
- **Hermeticity (B-M1):** stated `<cache>` derives from `$HOME`, so the harness's
  `HOME=$tmp` redirects every write — the fact that makes INV-9/INV-11 hermetic
  (§2.5).
- **INV-4 enumeration (A-M1):** added the `.log`-suffix EXEMPT case (blind spot
  (c) depends on it) to INV-4's per-class test list.
- **Return polarity (A-M2):** documented the inverse polarity — `is_source_search`
  0 = match vs `throttled` 0 = suppress — with a "comment at both call sites"
  note (§2.6).
- **Fixture isolation (A-M3):** INV-8 runs single-threaded in its own `mktemp -d`
  `HOME`, not shared with any other case.
- **`git grep` (B-M3):** noted it is always repo-scoped, so it needs no path
  guard (§2.2 algorithm).

Convergence: loop 1 (CRITICAL/HIGH design) → loops 2-3 (MEDIUM) → loop 4
(line-cites) → loop 5 (contract-precision polish). No architectural/structural
finding survived a cold re-read past loop 1. Spec accepted for implementation.
- **impl — ANTS-5520 amendment (2026-11-04), no reviewer dispatched** — PROJECT_FIND now also needs the command to pipe into an `xargs` running a content searcher; a name-only `find` no longer warns. Origin: ANTS-5520 (a Deedbox session saw the tip on `find src tests -name '*.py' | sort`). The §2.2 table row for `find src -name '*.cpp'` flips to no warn. Verdict diff over 652 past `find` commands: 20 moved to no warn, all pure listings.
- **doc-fix (2026-11-13, ANTS-5559; no reviewer dispatched)** — INV-1's test clause now states its result (`true`, exit status 0). Run that day against `hooks/ants-bash-veto.sh`: exit 0 on the clause's input, exit 4 on a plain `ls`.
- **impl — ANTS-5776 amendment (2026-11-22), no reviewer dispatched** — § 2.4's tally row adds kind, bypassed and ppid; a `# ants-bypass` search or read is tallied as bypassed instead of skipped. INV-13 and INV-14 added, both seen red against the pre-change hook, then green. § 5's unified-readout bullet now names `token_usage`'s raw_side (ANTS-5311 INV-13).
