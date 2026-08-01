# ANTS-3757 — the roadmap migration read half

Test contract for `RoadmapMigrate::findRoadmaps()` and
`RoadmapMigrate::planFrom()` (`src/roadmapmigrate.{h,cpp}`, `ants_core_lib`,
Qt6::Core only). **Both signatures changed at ANTS-3766**, which widened
discovery from one source per project to the live roadmap plus its rotated
archives: `findRoadmap()` became `findRoadmaps()` returning a `Discovery`, and
`planFrom()` takes that `Discovery` rather than a markdown/path pair. This file
therefore also carries ANTS-3766's INV-1..8, INV-10, INV-11 and INV-13.

The design contract is [`docs/specs/ANTS-3757-roadmap-migration-read.md`](../../../docs/specs/ANTS-3757-roadmap-migration-read.md);
its § 2.1 declarations are the single statement of shape and INV-1..13 in its
§ 3 are the invariants this file tests. **This document does not restate them** —
it records the fixtures, the oracle, and the four places building the code
proved a spec clause wrong or under-specified. Those four are also written into
that spec's cold-eyes loop log as an implementation row.

## Fixtures

`fixtures/` holds committed roadmaps, one directory per project, so
`findRoadmaps()` and the survey oracle both see the shape they take in the
corpus. `tools/roadmap-corpus-survey.py` walks a root's immediate
subdirectories, so `fixtures/discovery/*` is deliberately one level deeper and
never enters the survey's view.

| Fixture | Format | What only it carries |
|---|---|---|
| `antsv1/ROADMAP.md` | ants-v1 | the status legend, a status-marked detail line at column 0, a table, a fence inside an item body, `###` nesting, pre-heading content |
| `gfm/ROADMAP.md` | github-task-list | a checkbox item with no id, for which the reader sets a `synthetic` content-hash id |
| `passes/roadmap.md` | pass-headings | the lowercase filename, a content-free `- **Status**:` line, a Status line in no block, a block with no Status line at all |
| `identity/ROADMAP.md` | ants-v1 | the reference-vs-declaration tokens, the markdown link in the leading slot, the folded-id collision |
| `malformed/ROADMAP.md` | ants-v1 | `[ANTS-119&]` — see *Oracle* below for why it is not in the parity set |
| `prose/ROADMAP.md` | ants-v1 | zero items with a table and trailing narration |
| `empty/ROADMAP.md` | ants-v1 | zero bytes |
| `discovery/{upper,lower,both,none,badutf8}/` | — | INV-1's five discovery outcomes |

## Oracle

INV-2 compares the per-fixture item count against `expected-counts.json`,
generated out-of-band by `tools/roadmap-corpus-survey.py` and committed. The
survey is an independently written parser, which is the whole point: a test
written from the spec's own rules can only confirm what the rules say. **No
interpreter runs from `test_core`** — the C++ test reads the committed file.

Regenerate with the command in the file's own `_regenerate` key, and read the
diff. Never regenerate to make the test pass.

`malformed/` is excluded from the parity set, named in the file's `_excluded`
key with its reason: the survey's bold-headline test is applied to the text
*after* a recognised id token, so a bullet carrying both an unrecognised leading
token and a bold headline (`- ✅ [ANTS-119&] **…**`) fails a test
`roadmap-data-model.md` § 7.2 says it should pass. The survey under-counts that
shape; `planFrom()` admits it, as § 2.4 and § 2.3 both require. Filed as a
survey defect rather than absorbed into the expectation file.

## What building it settled

Four rules the spec left open or stated wrongly. Each is an amendment to
`docs/specs/ANTS-3757-roadmap-migration-read.md`, carried in its loop log.

1. **§ 2.4's headline half is `headline` non-empty, not a head-anchored bold
   span.** The oracle admits `- 📋 [CVE-2017-1000117](url) **A headline.**` as an
   item because it found an id token; INV-3 requires the same bullet to plan
   with an empty `id` *and* `idAllocationOwed`, which only makes sense if it is
   an item. A head-anchored test rejects it and breaks parity. The reader
   already sets `headline` from a bold span anywhere in an ants-v1 body, always
   sets one on the GFM path, and always sets one for a pass block — so
   `!headline.isEmpty() || !idToken.isEmpty()` is § 2.4's conjunction with its
   own refinement, in one line, with no second bullet parser.
2. **§ 2.5's id-shaped detector has no consumer: position is the whole rule.**
   Applying the dash-optional grammar as a *gate* on the leading-slot token
   would reject `[ANTS-119&]`, which § 2.3 requires be quarantined rather than
   issued a second identity. So a non-empty leading-slot token is a declared id;
   strict § 3.5.1 → `parsed`, anything else → `quarantined`. The markdown-link
   guard is the reader's `(?![(:])`, already shipped.
3. **The walk is fence-aware and the reader is not, so the walk is authoritative
   for structure.** § 2.11 requires a `##` inside a fence not be read as a
   heading; `RoadmapParse` has no fence mask, so its `sectionSlug` and the
   walk's can differ on exactly that input. Every item's `sectionSlug` therefore
   comes from the walk by line containment, and a reader record whose span
   starts inside a fence is not an item. § 2.11's "equal by construction" holds
   for every document with no heading inside a fence, which is all ten projects.
4. **A section-level fence joins the preceding item's body only when it
   immediately follows that item.** § 2.11 says "the preceding item"; with
   narration in between, extending the item's span across it makes INV-11
   overlap rather than partition. Separated by anything but blank lines, the
   fence is narration.

## Test plan

One `TEST()` per invariant, named `InvNShortName`, in
`test_roadmap_migrate_read.cpp`, on the `test_core` bundle (`features;fast`).
Each was shown RED under the mutation its own *Breaks when* clause names before
being accepted green; the results, including the clauses that did not redden,
are in the spec's loop log.
