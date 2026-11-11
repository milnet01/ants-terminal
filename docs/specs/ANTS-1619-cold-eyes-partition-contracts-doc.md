# ANTS-1619 — cold_eyes_partition contracts-doc visibility + summary alignment

**Status:** shipped
**Kind:** enhancement
**Source:** cross-session-reports-2026-05-17/18 (RetroDB Issues 1/2/7, RetroDB Observation A)
**Pull:** 22 (MCP cold-eyes partition fold-in)
**Composes with:** [[ANTS-1631]] (probe-set widening — same engine, same pull),
[[ANTS-1634]] (UX polish — same engine, same pull), [[ANTS-1506]] / [[ANTS-1529]]
/ [[ANTS-1571]] (case-insensitive + extension-tolerant + community-contract
helpers this builds on).

## 1. Problem

`cold_eyes_partition(scope="default")` is the entry point of the `/cold-eyes`
workflow. Two RetroDB cross-session reports surfaced three correctness gaps:

1. **Summary names files the lane doesn't carry.** The contracts-lane summary
   echoed `"CLAUDE.md + README.md + ROADMAP.md + CHANGELOG.md (cross-cutting)"`
   while `doc_paths[]` only listed two of them. [[ANTS-1506]]'s "summary tracks
   doc_paths" change closed the obvious gap, but there was no
   caller-inspectable trace of which docs were *probed but missed* — so a
   sparse partition reads as "no docs" instead of "5 of 8 docs missing".

2. **`data/changelog.yaml` invisible to the contracts lane.** [[ANTS-1574]]
   added `data/changelog.yaml` to `project_layout`'s probe set so that
   RetroDB (which keeps a runtime-readable structured changelog there) gets
   its changelog surfaced. `cold_eyes_partition`'s contract loop only probes
   `CHANGELOG.<ext>` at the project root, so the same RetroDB project hits a
   contracts lane missing its changelog.

3. **No debug field for "which code path built this partition".** A caller
   triaging an unexpected partition can't tell at a glance whether the
   override loaded, the override was rejected and fell back, or no override
   exists. The `override_warning` field surfaces the rejection case; nothing
   surfaces the "override absent vs. accepted" distinction without inspecting
   `path` and reverse-engineering its `"<default>"` sentinel.

## 2. Fix

### 2.1 Discovered + missing contract-file echo

`PartitionResult` gains two fields:

```c++
QStringList discoveredContractFiles;
QStringList missingContractFiles;
```

Populated inside `derivePartition`'s contracts-lane builder, mirroring the
exact probe order:

- For each canonical stem (`CLAUDE`, `README`, `ROADMAP`, `CHANGELOG`):
  resolves via `resolveContractStem(projectPath, stem, exts)`. On hit, append
  the resolved relative path to `discoveredContractFiles` *and* to
  `contracts.docPaths`. On miss, append the canonical `<stem>.md` spelling to
  `missingContractFiles`.
- For each community-contract name (`CONTRIBUTING.md`, `SECURITY.md`,
  `LEGAL.md`, `CODE_OF_CONDUCT.md`): same pattern — `discoveredContractFiles`
  on hit, name verbatim into `missingContractFiles` on miss.

`remotecontrol.cpp::cmdColdEyesPartition` emits both arrays unconditionally
(empty arrays are fine; absence-encoding is reserved for fields with
absence-semantics, e.g. `override_warning`).

### 2.2 `data/changelog.*` + `data/roadmap.*` promotion

A second contract-stem table runs after the root probe:

```c++
static const std::vector<std::pair<QString, QStringList>> kDataContracts = {
    { QStringLiteral("data/changelog"),
      { QStringLiteral(".yaml"), QStringLiteral(".yml"),
        QStringLiteral(".json") } },
    { QStringLiteral("data/roadmap"),
      { QStringLiteral(".yaml"), QStringLiteral(".yml"),
        QStringLiteral(".json") } },
};
```

Hits append to `discoveredContractFiles` + `contracts.docPaths`, with a
`contracts.docPaths.contains(resolved)` dedup so a project that ships
both `CHANGELOG.md` and `data/changelog.yaml` doesn't double-list. Misses
do not populate `missingContractFiles` — the data-path probe is a
project-style "if you have one" surface, not a required contract.

### 2.3 `partition_source` debug echo

`PartitionResult` gains a `QString partitionSource` field. Its value:

- `"override"` — set inside the override-Ok early-return branch of
  `derivePartition`.
- `"default"` — the type's default; left unchanged in every other path
  (absent override, malformed override, AllLanesFiltered override).

`remotecontrol.cpp::cmdColdEyesPartition` always emits the field on the
envelope (`env["partition_source"] = m_coldEyesCache.partitionSource`).
Combined with the existing `path` field (`"<default>"` vs.
`".cold-eyes/partition.json"`) and `override_warning` (set iff the override
was found but rejected), callers can disambiguate the four states
(`absent`, `loaded`, `parse_error`, `version_mismatch`) without
heuristics.

## 3. Invariants

- **INV-1** (discovered echo). For every contract stem that resolves to a
  file on disk, the resolved relative path appears in both
  `discoveredContractFiles` and `contracts.docPaths`.
