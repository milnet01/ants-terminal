# ANTS-5734 trace verb — review loop log

Record for [`../specs/ANTS-5734-trace-verb.md`](../specs/ANTS-5734-trace-verb.md) § 12.

## Cold-eyes loop log

| Loop | Date | Lanes | Q1 | Q2 | Q3 | Q4 | Outcome |
|------|------|-------|----|----|----|----|---------|
| 1 | 2026-10-16 | 2 (neutral-lane; every lane held every question) | 1 | 5 | 4 | 2 | 12 findings, 12 verified / 0 dismissed; 12 fixed. One [Q1] (FocusedTest::resolve misuse) found and fixed while building the packet, before dispatch. Both lanes found the central defect: a RemoteControl handler would pull the grammars into ants-terminal; trace::serve in ants_trace_lib with a directly built RcHandler on the Bulk lane replaces it, registered before the --call return (new INV-21). Also: INV-17 made able to fail (nm check), INV-10 names its empty sections, Markdown all goes to docs[], CMake is unparsed until its grammar ships, built-in query copies, suffix scope match, tests.files counts call sites, roadmap read as roadmap_query reads it, INV-6 and INV-11 fixtures exercise their Breaks-when. Five open questions: two became findings above (roadmap source, rcDelegate thread); three resolved clean (default test root is tests/; swift parser.c omission is upstream's, per its vendor notice; docsDir dissolved by the docs[] fix). Loop 2 dispatched. |
