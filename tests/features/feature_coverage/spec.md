# Feature coverage — spec↔code drift & CHANGELOG↔test mapping

Two parsers with fuzzy behaviour. The lanes are additive (finding an
issue on one lane doesn't block the other), but both share the same
"silent coverage gap" failure mode the test is guarding against —
a shipped feature with no locking test.

## Background

Origin: follow-on to the 2026-04-21 RetroDB audit-hygiene work. Once
the scanner calibration was tightened, the remaining signal loss was
*upstream* of the scanners — features shipping without any test to
pin them, spec text referring to symbols that had since been renamed.

These two lanes surface the gap at audit time rather than bug-hunt
time. They're Info/Minor severity — the value is raising awareness,
not gating releases.

## Lane 1 — Spec token extraction (invariants 1-6)

The extractor scans markdown for backtick-fenced tokens and filters
to identifier-shaped ones.

1. **Empty input → empty list.** `extractSpecTokens("")` returns `[]`.

2. **Identifier shapes accepted.** CamelCase, snake_case, kebab-case,
   scoped::names, and dotted.ids are all recognized. Given

       The `RemoteControl::dispatch` routes `"launch"` to `cmd_launch`
       via `new-tab` and `helper.func`.

   the extractor returns `{RemoteControl::dispatch, cmd_launch,
   new-tab, helper.func}` (quoted `"launch"` is excluded — the
   quotes aren't part of a backticked token).

3. **Short tokens dropped.** Backtick content shorter than 4 chars
   (e.g. `\n`, `id`, `ok`) is not returned.

4. **Stopwords dropped.** Common language keywords and Qt types
   (`QString`, `nullptr`, `class`, `void`) are filtered, because a
   generic codebase always contains these and reporting drift on
   them would be noise.

5. **Dedup by token.** A token that appears on lines 3 and 17 is
   reported once, with `line == 3` (first occurrence wins — readers
   navigate to the earliest mention).

6. **Line numbers are 1-based.** The first line of the input is
   line 1, not line 0.

## Lane 1 — Drift detection (invariants 7-8)

7. **Predicate controls the filter.** `findDriftTokens` with a
   predicate that returns `true` for `foo` and `false` for `bar`
   returns only `bar` given input that mentions both.

8. **No drift → empty list.** When the predicate returns `true` for
   every candidate, the result is `[]`.

## Lane 2 — CHANGELOG bullet extraction (invariants 9-13)

The extractor reads the topmost `## ` section of a Keep-a-Changelog
body.

9. **No `## ` header → empty list.** Files without a version header
   return `[]`.

10. **Top section only.** Given two `## [x.y.z]` headers, only
    bullets between the first and second are returned.

11. **Section tagging.** Bullets under `### Added` are tagged
    `section = "Added"`, under `### Fixed` are `"Fixed"`. Bullets
    before any `### ` heading have `section = ""` (unusual but
    tolerated).

12. **Leading `- ` stripped.** A bullet line `- Foo bar.` yields
    `text = "Foo bar."`.

13. **Line numbers are 1-based and point at the bullet line.**
    A bullet on source line 15 has `line == 15`.

## Lane 2 — Fuzzy bullet↔title matching (invariants 14-17)

14. **Backtick-token match wins.** A bullet mentioning `` `launch` ``
    matches a title containing `` `launch` ``, regardless of
    surrounding prose.

15. **Significant-word fallback.** Bullets without backtick tokens
    match a title if ≥2 of the bullet's first-120-chars
    significant words (≥4 chars, non-stopword, lowercase) appear
    in the title.

16. **No match → false.** A bullet with unrelated content
    (different feature name, no shared backtick tokens, <2
    significant-word overlap) returns `false` against any title list.

17. **Empty title list → false.** A bullet vs `[]` always returns
    `false` (nothing to match against).

## On-disk runners (invariants 18-22)

The two runners compose the parsers above with a real project tree.
These invariants pin the resolution rules, which is where both lanes
have misfired on projects whose layout differs from this one's.

18. **A cited filename that exists in the tree is not drift.**
    `runSpecDriftCheck` resolves tokens against a blob that includes
    every walked file's project-relative path, so a spec citing its
    own sibling test file (`test_min_size.py`) reports nothing —
    even when no other file's *text* mentions that filename.
    Before ANTS-4098 the blob held file *contents* only, so a cited
    test filename resolved by accident (a build file listing it) and
    on a project whose test files are named in no manifest, every
    such citation was reported as drift.

19. **A cited symbol that exists nowhere is still drift.** The
    path-manifest widening must not silence the lane: a spec citing
    `ghost_symbol_xyz`, absent from every file's text *and* every
    file's path, is still reported.

20. **Entry-id extraction.** `extractChangelogEntryId` returns the
    trailing parenthesised id of a CHANGELOG bullet
    (`**Thing** (FIBR-0042)` → `FIBR-0042`) and `""` when there is
    none. Only a *trailing* id counts — that is the position
    `changelog_log` writes it in.

21. **Id-keyed coverage beats prose.** In
    `runChangelogCoverageCheck` a bullet carrying an id is covered
    when that id appears anywhere in the `tests/features/*` corpus,
    whatever its prose says. Before ANTS-4099 the lane compared
    bullet prose against each spec's H1 title alone, so a project
    keying coverage by ticket id failed every entry — changelog
    prose is written for users and spec titles for developers, so
    the better the prose, the worse the match.

22. **Fallbacks are unchanged.** A bullet whose id appears nowhere
    in the corpus is still reported, and a bullet carrying no id at
    all is matched by invariants 14-15 exactly as before.

## Acceptance

- `ctest -L fast -R feature_coverage` passes.
- Run the audit on this very project with the two checks enabled:
  lane-1 emits ≤5 drift findings against current spec corpus;
  lane-2 emits ≤3 coverage findings against current CHANGELOG
  top section. (Exact counts vary as the project evolves; the
  point is the signal rate is low — the checks are accurate
  enough to be useful without drowning the real findings.)

## Why this test exists

Both parsers are regex- and heuristic-driven. A well-meaning edit
to tighten the stopword list or loosen the identifier shape can
silently change the signal rate of the audit — too strict drops
real findings, too loose drowns them. This test pins the expected
behaviour on curated inputs so changes are caught at commit time.
