# ANTS-5315 — `roadmap_log op:"place_by_release"`: file shipped items under the release they shipped in

**Status:** accepted (2026-10-19).
**Kind:** feature.
**Source:** ROADMAP.md ANTS-5315 step 4 (claude-ab-request-2026-09-24; user decisions 2026-10-19).
**Composes with:** ANTS-1878 § 2.3a (release-section placement), ANTS-4501 (the `backfill_dates` dry-run discipline), ANTS-5704 (`plan_id`).

**Layman:** One command sorts every finished roadmap item under the release it shipped in, shows you the whole result before changing anything, and leaves unfinished work where it is.

## 1. Problem

The roadmap is meant to read as a release plan (`## 0.1.0 — Theme`, then `0.2.0`, …). Steps 1 to 3 of ANTS-5315 built the pieces: `roadmap_query version:` (`RoadmapIndex::findByVersion`), and SemVer placement in `create_section` (ANTS-1878 § 2.3a). No tool yet puts items into release sections, and Ants Terminal's roadmap is not organised that way:

1. **Shipped work sits in topic and triage sections.** Measured 2026-10-19 on the live store: Ants Terminal holds 3,411 items, 2,740 of them shipped (`sqlite3` over `item`, `project_id = 1`). They sit under sections such as `Ants MCP feedback from CC sessions — 2026-08-18 second triage`, which name when work was filed, not when it shipped.
2. **Version-led titles do not mean releases.** Titles such as `0.7.65 — Bundle G indie-review sweep …` are wave containers. They hold open items, and they are not in version order. For shipped items, the version at the start of the title they sit under matches a CHANGELOG release citing them for 103 of 1,033 items (measurement script, § 6.1). `RoadmapIndex::versionOfTitle` also reads a range title such as `0.7.50–0.7.59 — …` as the release `0.7.50`.
3. **Moving 2,740 items by hand is not feasible, and a wrong move is hard to see afterwards.** `amend_field field:"section"` (`RemoteControl::cmdRoadmapLogAmendSection`) moves items in bulk, but something must first decide where each one goes, and show that before it writes.

## 2. Surface

A new op on the existing `roadmap_log` verb, `op:"place_by_release"`. Its body is `RemoteControl::cmdRoadmapLogPlaceByRelease` in a new file, `src/remotecontrol_roadmap_release.cpp`, joining the same library as `src/remotecontrol_roadmap_backfill.cpp`. The target project is resolved from `caller_cwd` the way every store op resolves it (`roadmapSectionOpTarget`). A project that is not store-backed refuses through `rlNotStoreServedRefusal`, as `backfill_dates` does: `project_not_registered` with no project row, `unsupported_format` with one (`docs/standards/mcp-error-codes.md`, the `op_unsupported` row).

### 2.1 Request

| Field | Type | Meaning |
|---|---|---|
| `caller_cwd` | string, required | Picks the project. |
| `dry_run` | bool | `true` plans and reports, writes nothing, and returns a `plan_id` (ANTS-5704). |
| `plan_id` | string | Applies a plan from an earlier dry run of this op. |
| `sample_limit` | int, 1–200, default 50 | Cap on each sample array in the reply. |

**Dry run first is enforced.** A request with neither `dry_run:true` nor `plan_id` refuses `bad_args`, with a hint naming `dry_run:true`. The generic wrapper `RemoteControl::cmdRoadmapLog` re-dispatches a stashed plan's request. It marks that re-dispatch so this op can tell an apply from a direct call. The mark is internal and is not a request field a caller can send.

### 2.2 Where each shipped item belongs

For each item with `status = 'shipped'`, the op computes at most one **placement** — a release version, `X.Y.Z` — in this order. The first source that gives one wins.

1. **The CHANGELOG.** The project's changelog (`ChangelogQuery::parse`, path from the project layout) is read. Every `## [X.Y.Z]` section citing the item's id, by `ChangelogQuery`'s id extraction, is a candidate. The placement is the **lowest** cited version, by SemVer. `## [Unreleased]` and non-`X.Y.Z` headings are never candidates.
2. **The git tags.** Stable release tags are `vX.Y.Z` or `X.Y.Z`. A tag with any suffix (`v1.0.0-rc1`) is ignored. The placement is the lowest stable tag, by SemVer, whose history contains any commit whose **subject begins with the item's id** followed by a non-word character (`ANTS-1234: …`, `ANTS-1234 fix …`). Such commits that no stable tag contains give no placement.
3. **Nothing else.** Section titles, `item.shipped` dates and tag dates are not sources (§ 9 says why).

