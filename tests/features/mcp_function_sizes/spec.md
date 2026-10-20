# `function_sizes` — ANTS-5787

A ranked shortlist of the longest functions in given paths, for a
readability review. The shape was agreed with claude-config (CFG-0830,
2026-10-19). The seam is `src/functionsizes.{h,cpp}`; the file walk it
shares with the other readability verbs is `src/codewalk.{h,cpp}`.

## Invariants

| # | Invariant | Test |
|---|---|---|
| 1 | Rows are ordered by `lines`, longest first, then by file and `body_start`. `lines` is `body_end - body_start + 1`. `top` caps the rows (default 20). | `FunctionSizes.Inv1RankedLongestFirst` |
| 2 | A brace-family body ends at the brace that balances its first `{`, skipping braces in comments and strings. A declaration ending in `;` is not a row. | `FunctionSizes.Inv2BraceBodyEndsAtItsBrace` |
| 2a | A function whose first `{` comes at or after the next symbol's line has no brace body (`fun f() = x`), and a Ruby function closes with `end`: each is counted in `scanned.unmeasured`, never given another function's size. | `FunctionSizes.Inv2aNoBraceBodyIsUnmeasured` |
| 3 | A Python body ends at its last non-blank line indented deeper than its `def`. | `FunctionSizes.Inv3PythonBodyEndsByIndentation` |
| 4 | `nesting_depth` counts the body's own level as 1 and each level opened inside it as one more. | `FunctionSizes.Inv4NestingDepth` |
| 5 | A directory walk leaves out test and generated files unless `include_tests` or `include_generated` is set, and `scanned` counts each kind left out. A file named in `paths` is measured whatever it is. | `FunctionSizes.Inv5TestsAndGeneratedLeftOut` |
| 6 | Every reply names what it looked at: `scanned.files`, `scanned.functions_measured` and `scanned.unmeasured`, so an empty answer is not a silent one. | `FunctionSizes.Inv6EmptyAnswerSaysWhatItScanned` |
| 7 | `top` outside 1 to 500 and a non-string `paths` entry refuse `bad_args`. A path outside the root or missing refuses `bad_path`. | `FunctionSizes.Inv7Refusals` |
| 8 | The verb is registered in `registerProjectScopedVerbs` with `CallerCwdContract::Required`, matches `callerCwdContractFor`, and its schema is a closed object. | `FunctionSizes.Inv8Wiring` |
