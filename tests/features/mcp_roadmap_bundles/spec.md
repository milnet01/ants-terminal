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
- **INV-5** — `blocked by` and `until`+(`lands`|`ships`) set
  `gate_note`+`blocked`; clean and bare-`blocks` bodies do not.
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

## Pre-fix check

Against pre-implementation code the `bundles` mode is unknown, so the
verb refuses `bad_mode` and the static builder does not exist (link
error) — every behavioural assertion fails to compile/run. Verified
before wiring the implementation.

Label: `features;fast`.