- **INV-2** (missing echo). For every canonical contract stem that fails to
  resolve, the canonical `.md` spelling (`CLAUDE.md`, `README.md`,
  `ROADMAP.md`, `CHANGELOG.md`) or the literal community-doc name appears
  in `missingContractFiles`.
- **INV-3** (data-path promotion). When `data/changelog.<yaml|yml|json>`
  resolves, the resolved relative path appears in `contracts.docPaths`
  even when the root `CHANGELOG.md` also resolved (no double-listing — the
  helper dedups via `contracts.docPaths.contains`).
- **INV-4** (partition_source default). When no override file is present,
  `result.partitionSource == "default"`.
- **INV-5** (partition_source override). When `.cold-eyes/partition.json`
  parses cleanly (override.status == Ok), `result.partitionSource ==
  "override"`.
- **INV-6** (partition_source malformed fall-back). When the override file
  exists but fails to parse / has wrong version / has every lane filtered
  out, `result.partitionSource == "default"` AND `overrideWarning` is
  non-empty.
- **INV-7** (envelope emission). The MCP envelope from
  `cmdColdEyesPartition` carries the keys `partition_source`,
  `discovered_contract_files`, and `missing_contract_files` on every
  success response (unconditionally, even when arrays are empty — the
  caller can `len(arr)` without an exists-check).

## 4. Test

Extended `tests/features/cold_eyes_engine/test_cold_eyes_engine.cpp`:

- `Ants1619DiscoveredAndMissingContractsTracked` (INV-1, INV-2) — workspace
  with `CLAUDE.md` + `README.md`, no `ROADMAP.md`/`CHANGELOG.md`. Asserts
  `discoveredContractFiles` carries the two existing files and
  `missingContractFiles` carries `"ROADMAP.md"` + `"CHANGELOG.md"`.
- `Ants1619CommunityContractMissesTracked` (INV-2) — workspace with no
  community docs at all. Asserts `missingContractFiles` carries
  `"CONTRIBUTING.md"`, `"SECURITY.md"`, `"LEGAL.md"`, `"CODE_OF_CONDUCT.md"`.
- `Ants1619DataChangelogPromoted` (INV-3) — workspace with `CHANGELOG.md`
  AND `data/changelog.yaml`. Asserts `contracts.docPaths` carries both,
  and the resolved relative path appears only once (no double-listing).
- `Ants1619DataChangelogStandaloneWhenRootAbsent` (INV-3) — workspace with
  only `data/changelog.yaml`. Asserts `contracts.docPaths` carries that
  file even though no root `CHANGELOG.md` exists.
- `Ants1619PartitionSourceDefault` (INV-4) — bare workspace.
  `r.partitionSource == "default"`.
- `Ants1619PartitionSourceOverride` (INV-5) — workspace with a valid
  `.cold-eyes/partition.json`. `r.partitionSource == "override"`.
- `Ants1619PartitionSourceMalformedFallsBack` (INV-6) — workspace with a
  garbage-JSON override. `r.partitionSource == "default"` AND
  `r.overrideWarning` is non-empty.

## 5. Out of scope

- **Probing arbitrary data-path stems beyond `changelog` / `roadmap`.**
  RetroDB and the runtime-readable shape are the load-bearing case; other
  shapes can be added on demand without re-spec.
- **Re-emit `summary` as a structured field listing missing docs inline.**
  The summary stays a human-readable echo (already trimmed to
  `doc_paths` via [[ANTS-1506]]); the new `missing_contract_files[]` array
  is the machine-readable surface for "what was probed and didn't resolve".

## 6. RAM / build budget

- Engine: two new `QStringList` members on `PartitionResult` + one
  `QString` (`partitionSource`). Each `PartitionResult` instance holds
  ≤ 8 entries (4 canonical + 4 community), bounded by the probe table.
  Negligible vs. the existing `lanes` list.
- Cache: `m_coldEyesCache` already stores `PartitionResult` by value
  (`remotecontrol.h:m_coldEyesCache`). New fields ride along; no extra
  allocation pattern.
- Engine cost: same probe loop (no extra `stat` calls — the existing
  `caseInsensitiveResolve` short-circuits the canonical case). The
  `data/changelog.*` probe adds at most one `caseInsensitiveResolve`
  call per ext (3 calls per stem, 2 stems = 6 extra fs probes on the
  miss case; 1 probe on the common single-hit case).
- MCP envelope: 3 new top-level fields (`partition_source` ≤ 8 bytes,
  two arrays bounded by the contract-table size). + ~120 bytes worst
  case.

## 7. Sources

- RetroDB cross-session report 2026-05-17 — Issues 1, 2, 7.
- RetroDB cross-session report 2026-05-18 — Observation A.
- [[ANTS-1506]] — case-insensitive contract resolver.
- [[ANTS-1529]] — extension-tolerant contract resolver.
- [[ANTS-1571]] — community-contract docs + standards name-glob fallback.
- [[ANTS-1574]] — project_layout `data/changelog.yaml` probe (mirrored here).