An item with no placement is **unplaced** and is not moved. Each unplaced item is reported with one reason:

- `unreleased`: its only evidence is `## [Unreleased]`, or a commit no stable tag contains yet.
- `no_evidence`: neither source names it.
- `human_placed`: see § 2.4.

Where both sources give a version and they differ, the CHANGELOG's wins. The pair is counted and sampled in `disagreements` (§ 2.6).

### 2.3 Release sections

A **version-led section** is a level-2 section whose title begins with a numbered version according to `RoadmapIndex::versionOfTitle`. A patch stream (`0.4.x`) is not one. A version is **released** when a stable tag or a `## [X.Y.Z]` CHANGELOG heading names it.

**All of the project's sections are read from the store**, including sections whose `source_path` names an archive file (`docs/roadmap/0.6.md`). `create_section`'s duplicate check reads only the live `ROADMAP.md`, so a version filed in an archive would otherwise get a second section.

For each released version `V` that has at least one placed item, the op picks one **release section**, taking the first rule that applies:

1. **Already established.** A version-led section for `V` that holds an item this op placed there (§ 2.4's marker) is the release section. If several do, the one earliest in section order wins.
2. **Reusable.** A version-led section for `V` qualifies when its title is not a range, and every item left in its subtree after the planned moves has placement `V` or is `human_placed`. An empty section qualifies. If several qualify, the one earliest in section order wins.
3. **New.** Otherwise the op creates a level-2 section in the live roadmap, titled `V — released YYYY-MM-DD`. The date is the stable tag's creator date. With no tag, it is the CHANGELOG heading's date if that date is ISO `YYYY-MM-DD`. With neither, the title is `V — released`. It is placed by ANTS-1878 § 2.3a's rule, in ascending SemVer order. The rule reads the live sections' titles as they will stand after this plan's retitles, plus the sections this plan has already created.

Every placed item not already in its release section's subtree moves there, after the section's last element. Items that are already there stay in place.

**Retitling.** After the moves, every version-led section for a released version that is not a release section is retitled. Its version moves to the end of the title, so a version at the start of a level-2 heading always means a release (user, 2026-10-19). The leading version text is everything before the first ` — `, so a range comes with it. It is removed, and ` (<that text> wave)` is appended to the rest. A title with no ` — ` becomes `Release wave (<title>)`:

| Before | After |
|---|---|
| `0.7.65 — Bundle G indie-review sweep + ANTS-1118 fix-pass (target: 2026-05)` | `Bundle G indie-review sweep + ANTS-1118 fix-pass (target: 2026-05) (0.7.65 wave)` |
| `0.7.50–0.7.59 — indie-review sweep + companion prep — shipped 2026-04-28+ (in flight at 0.7.59)` | `indie-review sweep + companion prep — shipped 2026-04-28+ (in flight at 0.7.59) (0.7.50–0.7.59 wave)` |
| `0.7.7` | `Release wave (0.7.7)` |

The slug follows the title, as `retitle_section` derives it. Sections for versions not yet released (`0.8.0 — multiplexing + marketplace (target: 2026-08)`) are not retitled. Their shipped items move out, and their open items stay.

**Open items never move.** Neither do items with status `considered` or `dropped`. They stay in their named sections (user, 2026-10-19). The op creates no `## Unscheduled` section.

### 2.4 Recording a placement, and leaving a human's alone

The store has no record of who filed an item (`history` has no actor column; `element` rows carry no provenance). So the op writes its own marker. For each item it moves, it writes the `section` history row that `amend_field` writes, plus a second row in the same stamp:

```
history(item_pk, changed_at = <the move's stamp>, seq = <next>,
        field = 'placed_by', old_value = NULL, new_value = 'place_by_release')
```

An item the op would otherwise move is **`human_placed`**, and stays, when either of these holds:

- it has a `placed_by` row and a `section` row after it in (`changed_at`, `seq`) order: someone moved it after this op placed it. The op writes its `placed_by` row after its own `section` row; or
- it sits in a release section picked by § 2.3 rule 1, and has no `placed_by` row: someone filed it there by hand.

An item already in its own release section is never moved, so neither case applies to it.

Both cases rest on the marker. Over `RoadmapStore`'s history cap, `rlFlushHistory` writes no rows and still reports success. So when this op's rows would exceed the cap, the apply refuses `store_failed` and writes nothing.

### 2.5 One plan, one write

The op decides every move, creation and retitle before opening a transaction, as `cmdRoadmapLogBackfillDates` does. The dry run plans exactly what the apply would write. All writes then land in one `RoadmapWrite::commitAndRender`, so the roadmap re-renders once. The apply re-reads the CHANGELOG and git, then re-plans. A change landing between the preview and the apply therefore shows up in the apply's own reply, which is the record of what was written.

Git is read through `HostExec`, as `rlWalkGitForDates` reads it, under the 50 s wall budget `backfill_dates` uses (ANTS-5095: under the client's 60 s timeout). A git start failure, a non-zero exit or a budget overrun refuses `git_failed` (`docs/standards/mcp-error-codes.md`), and nothing is written.

### 2.6 Reply

```json
{
  "ok": true, "op": "place_by_release", "dry_run": true, "plan_id": "…",
  "moved_count": 0,        "moved":   [{"id": "", "from": "", "to": "", "version": "", "source": "changelog|tag"}],
  "already_placed_count": 0,
  "unplaced_count": 0,     "unplaced": [{"id": "", "reason": "unreleased|no_evidence|human_placed", "section": ""}],
  "unplaced_by_reason": {"unreleased": 0, "no_evidence": 0, "human_placed": 0},
  "sections_created": [{"slug": "", "title": "", "after_section": ""}],
  "sections_reused":  [{"slug": "", "version": "", "rule": "established|reusable"}],
  "sections_retitled": [{"from_slug": "", "to_slug": "", "from": "", "to": ""}],
  "sections_emptied": [""],
  "disagreements": {"both_count": 0, "differ_count": 0, "sample": [{"id": "", "changelog": "", "tag": ""}]},
  "multi_cited_count": 0,
  "sources": {"changelog": "CHANGELOG.md", "stable_tags": 0}
}
```

Each array is capped at `sample_limit` and carries a `*_truncated` flag when capped. Counts are never capped. The four `sections_*` arrays are complete, never sampled. Each holds at most one row per release or per existing section. `sections_emptied` names the sections the moves leave with no items. The op does not delete them.

## 3. Invariants

- **INV-1** — A request with neither `dry_run:true` nor `plan_id` refuses `bad_args`, and the store is unchanged. *Test:* `tests/features/roadmap_place_by_release`: the refusal's code is `bad_args` and its hint names `dry_run:true`, and the store's `history` row count and the bytes of `ROADMAP.md` are equal before and after. *Breaks when:* the op applies on a direct call.
- **INV-2** — A dry run writes nothing. When nothing changes between the two calls, the apply of its `plan_id` writes exactly the moves, creations and retitles the dry run reported. *Test:* `tests/features/roadmap_place_by_release`: dry run, compare store and file bytes, apply, then compare the apply's `moved`, `sections_created` and `sections_retitled` with the dry run's. *Breaks when:* planning reads state the apply changes first, or a dry run commits.
- **INV-3** — Placement follows § 2.2's order. With a CHANGELOG citation, the lowest cited `## [X.Y.Z]` wins over any tag. The tag source is used only with no citation. *Test:* `tests/features/roadmap_place_by_release`. The fixture repo has item A cited in `[0.2.0]` and `[0.3.0]` with a subject commit under `v0.1.0`, and item B uncited with a subject commit under `v0.2.0`. A goes to `0.2.0` and B to `0.2.0`; A is counted in `differ_count` and `multi_cited_count`. *Breaks when:* the tag wins, or the highest citation wins.
- **INV-4** — No section title, `item.shipped` date or tag date ever produces a placement. *Test:* `tests/features/roadmap_place_by_release`: an item cited nowhere, not named by any commit subject, with a `shipped` date, and sitting under `## 0.1.0 — …`, is reported `no_evidence` and does not move. *Breaks when:* a fallback source is added.
- **INV-5** — Only shipped items move. Open, `considered` and `dropped` items keep their section and position, even when the CHANGELOG cites them. *Test:* `tests/features/roadmap_place_by_release`: in one apply, a shipped item cited in `[0.2.0]` moves and a planned item cited there keeps its `element` row unchanged. *Breaks when:* status is not checked.
- **INV-6** — An item a human moved after this op placed it, or filed by hand into a release section, is reported `human_placed` and never moved. *Test:* `tests/features/roadmap_place_by_release`: apply once, move item A with `amend_field field:"section"`, then run again. A is `human_placed` and stays. *Breaks when:* the `placed_by` marker is not written or not read.
- **INV-7** — A second run straight after an apply plans no moves, creations or retitles. *Test:* `tests/features/roadmap_place_by_release`: apply, then dry run again. `moved_count` 0, `sections_created` and `sections_retitled` empty. *Breaks when:* a release section the op created fails § 2.3 rule 1 on the re-run, or a retitled title still begins with a version.
- **INV-8** — A version already carried by a section in an archive file gets no second section. *Test:* `tests/features/roadmap_place_by_release`: a store whose `0.1.0` section has `source_path` `docs/roadmap/0.1.md` and qualifies under rule 2 receives `0.1.0`'s items, and `sections_created` has no `0.1.0` row. *Breaks when:* sections are read from `ROADMAP.md` alone.
- **INV-9** — After an apply, no level-2 title begins with a released version unless it is that version's release section. *Test:* `tests/features/roadmap_place_by_release`: the fixture's `0.1.0–0.1.9 — wave` section holding an open item is retitled to `wave (0.1.0–0.1.9 wave)`, and `versionOfTitle` of every remaining level-2 title is empty, a stream, unreleased, or a release section's. *Breaks when:* retitling skips range titles, or reuses a section holding an open item.
- **INV-10** — New release sections stand in SemVer order among the version-led sections, so `0.10.0` follows `0.9.0`. *Test:* `tests/features/roadmap_place_by_release`: the fixture tags `v0.9.0` and `v0.10.0` and has a live `## 1.0.0 — target` section, and a `## 0.10.0 — wave` section holding an open item stands after it. Rendered order of the version-led sections is `0.9.0`, `0.10.0`, `1.0.0`. *Breaks when:* creation order or text order is used, or placement reads a title before its retitle.
- **INV-11** — A pre-release tag never gives a placement, and never forms part of a title. *Test:* `tests/features/roadmap_place_by_release`: a subject commit contained only by `v0.4.0-rc1` is reported `unreleased`. *Breaks when:* the stable-tag filter is missing.
- **INV-12** — The apply re-renders once. It writes one `section` history row and one `placed_by` row per moved item, all with one `changed_at`. *Test:* `tests/features/roadmap_place_by_release`: count `history` rows by field after an apply of N moves → N `section` rows and N `placed_by` rows, all 2N sharing one `changed_at`. *Breaks when:* moves are written through repeated `amend_field` calls, or the marker is skipped.

## 4. Failure modes

| Condition | Result |
|---|---|
| Project not store-backed | `project_not_registered` or `unsupported_format` (§ 2); nothing written. |
| Another writer holds the project | `roadmap_busy` (`RoadmapWriteHold`); nothing written. |
| No CHANGELOG | The CHANGELOG source gives nothing; tags still run. `sources.changelog` is `null`. |
| Not a git repository | The tag source gives nothing. `sources.stable_tags` is 0. |
| Git fails to start, exits non-zero, or exceeds the budget | `git_failed`; nothing written. |
| The history rows would exceed the store's cap | `store_failed`; nothing written (§ 2.4). |
| A plan expired or used | `plan_not_found`, from the wrapper (ANTS-5704). |
| The render gate refuses | The transaction rolls back; the wrapper restores the plan. |

## 5. RAM / build cost

The plan holds one row per shipped item and one per section. It is bounded by the store, and freed when the call returns. The commit subjects git returns are bounded by the repository's history, and read once per call. No new build target or library; the new file joins an existing library.

## 6. Tests

Feature test `tests/features/roadmap_place_by_release/`, which covers INV-1, INV-2, INV-3, INV-4, INV-5, INV-6, INV-7, INV-8, INV-9, INV-10, INV-11 and INV-12, label `features;fast`. It builds a real git repository with tags in a `QTemporaryDir`, as `tests/features/roadmap_backfill_dates` does, and runs under `ants_test::XdgGuard`, so it never touches the live store. Each test runs against pre-change source first and must fail there; the op does not exist, so every request refuses.

### 6.1 Measured figures

A measurement script produced every figure in § 1 and § 7. It read Ants Terminal's `CHANGELOG.md`, the live store read-only, and `git log` per stable-tag range in SemVer order, on 2026-10-19 at `a524fd80`. These figures describe today's data and are not invariants. The op's dry run is the authority from then on.

## 7. Expected first run on Ants Terminal

Measured as § 6.1 says. Figures for the sources the op uses:

| Quantity | Value |
|---|---|
| Shipped items | 2,740 |
| Placed by CHANGELOG | 1,866 (agree with tag 1,108; differ 76; CHANGELOG only 682) |
| Placed by tag only | 216 |
| Unplaced, `unreleased` | 76 (73 by tag, 3 by `[Unreleased]`) |
| Unplaced, `no_evidence` | 582 |
| Distinct releases receiving items | 51 |
| Cited in more than one release | 148 |

The disagreement rate, 76 of 1,184 items where both sources speak (6.4%), is the wrong-placement estimate for items placed by tag alone.

**The run is on Ants Terminal first.** Any other project gets a dry run, sent to claude-ab (mail to `claude-config`) before any write. That is procedure, not code: the op cannot tell who is calling.

## 8. Reload

A `roadmap_log` op needs no tab or terminal state. `roadmap_log` is registered in `mcp::registerProjectScopedVerbs()` (`src/mcptoolregistry.cpp`), so `ants-mcpd` serves it. A rebuilt `ants-mcpd` reaches clients on their next `/mcp` reconnect, with no terminal relaunch. Its code runs in `ants-mcpd`'s process because that process already serves every roadmap write. It holds no running state beyond the wrapper's existing plan stash, so it has no stop/start control of its own.

## 9. Decisions and alternatives

- **CHANGELOG first, tags second, measured** — claude-ab, 2026-09-24. The CHANGELOG is the project's own statement of what shipped where; a tag only says what landed between two tags.
- **Lowest citation wins** — this spec. Of the 107 multi-cited items with a tag placement, the tag agrees with the lowest citation for 75 and with the highest for 19. A later citation is usually a follow-up mention.
- **Tag source reads commit subjects, not bodies** — this spec. Reading any mention places 317 more items, but agrees with the CHANGELOG for 1,592 of 1,853 items (86%), against 94% for subjects (§ 7).
- **Rejected: section titles and dates as sources.** Section versions rarely match the CHANGELOG (§ 1 item 2). A placement by the first tag dated on or after `item.shipped` matches for 248 of 765. 500 of the 582 unplaced items have no `shipped` date at all.
- **Unplaced items stay put** — user, 2026-10-19. The alternatives were guessing from any commit mention, and a `## No release` section; most of those items did ship, so that label would be untrue.
- **Wave titles move their version to the end** — user, 2026-10-19. Rejected: reusing `0.7.65 — Bundle G …` as the 0.7.65 release. Its open items would read as shipped.
- **A `placed_by` history row, not a schema change** — this spec. `kSchemaVersion` (`src/roadmapstore.h`) is a one-way door for every project on the machine. `item.provenance` is per field and does not change when someone moves an item later, so it cannot see a later move. Rejected: one roadmap section per run as a record (it needs a schema change), and treating every item in a release section as human-placed (that would stop the first run from reusing `0.5.0`'s empty archived section).
- **Dry run enforced, not advised** — this spec. ANTS-4501's dry run is optional; this op moves most of a project's items in one call.

## 10. Out of scope

- Re-archiving old minors after the run (release sections for 0.1 to 0.6 land in the live file). Deferred to `rotate_minor`, tracked by ANTS-5778.
- Making `create_section`'s duplicate-version check see archive files. This op does its own check (§ 2.3); the verb's fix is tracked by ANTS-5777.
- Deleting emptied sections. They are reported; removing them is a human call (`delete_section`).
- Placing open items by `item.milestone`. 0 of 3,411 items set it (`sqlite3`, 2026-10-19), and the user ruled that open items stay where they are.
- Running on any other project's store before claude-ab has the dry run. That is a procedural boundary (§ 7), not a code guard.

## 11. Cross-doc impact

- `src/claudeintegration.cpp`: the `roadmap_log` op enum and argument descriptions (140-character cap; `McpEagerLoad`).
- `tests/features/roadmap_log_op_enum/spec.md`: the op list.
- `docs/standards/mcp-behavioural-notes.md`: a `place_by_release` note.
- CHANGELOG `[Unreleased]`: what shipped.
- ANTS-1878 is cited, not changed: the op reuses § 2.3a's ordering rule and does not call `create_section`.

## Cold-eyes loop log

Rows live in [`../reviews/ANTS-5315-release-sections-migration-loop-log.md`](../reviews/ANTS-5315-release-sections-migration-loop-log.md).
