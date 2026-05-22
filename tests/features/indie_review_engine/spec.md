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
- INV-9 (ANTS-1445): synthesisPrompt wraps each per-lane report in a
  `<lane_report lane="…">…</lane_report>` fence; the threat-model
  block (when non-empty) wraps in `<threat_model>…</threat_model>`.
  Both fence pairs stay balanced (exactly one open + one close per
  lane / per threat-model block).
- INV-10 (ANTS-1445): a hostile per-lane report cannot escape its
  fence — the helper rewrites both the inner `<lane_report` (open)
  and `</lane_report>` (close) markers to HTML-escaped form so the
  outer fence's open + close tags stay unique. Attacker text
  remains inside the fence as quoted data.
- INV-11 (ANTS-1445): threat-model extras containing
  `<threat_model>` or `</threat_model>` are similarly escaped to
  keep the threat-model fence balanced.
- assembleThreatModelExtras concatenates CLAUDE.md / SECURITY.md /
  .semgrep.yml under `=== <header> ===` markers; missing files
  contribute empty bodies.
- INV-12 (ANTS-1288): suggestedMerges flags pairs of lanes with
  duplicate summaries — byte-identical summaries → rationale
  "identical summary text"; length-gated normalised Levenshtein
  similarity ≥ 0.90 → "near-identical summary text (NN% similar)";
  distinct summaries are never flagged. Advisory only — it does not
  mutate the partition (complements the ANTS-1685 file-set dedup,
  which already merges lanes resolving to the *same* files). The
  `indie_review_partition` MCP envelope surfaces these as
  `suggested_merges:[{lanes:[a,b],rationale}]`.
