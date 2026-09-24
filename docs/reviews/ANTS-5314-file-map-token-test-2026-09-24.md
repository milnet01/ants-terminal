# ANTS-5314 — does a file map save tokens? Measured: no

Genre: record

Date: 2026-09-24. Asked for by claude-ab (v2 orchestrator), with the
condition that the map ships only if a session reading it spends fewer
tokens answering "where does X live" than one that does not.

## Headline

**The map did not win. No map file is committed.**

- Both arms answered all eight questions correctly.
- The map arm spent **more**: 88,093 tokens per question on average against
  84,768, about 3,300 more (+3.9%). It was cheaper on 3 of 8 questions.
- **The agents given the map mostly did not read it.** Of the four map-arm
  transcripts checked, two searched inside it with `workspace_search` and
  two never touched it. Reading it whole would have cost about 13,000
  tokens more, per question.

## Why it cannot win here

A lookup is already cheap in this project. Every agent sat at about 80,000
tokens of fixed context before starting, and found the answer in 2 to 11
tool calls on top. The indexed verbs (`codebase_index`, `find_definition`,
`workspace_search`) answer "where is X" in one call. A 52 KB map read up
front costs more than the searches it would replace.

## What was tested

- **The map:** `tools/measure/filemap_prototype.py` over the 2,369 tracked
  files. 614 are listed one by one; 1,755 sit inside two collapsed
  directories (`docs/specs/`, `tests/features/`), and each collapsed line
  says how many files it holds. 500 of the listed files carry a quoted
  purpose; the rest say "no stated purpose". 52,073 bytes.
- **Questions:** shipped roadmap items whose own commits changed one or two
  source modules, drawn with `random.seed(5314)` from 583 candidates. In
  draw order, the first eight that had shipped. Each question is the item's
  roadmap headline, verbatim. The ground truth is the source files the
  item's commits changed.
- **Arms:** the same `ants-explore` agent and the same prompt. The map arm's
  prompt adds one sentence: the map's path, and "read it first and use it
  to decide where to look".
- **Measure:** the `subagent_tokens` the harness reports for each agent, and
  whether the answer names a ground-truth file.

## Results

| Item | No map: tokens | Tool calls | Map: tokens | Tool calls | Both correct |
|---|---|---|---|---|---|
| ANTS-4822 | 86,198 | 8 | 96,251 | 7 | yes |
| ANTS-5021 | 84,250 | 4 | 93,563 | 5 | yes |
| ANTS-4647 | 83,937 | 4 | 95,563 | 11 | yes |
| ANTS-4836 | 82,477 | 2 | 82,825 | 2 | yes |
| ANTS-1257 | 86,935 | 3 | 86,796 | 4 | yes |
| ANTS-1707 | 86,736 | 8 | 84,197 | 5 | yes |
| ANTS-4392 | 83,458 | 3 | 83,472 | 3 | yes |
| ANTS-2090 | 84,156 | 5 | 82,078 | 2 | yes |
| **Mean** | **84,768** | | **88,093** | | 8/8 each |

## Limits

- **n = 8, and correctness saturated.** Every question was answered in both
  arms, so the test can speak only to cost, not to whether a map rescues a
  lookup that would otherwise fail.
- **The agent type steers.** `ants-explore` is told to prefer the indexed
  verbs, which is why the map arm grepped the map rather than reading it. A
  session with no code index might behave differently. Untested: that is
  the case where a map could still pay, and the prototype is kept so it can
  be run there.
- **Where the purposes come from limits the map.** It quotes header
  comments, and core files like `src/terminalgrid.h` and
  `src/fileoutline.cpp` have none, so they read "no stated purpose".
  `docs/subsystems.md` fills some of those gaps. Some header sentences are
  honest quotes and poor purposes: `auditdialog`'s first sentence is a note
  about a different file.
