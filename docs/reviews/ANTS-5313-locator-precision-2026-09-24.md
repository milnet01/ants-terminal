# ANTS-5313 — `doc_symbols` locator precision, measured

Genre: record

Date: 2026-09-24. Asked for by claude-ab (v2 orchestrator), as the
condition on `~/.claude/docs/reviews/v2-mechanical-checks-proposal-2026-09-24.md`
§ The one thing worth building.

## Headline

**Superseded in part by § Re-measure after the resolver fix, below.** The
first measurement, kept as it was taken:

**The rate is bad for bare names. Do not hand the locator to a lane as-is.**

- **Overall:** 14 of 60 sampled locators point at the wrong thing — 23%
  (95% Wilson interval 14–35%).
- **Function-shaped or qualified names** (`foo()`, `A::b`): 0 of 14 wrong
  (interval 0–22%). The sample is small; read it as "no failure seen".
- **Bare names** (`scope`, `projectName`): 14 of 46 wrong — 30% (interval
  19–45%).

**Every wrong entry is the same defect.** The resolver accepts any
declaration of the name, so a common word resolves to whatever local
variable, parameter or forward declaration happens to carry it. Eleven of
the fourteen are a function-local variable, local struct or parameter; one
is an unrelated struct member; two are `class QFile;`-style forward
declarations of a Qt class. Seven of the fourteen cite a file under
`tests/`, and no correct entry in the sample does.

## Method

- **Corpus:** `docs/specs/*.md` and `docs/standards/*.md` in this repository
  — the same 292 documents as the proposal, read in place. The proposal's
  added header lines do not matter here: `doc_symbols` reads code spans, not
  headers.
- **Engine:** the shipped `DocSymbols::scan` + `DocSymbols::locate`, linked
  from `build/*.a` by `tools/measure/doc_symbols_locator_harness.cpp`, one
  document per call. That is what `doc_symbols {path:<doc>, mode:"locator"}`
  does. The exclusion list is built the verb's way: refusal codes from
  `docs/standards/mcp-error-codes.md`, plus the registered tool names.
- **Sample:** `tools/measure/doc_symbols_locator_sample.py 60` —
  `random.seed(20260924)` over the sorted population of
  `(document, symbol, locator)` entries, n = 60.
- **Classification:** each entry was judged by reading the document line
  beside the cited code line, and more context where one line did not
  settle it. **Wrong** means the cited line is not the entity the document
  means. A right file with the wrong entity counts as wrong.

## Population

| Bucket | Entries (per-document distinct symbols, summed) |
|---|---|
| `located` | 4,431 |
| `ambiguous` | 3,789 |
| `unresolved` | 2,550 |
| `not_checked` | 0 (no document hit the budget or the deadline) |

- **A mechanical check over all 4,431 locators:** every cited line contains
  the identifier. So no locator is garbage. The failures are all "a real
  declaration of that name, but not the one meant", which only reading can
  catch.
- **`ambiguous` is nearly as large as `located`.** A lane handed the locator
  form gets a count, not a place, for close to half the names that resolve.
- **Shape of the located population:** 1,307 function-shaped or qualified,
  3,124 bare. The bare class is the larger one, so the 30% dominates.
- **Where the locators point:** 390 cite a file under `tests/`; 79 cite a
  `class X;` / `struct X;` forward declaration.

## What would fix it (the ruling took candidate 2)

These are candidates, not results. Each came from reading the same 60
entries, so judging a fix against this sample would be fitting it. A fix
must be re-measured on a new seed.

1. **Locate only function-shaped and qualified names.** Bare names go to a
   separate bucket rather than to a guess. Cheapest, and the one class with
   no failure seen. It drops the correct bare locators too (`m_statusTimer`,
   `kSpecStopwords`), which were 32 of 46 bare entries in the sample.
2. **Stop the resolver counting a forward declaration or a function-local
   variable or parameter as a candidate.** The root-cause fix: it would
   improve `find_definition` as well. It is also the larger change.
3. **Rank a `tests/` candidate below a non-test one.** Seven of the fourteen
   failures, and no success, sit in `tests/`. A heuristic; it would not
   catch the five wrong entries in `src/`.

## The classified sample

