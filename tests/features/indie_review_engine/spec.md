# indie_review_engine — IndieReviewEngine pure functions (ANTS-1112)

Locks the six pure helpers in src/indiereviewengine.{h,cpp}.

## INVs

- INV-1: derivePartition returns >= 1 lane for any CLAUDE.md
  containing `## Module map (src/)` with at least one bullet.
- INV-2: derivePartition honours
  `<projectPath>/.indie-review/partition.json` when present.
- INV-3: assembleBrief emits `=== Lane: NAME ===` first line +
  `=== file: <path> ===` for each source.
- INV-4: extractFileLineCitations returns a Citation with
  `(file, line)` for `src/foo.cpp:42` (when foo.cpp exists in the
  fixture), and skips citations whose file doesn't resolve.
- INV-5: corroboratedFindings returns one finding cited by 2 lanes
  for two reports both citing `src/foo.cpp:42`.
- INV-6: corroboratedFindings treats `(file, -1)` and `(file, 42)`
  as distinct keys.
- INV-7: synthesisPrompt emits `## Lane: <name>` for every lane key.
- INV-8: templateIndieReviewFoldInBlock first line is
  `### 🔍 Indie-review fold-in (DATE)`; first bullet starts with
  `- 📋 [ANTS-<id>]`; bullet contains `Kind: review-fix.`.
- assembleThreatModelExtras concatenates CLAUDE.md / SECURITY.md /
  .semgrep.yml under `=== <header> ===` markers; missing files
  contribute empty bodies.
