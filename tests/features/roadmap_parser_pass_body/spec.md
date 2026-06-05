# ANTS-2030 — pass-heading bullet body carries the under-heading prose

## Background

`roadmap_query include_body:true` on a `#### Pass N.M` heading roadmap
returned `body == headline` because `parsePassHeadingBullets` set
`rec.body = headline`. Callers (RetroDB) therefore fell back to a raw
Read to scope each pass. The detail under the heading — the
`- **Status**:` / `- **Finding**:` / `- **Decision**:` / `- **Items**:`
bullets — was never surfaced.

## Invariants

### INV-1 — body is the under-heading prose, not the headline

`parsePassHeadingBullets` populates `rec.body` with the lines between a
`#### Pass N.M …` heading and the next heading (level ≤ 4), so the
body contains the `**Status**` / `**Finding**` prose and is distinct
from the headline.

### INV-2 — body is bounded by the next heading

The body of one pass stops at the next `#### Pass` heading — a pass's
body never bleeds into the following pass's content.

### INV-3 — leading/trailing blank lines trimmed

A blank line between the heading and the first content line (and
trailing blanks before the next heading) are stripped; interior blanks
are preserved.

### INV-4 — empty section falls back to the headline

A heading with no content beneath it keeps `body == headline` so the
body is never empty (prior contract preserved).

### INV-5 — downstream truncation cap unchanged

The 2 KiB body cap + `body_truncated` flag is applied at emit by
`rcSetBodyFields` (ANTS-1517); `parsePassHeadingBullets` does not
re-implement it. Verified by source-grep that the cap site is untouched.

## Test plan

Behavioural test against `RoadmapDialog::parseBullets` with synthetic
pass-heading fixtures. INV-5 is a source-grep against remotecontrol.cpp
confirming `rcSetBodyFields` remains the single truncation site.