| # | Symbol | Document | Cited | Verdict |
|---|---|---|---|---|
| 1 | `kSpecStopwords` | `docs/specs/ANTS-1113.md:358` | `src/featurecoverage.cpp:31` | right |
| 2 | `totalCount()` | `docs/specs/ANTS-1407.md:57` | `src/claudetasklist.h:78` | right |
| 3 | `roadmapKindFilters` | `docs/specs/ANTS-1238.md:728` | `src/config.cpp:365` | right |
| 4 | `MigrationPlan` | `docs/specs/ANTS-3796-section-record-completeness.md:270` | `src/roadmapmigrate.h:197` | right |
| 5 | `startPcallBudget()` | `docs/specs/ANTS-2093.md:57` | `src/luaengine.cpp:296` | right |
| 6 | `cmd_promote` | `docs/specs/ANTS-3661.md:202` | `packaging/cut-rc.sh:715` | right |
| 7 | `internal` | `docs/specs/ANTS-3810-round-trip-oracle-and-acyclicity.md:615` | `tests/features/roadmap_read_seam/test_roadmap_read_seam.cpp:709` | **wrong** — test-local variable; doc means an item class |
| 8 | `m_statusTimer` | `docs/standards/status-bar.md:6` | `src/mainwindow.h:346` | right |
| 9 | `hasStub` | `docs/specs/ANTS-3504.md:119` | `src/feedbackfile.cpp:1058` | right |
| 10 | `shellCwd()` | `docs/specs/ANTS-3572.md:103` | `src/terminalwidget.cpp:5263` | right |
| 11 | `PlannedItem` | `docs/specs/ANTS-3810-round-trip-oracle-and-acyclicity.md:258` | `src/roadmapmigrate.h:44` | right |
| 12 | `scope` | `docs/standards/test-audit-resume.md:74` | `tests/features/test_audit_polyglot_and_strip/test_polyglot_and_strip.cpp:30` | **wrong** — test parameter; doc means a verb argument |
| 13 | `score()` | `docs/specs/ANTS-1896.md:66` | `src/modelrecommender.cpp:153` | right |
| 14 | `cmdGetText` | `docs/specs/ANTS-1244.md:59` | `src/remotecontrol_terminal.cpp:122` | right |
| 15 | `required` | `docs/specs/ANTS-1985.md:107` | `tests/features/ci_workflow_deps/test_ci_workflow_deps.cpp:77` | **wrong** — test-local; doc means a JSON-schema key |
| 16 | `QFileSystemWatcher` | `docs/specs/ANTS-1873.md:21` | `src/pluginmanager.h:14` | **wrong** — forward declaration of a Qt class |
| 17 | `kReadToolMaxBytesCeiling` | `docs/specs/ANTS-2021.md:276` | `src/remotecontrol.h:296` | right |
| 18 | `storeIfChanged` | `docs/specs/ANTS-1150.md:131` | `src/config.cpp:254` | right |
| 19 | `RemoteControl::cmdFocusedTest` | `docs/specs/ANTS-1302.md:165` | `src/remotecontrol_state.cpp:3556` | right |
| 20 | `resolution` | `docs/specs/ANTS-3661.md:303` | `src/docsymbols.h:54` | right (borderline: the struct field behind the wire key the doc names) |
| 21 | `layman` | `docs/specs/ANTS-4065-import-mapping-contract.md:62` | `tests/features/roadmap_rotate_minor/test_roadmap_rotate_minor.cpp:108` | **wrong** — test parameter; doc means an item field |
| 22 | `formatVersion` | `docs/specs/ANTS-3446.md:844` | `src/feedbackfile.h:94` | right |
| 23 | `PlanOptions` | `docs/specs/ANTS-1290.md:390` | `src/plantemplateengine.h:27` | right |
| 24 | `StatsConfig::windowDays` | `docs/specs/ANTS-1941.md:15` | `src/modelswitchledger.h:215` | right |
| 25 | `projectName` | `docs/specs/ANTS-3855-roadmap-migrate-verb.md:106` | `src/claudeprojects.cpp:408` | **wrong** — dialog local; doc means a roadmapmigrate.h field |
| 26 | `Write` | `docs/specs/ANTS-1890.md:136` | `src/remotecontrol_roadmap_backfill.cpp:305` | **wrong** — local struct; doc means the Write tool |
| 27 | `commandStartMs` | `docs/specs/ANTS-5078-export-streaming.md:145` | `src/terminalgrid.h:111` | right |
| 28 | `rcStatusEmoji` | `docs/specs/ANTS-4977-dropped-status.md:50` | `src/remotecontrol.cpp:1038` | right |
| 29 | `QFile` | `docs/specs/ANTS-3855-roadmap-migrate-verb.md:825` | `src/roadmapsource.h:29` | **wrong** — forward declaration of a Qt class |
| 30 | `cmdChangelogLogAddBatch` | `docs/specs/ANTS-3833-remotecontrol-decomposition.md:104` | `src/remotecontrol_changelog.cpp:1325` | right |
| 31 | `rxBold` | `docs/specs/ANTS-3808-item-body-and-trailer-suppression.md:124` | `src/roadmapparse.cpp:1153` | right |
| 32 | `RoadmapSource::bulletsFor()` | `docs/specs/ANTS-3815-store-source-format-column.md:357` | `src/roadmapsource.cpp:585` | right |
| 33 | `package` | `docs/specs/ANTS-2093.md:47` | `src/debtsweepengine.h:210` | **wrong** — struct member; doc means the Lua `package` library |
| 34 | `parseShippedDates` | `docs/specs/ANTS-1154.md:62` | `src/roadmapdialog.cpp:1810` | right |
| 35 | `behind` | `docs/specs/ANTS-1569.md:177` | `src/remotecontrol_state.cpp:1908` | **wrong** — session_orient local; doc means a git_state reply field |
| 36 | `internal` | `docs/specs/ANTS-3758-roadmap-render.md:580` | `tests/features/roadmap_read_seam/test_roadmap_read_seam.cpp:709` | **wrong** — test-local variable; doc means an item class |
| 37 | `m_inTransaction` | `docs/specs/ANTS-3781-roadmap-store-schema-upgrade.md:528` | `src/roadmapstore.h:1051` | right |
| 38 | `kExclusions()` | `docs/specs/ANTS-1351.md:67` | `src/auditrunner.cpp:167` | right |
| 39 | `startExport` | `docs/specs/ANTS-5078-export-streaming.md:116` | `src/terminalwidget.cpp:5682` | right |
| 40 | `transport` | `docs/specs/ANTS-2079.md:353` | `tests/features/mcp_verify_changes_timeout_headroom/test_mcp_verify_changes_timeout_headroom.cpp:113` | **wrong** — test-local; doc means a literal string |
| 41 | `testGlobs` | `docs/specs/ANTS-1624.md:80` | `src/debtsweepengine.cpp:833` | **wrong** — debtsweepengine.cpp local; doc means a testauditengine.cpp table field |
| 42 | `defaultSocketPath` | `docs/specs/ANTS-3833-remotecontrol-decomposition.md:140` | `src/remotecontrol.cpp:2388` | right |
| 43 | `detail` | `docs/standards/mcp-behavioural-notes.md:203` | `tests/_support/expect.h:81` | **wrong** — test-helper parameter; doc means the tool_info field |
| 44 | `detectRoadmapFormat()` | `docs/specs/ANTS-3793-roadmap-consumer-cutover.md:511` | `src/roadmapparse.cpp:1660` | right |
| 45 | `TerminalGrid::maxScrollback` | `docs/specs/ANTS-5219-scrollback-line-cap.md:17` | `src/terminalgrid.h:215` | right |
| 46 | `modelId` | `docs/specs/ANTS-1895.md:279` | `src/modelswitchledger.h:152` | right |
| 47 | `runProbes` | `docs/specs/ANTS-1145.md:144` | `src/diffviewer.cpp:367` | right |
| 48 | `ResolvedRoot` | `docs/specs/ANTS-4932-standalone-mcp-server.md:290` | `src/resolvedroot.h:26` | right |
| 49 | `weightForTurnIndex` | `docs/specs/ANTS-1890.md:394` | `src/modelrecommender.cpp:133` | right |
| 50 | `cmdGetText` | `docs/specs/ANTS-2132-async-mcp-dispatch.md:107` | `src/remotecontrol_terminal.cpp:122` | right |
| 51 | `setSectionIntro()` | `docs/specs/ANTS-3782-roadmap-section-provenance.md:152` | `src/roadmapstore.cpp:1322` | right |
| 52 | `RemoteControl::filterControlChars` | `docs/specs/ANTS-1335.md:14` | `src/remotecontrol.h:151` | right |
| 53 | `m_roadmapCacheBullets` | `docs/specs/ANTS-1357.md:215` | `src/remotecontrol.h:1481` | right |
| 54 | `isRenderable()` | `docs/specs/ANTS-3810-round-trip-oracle-and-acyclicity.md:176` | `src/roadmaprender.cpp:58` | right |
| 55 | `computeToken` | `docs/specs/ANTS-1397.md:470` | `src/testauditengine.cpp:518` | right |
| 56 | `clearStatusMessage` | `docs/specs/ANTS-1146.md:215` | `src/mainwindow.cpp:6454` | right |
| 57 | `MainWindow::setupClaudeMcpProviders` | `docs/specs/ANTS-1113.md:297` | `src/mainwindow.cpp:4369` | right |
| 58 | `release_notes` | `docs/specs/ANTS-2164.md:180` | `packaging/cut-rc.sh:182` | right |
| 59 | `Settings` | `docs/specs/ANTS-3771-id-format-declaration.md:690` | `src/projectsettings.h:22` | right |
| 60 | `foldIn` | `docs/specs/ANTS-1727.md:269` | `src/testauditengine.cpp:1987` | right |

