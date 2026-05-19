# cold_eyes_engine — feature-conformance spec

**Owner:** ANTS-1319 (`docs/specs/ANTS-1319.md`)
**Engine:** `src/coldeyesengine.{h,cpp}`

Exercises `ColdEyesEngine::derivePartition` /
`assembleBriefManifest` / `extractCitedCodePaths` /
`crossDocDiffFromDir` / `templateColdEyesFoldInBlock`.
Standalone GoogleTest binary linking only the engine's compile units
through `ants_core_lib` (no `Qt6::Widgets` event loop required).

## Cases (ENG-1..ENG-10)

| # | Case | Asserts |
|---|---|---|
| ENG-1 | `derivePartition(Default)` builds canonical lane set | `contracts` / `standards` / `decisions` lanes present when their docs exist |
| ENG-2 | Spec lanes emitted only for 📋/🚧 ROADMAP entries | shipped (✅) IDs absent from active set |
| ENG-3 | Spec-lane cap honoured (INV-2) | 15 fake active specs → 12 returned + `truncated=true` |
| ENG-4 | `assembleBriefManifest` is paths-only (INV-3) | `brief` contains "Read each doc"; no doc-body bytes inlined |
| ENG-5 | `crossReferenceDocs` is contract trio + CHANGELOG (INV-4) | exact set, lane's own docs de-duped |
| ENG-6 | `extractCitedCodePaths` resolves `src/foo.{h,cpp}` mentions | hits real paths; rejects non-existent paths |
| ENG-7 | `crossDocDiffFromDir` delegates to indie-review engine | empty dir → empty findings |
| ENG-8 | `templateColdEyesFoldInBlock` heading matches INV-7 | regex `^### 📝 Cold-eyes \d{4}-\d{2}-\d{2}$` |
| ENG-9 | Path-rule defence on cited-code paths (INV-13) | `src/../etc/passwd` mention rejected |
| ENG-10 | `parseScope` covers all enum values + bad-input | "default" / "docs_only" / "contracts_only" / "weird" |

### Cold-eyes-engine fold-in pull 13 (2026-05-18)

Eight additional cases shipped with pull 13 across the
ANTS-1411 / 1412 / 1413 / 1440 fixes:

| # | Case | Asserts |
|---|---|---|
| ANTS-1411 INV-A | `SpecLanesIncludeNonAntsFilenames` | DS01/FP05/P04.md files surface as `spec/<basename>` lanes even without a ROADMAP |
| ANTS-1411 INV-B | `AntsShapedSpecsStillGatedByActiveRoadmap` | shipped ✅ ANTS-NNNN.md stays out; non-ANTS shapes always in |
| ANTS-1412 INV-A | `MalformedPartitionOverrideSurfacesWarning` | parse error → `overrideWarning` non-empty + fallback to default |
| ANTS-1412 INV-B | `PartitionOverrideWrongVersionSurfacesSchemaHint` | wrong-version warning embeds the expected schema example |
| ANTS-1412 INV-C | `ValidPartitionOverrideHasNoWarning` | clean override → empty warning, overridePath=.cold-eyes/partition.json |
| ANTS-1440 INV-A | `SpecLaneSummaryIsParsedH1` | spec lane summary is the parsed `# ` H1, not the generic placeholder |
| ANTS-1440 INV-B | `SpecLanePairsWithExpandsCrossRefs` | header preamble `ANTS-NNNN` tokens append to crossReferenceDocs |
| ANTS-1413 INV-A | `SingleDocBriefAssemblesNeighbourhood` | sameDirSiblings + standards + rootContracts + reviewers populate |

ANTS-1414's `cross_doc_diff` MCP alias is wired through
`RemoteControl::cmdCrossDocDiff`; behaviour is covered by the
existing `cold_eyes_cross_doc_diff` regression set (same engine
helper) plus `mcp_tool_prefix_tags.Inv3EveryRegisteredToolHasBucket`
(ensures the new tool is bucketed under `cold-eyes`).

### Cold-eyes-engine fold-in pull 22 (2026-05-19)

Eighteen additional cases shipped with pull 22 across the
ANTS-1619 / 1631 / 1634 cold-eyes partition bundle:

| # | Case | Asserts |
|---|---|---|
| ANTS-1619 INV-1+2 | `Ants1619DiscoveredAndMissingContractsTracked` | discovered/missing arrays mirror probe outcome |
| ANTS-1619 INV-2 | `Ants1619CommunityContractMissesTracked` | CONTRIBUTING/SECURITY/LEGAL/CODE_OF_CONDUCT misses surface |
| ANTS-1619 INV-3 | `Ants1619DataChangelogPromoted` | `data/changelog.yaml` lands in contracts.docPaths without double-list |
| ANTS-1619 INV-3 | `Ants1619DataChangelogStandaloneWhenRootAbsent` | data-path resolves alone when root CHANGELOG.md absent |
| ANTS-1619 INV-4 | `Ants1619PartitionSourceDefault` | no override → `partitionSource == "default"` |
| ANTS-1619 INV-5 | `Ants1619PartitionSourceOverride` | valid override → `partitionSource == "override"` |
| ANTS-1619 INV-6 | `Ants1619PartitionSourceMalformedFallsBack` | malformed override → `"default"` + non-empty warning |
| ANTS-1631 INV-2 | `Ants1631PrivateInternalForkSpecsSurface` | specs under private/internal/fork roots surface as lanes |
| ANTS-1631 INV-3 | `Ants1631DedupAcrossSpecRoots` | same basename in two roots → single lane (no duplicate) |
| ANTS-1631 INV-4 | `Ants1631DocsEngineSpecMdSurfaces` | `docs/engine/<sub>/spec.md` surfaces (App-Build engine layout) |
| ANTS-1631 INV-5 | `Ants1631SrcModuleSpecMdSurfaces` | `src/<module>/spec.md` surfaces (per-module spec layout) |
| ANTS-1631 INV-6 | `Ants1631DocsPhasesSurfaces` | `docs/phases/*.md` surfaces (phased-design doc layout) |
| ANTS-1631 INV-7 | `Ants1631DatePrefixedSpecsSurface` | `2026-MM-DD-*.md` names accepted by the walker |
| ANTS-1631 INV-8 | `Ants1631DeepNestedNotSurfaced` | level-3+ files not walked (vendor-tree guard) |
| ANTS-1634 INV-3 | `Ants1634AuditInfraLaneFires` | `docs/audit/` with ≥ 2 .md files → `audit-infra` lane |
| ANTS-1634 INV-4 | `Ants1634AuditInfraLaneRespectsThreshold` | 1 .md → no lane (sub-threshold guard) |
| ANTS-1634 INV-5 | `Ants1634AuditInfraPrivatePath` | `docs/private/audit/` fallback |
| ANTS-1634 INV-6 | `Ants1634AuditInfraOneLevelDeep` | subdir .md files don't count toward threshold |

## Build wiring

`tests/features/cold_eyes_engine/test_cold_eyes_engine.cpp` is added to
the `test_audit` bundle in `CMakeLists.txt` next to
`tests/features/roadmap_index_engine/test_roadmap_index_engine.cpp`.

## Test workspace

Each case sets up a `QTemporaryDir` workspace with the minimal file set
the case needs (a stub `ROADMAP.md`, stub `docs/`, etc.) — the engine
operates on a project root path, never on the live repo.
