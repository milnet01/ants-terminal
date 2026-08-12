# Feature: roadmap_query mode:"bundles"

Test contract for ANTS-1922 (`docs/specs/ANTS-1922.md`). Locks the
behavioural invariants of `RemoteControl::buildRoadmapBundlesEnvelope`
(the pure builder behind `roadmap_query mode:"bundles"`) plus the
dispatch/schema wiring via source-grep.

The builder is a public static taking `(const QJsonArray &cacheBullets,
int softCapBytes)` so the test drives it directly with hand-authored
bullet arrays — no QTemporaryDir, no GUI tab, no roadmap file. Each
bullet object carries the keys the builder reads: `id`, `status`
(emoji), `headline`, `headline_oneline`, `lanes`, `body`.

## Invariants under test (mirrors ANTS-1922 §3)

- **INV-1** — envelope has all six keys (`ok, mode, bundles,
  bundle_count, active_total, truncated`); every item is active
  (📋/🚧); no ✅ id appears.
- **INV-2** — token-Jaccard ≥ 0.50 (≥2 shared) items share a bundle;
  transitive A–B–C collapses; unrelated items stay separate.
- **INV-3** — `truncated:false` ⟹ `sum(size) == active_total`.
- **INV-4** — a 📋 with a ✅ sibling at Jaccard ≥ 0.60 carries
  `possibly_resolved_by` + `possibly_resolved_score` (canonical
  `int(jac*100+0.5)` rounding); identical → 100; exactly-0.60 →
  present, score 60 (locks the inclusive `>=` gate); below-gate and
  no-match → absent.
- **INV-5** — `blocked by`, `wait for ` and `until`+(`lands`|`ships`)
  set `gate_note`+`blocked`; clean and bare-`blocks` bodies do not.
- **INV-6** — `bundles` + `section`/`id`/`ids` ⟹ `bad_mode_combo`
  (source-grep on the three guards).
- **INV-7** — a passed `status` is ignored, no refusal (source-grep:
  early-return branch before the status filter switch).
- **INV-8** — warm path reads `m_roadmapCacheBullets` const, no
  `parseBullets` (source-grep).
- **INV-9** — `bundle_label` non-empty + deterministic; lowest-id
  fallback when no token qualifies and no lanes.
- **INV-10** — empty active set ⟹ `bundles:[]`, counts 0,
  `truncated:false`.
- **INV-11** — byte-stable across repeated calls.
- **INV-12** — a low `softCapBytes` truncates by whole bundles
  (`truncated:true`, items intact, `sum < active_total`); the
  `setBundleSoftCapOverride` seam is wired into the branch (source-grep).
- **INV-13** — the label lane-fallback folds case (`claude`/`Claude` →
  one bucket).
- **INV-14** — (ANTS-3388) per-module `<verb> <path>`-template bullets
  whose discriminating tokens are all paths/filenames (denoised away, so
  they share < 2 cluster tokens) still cluster when they share the same
  `Kind`, a lane, and an identical structural stem (leading verb + first
  & last path segment + segment count) — e.g. three `Author
  src/mame_curator/<mod>/spec.md` docs items land in one bundle.
- **INV-15** — (ANTS-3388) the structural assist is conjunctive: a
  differing `Kind`, a disjoint lane set, or a different template shape
  (root/leaf/depth) does NOT merge — so it cannot over-cluster.
- **INV-16** — (ANTS-3388) the envelope always carries
  `no_clusters_found` (bool): `true` ⟺ no bundle in the full pre-cap set
  reached size ≥ 2 (all-singletons, or an empty active set), so a caller
  distinguishes "grouped into singletons" from a real grouping.
- **INV-17** — (ANTS-4088) `bundle_label` tokens are denoised the same way
  the cluster edge's tokens are, plus per-token punctuation stripping.
  `rcNormaliseHeadline` only chops trailing punctuation off the WHOLE
  headline, so per-token punctuation survived into the label, and
  `isNoiseToken` was applied to the cluster tokens but never to the label
  ones. A label token is now stripped of leading/trailing punctuation
  first, then dropped if it is a stop-word, ≤ 2 chars, all-digits, or
  path-like (contains `.` or `/`). So `"most" 57 - duplication:
  bookmarked in foo.cpp` labels as `bookmarked duplication most`, not
  `"most" - 57`. Stripping also merges `"most` and `most` into one
  frequency bucket. The fallback ladder is untouched: a bundle whose
  every token denoises away still falls through majority-token →
  most-common case-folded lane → lowest member id (INV-9 / INV-13).

## Pre-fix check

Against pre-implementation code the `bundles` mode is unknown, so the
verb refuses `bad_mode` and the static builder does not exist (link
error) — every behavioural assertion fails to compile/run. Verified
before wiring the implementation.

Label: `features;fast`.