## Re-measure after the resolver fix

**Ruling (claude-ab):** candidate 2 alone — fix the resolver, do not route
around it. Re-measured on a NEW seed, fixed before any output was read.

**What changed.** The resolver tracks, per C++/GLSL line, whether it starts
inside a function body or a parameter list, and tags a match there
`local` instead of `definition` / `declaration`. A function-local lambda
(`auto NAME = [`) keeps its kind, because documents cite those by name.
The locator treats `local` rows and `class X;`-shaped forward
declarations as no candidate, and reports a symbol that has only those in a
new `declared_only` bucket, rather than calling it `unresolved`.

**Method:** as above; seed **5313**, n = 60.

| | Before (seed 20260924) | After (seed 5313) |
|---|---|---|
| All | 14/60 = 23% (14–35%) | 8/60 = 13% (7–24%); 7/60 lenient |
| Function-shaped or qualified | 0/14 (0–22%) | 1/23 (1–21%) |
| Bare | 14/46 = 30% (19–45%) | 7/37 = 19% (9–34%); 6/37 lenient |

"Lenient" counts entry 8 as right: it names the struct member a local is
copied from, where the document means the local.

**Population, before → after:** located 4,431 → 4,102; ambiguous
3,789 → 3,179; unresolved 2,550 → 2,550; declared_only — → 939. Locators
citing a file under `tests/`: 390 → 195.

**What is left, by class.**

- **Test-file helper functions** (5 of 8): `compact`, `status` (twice),
  `counts`, `stage`. Each is a real namespace-scope function in a test,
  so the resolver is right that it is a definition. The document means a
  field, an argument or a plain word. This is the bare-name class proper:
  no fix to scope tracking reaches it.
- **A qualifier not honoured** (1): `QFileSystemWatcher::fileChanged`
  located at `ClaudeIntegration::fileChanged`. The first qualified-name
  failure seen in either sample.
- **An unrelated member of the same name** (1, plus borderline entry 8).

**Measurement caveat.** The walk reads the working tree, and an untracked
prototype (`tools/filemap.py`) sat in it during this run. One population
entry (`shape`) resolved into it, and it is not in the sample.

### The classified sample (seed 5313)

| # | Symbol | Document | Cited | Verdict |
|---|---|---|---|---|
| 1 | `isGrammaticalId()` | `docs/specs/ANTS-3793-roadmap-consumer-cutover.md:273` | `src/roadmapmigrate.cpp:207` | right |
| 2 | `ReviewDialogBase::beginRound` | `docs/specs/ANTS-5010-plaintext-prompt-warning.md:64` | `src/reviewdialogbase.cpp:290` | right |
| 3 | `scanDoc` | `docs/specs/ANTS-3786-docsindex-header-field.md:48` | `src/docsindex.cpp:74` | right |
| 4 | `cmdSessionMemory` | `docs/specs/ANTS-1372.md:165` | `src/remotecontrol_coldeyes.cpp:850` | right |
| 5 | `mappedIds` | `docs/specs/ANTS-3442.md:108` | `src/feedbackfile.h:88` | right |
| 6 | `parsePassHeadingBullets` | `docs/specs/ANTS-1583.md:185` | `src/roadmapparse.cpp:787` | right |
| 7 | `mcp::setSpillDirOverride` | `docs/specs/ANTS-1922.md:124` | `src/mcpspill.cpp:690` | right |
| 8 | `contract` | `docs/specs/ANTS-1415.md:125` | `src/claudeintegration.h:756` | borderline: the `RegisteredTool::contract` member, where the doc means a local copied from it |
| 9 | `Config::aiEndpoint()` | `docs/standards/mcp-error-codes.md:130` | `src/config.cpp:1019` | right |
| 10 | `offloadBody` | `docs/specs/ANTS-3578.md:19` | `src/mcpspill.cpp:236` | right |
| 11 | `RemoteControl::m_main` | `docs/specs/ANTS-3572.md:595` | `src/remotecontrol.h:1585` | right |
| 12 | `RoadmapStore::canonicalJson()` | `docs/specs/ANTS-3810-round-trip-oracle-and-acyclicity.md:185` | `src/roadmapstore.cpp:138` | right |
| 13 | `populateChecks()` | `docs/specs/ANTS-1111.md:479` | `src/auditdialog_catalogue.cpp:25` | right |
| 14 | `detectProjectFrameworks` | `docs/specs/ANTS-1111.md:395` | `src/audithygiene.cpp:140` | right |
| 15 | `kBulkBusyTimeoutMs` | `docs/specs/ANTS-3781-roadmap-store-schema-upgrade.md:470` | `src/roadmapstore.h:62` | right |
| 16 | `startPcallBudget` | `docs/specs/ANTS-2093.md:664` | `src/luaengine.cpp:296` | right |
| 17 | `createdSchema()` | `docs/specs/ANTS-3815-store-source-format-column.md:538` | `src/roadmapstore.h:169` | right |
| 18 | `ResolvedRoot` | `docs/specs/ANTS-1401.md:1` | `src/resolvedroot.h:26` | right |
| 19 | `acquire` | `docs/specs/ANTS-5144-shared-socket-listener.md:131` | `src/localsockethub.cpp:49` | right |
| 20 | `RemoteControl::dispatch` | `docs/specs/ANTS-2132-async-mcp-dispatch.md:84` | `src/remotecontrol.cpp:2658` | right |
| 21 | `totalSaved` | `docs/specs/ANTS-3579.md:136` | `src/tokenusageengine.cpp:173` | right |
| 22 | `discoveredContractFiles` | `docs/specs/ANTS-1619.md:54` | `src/coldeyesengine.h:108` | right |
| 23 | `changedFilesCount` | `docs/specs/ANTS-1504.md:234` | `src/auditrunner.h:167` | right |
| 24 | `surfacesResolved` | `docs/specs/ANTS-4127-test-surface-resolution.md:395` | `src/speclint.h:139` | right |
| 25 | `sentenceStop()` | `docs/specs/ANTS-3808-item-body-and-trailer-suppression.md:374` | `src/roadmapparse.cpp:657` | right |
| 26 | `checkCallerCwd` | `docs/specs/ANTS-1630.md:215` | `src/remotecontrolgate.cpp:11` | right |
| 27 | `shellCwd` | `docs/specs/ANTS-1435.md:381` | `src/terminalwidget.cpp:5263` | right |
| 28 | `RemoteControl::cmdColdEyesBrief` | `docs/specs/ANTS-1634.md:110` | `src/remotecontrol_coldeyes.cpp:123` | right |
| 29 | `sliceSection` | `docs/specs/ANTS-1287.md:207` | `src/roadmapindex.cpp:159` | right |
| 30 | `error` | `docs/specs/ANTS-3855-roadmap-migrate-verb.md:750` | `src/scrollbackexporter.h:37` | **wrong** — `scrollbackexporter.h` accessor; doc means the migrate `Outcome::error` field |
| 31 | `buildAppStylesheet` | `docs/specs/ANTS-1147.md:102` | `src/themedstylesheet.cpp:13` | right |
| 32 | `detectRoadmapFormat()` | `docs/specs/ANTS-3809-roadmap-write-half.md:496` | `src/roadmapparse.cpp:1660` | right |
| 33 | `defaultSocketPath()` | `docs/specs/ANTS-1117.md:54` | `src/remotecontrol.cpp:2388` | right |
| 34 | `applyPlanFields()` | `docs/specs/ANTS-3822-consumer-write-history.md:300` | `src/roadmapmigrateload.cpp:633` | right |
| 35 | `m_pcallTimer` | `docs/specs/ANTS-1750.md:260` | `src/luaengine.h:255` | right |
| 36 | `compact` | `docs/specs/ANTS-3578.md:397` | `tests/features/mcp_result_offload/test_mcp_result_offload.cpp:464` | **wrong** — test-file helper function; doc means the `compact` argument |
| 37 | `Options::excludedNames` | `docs/specs/ANTS-3661.md:667` | `src/docsymbols.h:66` | right |
| 38 | `mcp::setHintLatchEnabled` | `docs/standards/mcp-config-keys.md:143` | `src/mcpprojection.cpp:52` | right |
| 39 | `status` | `docs/specs/ANTS-3809-roadmap-write-half.md:526` | `tests/features/doc_citations/fixture.h:59` | **wrong** — test-fixture helper function; doc means a roadmap `status` field |
| 40 | `QFileSystemWatcher::fileChanged` | `docs/specs/ANTS-1117.md:153` | `src/claudeintegration.h:635` | **wrong** — `ClaudeIntegration::fileChanged`; the qualifier `QFileSystemWatcher::` was not honoured |
| 41 | `counts` | `docs/specs/ANTS-1254.md:54` | `tests/features/doc_symbols_only_filter/test_doc_symbols_only_filter.cpp:68` | **wrong** — test-file helper function; doc means the reply's `counts` field |
| 42 | `roadmapExpandedSections` | `docs/specs/ANTS-1154.md:488` | `src/config.cpp:759` | right |
| 43 | `RemoteControl::cmdRoadmapQuery()` | `docs/specs/ANTS-1244.md:300` | `src/remotecontrol_roadmap_query.cpp:1932` | right |
| 44 | `RcGate::checkCallerCwd` | `docs/specs/ANTS-1435.md:96` | `src/remotecontrolgate.cpp:11` | right |
| 45 | `m_timedOut` | `docs/specs/ANTS-1750.md:260` | `src/luaengine.h:243` | right |
| 46 | `cmdColdEyesPartition` | `docs/specs/ANTS-1619.md:126` | `src/remotecontrol_coldeyes.cpp:20` | right |
| 47 | `RoadmapStore::db()` | `docs/specs/ANTS-3765-roadmap-migration-load.md:31` | `src/roadmapstore.h:171` | right |
| 48 | `cmdApplyEdits` | `docs/specs/ANTS-2161.md:315` | `src/remotecontrol_workspace.cpp:2601` | right |
| 49 | `status` | `docs/specs/ANTS-1407.md:186` | `tests/features/doc_citations/fixture.h:59` | **wrong** — test-fixture helper function; doc means a task's `status` field |
| 50 | `rcStatusEmoji` | `docs/specs/ANTS-4977-dropped-status.md:50` | `src/remotecontrol.cpp:1038` | right |
| 51 | `IndieReviewEngine::kMaxScanBytes` | `docs/specs/ANTS-1344.md:23` | `src/indiereviewengine.h:55` | right |
| 52 | `m_filterDone` | `docs/specs/ANTS-1106.md:70` | `src/roadmapdialog.h:628` | right |
| 53 | `kRoadmapMinParseableSize` | `docs/specs/ANTS-1437.md:203` | `src/remotecontrol.h:1547` | right |
| 54 | `kKinds` | `docs/specs/ANTS-1238.md:294` | `src/roadmapdialog.cpp:139` | right |
| 55 | `pathTokensIn()` | `docs/specs/ANTS-4065-import-mapping-contract.md:816` | `src/roadmapmigrate.cpp:1197` | right |
| 56 | `stage` | `docs/specs/ANTS-1896.md:963` | `tests/features/docsindex_header_field/test_docsindex_header_field.cpp:56` | **wrong** — test-file helper function; doc means the English word "stage" |
| 57 | `cmdProjectSettings` | `docs/specs/ANTS-3833-remotecontrol-decomposition.md:107` | `src/remotecontrol_docs.cpp:1529` | right |
| 58 | `cmdCurrentState` | `docs/specs/ANTS-2160.md:180` | `src/remotecontrol_state.cpp:1109` | right |
| 59 | `RoadmapStore::reassignItemId()` | `docs/specs/ANTS-3765-roadmap-migration-load.md:376` | `src/roadmapstore.cpp:1981` | right |
| 60 | `compactResolved` | `docs/specs/ANTS-3447.md:34` | `src/feedbackfile.cpp:1034` | right |
