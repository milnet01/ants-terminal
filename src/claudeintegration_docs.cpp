// ANTS-1677 claudeintegration piece 5/9 — the document and index tool descriptors
#include "claudeintegration_internal.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace claudeintegrationdetail {

void appendToolSchemasDocs(QJsonArray &tools) {
                // ANTS-1637 — codebase_index: serve a pre-computed project
                // structural map (symbols-per-file, lane→files) so a session
                // stops re-deriving the project shape with grep/file_outline.
                QJsonObject ciTool;
                ciTool["name"] = "codebase_index";
                ciTool["description"] = QStringLiteral(
                    "Pre-computed project structural map, cached and lazily "
                    "refreshed. No selector returns a summary; symbol=Foo::bar its "
                    "definitions; lane=<name> a lane's files and symbols; "
                    "file_path=<rel> one file's outline. At most one selector (else "
                    "bad_args); a miss is ok:true, found:false. A summary with "
                    "empty:true carries empty_reason: on project_not_registered or "
                    "declared_roots_hold_no_source the index is inapplicable, not "
                    "proof of no code. caller_cwd required.");
                ciTool["detail"] = QStringLiteral(
                    "Pre-computed project structural map, cached + lazily "
                    "refreshed. No selector → a summary "
                    "(file_count / lanes / languages / roles); add "
                    "lane_files:true for a compact per-lane source_files digest "
                    "(navigable lane→file map). symbol=Foo::bar "
                    "→ every {path,line,kind} defining it (pre-indexed "
                    "find_definition, no re-grep). lane=<name> → that lane's "
                    "files + their symbols. file_path=<rel> → one file's cached "
                    "outline. At most one selector (≥2 → bad_args). A miss is "
                    "ok:true,found:false (not an error). Symbol coverage = "
                    "file_outline coverage. ANTS-4419: a summary with "
                    "file_count:0 carries empty:true PLUS empty_reason — "
                    "project_not_registered (source exists but not under src/ "
                    "or tests/, and no .ants/project.json says where; run "
                    "project_settings op:detect then op:init), "
                    "declared_roots_hold_no_source, or no_indexable_source — "
                    "with an empty_hint and an empty_detail carrying the "
                    "measured per-directory counts. READ IT before concluding "
                    "there is no code: on the first two reasons the index is "
                    "INAPPLICABLE, not authoritative, and a session that read "
                    "the bare empty:true as \"no code here\" fell back to grep "
                    "on a live 1400-file tree. ETag-304: an unchanged query "
                    "re-reads free. caller_cwd required.");
                ciTool["selection_hint"] = QStringLiteral(
                    "Use once at session start for the project's shape, or to "
                    "locate a symbol/lane, instead of repeated grep / "
                    "file_outline / CLAUDE.md reads.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    schema["additionalProperties"] = false;
                    QJsonObject props;
                    QJsonObject symProp; symProp["type"] = "string";
                        symProp["description"] = QStringLiteral(
                            "Exact, case-sensitive symbol name. Mutually "
                            "exclusive with lane / file_path.");
                    QJsonObject laneProp; laneProp["type"] = "string";
                        laneProp["description"] = QStringLiteral(
                            "Subsystem lane name. Mutually exclusive with "
                            "symbol / file_path.");
                    QJsonObject fpProp; fpProp["type"] = "string";
                        fpProp["description"] = QStringLiteral(
                            "Project file path (under caller_cwd). Mutually "
                            "exclusive with symbol / lane.");
                    QJsonObject laneFilesProp; laneFilesProp["type"] = "boolean";
                        laneFilesProp["description"] = QStringLiteral(
                            "Summary-only opt-in: also emit a compact per-lane "
                            "source_files digest (each lane's non-test paths, "
                            "sorted, globally capped; lane_digest_truncated "
                            "flags a cap hit) — a navigable lane→file map "
                            "without a per-lane call. Ignored under a selector. "
                            "(ANTS-3468) ANTS-3503: a project with no parseable "
                            "`## Module map` (empty lane digest) instead gets a "
                            "flat top-level `source_files` digest (same cap/flag) "
                            "so lane-less repos still get a first-call code map.");
                    props["symbol"]     = symProp;
                    props["lane"]       = laneProp;
                    props["file_path"]  = fpProp;
                    props["lane_files"] = laneFilesProp;   // ANTS-3468
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    props["etag_match"] = makeEtagMatchProp();   // ANTS-1499
                    props["fields"]     = makeFieldsProp();      // ANTS-1720
                    props["compact"]    = makeCompactProp();     // ANTS-2091
                    props["encoding"]   = makeEncodingProp();    // ANTS-2090
                    schema["properties"] = props;
                    ciTool["inputSchema"] = schema;
                }
                tools.append(ciTool);

                // ANTS-2139 — docs_index: serve a pre-computed project
                // documentation map (heading outline + title + status +
                // relative-link graph) so a session finds the right doc in
                // one call instead of grep/Read across an unfamiliar layout.
                QJsonObject diTool;
                diTool["name"] = "docs_index";
                diTool["description"] = QStringLiteral(
                    "Pre-computed project documentation map, cached and lazily "
                    "refreshed, for any layout. No selector returns a summary; "
                    "topic=<words> ranked docs; doc_path=<rel> one doc's headings "
                    "and links; id=<stem> docs whose filename stem matches. At most "
                    "one selector (else bad_args); a miss is ok:true, found:false. "
                    "Indexes headings, titles and paths, not body prose. caller_cwd "
                    "required.");
                diTool["detail"] = QStringLiteral(
                    "Pre-computed project documentation map, cached + lazily "
                    "refreshed. Project-agnostic (any layout, not just Ants). "
                    "No selector → a summary (per-doc {path,id,title,status,"
                    "heading_count} + dir rollup). topic=<words> → docs scored "
                    "over title/path/headings, ranked, with evidence. "
                    "doc_path=<rel> → one doc's heading outline + outbound "
                    "links + linked_from reverse edges. id=<stem> → every doc "
                    "whose filename stem matches. At most one selector (≥2 → "
                    "bad_args). A miss is ok:true,found:false (not an error). "
                    "Indexes headings/title/path, not full body prose. "
                    "ETag-304: an unchanged query re-reads free. caller_cwd "
                    "required.");
                diTool["selection_hint"] = QStringLiteral(
                    "Use once to find which doc covers a topic, or to read a "
                    "doc's outline, instead of repeated grep / Read. The "
                    "SOURCE-map sibling is codebase_index; spec_query stays "
                    "the deep per-spec reader.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    schema["additionalProperties"] = false;
                    QJsonObject props;
                    QJsonObject topicProp; topicProp["type"] = "string";
                        topicProp["description"] = QStringLiteral(
                            "Whitespace-separated keywords; scored over "
                            "title/path/headings. Mutually exclusive with "
                            "doc_path / id.");
                    QJsonObject dpProp; dpProp["type"] = "string";
                        dpProp["description"] = QStringLiteral(
                            "Project doc path (under caller_cwd) for one doc's "
                            "outline. Mutually exclusive with topic / id.");
                    QJsonObject idProp; idProp["type"] = "string";
                        idProp["description"] = QStringLiteral(
                            "Exact, case-sensitive filename stem (e.g. "
                            "\"ANTS-1637\"). Mutually exclusive with topic / "
                            "doc_path.");
                    props["topic"]      = topicProp;
                    props["doc_path"]   = dpProp;
                    props["id"]         = idProp;
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    props["etag_match"] = makeEtagMatchProp();   // ANTS-1499
                    props["fields"]     = makeFieldsProp();      // ANTS-1720
                    props["encoding"]   = makeEncodingProp();    // ANTS-2090
                    props["compact"]    = makeCompactProp();     // ANTS-4657
                    schema["properties"] = props;
                    diTool["inputSchema"] = schema;
                }
                tools.append(diTool);

                // ANTS-3601 — doc_integrity: deterministic dead-anchor /
                // broken-link / TOC-coverage checks over a doc set. The
                // grep-able rot cold-eyes reviewers otherwise find by hand.
                QJsonObject docInt;
                docInt["name"] = "doc_integrity";
                docInt["description"] = QStringLiteral(
                    "Deterministic markdown integrity check. Kinds: dead_anchor, "
                    "broken_link, toc_gap, heading_sequence, ungranted_tool (a "
                    "skill calling an mcp__ants__ verb its allowed-tools never "
                    "granted), list_count (a lead-in counting fewer items than its "
                    "list holds). Fence-aware. path=<file|dir> or paths=[...] scopes "
                    "the run (default: the docs dir); kinds=[...] filters. "
                    "Suppressed cases are counted, not reported. Refusals: bad_path, not_found. "
                    "caller_cwd required, or \"~global\" with an explicit path.");
                docInt["detail"] = QStringLiteral(
                    "Deterministic markdown doc-integrity check (no LLM). Reports "
                    "these kinds: dead_anchor (a [t](#slug) / [t](other.md#slug) "
                    "naming no real heading), broken_link (a [t](relpath) whose "
                    "target file is missing), toc_gap (a hand-maintained Table of "
                    "Contents that omits an H2 section or lists a duplicate), and "
                    "heading_sequence (ANTS-3700 — a numbered heading like "
                    "`## 5.7 Foo` that is lower than the sibling before it, skips "
                    "a number no sibling ever fills, or repeats one; siblings are "
                    "grouped by numeric parent prefix, a group's first heading is "
                    "never flagged, and unnumbered/prose headings are untouched), "
                    "and ungranted_tool (ANTS-3719 — a Claude Code skill whose "
                    "body calls `mcp__ants__<verb>` while its own allowed-tools "
                    "frontmatter never granted it, so the skill is unexecutable "
                    "as written; gated on the frontmatter carrying allowed-tools, "
                    "which scopes it to skill files, and matched on the "
                    "fully-qualified spelling only), and list_count (ANTS-5295 — "
                    "a lead-in ending in a colon, such as \"Three things to "
                    "know:\", over a list of sibling items that holds MORE "
                    "than it says; a shorter list is grouping and is never "
                    "reported, and a lead-in is trusted only where its last "
                    "sentence is at most ten words holding one number from "
                    "two to twelve, opening the sentence or closing it after "
                    "a determiner). "
                    "Fence-aware (fenced examples ignored); GitHub-compatible "
                    "heading slugs. path=<file|dir> scopes the run (a dir walks "
                    "its *.md recursively); paths=[...] scopes it to the UNION "
                    "of several files/dirs and wins over path (ANTS-4106 — a "
                    "post-fix sweep over the files a run edited, which no "
                    "single path covers); omitted → the project docs_dir (else "
                    "docs/). kinds=[...] filters findings + counts. A non-existent "
                    "in-root path is ok:true with empty checked_docs; a "
                    "root-escaping path refuses bad_path. THREE things are "
                    "deliberately NOT reported, each counted so a suppression "
                    "cannot be mistaken for a clean pass. ANTS-4641 — a numbering "
                    "gap the document itself accounts for, i.e. a Retired / "
                    "Removed / Withdrawn section listing the missing numbers as "
                    "`- **N.** …` items: such a document usually states that the "
                    "numbers are NOT reused, so renumbering to close the gap is "
                    "the one action it forbids. The numbers land in "
                    "`heading_sequence_suppressed` as {doc: [n, …]}. ANTS-4642 — "
                    "a link whose enclosing span is a verbatim QUOTATION of "
                    "another document (a double-quoted run of 30+ characters, "
                    "which may WRAP across lines, bounded by a blank line or a "
                    "fence): the target was correct where it was written and "
                    "rewriting it would falsify the quotation. Counted in "
                    "`broken_link_suppressed`; dead_anchor loses those links too, "
                    "since an anchor inside someone else's quoted sentence is no "
                    "more ours to fix than the path is. ANTS-4643 — a "
                    "home-relative link target, which names a location outside "
                    "the project exactly as a leading slash does and is skipped "
                    "the same way, rather than reported as a file that does not "
                    "exist. Emits docs_digest — a "
                    "fingerprint of the checked set (ANTS-3737) — so the ETag "
                    "tracks the DOCUMENTS, not just the findings: before that, "
                    "editing docs without changing a finding left the envelope "
                    "identical and etag_match returned a false 304, skipping the "
                    "post-fix re-check. ETag-304: unchanged docs re-read free. "
                    "caller_cwd required — or the `~global` / `~claude-config` "
                    "sentinel (ANTS-3719) to check the global Claude config tree "
                    "at ~/.claude, which has no docs/ dir, so pass an explicit "
                    "path there (e.g. path=\"skills\").");
                docInt["selection_hint"] = QStringLiteral(
                    "Use before a cold-eyes doc review, or after editing a long "
                    "contract doc, to catch dead anchors / broken links / TOC "
                    "drift mechanically. The literal-drift sibling is the "
                    "contract_doc_drift audit lane.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    schema["additionalProperties"] = false;
                    QJsonObject props;
                    QJsonObject pathProp; pathProp["type"] = "string";
                        pathProp["description"] = QStringLiteral(
                            "Project-relative file or directory to check "
                            "(default: the docs_dir override, else docs/). A "
                            "directory walks its *.md recursively.");
                    QJsonObject kindsProp; kindsProp["type"] = "array";
                        {
                            QJsonObject items; items["type"] = "string";
                            items["enum"] = QJsonArray{
                                QStringLiteral("dead_anchor"),
                                QStringLiteral("broken_link"),
                                QStringLiteral("toc_gap"),
                                QStringLiteral("heading_sequence"),
                                QStringLiteral("ungranted_tool"),
                                QStringLiteral("list_count")};
                            kindsProp["items"] = items;
                        }
                        kindsProp["description"] = QStringLiteral(
                            "Optional filter; narrows findings AND counts to "
                            "these kinds. Omitted → every kind.");
                    // ANTS-4106 — the multi-file form, matching file_outline's
                    // `paths` and read_regions' slice list.
                    QJsonObject pathsProp; pathsProp["type"] = "array";
                        {
                            QJsonObject items; items["type"] = "string";
                            pathsProp["items"] = items;
                        }
                        pathsProp["description"] = QStringLiteral(
                            "Project-relative files and/or directories to check "
                            "in ONE run, findings reported over their union "
                            "(ANTS-4106). Wins over `path` when both are sent. "
                            "Use it to scope a post-fix sweep to the files a "
                            "run actually edited — a real pass edits a handful "
                            "spread across the tree, which no single `path` "
                            "covers, so the whole-tree walk was what happened "
                            "and a pre-existing finding in an untouched file "
                            "got attributed to the pass. docs_digest and the "
                            "ETag key off the union, as they key off the walked "
                            "set today. A root-escaping entry refuses "
                            "bad_path.");
                    props["path"]       = pathProp;
                    props["paths"]      = pathsProp;   // ANTS-4106
                    props["kinds"]      = kindsProp;
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    props["etag_match"] = makeEtagMatchProp();   // ANTS-1499
                    // ANTS-4858 — parity with the sibling read verbs. Three
                    // projects reached for it here out of habit and got it
                    // back in `ignored_args`; all three called that behaviour
                    // correct, which is why this is a consistency gap and not
                    // a defect. Membership only: an ABSENT `compact` still
                    // does nothing here (mcpprojection.cpp's row), because the
                    // zeroed counts and heading_sequence_suppressed are the
                    // shape ANTS-4673 folded away from a caller who never
                    // asked.
                    props["compact"]    = makeCompactProp();    // ANTS-4858
                    schema["properties"] = props;
                    docInt["inputSchema"] = schema;
                }
                tools.append(docInt);

                // ANTS-3661 — doc_symbols: resolve the identifiers a doc
                // asserts something about. Reports; never judges.
                QJsonObject docSym;
                docSym["name"] = "doc_symbols";
                docSym["description"] = QStringLiteral(
                    "Resolve the identifiers a doc claims exist: inline code spans "
                    "like `Foo::bar()` are looked up with find_definition's "
                    "resolver and reported resolved | unresolved | not_checked | "
                    "referenced. "
                    "Fenced code, paths, keywords and MCP names are excluded. "
                    "REPORT-ONLY. Unresolved findings carry `shape` (call | "
                    "qualified | bare); read call-shaped ones first. not_checked "
                    "means never looked up. only narrows symbols[]. Read-only. "
                    "caller_cwd required. Forward lookup, so it can be large; "
                    "holding the names, use cited_by.");
                docSym["detail"] = QStringLiteral(
                    "Resolve the identifiers a doc claims exist. Harvests inline code spans "
                    "that look like `Foo::bar()` and looks each up with find_definition's "
                    "resolver, reporting resolved | unresolved | not_checked per occurrence. "
                    "Fenced code, paths, language keywords, MCP verb/argument names and "
                    "doc-examples regions are excluded. A bare lowercase word, or one capitalised "
                    "word such as Kind or PARTIAL (no ::, no ()), is reported ONLY if it "
                    "resolves — unresolved it is indistinguishable from a JSON key, a field "
                    "value or a lint code, so it is dropped "
                    "rather than guessed at. REPORT-ONLY: an unresolved name may be rot or a "
                    "forward reference to something the doc is about to create, and deciding "
                    "which is yours — the verb emits no severity and nothing auto-fixable. "
                    "not_checked means a needle the run never looked up (cap or deadline), "
                    "never 'does not exist'; truncated:true accompanies it. "
                    "ANTS-5357: referenced means no definition was found but the "
                    "project's code uses the name outside comments and strings — a "
                    "library's name, a standard flag, a parameter; used_at gives the "
                    "first use, and it raises no finding. Emits "
                    "docs_digest, a fingerprint of the checked set, so the ETag tracks the "
                    "documents and not just the findings (ANTS-3737). "
                    "ANTS-4359: the unresolved bucket is classified by the SHAPE the "
                    "document wrote — each finding carries `shape` (call | qualified | "
                    "bare) and `counts` carries `unresolved_by_shape`. Read the "
                    "call-shaped ones FIRST: a span with `()` is a claim about a "
                    "FUNCTION, where \"no such symbol\" is usually a real defect, while "
                    "bare identifiers are as often enum values, macros, CMake variables "
                    "or JSON keys. A classification, never a verdict — nothing is "
                    "filtered and no bucket is dropped. "
                    "ANTS-3689: `only` narrows symbols[] to one resolution class "
                    "(\"unresolved\" | \"not_checked\"); `counts` stays "
                    "whole-document and `symbols_filtered_out` reports what was "
                    "withheld. Reach for it on a single dense document — every "
                    "`resolved` row ships by default, which is the class nobody "
                    "reads and the reason an unfiltered single-doc run can spill "
                    "past the response cap. "
                    "ANTS-5313: mode:\"locator\" answers WHERE rather than WHETHER — "
                    "one file:line per distinct symbol, ambiguous counts where more "
                    "than one place matches, and no per-occurrence rows. Measured "
                    "wrong about 1 time in 8 (docs/reviews/ANTS-5313-locator-"
                    "precision-2026-09-24.md): verify a locator before acting on it. "
                    "Read-only. "
                    "caller_cwd required.");
                docSym["selection_hint"] = QStringLiteral(
                    "Use when reviewing a spec or design doc to get the short list of names "
                    "it mentions that resolve nowhere — the expensive half of a cold read.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    schema["additionalProperties"] = false;
                    schema["required"] = QJsonArray{QStringLiteral("caller_cwd")};
                    QJsonObject props;
                    QJsonObject dsPath; dsPath["type"] = "string";
                        dsPath["description"] = QStringLiteral(
                            "Project-relative file or directory to scan (a directory walks "
                            "*.md recursively). Default: the project's docs dir, else docs/.");
                    const QJsonObject dsCwd = makeCallerCwdReadProp();
                    const QJsonObject dsEtag = makeEtagMatchProp();
                    // ANTS-3689 — doc_citations' `only` shape, same rule:
                    // narrow the rows, leave counts whole-document.
                    QJsonObject dsOnly; dsOnly["type"] = "string";
                        dsOnly["enum"] = QJsonArray{
                            QStringLiteral("all"),
                            QStringLiteral("unresolved"),
                            QStringLiteral("not_checked")};
                        dsOnly["description"] = QStringLiteral(
                            "Filter over symbols[] only. \"unresolved\" keeps the rows "
                            "worth reading and drops the resolved ones nobody reads — "
                            "the difference between a reply you can read inline and one "
                            "that spills to a file. counts stay whole-document and "
                            "symbols_filtered_out reports what was withheld, so a short "
                            "list is not mistaken for a clean document. Default all.");
                    props["path"] = dsPath;
                    props["caller_cwd"] = dsCwd;
                    props["etag_match"] = dsEtag;
                    props["only"] = dsOnly;
                    // ANTS-5313 — the compact form a reviewing lane wants.
                    QJsonObject dsMode; dsMode["type"] = "string";
                        dsMode["enum"] = QJsonArray{
                            QStringLiteral("full"), QStringLiteral("locator")};
                        dsMode["description"] = QStringLiteral(
                            "\"locator\": one entry per DISTINCT symbol — "
                            "locators {symbol: \"file:line\"} (a definition beats a "
                            "declaration), ambiguous {symbol: n} where more than one "
                            "place matches (never a guess), unresolved[], "
                            "not_checked[], declared_only[] (declared only as a local or a "
                            "forward declaration), referenced {symbol: \"file:line\"} (used "
                            "by the code, defined elsewhere). `A::b` locates only a b inside A. "
                            "MEASURED: about 1 locator in 8 points at the wrong thing "
                            "(8/60 over 292 docs, ANTS-5313), mostly bare common words "
                            "hitting an unrelated helper; open the cited line before "
                            "relying on it. No symbols[] or findings[]. Refuses with "
                            "only. Default full.");
                    props["mode"] = dsMode;
                    // ANTS-5265 — workspace_search's count_only, same meaning.
                    QJsonObject dsCountOnly; dsCountOnly["type"] = "boolean";
                        dsCountOnly["description"] = QStringLiteral(
                            "Totals only: counts and findings_count, no symbols[] "
                            "or findings[]. Refuses with mode:\"locator\" or a "
                            "narrowing only.");
                    props["count_only"] = dsCountOnly;
                    schema["properties"] = props;
                    docSym["inputSchema"] = schema;
                }
                tools.append(docSym);

                // ANTS-3662 — spec_lint: the greppable half of the spec-format
                // contract, which /cold-eyes § 1e hand-rolls every review pass.
                QJsonObject specLint;
                specLint["name"] = "spec_lint";
                specLint["description"] = QStringLiteral(
                    "Check specs for structural defects: invariant_no_test, "
                    "invariant_id_gap, loop_row_no_outcome, test_coverage_gap, "
                    "test_coverage_unverifiable, command_test_no_expectation, "
                    "header_*, filename_id_mismatch, duplicate_invariant, "
                    "invariants_unparsed and "
                    "missing_section (needs a standard's required-sections "
                    "block). Read skipped[] and invariants_found: "
                    "a skipped check or zero invariants is silence, not a clean "
                    "result. counts cover the full scan; max_findings caps "
                    "findings[]. Read-only. caller_cwd required.");
                specLint["detail"] = QStringLiteral(
                    "Check specs for the structural defects /cold-eyes § 1e greps for by "
                    "hand: an INV-N with no test-surface clause (invariant_no_test), a gap "
                    "in the doc's own id sequence (invariant_id_gap \u2014 a CANDIDATE since ANTS-3684: a spec carrying a subset of a parent's invariants keeps the parent's ids, and renumbering would break the citation), a cold-eyes loop-log "
                    "row with no outcome (loop_row_no_outcome), an invariant the Tests "
                    "section names no coverage for (test_coverage_gap) or a Tests section "
                    "stating coverage in notation that cannot be expanded, such as "
                    "INV-1..7 or INV-1/2 (test_coverage_unverifiable) \u2014 both ANTS-4623, "
                    "both CANDIDATES, and both skipped entirely when the Tests section "
                    "names no id at all, since comparing against nothing condemns every "
                    "invariant; `test_coverage_checked` says which happened. And a test clause that is a "
                    "command stating nothing it should return (command_test_no_expectation, "
                    "a CANDIDATE — never auto-fixable, and no subprocess is ever run). "
                    "Tombstoned invariants (*moved to X*, *withdrawn — …*) are exempt from "
                    "both invariant checks, ANTS-4351: however the body is WRAPPED (the "
                    "body is joined before matching, as a reader does — a hard-wrapped "
                    "tombstone used to come back as invariant_no_test, byte-identical to a "
                    "genuinely untested invariant). missing_section runs ONLY when the "
                    "project's format standard carries a <!-- required-sections --> block; "
                    "without one it is skipped and sections_checked comes back false, which "
                    "is the shipping default — a check against a guessed list would fire on "
                    "every conforming spec. ANTS-4390: the standard is looked for at "
                    "docs/standards/{spec-format,specs}.md and then standards/{...}.md, so a "
                    "repository that IS the standards set (no docs/ prefix) can check its "
                    "own specs. ANTS-4080: those same four are then tried under ~/.claude/, "
                    "where the spec-format standard has been authoritative since 2026-08-08 "
                    "— so a project carrying only a delta is no longer linted against "
                    "nothing, and sections_source prefixes the global hit with `~global/`. "
                    "ANTS-3784: a document may declare a DELIBERATE id floor with an "
                    "<!-- invariant-id-base: N --> line outside fenced code; missing numbers "
                    "below N are counted as suppressed rather than reported, which is the "
                    "only route for a spec that continues a sibling's numbering on a corpus "
                    "that otherwise numbers per-document. ANTS-4373: when the check is skipped the envelope says so "
                    "in fields a caller reads rather than in a boolean it does not — "
                    "skipped[] naming EVERY gated check that did not run, EACH with its "
                    "own hint that names it back (ANTS-4676): skipped_hint explains "
                    "missing_section and names the paths consulted, surfaces_skipped_hint "
                    "explains test_surface_absent / test_surface_unresolved / "
                    "test_surface_unwired. ANTS-4666: invariant_no_test is NOT in that "
                    "list and never was gated — it fires for any INV-N with no *Test:* "
                    "clause, so an envelope naming it as skipped while returning its "
                    "findings could be read neither way. ANTS-4679: what turns the "
                    "surface checks on is a `tests/features/<name>/` directory, the only "
                    "shape this verb resolves. "
                    "ANTS-4889: a skip hint is emitted ONLY when its check was "
                    "skipped, so requesting one by name in fields= and seeing it "
                    "return in fields_unmatched is the signal the check RAN - "
                    "absence of the hint is the pass. Stated because a caller "
                    "asking for it defensively cannot otherwise tell \"check ran\" "
                    "from \"field misspelled\". "
                    "ANTS-4894: invariants_found reports how many invariants were "
                    "RECOGNISED, on every run, because ZERO is the alarm - a "
                    "document whose invariants are in a form the format standard "
                    "does not define parses to none, and every per-invariant check "
                    "then runs over an empty set while the envelope stays "
                    "comparable with one that checked them all. "
                    "ANTS-4737: counts and findings_total are taken over the FULL scan "
                    "and max_findings caps only findings[], so two runs at different caps "
                    "are comparable \u2014 the cap used to choose the count (81 uncapped, 39 at "
                    "max_findings:40 over one unchanged corpus), which silently voids a "
                    "before/after measurement. ANTS-4738: writing the marker as "
                    "<!-- required-sections: prefix --> matches a numbered entry on its "
                    "`## N. Name` prefix, so a descriptive suffix passes; verbatim stays "
                    "the default, since loosening it silently would weaken every corpus "
                    "relying on the exact match. ANTS-4739: a document carrying "
                    "<!-- spec-lint: no-required-sections --> outside fenced code is "
                    "exempt from missing_section, for a mixed corpus holding pre-standard "
                    "specs, build plans and ledgers that can never conform; the number of "
                    "exempted documents is echoed as sections_exempt_docs so an exemption "
                    "is never mistaken for a clean pass. "
                    "sections_source names the standard that DID resolve (null "
                    "when none did). "
                    "ANTS-4926: a `~global/` prefix says the global standard ANSWERED, not "
                    "that the project adopted none - a conforming project keeps no local "
                    "copy (the standard forbids one) and writes deltas to "
                    "docs/standards/spec-format-overrides.md, so adoption and absence "
                    "looked identical. sections_source_reason separates them: "
                    "project_standard | adoption_marker | local_standard_no_block | "
                    "no_local_standard, with sections_source_hint on the three arms a "
                    "caller can misread. On adoption_marker the section findings are REAL "
                    "and must not be dropped on the prefix. "
                    "Treat a skipped run as SILENT about section structure, "
                    "never as a clean structural result. Size is reported in line_count, never as a finding. "
                    "Emits docs_digest, a fingerprint of the checked set, so the ETag "
                    "tracks the documents and not just the findings (ANTS-3737). "
                    "Read-only. caller_cwd required.");
                specLint["selection_hint"] = QStringLiteral(
                    "Use before a spec review to clear the mechanical findings, so reviewer "
                    "attention goes to judgement rather than to grep-able trivia.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    schema["additionalProperties"] = false;
                    schema["required"] = QJsonArray{QStringLiteral("caller_cwd")};
                    QJsonObject props;
                    QJsonObject slPath; slPath["type"] = "string";
                        slPath["description"] = QStringLiteral(
                            "Project-relative spec file or directory (a directory walks *.md "
                            "recursively). Default: the project's specs dir, else docs/specs/.");
                    const QJsonObject slCwd = makeCallerCwdReadProp();
                    QJsonObject slMax; slMax["type"] = "integer";
                        slMax["description"] = QStringLiteral(
                            "Run-wide cap on findings[] (default 500, clamped to [1,5000]). "
                            "truncated:true accompanies a capped run.");
                    const QJsonObject slEtag = makeEtagMatchProp();
                    props["path"] = slPath;
                    props["caller_cwd"] = slCwd;
                    props["max_findings"] = slMax;
                    props["etag_match"] = slEtag;
                    // ANTS-5537 — diff scope (docs/specs/ANTS-3662.md § 2.4).
                    QJsonObject slSince; slSince["type"] = "string";
                        slSince["description"] = QStringLiteral(
                            "A git ref. Scope findings to what changed between that "
                            "commit and the working tree (untracked files count as "
                            "changed): findings[] holds in-diff findings only, each "
                            "carrying in_diff:true, and counts_in_diff / "
                            "counts_out_of_diff account for the rest. A spec stamped "
                            "<!-- ants-spec-format: N --> is checked in full. Refuses "
                            "not_a_git_repo, bad_args (unresolvable ref) or git_failed; "
                            "never falls back to a full-file run.");
                    QJsonObject slStaged; slStaged["type"] = "boolean";
                        slStaged["description"] = QStringLiteral(
                            "true scopes findings to what is staged (index against "
                            "HEAD, or the empty tree before the first commit), "
                            "checking each staged spec's index copy. Exclusive with "
                            "`since` (bad_mode_combo); false is the same as absent.");
                    props["since"]  = slSince;
                    props["staged"] = slStaged;
                    // ANTS-4663 — this schema is
                    // additionalProperties:false, so `fields=` was not
                    // merely unhonoured on a corpus run, it was
                    // undeclared: a strict client refuses it before the
                    // dispatcher is reached. Declared here with the full
                    // description; ANTS-4524 later made the property
                    // universal, and the injection skips a schema that
                    // already carries its own.
                    props["fields"]  = makeFieldsProp();   // ANTS-1720
                    props["compact"] = makeCompactProp();  // ANTS-2091
                    schema["properties"] = props;
                    specLint["inputSchema"] = schema;
                }
                tools.append(specLint);

                // ANTS-4108 — spec_conformance: the EXECUTABLE half of spec
                // review. spec_lint greps a spec's structure; this RUNS the
                // patterns it prescribes against the examples beside them.
                QJsonObject specConf;
                specConf["name"] = "spec_conformance";
                specConf["description"] = QStringLiteral(
                    "Run a spec's own regex pcre2 fences against the `| input | "
                    "expected |` table beside them, instead of reading them. A row "
                    "whose result differs is a FINDING, a fence with no table a "
                    "CANDIDATE, a per-case timing an OBSERVATION. Executes patterns "
                    "only, never code. Read executable_fences with "
                    "skipped_fences[]: zero executable fences means the run checked "
                    "nothing. Refusals: bad_args, bad_path, not_found; "
                    "unsupported_engine per fence. Read-only. caller_cwd and `path` "
                    "required.");
                specConf["detail"] = QStringLiteral(
                    "Run a spec's own regex patterns against the `| input | expected |` "
                    "table beside them, instead of reading them. Three buckets: a row "
                    "whose actual result differs from `expected` is a FINDING; a "
                    "```regex fence with no table beside it is a CANDIDATE; a per-case "
                    "timing is an OBSERVATION (which is what catches a fixture that "
                    "returns before the pattern under test ever runs). Executes "
                    "PATTERNS ONLY — never fenced code, so nothing it runs can reach "
                    "the filesystem or the network. Engine tag `pcre2` only; anything "
                    "else refuses `unsupported_engine` per fence rather than "
                    "substituting an engine. Refusals: bad_args (absent `path`, "
                    "`max_cases` outside [1,1000] — refused, never clamped), bad_path, "
                    "not_found. "
                    "ANTS-4370: a fence this verb cannot execute is reported "
                    "rather than ignored — `skipped_fences[]` names each one "
                    "with its tag and line, and `executable_fences` is the "
                    "denominator beside `cases_run`. A spec whose patterns sit "
                    "in ```python fences used to return EVERY bucket empty, "
                    "which reads as a pass; `executable_fences:0` with a "
                    "non-empty `skipped_fences[]` means this run checked "
                    "NOTHING. Those fences are still not candidates — that is "
                    "§ 2.6 on purpose, since every code block in every spec "
                    "would otherwise be a finding. "
                    "Read-only. caller_cwd required; `path` required.");
                specConf["selection_hint"] = QStringLiteral(
                    "Use on a spec that prescribes regexes, before implementing it: a "
                    "cold read passes a wrong pattern, running it does not.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    schema["additionalProperties"] = false;
                    schema["required"] = QJsonArray{QStringLiteral("caller_cwd"),
                                                    QStringLiteral("path")};
                    QJsonObject props;
                    QJsonObject scPath; scPath["type"] = "string";
                        scPath["description"] = QStringLiteral(
                            "Project-relative path to ONE markdown document. Required — "
                            "this verb executes a document, so there is no tree walk to "
                            "default to (unlike spec_lint).");
                    const QJsonObject scCwd = makeCallerCwdReadProp();
                    QJsonObject scMax; scMax["type"] = "integer";
                        scMax["description"] = QStringLiteral(
                            "Cap on executed cases (default 200, range [1,1000]). Out of "
                            "range REFUSES bad_args rather than clamping; a capped run "
                            "returns truncated:true so a partial run never reads as a "
                            "complete one.");
                    QJsonObject scEtag; scEtag["type"] = "string";
                        scEtag["description"] = QStringLiteral(
                            "Server-issued etag from a prior call; an unchanged document "
                            "short-circuits to {ok:true, unchanged:true}. The etag covers "
                            "the envelope minus observations[] (per-run timings), so it "
                            "tracks the answer and not the clock.");
                    props["path"] = scPath;
                    props["caller_cwd"] = scCwd;
                    props["max_cases"] = scMax;
                    props["etag_match"] = scEtag;
                    schema["properties"] = props;
                    specConf["inputSchema"] = schema;
                }
                tools.append(specConf);

                // ANTS-3660 — doc_dedup: the same passage written twice, which
                // /cold-eyes § 1e has no mechanical check for at all.
                QJsonObject docDedup;
                docDedup["name"] = "doc_dedup";
                docDedup["description"] = QStringLiteral(
                    "Find near-duplicate PASSAGES across a doc set (the same fact "
                    "written twice): paragraphs are shingled into word 3-grams and "
                    "pairs at or above min_similarity are reported as Jaccard. "
                    "Returns pairs[] ({a,b,similarity}) and clusters[] (the "
                    "readable view). Fence-aware. REPORT-ONLY and never "
                    "auto-fixable; it finds shared words, not shared meaning. "
                    "caller_cwd required.");
                docDedup["detail"] = QStringLiteral(
                    "Find near-duplicate PASSAGES across a doc set — the same fact written "
                    "twice, which /cold-eyes Phase 4 names as the usual cause of a review "
                    "that will not converge. Segments each markdown file into paragraphs "
                    "(a list item is one passage, marker line included), shingles them into "
                    "word 3-grams and reports every pair at or above min_similarity as "
                    "Jaccard. Fence-aware: two identical code samples are not a duplicated "
                    "fact. pairs[] carries both ends ({a,b,similarity}); clusters[] groups "
                    "them into connected components, which is the readable view — N docs "
                    "sharing one stanza are N(N-1)/2 pairs but ONE thing to fix. "
                    "REPORT-ONLY and never auto-fixable: which copy is canonical is a "
                    "question about document architecture, and deleting the wrong one "
                    "destroys the authoritative copy. Generated artifacts "
                    "(AUTOMATED_AUDIT_REPORT*, superpowers/) and pointer-only paragraphs "
                    "are excluded; short paragraphs below min_words are too. exclude_headings "
                    "also leaves out a section the corpus's own standard requires in every "
                    "document, and sections_excluded says how many went. Finds shared "
                    "WORDS, not shared meaning — a clean run is not proof of no "
                    "duplication. caller_cwd required.");
                docDedup["selection_hint"] = QStringLiteral(
                    "Use before a documentation review: every duplicated fact is re-read by "
                    "every lane on every loop and is a standing source of future findings, "
                    "so finding them once up front is cheaper than rediscovering them one "
                    "at a time.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    schema["additionalProperties"] = false;
                    schema["required"] = QJsonArray{QStringLiteral("caller_cwd")};
                    QJsonObject props;
                    QJsonObject ddPath; ddPath["type"] = "string";
                        ddPath["description"] = QStringLiteral(
                            "Project-relative file or directory (a directory walks *.md "
                            "recursively). Default: the project's docs dir, else docs/.");
                    const QJsonObject ddCwd = makeCallerCwdReadProp();
                    QJsonObject ddSim; ddSim["type"] = "number";
                        ddSim["description"] = QStringLiteral(
                            "Jaccard threshold, at or above which a pair reports (default "
                            "0.40, clamped to [0,1]). Measured on this corpus: 0.40 yields "
                            "275 pairs in 128 clusters and catches both known real pairs; "
                            "0.80 narrows to 112 pairs in 26 clusters. Raising it discards "
                            "true positives to make the report look tidy.");
                    QJsonObject ddWords; ddWords["type"] = "integer";
                        ddWords["description"] = QStringLiteral(
                            "Minimum words in a comparable paragraph (default 15, clamped "
                            "to [1,1000]). Keep it low — the real finds are 19-31 words, "
                            "so a value set to suppress noise deletes them.");
                    QJsonObject ddShingle; ddShingle["type"] = "integer";
                        ddShingle["description"] = QStringLiteral(
                            "Words per shingle (default 3, clamped to [2,10]). Larger is "
                            "stricter about word order, smaller pairs more freely.");
                    QJsonObject ddHeadings; ddHeadings["type"] = "array";
                        ddHeadings["items"] = QJsonObject{{"type", "string"}};
                        ddHeadings["description"] = QStringLiteral(
                            "Leave out the sections under these headings, subsections "
                            "included: ones every document must carry. Case ignored.");
                    const QJsonObject ddEtag = makeEtagMatchProp();
                    props["path"] = ddPath;
                    props["caller_cwd"] = ddCwd;
                    props["min_similarity"] = ddSim;
                    props["min_words"] = ddWords;
                    props["shingle_size"] = ddShingle;
                    props["exclude_headings"] = ddHeadings;
                    props["etag_match"] = ddEtag;
                    schema["properties"] = props;
                    docDedup["inputSchema"] = schema;
                }
                tools.append(docDedup);

                // ANTS-3663 — doc_lint: all six deterministic doc checkers in
                // one call, over one enumeration and one shared read.
                QJsonObject docLint;
                docLint["name"] = "doc_lint";
                docLint["description"] = QStringLiteral(
                    "Run every deterministic document check in one call "
                    "(doc_integrity, doc_citations, doc_dedup, doc_symbols, "
                    "spec_lint, doc_facts) and return one findings list in a total order. "
                    "Narrow with checks[] (an unknown name refuses bad_args); "
                    "max_findings pages after the sort while counts cover the whole "
                    "run. REPORT-ONLY unless fix:true, which repairs toc_gap only; "
                    "dry_run:true previews. Read checks_run[] with check_errors[] "
                    "and skipped[]: an errored checker's counts are a floor. "
                    "caller_cwd required.");
                docLint["detail"] = QStringLiteral(
                    "Run EVERY deterministic document check in one call and return one "
                    "findings list: doc_integrity (dead anchors, broken links, TOC gaps, "
                    "heading order, ungranted skill tools), doc_citations (path:line "
                    "citations that no longer resolve), doc_dedup (the same passage "
                    "written twice), doc_symbols (backticked identifiers that resolve "
                    "nowhere), spec_lint (the greppable half of the spec-format "
                    "contract) and doc_facts (a list lead-in whose count is wrong, an "
                    "INV-N defined twice, leaked tool-call markup, a version claim that "
                    "disagrees with the project version, an MCP call example passing an "
                    "argument the verb does not take — run it alone with "
                    "checks:[\"doc_facts\"]). One walk and one shared read serve the four "
                    "native checkers, so this is cheaper than the five calls it replaces. "
                    "Narrow with checks[]; an unknown name refuses bad_args rather than "
                    "reading as all six. findings[] is in a TOTAL order (file, line, "
                    "verb, kind, message) so two runs diff cleanly, and max_findings "
                    "pages it AFTER that sort while counts describes the whole run. "
                    "REPORT-ONLY unless you pass fix:true, which repairs the one "
                    "auto-fixable kind (toc_gap) as a patch and leaves every other line "
                    "byte-identical; dry_run:true previews that down the same path and "
                    "writes nothing. Read checks_run[] together with "
                    "check_errors[] — a checker with entries there ran INCOMPLETELY and "
                    "its counts are a floor, not a total; skipped[] plus truncated say "
                    "the run did not cover the whole tree. spec_lint is applied only to "
                    "documents under the project's specs dir, because a README has no "
                    "invariants. staged:true checks the git index copy of every "
                    "staged *.md instead of the worktree, for a commit-time run. "
                    "caller_cwd required.");
                docLint["selection_hint"] = QStringLiteral(
                    "Use before any documentation review or pre-pass: it runs the whole "
                    "deterministic layer in one call, where the five verbs it composes "
                    "each re-walk the same tree when called separately.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    schema["additionalProperties"] = false;
                    schema["required"] = QJsonArray{QStringLiteral("caller_cwd")};
                    QJsonObject props;
                    QJsonObject dlPath; dlPath["type"] = "string";
                        dlPath["description"] = QStringLiteral(
                            "Project-relative file or directory (a directory walks *.md "
                            "recursively). Default: the project's docs dir, else docs/.");
                    const QJsonObject dlCwd = makeCallerCwdReadProp();
                    QJsonObject dlChecks; dlChecks["type"] = "array";
                        dlChecks["items"] = QJsonObject{{"type", "string"}};
                        dlChecks["description"] = QStringLiteral(
                            "Which checkers to run (default: all six). Values are exactly "
                            "the `verb` strings the findings carry — doc_integrity, "
                            "doc_citations, doc_dedup, doc_symbols, spec_lint, doc_facts — so a name "
                            "read off a finding can be fed straight back. An unknown name "
                            "REFUSES bad_args with the accepted list: a filter that "
                            "silently widened would return a clean-looking report about a "
                            "question nobody answered.");
                    QJsonObject dlMax; dlMax["type"] = "integer";
                        dlMax["description"] = QStringLiteral(
                            "Cap on findings[] (default 500, clamped to [1,5000]). Applied "
                            "AFTER the total sort, so the page is a deterministic prefix "
                            "and raising the cap only appends. `counts` is computed before "
                            "it and always describes the whole run.");
                    QJsonObject dlFix; dlFix["type"] = "boolean";
                        dlFix["description"] = QStringLiteral(
                            "Repair what can be repaired (default false — the verb is "
                            "REPORT-ONLY unless asked). Exactly one kind is auto-fixable: "
                            "`toc_gap`, where a hand-maintained table of contents "
                            "disagrees with the document's own headings and the document "
                            "already contains the correct answer. The repair is a PATCH — "
                            "it inserts or deletes the one entry the finding names and "
                            "leaves every other line byte-identical, including H3 entries, "
                            "free-text rows and entries matching no heading. Everything "
                            "else is report-only by design: deleting one of two duplicated "
                            "facts needs a decision about which is canonical, an "
                            "unresolved symbol may be a forward reference, renumbering an "
                            "INV-N breaks every citation of it, and a missing section "
                            "needs prose only an author can write. Adds `fixed[]` (per "
                            "finding), `files_written` (per file — three gaps in one "
                            "document are three entries and one write) and, when a repair "
                            "did not land, `fix_errors[]` with a reason: `stale` (the file "
                            "changed between the walk and the write, so nothing was "
                            "written for it), `read_failed`, `no_template` (a TOC with no "
                            "existing entry whose form a new one could copy) or "
                            "`write_failed`. Those keys are ABSENT under fix:false rather "
                            "than zero, so \"nothing needed repairing\" is distinguishable "
                            "from \"never asked\".");
                    QJsonObject dlDry; dlDry["type"] = "boolean";
                        dlDry["description"] = QStringLiteral(
                            "Compute the repair and write NOTHING, down the same code "
                            "path — a preview assembled separately is a preview that "
                            "drifts. The response echoes `dry_run:true` so a stored "
                            "envelope can be told from a run that really wrote, and "
                            "`files_written` is then the would-be count. REFUSES bad_args "
                            "without `fix:true`: the fix keys are absent under fix:false, "
                            "so a silently-accepted preview would return exactly the "
                            "envelope of a plain read and a mistyped flag pair would read "
                            "as \"nothing to repair\".");
                    props["path"] = dlPath;
                    props["caller_cwd"] = dlCwd;
                    props["checks"] = dlChecks;
                    QJsonObject dlHeadings; dlHeadings["type"] = "array";
                        dlHeadings["items"] = QJsonObject{{"type", "string"}};
                        dlHeadings["description"] = QStringLiteral(
                            "doc_dedup: leave out the sections under these headings, "
                            "subsections included. Case ignored.");
                    props["exclude_headings"] = dlHeadings;
                    props["max_findings"] = dlMax;
                    props["fix"] = dlFix;
                    props["dry_run"] = dlDry;
                    // ANTS-5294 — ANTS-3663 § 2.7.
                    QJsonObject dlStaged; dlStaged["type"] = "boolean";
                        dlStaged["description"] = QStringLiteral(
                            "Check the git index copy of each staged *.md, not the worktree; no path means the whole project. fix:true refuses bad_mode_combo.");
                    props["staged"] = dlStaged;
                    // ANTS-5293 — tests/features/doc_lint_genre/spec.md.
                    QJsonObject dlGenre; dlGenre["type"] = "string";
                        dlGenre["enum"] = QJsonArray{QStringLiteral("path"), QStringLiteral("auto")};
                        dlGenre["description"] = QStringLiteral(
                            "path (default): spec_lint by specs dir. auto: each document's Genre: line picks its checks; adds genres and genre_unknown.");
                    props["genre"] = dlGenre;
                    // NO etag_match, deliberately: doc_lint is absent from
                    // isEtagSupportedTool (INV-13) and offering the argument
                    // would advertise a short-circuit it never performs.
                    schema["properties"] = props;
                    docLint["inputSchema"] = schema;
                }
                tools.append(docLint);

                // ANTS-3636 — doc_citations: resolve every path:line citation
                // in one doc and return the line it points at, so a reviewer
                // stops opening 33 files by hand to verify them.
                QJsonObject docCit;
                docCit["name"] = "doc_citations";
                docCit["description"] = QStringLiteral(
                    "Resolve a doc's `path:line` citations and return the cited "
                    "text. Each carries status ok | missing_file | foreign_path | "
                    "out_of_range | read_error | ambiguous | unresolved; "
                    "`path::symbol` also resolves. only=\"stale\" narrows to non-ok. "
                    "counts mixes status and overlay buckets (see "
                    "counts_overlay_keys). A zero with unrecognised_candidates is "
                    "not a passed check. Read-only. caller_cwd required, or "
                    "\"~global\".");
                docCit["detail"] = QStringLiteral(
                    "ANTS-4565 — pass `caller_cwd: \"~global\"` (alias "
                    "\"~claude-config\") to check citations in a doc under "
                    "~/.claude/, the same sentinel doc_integrity, "
                    "workspace_search, file_outline and read_region accept. "
                    "This verb was the one member of that set that refused "
                    "it, so a run over a skill tree took two of its three "
                    "checks through the sentinel and hand-rolled the third. "
                    "Note the basename fallback is unavailable under the "
                    "sentinel (that root has no codebase index), so a "
                    "citation resolves project-relative or not at all. "
                    "Resolve a doc's `path:line` citations against the files and return the "
                    "cited line TEXT. Scans one markdown/text file, finds every src/a.cpp:12 "
                    "/ a.cpp:10-12 / `:45` continuation, resolves it (project-relative, then "
                    "the codebase-index basename map, then the repo root), and reports each "
                    "with status ok | missing_file | foreign_path | out_of_range | read_error "
                    "| ambiguous | unresolved plus the text. Fenced examples are skipped; a "
                    "citation inside an inline code span IS harvested. ANTS-4085 — "
                    "`foreign_path` is a path-shaped citation whose leading segment names no "
                    "directory here AND whose basename exists nowhere in the project: almost "
                    "always a QUOTATION of another repository's path, so it is reported and "
                    "counted but is NOT stale, because there is nothing here to fix and a "
                    "checker whose output is never empty stops being read. A path whose "
                    "basename the project DOES know stays `missing_file` — that is our file, "
                    "genuinely gone. only=\"stale\" narrows to the "
                    "non-ok ones; counts stay unfiltered. ANTS-4918 — a citation on a "
                    "Cold-eyes loop-log table row (first cell a loop number, second an "
                    "ISO date) carries loop_log_row:true, is counted in "
                    "counts.loop_log_row, and is left out of only=\"stale\" whatever its "
                    "status: a landed loop-log row is never edited, so its citation cannot "
                    "be fixed. A continuation after a foreign_path antecedent is "
                    "foreign_path too. ANTS-4087 — `counts` mixes "
                    "STATUS buckets (which partition `count`) with OVERLAY buckets "
                    "layered on the ok subset, so the map does NOT sum to `count`; "
                    "`counts_overlay_keys` names the overlays, so assert "
                    "sum(counts[k] for k not in counts_overlay_keys) == count. "
                    "`unchecked` means resolved-but-nothing-to-cross-check (the doc "
                    "gave no anchor needle), NOT unverified — a clean pass over "
                    "anchorless citations reports every one of them there. offset pages; max_range_lines "
                    "bounds the lines per citation; max_bytes the response; max_doc_lines the "
                    "scanned prefix. Tokens that look like citations but resolve nowhere land "
                    "in unparsed[] rather than being guessed at. "
                    "ANTS-4743: a count of 0 now says whether it is CLEAN or "
                    "SILENT. ANTS-4748 widened the grammar to a backticked "
                    "`path::symbol` (see forms_recognised, on every reply): it "
                    "resolves ONLY where the path resolves and the symbol is "
                    "unique in that file, and such a citation carries "
                    "`resolved_via:\"symbol\"` because its `start_line` was "
                    "looked up rather than authored. Anything else — an "
                    "overload, a name that has moved, an unresolvable path — "
                    "resolves to nothing and is counted as unchecked, since a "
                    "false ok is worse than an admitted silence. The "
                    "spec-format standard forbids authors writing `path:line` "
                    "— cite the symbol, not the line — so a corpus using "
                    "neither form still scans to zero with an EMPTY unparsed[] "
                    "reinforcing it: nothing was rejected because nothing was "
                    "recognised as a candidate. A zero over a document holding "
                    "citation-shaped spans (`Class::method()`, a bare "
                    "backticked path, an unresolvable `path::symbol`) carries "
                    "`unrecognised_candidates` and a hint. Do NOT record such "
                    "a run as a citation check that passed. ANTS-5296 — "
                    "paths:true also checks a bare path in a single-line code "
                    "span (`tools/check-doc.py`), which carries no `:line` and "
                    "so is no citation. A token is judged only where its "
                    "leading segment is a real directory under the root or "
                    "under the document's own directory; elsewhere it is "
                    "counted as `foreign`, never reported missing. Flags, URLs, "
                    "home and absolute paths, `.`/`..` segments, placeholders, "
                    "globs and words with no '/' are not paths, and "
                    "unbackticked prose is not scanned. Emits path_tokens[] "
                    "({line, token, status ok | missing, path when ok}; "
                    "only=\"stale\" keeps the missing ones), path_token_counts "
                    "and path_tokens_checked. Its `foreign` and `skipped` (a "
                    "loop-log row's token) are overlay keys outside "
                    "path_tokens_checked. "
                    "Existence is a stat; nothing is read. Read-only. "
                    "caller_cwd required.");
                docCit["selection_hint"] = QStringLiteral(
                    "Use when reviewing a doc full of file:line references, to check every "
                    "one still points where it claims — instead of opening each file by "
                    "hand.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    schema["additionalProperties"] = false;
                    schema["required"] = QJsonArray{QStringLiteral("caller_cwd"),
                                                    QStringLiteral("path")};
                    QJsonObject props;
                    QJsonObject dcPath; dcPath["type"] = "string";
                        dcPath["description"] = QStringLiteral(
                            "Project-relative path to a single FILE to scan (a directory "
                            "refuses bad_args). Any UTF-8 text file, not only .md.");
                    QJsonObject dcOnly; dcOnly["type"] = "string";
                        dcOnly["enum"] = QJsonArray{QStringLiteral("all"),
                                                    QStringLiteral("stale")};
                        dcOnly["description"] = QStringLiteral(
                            "Filter over citations[] only: \"stale\" keeps the ones that did "
                            "not resolve cleanly. counts stay whole-doc. Default all.");
                    QJsonObject dcOffset; dcOffset["type"] = "integer";
                        dcOffset["description"] = QStringLiteral(
                            "0-based index into the post-filter list. next_offset in the "
                            "response resumes the page when a cap dropped entries.");
                    QJsonObject dcRange; dcRange["type"] = "integer";
                        dcRange["description"] = QStringLiteral(
                            "Max lines of text per citation (1-20, default 3). A longer cited "
                            "range sets range_truncated; end_line still echoes the doc.");
                    QJsonObject dcBytes; dcBytes["type"] = "integer";
                        dcBytes["description"] = QStringLiteral(
                            "Response byte budget (64 KiB - 4 MiB, default 128 KiB). A soft "
                            "ceiling: one entry always ships so paging cannot stall.");
                    QJsonObject dcDocLines; dcDocLines["type"] = "integer";
                        dcDocLines["description"] = QStringLiteral(
                            "Lines of the doc to scan (1000-50000, default 20000). "
                            "scanned_lines < doc_lines says the tail was not looked at.");
                    props["path"]            = dcPath;
                    props["only"]            = dcOnly;
                    props["offset"]          = dcOffset;
                    props["max_range_lines"] = dcRange;
                    props["max_bytes"]       = dcBytes;
                    props["max_doc_lines"]   = dcDocLines;
                    {   // ANTS-4386 — quotation check.
                        QJsonObject q; q["type"] = "boolean";
                                       q["default"] = false;
                                       q["description"] = QStringLiteral(
                            "Optional (ANTS-4386). Also check QUOTATIONS: a "
                            "double-quoted span of at least `quote_min_chars` "
                            "characters, resolved against the document "
                            "attributed to it by a backticked path nearby. "
                            "Emits `quotes[]` "
                            "({line, text, target, status, candidates}) with "
                            "status ok | not_found | ambiguous | no_target | "
                            "target_unresolved, plus `quote_counts` and "
                            "`quotes_checked` — a "
                            "zero says the pass ran and found nothing to "
                            "check, not that every quotation is sound. "
                            "Matching folds runs of whitespace INCLUDING "
                            "NEWLINES on both sides: a quotation in a "
                            "hard-wrapped document does not survive a "
                            "line-oriented search, and reporting \"not "
                            "found\" for a phrase that is present is worse "
                            "than not checking — it makes a reviewer \"fix\" "
                            "a passage that was already correct. An "
                            "attribution naming several documents is "
                            "`ambiguous` WITH the hit list, never a "
                            "guess. A quotation inside a fence is a specimen "
                            "and is skipped. ANTS-4637 — so is one sitting in "
                            "a LOOP-LOG row (first cell a loop number, second "
                            "an ISO date): such a row quotes the wording a fix "
                            "REPLACED, so not_found is correct and permanent "
                            "there, and 271 of 520 unfenced quotations in one "
                            "measured corpus were of that class. The skip is "
                            "REPORTED as `quote_counts.skipped`, which sits "
                            "outside the status partition and outside "
                            "`quotes_checked` — `quote_counts_overlay_keys` "
                            "names it, so assert sum(quote_counts[k] for k not "
                            "in that list) == quotes_checked. ANTS-4638 — "
                            "attribution is PARAGRAPH-scoped: same line first, "
                            "then the nearest preceding line of the same "
                            "blank-line-delimited paragraph, because a "
                            "hard-wrapped sentence puts the path and its "
                            "quotation on adjacent lines. ANTS-4640 — inside a "
                            "TABLE the scope is the ROW: the quotation's own "
                            "cell, else the row's FIRST cell, which is its "
                            "subject by convention. An impact table names the "
                            "source first and the affected module second, so "
                            "last-path-wins picked the wrong one by "
                            "construction. ANTS-4639 — a token becomes a "
                            "`target` only if it names a document (.md, "
                            ".markdown, .txt, .rst, .adoc, .org), so a "
                            "backticked config key like `core.hooksPath` no "
                            "longer does; a bare basename resolves against the "
                            "scanned document's OWN directory before the repo "
                            "root and the basename index; and an attribution "
                            "that PARSED but did not resolve is "
                            "`target_unresolved`, never `no_target` — those "
                            "mean the check did not run versus the check ran "
                            "and the quotation is stale. ANTS-4696 — "
                            "attribution STOPS at a path-shaped token that "
                            "names no document extension (`docs/decisions/"
                            "ADR-0003`, an ADR cited by convention) instead of "
                            "walking past it to an older document in the same "
                            "paragraph: that fallthrough reported the "
                            "quotation `not_found` in a file never claimed as "
                            "its source, and not_found is the actionable "
                            "status, so its natural repair was to edit a "
                            "correct passage. Such a quotation is `no_target` "
                            "and carries `rejected_target` naming the token "
                            "plus a `hint`; the repair is usually adding the "
                            "extension. A token with no '/' is not "
                            "path-shaped, so ANTS-4639's config key is "
                            "unaffected. Off by default.");
                        props["quotes"] = q;
                        QJsonObject qm; qm["type"] = "integer";
                                        qm["default"] = 30;
                                        qm["minimum"] = 10;
                                        qm["maximum"] = 500;
                                        qm["description"] = QStringLiteral(
                            "Optional (ANTS-4386). Minimum length for a "
                            "double-quoted span to count as a quotation. "
                            "Keeps the check off ordinary quoted words.");
                        props["quote_min_chars"] = qm;
                    }
                    {   // ANTS-5296 — path-token check.
                        QJsonObject pt; pt["type"] = "boolean";
                                        pt["default"] = false;
                                        pt["description"] = QStringLiteral(
                            "Also check bare backticked paths exist, where their "
                            "first segment is a real directory here. Emits "
                            "path_tokens[]. Off by default.");
                        props["paths"] = pt;
                    }
                    props["caller_cwd"]      = makeCallerCwdReadProp();
                    props["etag_match"]      = makeEtagMatchProp();   // ANTS-1499
                    props["encoding"]        = makeEncodingProp();    // ANTS-2090
                    schema["properties"] = props;
                    docCit["inputSchema"] = schema;
                }
                tools.append(docCit);

                // ANTS-2161 — project_settings: detect a misplaced layout +
                // create/update <root>/.ants/project.json so a non-src/
                // project (e.g. code under linuxdoom-1.10/) stops getting an
                // empty codebase_index. Companion to the read-side ANTS-2160.
                QJsonObject psTool;
                psTool["name"] = "project_settings";
                psTool["description"] = QStringLiteral(
                    "Read, detect or write the repo-committed "
                    "<root>/.ants/project.json layout file. op:\"get\" reads "
                    "{present, declared, undeclared[], unavailable[], "
                    "declared_missing[]}, arrays always present, empty when none. "
                    "op:\"detect\" also proposes keys in "
                    "`suggestion` (source_roots, test_roots, docs_dir, specs_dir, "
                    "roadmap, changelog). op:\"init\" writes the detected keys. "
                    "op:\"set\" creates or updates keys (null clears), including "
                    "id_format {prefix?, pattern?} and repo_root. Refusals: bad_args, bad_path, "
                    "settings_exists, unrecognised_format, id_format_mismatch. "
                    "caller_cwd required.");
                psTool["detail"] = QStringLiteral(
                    "Detect a non-standard project layout and create/update the "
                    "repo-committed <root>/.ants/project.json (the ANTS-2160 "
                    "reader's source). ANTS-4903 — op:\"get\" is the READ: "
                    "{present, declared, undeclared[], unavailable[], "
                    "declared_missing[], op:\"get\"} and NO `suggestion`. It "
                    "is the op to reach for at orientation, when the question "
                    "is \"what does this project declare, and what is still "
                    "unset?\" — op:\"detect\" answers a different question "
                    "(what SHOULD it declare), and routing the read through a "
                    "verb whose name says it proposes made a caller reason "
                    "about whether the call was safe before trusting it, with "
                    "an inert `suggestion` block on an already-declared "
                    "project as the reward. Both are read-only and share one "
                    "implementation, so they cannot disagree about what is "
                    "declared. op:\"detect\" (read-only) → {present, "
                    "suggestion:{source_roots?, reason, counts_computed, "
                    "default_source_count?, total_source_count?}} — suggests "
                    "source_roots when the "
                    "default src/+tests/ walk would miss most of the repo's "
                    "code, and (ANTS-3705) echoes the CURRENT declaration as "
                    "`declared` plus `declared_missing[]` naming any declared "
                    "path that no longer resolves — so reading "
                    ".ants/project.json by hand is never needed. A missing "
                    "entry is DROPPED by the reader, which is otherwise "
                    "indistinguishable from never having been declared. "
                    "ANTS-4093 — detect suggests ALL SIX recognised keys, not "
                    "just source_roots: test_roots / docs_dir / specs_dir / "
                    "roadmap / changelog are proposed from their conventional "
                    "paths whenever those resolve. ANTS-4815 — the recognised "
                    "keys with no declaration are split by whether you can ACT "
                    "on them: `undeclared[]` is a to-do list (a candidate path "
                    "is on disk, so op:set will accept it), while "
                    "`unavailable[]` is not (nothing to point at, and op:set "
                    "refuses bad_path for a path that is not there). Before "
                    "the split, a project that legitimately keeps no specs had "
                    "a permanently non-empty `undeclared[]` it could only "
                    "clear by creating directories it did not want. A reason of "
                    "\"no source_roots override needed\" is about source_roots "
                    "ALONE — it does not mean there is nothing to configure. "
                    "ANTS-4092 — gitignored directories are excluded from the "
                    "walk before it counts them (one `git check-ignore`, "
                    "matching workspace_search's respect_gitignore default) "
                    "and are named in `excluded[]` alongside the vendored / "
                    "build dirs; a repo-less or git-less root walks as before. "
                    "ANTS-4648 — `counts_computed` says whether the walk ran "
                    "at all: on present:true it short-circuits before walking, "
                    "so `default_source_count` / `total_source_count` are "
                    "OMITTED rather than reported as 0, which read as a "
                    "measurement (a ~3000-line project reported as holding no "
                    "source). Absent = not taken; read `declared`, "
                    "`declared_missing[]` and `undeclared[]` for that path. "
                    "op:\"init\" → write the detected (or explicit) keys; "
                    "refuses settings_exists if the file is present (no "
                    "clobber); writes nothing (written:false) when there's "
                    "nothing to suggest. op:\"set\" → create-or-update any key "
                    "(merges into an existing file, preserving keys it doesn't "
                    "touch; a null value clears a key); refuses bad_args when no "
                    "key is supplied, unrecognised_format on a malformed "
                    "existing file. Declared paths are validated under the root "
                    "(bad_path) and the file is written world-readable. "
                    "ANTS-3771 — op:\"set\" also takes `id_format`, the "
                    "project's own ID grammar and the one non-path key: "
                    "{prefix?, pattern?}. `prefix` decides what an allocation "
                    "renders and refuses a written id whose prefix disagrees "
                    "(id_format_mismatch); `pattern` (PCRE2, <=512 bytes) says "
                    "which text of a GFM bold lead-in IS the id, so "
                    "`**AX1. Some headline**` reports `AX1` instead of the whole "
                    "sentence. Both optional and independent; an invalid one "
                    "refuses bad_args at write time and is DROPPED at read time. "
                    "op:\"detect\" cannot suggest it — a grammar is not a path "
                    "on disk. caller_cwd required.");
                psTool["selection_hint"] = QStringLiteral(
                    "Use op:get to read what this project declares; op:detect "
                    "when codebase_index comes back near-empty; op:init to "
                    "accept the suggestion, op:set to declare "
                    "source_roots/docs_dir/roadmap explicitly.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    schema["additionalProperties"] = false;
                    QJsonObject props;
                    QJsonObject opProp; opProp["type"] = "string";
                        opProp["enum"] = QJsonArray{QStringLiteral("get"),
                            QStringLiteral("detect"),
                            QStringLiteral("init"), QStringLiteral("set")};
                        opProp["description"] = QStringLiteral(
                            "get (the READ: what IS declared, and what is "
                            "still unset — no proposal, ANTS-4903) | detect "
                            "(what SHOULD be declared; also read-only) | init "
                            "(create, no clobber) | set (create-or-update). "
                            "Required.");
                    const auto dirArrayProp = []() {
                        QJsonObject p; p["type"] = "array";
                        QJsonObject items; items["type"] = "string"; p["items"] = items;
                        return p;
                    };
                    QJsonObject srProp = dirArrayProp();
                        srProp["description"] = QStringLiteral(
                            "Source root dirs (repo-relative) for init/set; "
                            "replaces the src/ default. Must exist.");
                    QJsonObject trProp = dirArrayProp();
                        trProp["description"] = QStringLiteral(
                            "Test root dirs (repo-relative); replaces tests/.");
                    QJsonObject ddProp; ddProp["type"] = "string";
                        ddProp["description"] = QStringLiteral("Docs dir (repo-relative).");
                    QJsonObject sdProp; sdProp["type"] = "string";
                        sdProp["description"] = QStringLiteral("Specs dir (repo-relative).");
                    QJsonObject rmProp; rmProp["type"] = "string";
                        rmProp["description"] = QStringLiteral("Roadmap file (repo-relative).");
                    QJsonObject clProp; clProp["type"] = "string";
                        clProp["description"] = QStringLiteral("Changelog file (repo-relative).");
                    // ANTS-3771 — the one non-path key. additionalProperties is
                    // false so an unknown member is client-rejected before the
                    // verb sees it, which matches applyWrite()'s own refusal.
                    QJsonObject ifProp; ifProp["type"] = "object";
                        ifProp["additionalProperties"] = false;
                        QJsonObject ifProps;
                        QJsonObject ifPfx; ifPfx["type"] = "string";
                            ifPfx["description"] = QStringLiteral(
                                "Canonical ID prefix (1-16 chars of "
                                "[A-Za-z0-9_-], at least one letter, e.g. ANTS "
                                "or 3D_E). Beats the store's id_prefix row and "
                                "the markdown sniff; an explicit id_prefix "
                                "argument on roadmap_log still wins.");
                        QJsonObject ifPat; ifPat["type"] = "string";
                            ifPat["description"] = QStringLiteral(
                                "PCRE2, <=512 bytes, matched against a GFM bold "
                                "lead-in with its trailing '.' already dropped. "
                                "Capture group 1 is the id (the whole match when "
                                "there is no group); the text it did not consume "
                                "is the headline. A NON-match changes nothing — "
                                "the bullet keeps the id it has today.");
                        ifProps["prefix"] = ifPfx;
                        ifProps["pattern"] = ifPat;
                        ifProp["properties"] = ifProps;
                        ifProp["description"] = QStringLiteral(
                            "The project's own ID grammar (op:set). Both members "
                            "optional and independent.");
                    props["op"]           = opProp;
                    props["source_roots"] = srProp;
                    props["test_roots"]   = trProp;
                    props["docs_dir"]     = ddProp;
                    props["specs_dir"]    = sdProp;
                    props["roadmap"]      = rmProp;
                    props["changelog"]    = clProp;
                    props["id_format"]    = ifProp;              // ANTS-3771
                    // ANTS-5667 — set-only, like id_format: detect never
                    // proposes it.
                    QJsonObject rrProp; rrProp["type"] = "string";
                        rrProp["description"] = QStringLiteral(
                            "op:set only. For an outer folder that is not a git "
                            "repository: the child folder holding its clone "
                            "(e.g. \"hells-gate-recomp\"). The roadmap, id_format "
                            "and mailbox are then the clone's; the other keys "
                            "stay relative to this folder. Must be an existing "
                            "directory inside it, else bad_path.");
                    props["repo_root"]    = rrProp;
                    // ANTS-4890 — set-only, a project's spec convention.
                    QJsonObject tccProp; tccProp["type"] = "boolean";
                        tccProp["description"] = QStringLiteral(
                            "op:set only. true: an invariant's *Test:* clause "
                            "counts as coverage, so spec_lint's test_coverage_gap "
                            "skips it.");
                    props["spec_test_clause_coverage"] = tccProp;
                    // ANTS-5728 — set-only, the project's own choice.
                    QJsonObject ccProp; ccProp["type"] = "boolean";
                        ccProp["description"] = QStringLiteral(
                            "op:set only. true: Ants starts this project's Claude "
                            "Code sessions with --chrome. Read at each launch.");
                    props["claude_chrome"] = ccProp;
                    // ANTS-4978 — set-only, the reason behind the settings.
                    QJsonObject noteProp; noteProp["type"] = "string";
                        noteProp["description"] = QStringLiteral(
                            "op:set only. Why the settings are as they are; "
                            "stored as `_comment`, echoed by get as `note`. "
                            "\"\" clears it.");
                    props["note"] = noteProp;
                    props["dry_run"]      = makeDryRunProp();    // ANTS-2227
                    props["caller_cwd"]   = makeCallerCwdReadProp();
                    schema["properties"]  = props;
                    schema["required"]    = QJsonArray{QStringLiteral("op")};
                    psTool["inputSchema"] = schema;
                }
                tools.append(psTool);

#ifdef ANTS_LUA_PLUGINS
                // ANTS-2093 — project_query: run an agent-supplied READ-ONLY
                // Lua snippet server-side and return ONLY its result (the
                // code-execution token-saver). Listed only in ANTS_LUA_PLUGINS
                // builds — the verb is unregistered without the Lua subsystem.
                {
                    QJsonObject pqTool;
                    pqTool["name"] = "project_query";
                    pqTool["description"] = QStringLiteral(
                        "Run a small READ-ONLY Lua snippet over the project's files "
                        "server-side and get back only its computed result, for "
                        "aggregate questions (count TODOs, which files import X). API: "
                        "project.list(subdir?), project.read(relpath), project.root(), "
                        "plus string/table/math/utf8. The snippet must `return` its "
                        "answer. Sandboxed to caller_cwd; memory, time and output "
                        "capped. Returns {ok, result, elapsed_ms}. Refusals: "
                        "query_error, query_timeout, query_oom, result_too_large, "
                        "query_disabled. caller_cwd required.");
                    pqTool["detail"] = QStringLiteral(
                        "Run a small READ-ONLY Lua snippet over the project's "
                        "files server-side and get back ONLY its computed "
                        "result — not the file text. The token-saver for "
                        "\"compute an aggregate across files\" questions (count "
                        "TODOs, which files import X, sum sizes): the snippet "
                        "greps/filters/counts itself and you receive just the "
                        "number/list. API (the whole surface): project.list("
                        "subdir?)->array of project-relative file paths "
                        "(sorted, skips .git/) — the Glob replacement; "
                        "project.read(relpath)->string, "
                        "project.root()->string; plus the string/table/math/utf8 "
                        "stdlib. The snippet MUST `return` a value (the answer). "
                        "Sandboxed: read-only, confined to caller_cwd (no "
                        "escape, no write, no network, no os/io), 10 MiB + "
                        "wall-clock + output-size capped. NOT a workspace_search "
                        "replacement — scope to a known file set (or a "
                        "workspace_search result) then aggregate, don't pull the "
                        "whole tree through project.read. Success {ok, result, "
                        "elapsed_ms}; refusals query_error (bad/erroring snippet "
                        "or path escape), query_timeout, query_oom, "
                        "result_too_large (aggregate harder), query_disabled "
                        "(feature off). caller_cwd required.");
                    pqTool["selection_hint"] = QStringLiteral(
                        "Use when you'd otherwise Read several files to "
                        "count/filter/aggregate over them, or Glob to list "
                        "files by name: project.list enumerates paths "
                        "server-side. Ship the logic as a Lua snippet, get "
                        "back only the answer.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    schema["additionalProperties"] = false;
                    QJsonObject props;
                    QJsonObject codeProp;
                    codeProp["type"] = "string";
                    codeProp["description"] = QStringLiteral(
                        "The read-only Lua snippet. Must `return` a value. "
                        "Has project.read/list/root + string/table/math/utf8.");
                    props["code"]       = codeProp;
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    schema["properties"] = props;
                    schema["required"]   = QJsonArray{QStringLiteral("code"),
                                                      QStringLiteral("caller_cwd")};
                    pqTool["inputSchema"] = schema;
                    tools.append(pqTool);
                }
#endif

                // ANTS-1961 — feedback_query: read the un-triaged tail of
                // a *_Ants_MCP_Feedback.md file instead of a full Read.
                {
                    QJsonObject t;
                    t["name"] = "feedback_query";
                    t["description"] = QStringLiteral(
                        "Read only the UN-TRIAGED tail of a *_Ants_MCP_Feedback.md "
                        "file: on v2, findings with an unfilled **Proposed ID:**; on "
                        "v1, blocks after the last maintainer table. Returns {ok, path, "
                        "delta, delta_present, mapped_ids, mapped_id_status, "
                        "format_version, suspected_untagged, truncated, etag, ...}. "
                        "`path` is optional. include_tracking:true adds tracking rows. "
                        "Refusals: bad_args, not_feedback_file, bad_path, not_found.");
                    t["detail"] = QStringLiteral(
                        "Read only the UN-TRIAGED tail of a cross-session "
                        "*_Ants_MCP_Feedback.md file instead of Read-ing the "
                        "whole file. The un-triaged rule is VERSION-DEPENDENT "
                        "(ANTS-3448): on a v1 file the delta is everything a "
                        "contributor appended after the last maintainer tracking "
                        "block; on a v2 file (`<!-- ants-mcp-feedback: 2 -->` or "
                        "higher) it is the findings whose inline "
                        "`**Proposed ID:**` line is still unfilled (no id, no n/a "
                        "closure). Returns {ok, path, delta, delta_present, "
                        "delta_line_count, delta_start_line, mapped_ids, "
                        "mapped_id_status, format_version, suspected_untagged, "
                        "maintainer_block_count, last_maintainer_line, "
                        "truncated, etag}. `mapped_ids` are the assigned inline "
                        "`**Proposed ID:**` ids on a v2 file, or the ANTS-NNNN "
                        "ids cited in maintainer blocks on v1. `mapped_id_status` "
                        "(ANTS-3478; present only when mapped_ids is non-empty) "
                        "resolves each mapped id's LIVE status against the caller "
                        "project's ROADMAP.md — [{id, status, shipped_date?}] with "
                        "status one of 📋/🚧/✅/💭 or \"unknown\" (absent from the "
                        "live roadmap; may have archived — never silently ✅). "
                        "`mapped_id_parts` (ANTS-4079) maps a mapped id that was "
                        "split into parts to [{id, status}] for each part, so a "
                        "shipped parent is not read as all of its work shipped; "
                        "absent when no mapped id has parts. "
                        "`shipped_date` (ANTS-3504) is the fix's ship-date (the id's "
                        "last ROADMAP `Resolved` date), present ONLY on ✅ ids that "
                        "have one — compare it against session_orient "
                        "server_build.build_date to tell a shipped fix from a "
                        "stale binary before re-reporting — ANTS-4741 now does "
                        "that comparison for you: a shipped entry whose date is "
                        "not strictly older than this server binary's build "
                        "date carries `possibly_stale_binary:true` (ANTS-4843: "
                        "so does a shipped entry with no date, marked "
                        "`stale_check:\"no_shipped_date\"`), and the "
                        "envelope then also echoes `server_build_date`, "
                        "`server_build_commit` and a hint, so both operands sit "
                        "in one reply. A SAME-DAY ship sets the flag, because "
                        "`shipped_date` has no time component to compare "
                        "against the build time — and same-day is the case most "
                        "likely to be misread. `format_version` "
                        "is the detected marker version (0/1/2/…). ANTS-4702 "
                        "— a version ABOVE the highest this build defines "
                        "also sets `format_version_unrecognised:true` plus a "
                        "`format_version_hint`. The delta rule is \"v2 or "
                        "higher\", so such a file parses and every verb works; "
                        "the marker is still wrong, and without the flag the "
                        "widening is invisible until some verb gates on an "
                        "exact version and fails far from the file that caused "
                        "it. The delta is sound either way — this reports the "
                        "marker, not the content. "
                        "`suspected_untagged` (v2 only) lists {heading, line} for "
                        "`### ` finding-shaped blocks a hand editor left with no "
                        "`**Proposed ID:**` line (empty on v1 / a clean v2 file). "
                        "Under v2 `delta` is a concatenation of (possibly "
                        "non-contiguous) findings — treat it as opaque text, do "
                        "NOT re-slice the file from `delta_start_line`. "
                        "include_tracking:true (ANTS-3371) adds `tracking` "
                        "— every maintainer tracking-table row "
                        "[{item, ids, status, notes?}] in document order "
                        "(later rows supersede earlier ones for the same "
                        "id) — so a session sees which of its prior "
                        "suggestions shipped without hand-parsing the "
                        "tables. "
                        "Byte-capped (max_bytes, default 512 KiB, 4 MiB "
                        "ceiling) keeping the HEAD of the delta. `path` is "
                        "OPTIONAL (ANTS-3376): omit it and the conventional "
                        "<caller_cwd-leaf>_Ants_MCP_Feedback.md at the shared "
                        "root (the parent of caller_cwd) is derived — the "
                        "reply then carries path_derived:true. Refusals: "
                        "`bad_args` (no path AND no resolvable caller_cwd to "
                        "derive one), `not_feedback_file` (basename not "
                        "*_Ants_MCP_Feedback.md), `bad_path` (traversal), "
                        "`not_found` (an EXPLICIT `path` that does not "
                        "resolve). ANTS-4104 — a DERIVED default that does not "
                        "exist is NOT a refusal: \"nobody has filed anything "
                        "here yet\" is the state every project starts in, so it "
                        "returns ok:true with found:false, delta_present:false, "
                        "empty mapped_ids and a `reason`, keeping the "
                        "`candidates` + `hint` below. Every success carries "
                        "found:true, so one branch covers both. A `not_found` "
                        "envelope carries "
                        "`candidates` (sibling *_Ants_MCP_Feedback.md paths in "
                        "the same dir) + a `hint`; the caller's own file is "
                        "floated to the front, or `all_other_projects:true` "
                        "flags that every candidate belongs to a different "
                        "project (ANTS-3366/3376) — recover the right basename "
                        "without shelling out to `ls`. caller_cwd required.");
                    t["selection_hint"] = QStringLiteral(
                        "Use to pull just the new feedback from a shared "
                        "*_Ants_MCP_Feedback.md report file instead of "
                        "re-reading the whole thing each review.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    QJsonObject pathProp; pathProp["type"] = "string";
                        pathProp["description"] = QStringLiteral(
                            "Optional (ANTS-3376). Path to the "
                            "*_Ants_MCP_Feedback.md file. Absolute or "
                            "caller_cwd-relative. ANTS-4980 — this named a "
                            "corpus root holding no feedback file, and a "
                            "session that CONSTRUCTS an absolute path from a "
                            "wrong hint routes around ANTS-4647's guard, "
                            "which fires on the derivation route only, so "
                            "feedback_log would create a stranded file rather "
                            "than refuse. No literal root is given now: the "
                            "root is configurable "
                            "(claude.mcp_feedback_root), so any path written "
                            "here is a claim that goes stale on another "
                            "machine. PREFER DERIVATION — omit `path` and the "
                            "corpus file is resolved for you; "
                            "session_orient's feedback_pending block reports "
                            "the resolved `shared_root` if you need to see "
                            "it. Omit to derive "
                            "<caller_cwd-leaf>_Ants_MCP_Feedback.md at the "
                            "shared root (parent of caller_cwd).");
                    QJsonObject mbProp; mbProp["type"] = "integer";
                        mbProp["minimum"] = 1;
                        mbProp["description"] = QStringLiteral(
                            "Cap on the emitted `delta` bytes (default "
                            "512 KiB, server-clamped to 4 MiB). Keeps the "
                            "HEAD; sets truncated:true. delta_line_count "
                            "still reports the full count.");
                    QJsonObject itProp; itProp["type"] = "boolean";
                        itProp["description"] = QStringLiteral(
                            "Optional (ANTS-3371). When true, add a "
                            "`tracking` array of every maintainer "
                            "tracking-table row [{item, ids, status, "
                            "notes?}] in document order. Use for the "
                            "\"which of my prior suggestions shipped?\" "
                            "workflow without a full-file Read.");
                    props["path"]            = pathProp;
                    props["max_bytes"]       = mbProp;
                    props["include_tracking"] = itProp;
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    props["etag_match"] = makeEtagMatchProp();   // ANTS-1499
                    // ANTS-4665 — `delta` is the largest field here and grows
                    // with every un-triaged finding, while the commonest
                    // narrowing call is a duplicate check that wants
                    // mapped_ids and nothing else. So the cost was worst
                    // exactly when a file was busiest. NOT compacted by
                    // default (the row's second column is false): a dropped
                    // `delta_present:false` would be unreadable in the way
                    // ANTS-4673 documents.
                    props["fields"] = makeFieldsProp();          // ANTS-1720
                    props["compact"] = makeCompactProp();        // ANTS-4657
                    schema["properties"] = props;
                    // ANTS-3376 — `path` is now optional (derived from
                    // caller_cwd when omitted); only caller_cwd is required.
                    QJsonArray req;
                    req.append(QStringLiteral("caller_cwd"));
                    schema["required"] = req;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-1962 — feedback_log: append a contributor finding
                // block or a maintainer tracking block, rendered from
                // structured args (no hand-edited markdown).
                {
                    QJsonObject t;
                    t["name"] = "feedback_log";
                    t["description"] = QStringLiteral(
                        "Write to a cross-session *_Ants_MCP_Feedback.md finding. "
                        "The roadmap store holds the text; the file is a read-only "
                        "export. Contributor: op:\"append_finding\" (registers a new "
                        "file). Maintainer: op:\"assign_id\" (fill a finding's Proposed "
                        "ID), assign_id_batch, set_title, reimport. `path` is optional "
                        "and derived from caller_cwd's leaf. Returns {ok, op, path, "
                        "bytes_appended, date, created}. Refusals: bad_mode, bad_args, "
                        "retired, not_feedback_file, bad_path, not_found, not_v2, "
                        "not_utf8, export_drift, confirm_required, store_failed, "
                        "write_failed. caller_cwd required.");
                    t["detail"] = QStringLiteral(
                        "ANTS-4513: the feedback corpus lives in the roadmap "
                        "store. Each write takes the store's write lock, edits "
                        "the stored text, rebuilds its finding rows, and "
                        "rewrites the file as a read-only (0444) export, so two "
                        "sessions writing at once both land. A file with no "
                        "store row is imported on first touch; a file without "
                        "the `<!-- ants-mcp-feedback: 2 -->` marker refuses "
                        "`not_v2`, and one that is not plain UTF-8 refuses "
                        "`not_utf8`. A file edited by hand refuses every write "
                        "with `export_drift`: op:\"reimport\" with confirm:true "
                        "keeps the file's text, and deleting the file keeps the "
                        "store's. op:\"append_finding\" (contributor) appends a "
                        "dated session heading + one ### sub-block per finding "
                        "at the end, and registers a new file from the "
                        "skeleton. `path` is OPTIONAL (ANTS-3376): omit it and "
                        "the conventional <caller_cwd-leaf>_Ants_MCP_Feedback.md "
                        "at the shared root is derived (reply carries "
                        "path_derived:true). ANTS-4613: a leaf beginning with a "
                        "DOT (~/.claude) refuses `bad_args` with `candidates` "
                        "instead of deriving. ANTS-5500: unless the git origin "
                        "remote names the repository and exactly one candidate "
                        "matches it; then that file is used and "
                        "`path_derived_reason` says so. A maintainer op on a "
                        "path with neither a row nor a file refuses not_found "
                        "with `candidates` + `hint` (ANTS-3366). ANTS-3447: "
                        "op:\"assign_id\" fills ONE finding's \"**Proposed "
                        "ID:**\" line in place with the assigned ids, an n/a "
                        "closure or an awaiting question. ANTS-4646: "
                        "op:\"set_title\" rewrites the H1's project name after "
                        "a rename. The six ops that kept files tidy or served "
                        "format version 1 (compact_resolved, compact_shipped, "
                        "prune_tracking, append_tracking, migrate_v2, "
                        "set_format_version) refuse `retired`. caller_cwd "
                        "required.");
                    t["selection_hint"] = QStringLiteral(
                        "Use to add or triage feedback in a shared "
                        "*_Ants_MCP_Feedback.md file instead of hand-editing "
                        "markdown.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    QJsonObject pathProp; pathProp["type"] = "string";
                        pathProp["description"] = QStringLiteral(
                            "Optional (ANTS-3376). Path to the "
                            "*_Ants_MCP_Feedback.md file (absolute or "
                            "caller_cwd-relative). Omit to derive "
                            "<caller_cwd-leaf>_Ants_MCP_Feedback.md at the "
                            "shared root (parent of caller_cwd). ANTS-4647 — "
                            "that assumes caller_cwd sits DIRECTLY under the "
                            "shared root. For a NESTED project (a repo inside "
                            "another workspace) the parent is not the corpus, "
                            "and the derived file would land where no "
                            "maintainer sweep globs it — so when the parent "
                            "holds no *_Ants_MCP_Feedback.md and an ancestor "
                            "does, the verb refuses `bad_args` with "
                            "`candidates` from that ancestor rather than "
                            "creating a stranded file. A configured "
                            "`claude.mcp_feedback_root` (ANTS-4471) is "
                            "consulted first and REDIRECTS the derivation "
                            "there instead of refusing. With no corpus "
                            "anywhere above, this is the first file and it is "
                            "created as before.");
                    // ANTS-4646 — op:"set_title" only.
                    QJsonObject titleProp; titleProp["type"] = "string";
                        titleProp["description"] = QStringLiteral(
                            "op:\"set_title\" only. The PROJECT NAME, not the "
                            "whole heading: the existing H1 prefix is preserved "
                            "verbatim (the corpus carries both \"Ants MCP "
                            "feedback\" and \"Ants MCP Feedback\", and this "
                            "renames a project rather than normalising a "
                            "corpus), and only the text after the last em dash "
                            "is replaced. `changed:false` on an already-correct "
                            "title is a SUCCESS, so a rename is re-runnable; "
                            "refuses `no_h1` rather than inventing a heading "
                            "the format fixes the position of.");
                    QJsonObject opProp; opProp["type"] = "string";
                        { QJsonArray e; e.append(QStringLiteral("append_finding"));
                          e.append(QStringLiteral("assign_id"));
                          e.append(QStringLiteral("assign_id_batch"));  // ANTS-4671
                          e.append(QStringLiteral("set_title"));   // ANTS-4646
                          e.append(QStringLiteral("reimport"));    // ANTS-4513
                          opProp["enum"] = e; }
                        opProp["description"] = QStringLiteral(
                            "Required. \"append_finding\" (contributor). "
                            "ANTS-3447 — \"assign_id\" (maintainer) fills ONE "
                            "`### ` finding's `**Proposed ID:**` line: pass "
                            "`heading` (the verbatim `### ` line, + optional "
                            "`heading_line` to disambiguate a repeat) and "
                            "exactly one of `ids` (an ANTS-NNNN array → "
                            "comma-joined), `closure` (a reason string → "
                            "`n/a — <reason>`, or bare `n/a` when empty) or "
                            "`awaiting`. Replaces the existing / placeholder id "
                            "line, or inserts one when absent. Reports "
                            "`value`/`inserted`/`changed`/`bytes_delta`; "
                            "idempotent (a re-assign of the same value is a "
                            "no-op); `dry_run` previews. Refuses "
                            "`target_not_found` / `target_ambiguous` (+ "
                            "`candidates`). \"assign_id_batch\" applies "
                            "`assignments` (assign_id argument objects) in one "
                            "write; failures land in skipped[]. \"set_title\" "
                            "replaces the H1's project name with `title`. "
                            "ANTS-4513 — \"reimport\" settles `export_drift` by "
                            "keeping the hand-edited file's text; it needs "
                            "`confirm:true`, and without it refuses "
                            "`confirm_required` with `store_bytes` and "
                            "`file_bytes`.");
                    QJsonObject dateProp; dateProp["type"] = "string";
                        dateProp["description"] = QStringLiteral(
                            "Optional YYYY-MM-DD; defaults to today.");
                    QJsonObject labelProp; labelProp["type"] = "string";
                        labelProp["description"] = QStringLiteral(
                            "Optional session label (append_finding "
                            "heading suffix).");
                    QJsonObject hlProp; hlProp["type"] = "string";
                        { QJsonArray e; e.append(QStringLiteral("h1"));
                          e.append(QStringLiteral("h2")); hlProp["enum"] = e; }
                        hlProp["description"] = QStringLiteral(
                            "Optional finding heading level, default h2.");
                    QJsonObject noteProp; noteProp["type"] = "string";
                        noteProp["description"] = QStringLiteral(
                            "Optional prose under the heading (append_finding). "
                            "ANTS-3571 — on assign_id it is "
                            "written as a single `- **Note:**` bullet under the "
                            "finding's Proposed ID line (newlines folded to "
                            "spaces; re-assign replaces it in place).");
                    QJsonObject findingsProp; findingsProp["type"] = "array";
                        { QJsonObject it; it["type"] = "object";
                          QJsonObject fp;
                          QJsonObject s; s["type"] = "string";
                          fp["title"] = s; fp["what"] = s; fp["repro"] = s;
                          fp["impact"] = s; fp["suggested_fix"] = s;
                          it["properties"] = fp;
                          findingsProp["items"] = it; }
                        findingsProp["description"] = QStringLiteral(
                            "append_finding: ≥1 finding. Each: title "
                            "(required) + optional what/repro/impact/"
                            "suggested_fix.");
                    // ANTS-4513 — op:"reimport" only.
                    QJsonObject confirmProp; confirmProp["type"] = "boolean";
                        confirmProp["default"] = false;
                        confirmProp["description"] = QStringLiteral(
                            "op:\"reimport\" only: replace the store's text with "
                            "the hand-edited file's. Without it the op refuses "
                            "`confirm_required`.");
                    props["path"]          = pathProp;
                    props["op"]            = opProp;
                    props["title"]         = titleProp;           // ANTS-4646
                    props["date"]          = dateProp;
                    props["session_label"] = labelProp;
                    props["heading_level"] = hlProp;
                    props["note"]          = noteProp;
                    props["findings"]      = findingsProp;
                    props["confirm"]       = confirmProp;         // ANTS-4513
                    // ANTS-3447 — assign_id top-level fields.
                    QJsonObject headingProp; headingProp["type"] = "string";
                        headingProp["description"] = QStringLiteral(
                            "assign_id (ANTS-3447): the target finding's verbatim "
                            "`### ` heading line, including the `### ` prefix "
                            "(trimmed-matched against the finding headings).");
                    QJsonObject headingLineProp; headingLineProp["type"] = "integer";
                        headingLineProp["description"] = QStringLiteral(
                            "assign_id: optional 1-based line of the `### ` "
                            "heading — disambiguates a repeated heading "
                            "(target_ambiguous otherwise).");
                    QJsonObject idsProp; idsProp["type"] = "array";
                        { QJsonObject idIt; idIt["type"] = "string";
                          idsProp["items"] = idIt; }
                        idsProp["description"] = QStringLiteral(
                            "assign_id: the ANTS-NNNN ids to assign (each "
                            "^ANTS-[0-9]+$; rendered comma-joined, de-duplicated). "
                            "Supply EXACTLY ONE of ids / closure / awaiting.");
                    QJsonObject closureProp; closureProp["type"] = "string";
                        closureProp["description"] = QStringLiteral(
                            "assign_id: a closure reason → writes "
                            "`n/a — <reason>` (empty string → bare `n/a`). "
                            "Supply EXACTLY ONE of ids / closure / awaiting.");
                    props["heading"]       = headingProp;         // ANTS-3447
                    props["heading_line"]  = headingLineProp;     // ANTS-3447
                    props["ids"]           = idsProp;             // ANTS-3447
                    // ANTS-3631 — the third disposition. A QUESTION for the
                    // reporting session, rendered `_(awaiting reporter —
                    // <question>)_` and deliberately UN-TRIAGED, so the
                    // finding stays in that session's feedback_query delta
                    // carrying the question. Every other way of attaching one
                    // fills the slot, which is what removes the finding from
                    // the one surface its reader looks at.
                    QJsonObject awaitingProp; awaitingProp["type"] = "string";
                        awaitingProp["description"] = QStringLiteral(
                            "assign_id: a question for the reporting session "
                            "→ writes `_(awaiting reporter — <question>)_`. "
                            "UN-TRIAGED on purpose: the finding stays in their "
                            "feedback_query delta so they see the question, and "
                            "it is classified BEFORE the id and closure tests, "
                            "so a question quoting an id ('same as ANTS-1234?') "
                            "is not mistaken for an assignment. It contributes "
                            "no ids to mapped_ids. The reporter answers by "
                            "appending a new finding whose heading quotes the "
                            "original — never by editing the slot; you clear "
                            "the marker when you triage their answer. Supply "
                            "EXACTLY ONE of ids / closure / awaiting.");
                    props["closure"]       = closureProp;         // ANTS-3447
                    // ANTS-4671 — op:"assign_id_batch" operand.
                    QJsonObject assignmentsProp;
                    assignmentsProp["type"] = QStringLiteral("array");
                    { QJsonObject it; it["type"] = QStringLiteral("object");
                      assignmentsProp["items"] = it; }
                    assignmentsProp["description"] = QStringLiteral(
                        "op:\"assign_id_batch\" — the assignments to write "
                        "in ONE read and ONE atomic write. Each element "
                        "takes this op's existing per-call arguments "
                        "UNCHANGED: `heading` (required, matched verbatim), "
                        "optional `heading_line`, exactly one of `ids` / "
                        "`closure` / `awaiting`, and an optional `note`. So "
                        "there is no second argument grammar to learn — it "
                        "is assign_id with the read and write hoisted out of "
                        "the loop. A triage is inherently a batch: findings "
                        "are read together via feedback_query and decided "
                        "together, and nothing between the calls can change "
                        "the file. Per-assignment failures land in "
                        "`skipped[]` with their `index` and cost only their "
                        "own assignment, which matters here because a "
                        "heading is matched verbatim and one mistyped "
                        "heading must not cost the batch its other "
                        "closures. An ALL-failed batch refuses rather than "
                        "reporting success with nothing applied. Each "
                        "assignment sees the previous one's result, so two "
                        "aimed at one finding behave as two separate calls "
                        "would.");
                    props["assignments"]   = assignmentsProp;    // ANTS-4671
                    props["awaiting"]      = awaitingProp;        // ANTS-3631
                    props["dry_run"]       = makeDryRunProp();   // ANTS-2227
                    props["caller_cwd"]    = makeCallerCwdReadProp();
                    schema["properties"] = props;
                    // ANTS-3376 — `path` is now optional (derived from
                    // caller_cwd when omitted); op + caller_cwd required.
                    QJsonArray req;
                    req.append(QStringLiteral("op"));
                    req.append(QStringLiteral("caller_cwd"));
                    schema["required"] = req;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-2129 — audit_falsepos_log: append a confirmed
                // false positive to the prose review ledger
                // .ants_review_falsepos.jsonl (write side of ANTS-1457).
                {
                    QJsonObject t;
                    t["name"] = "audit_falsepos_log";
                    t["description"] = QStringLiteral(
                        "Append one confirmed false-positive record to "
                        "<project>/.ants_review_falsepos.jsonl, the prose ledger the AI "
                        "review skills read so a re-run does not re-litigate a "
                        "dismissed finding. Atomic append; do not hand-write the file. "
                        "It does NOT suppress audit_run findings: a static-analysis "
                        "finding goes to audit_dismiss. Returns {ok, path, "
                        "bytes_appended, created, timestamp, review_kind, consumed_by, "
                        "hint}. Refusals: bad_args, no_project, write_failed. "
                        "caller_cwd required.");
                    t["detail"] = QStringLiteral(
                        "Append one confirmed false-positive record to "
                        "<project>/.ants_review_falsepos.jsonl — the prose "
                        "ledger that review-code and check-code read, and that "
                        "the cold_eyes_brief / test_audit_brief / "
                        "indie_review_brief verbs filter by review_kind, so a "
                        "re-run doesn't re-litigate a dismissed finding. "
                        "review-tests and review-contract do not read it "
                        "(ANTS-5595). Atomic "
                        "O_APPEND (safe under concurrent CC sessions — do "
                        "NOT hand-write with the Write tool). Trims claim/"
                        "rationale to the read caps and refuses a record "
                        "over 3.5 KiB. Creates an absent ledger (mode 0644). "
                        "ANTS-4105 — this ledger is READ BY THE AI-REVIEW "
                        "BRIEFS and does NOT suppress anything in audit_run: "
                        "the engine's suppressions:\"auto\" filters on the "
                        "FINGERPRINT ledger (.audit_cache/learned-fp.jsonl), "
                        "which `audit_dismiss` writes. So a static-analysis "
                        "TOOL finding goes to audit_dismiss; a reviewer's "
                        "prose claim goes here. The two are deliberately not "
                        "merged — their keys differ (a prose claim vs a "
                        "file+rule+message hash). The reply repeats this as "
                        "`consumed_by` + `hint`. "
                        "Returns {ok, path, bytes_appended, created, "
                        "timestamp, review_kind, consumed_by, hint}. "
                        "Refusals: `bad_args` "
                        "(empty claim/rationale, non-canonical review_kind, "
                        "bad timestamp, or over-size record), `no_project` "
                        "(caller_cwd unresolved), `write_failed` (I/O error "
                        "or non-regular ledger path). caller_cwd required.");
                    t["selection_hint"] = QStringLiteral(
                        // Kept under the ANTS-1453 240-char hint budget; the
                        // full ANTS-4105 routing lives in the description.
                        "Use after a sweep fold-in classifies a finding "
                        "FALSE_POSITIVE (user-confirmed). NOT for a "
                        "static-analysis tool finding — that goes to "
                        "audit_dismiss; this ledger feeds reviewer briefs and "
                        "suppresses nothing in audit_run.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    QJsonObject rkProp; rkProp["type"] = "string";
                        { QJsonArray e;
                          e.append(QStringLiteral("audit"));
                          e.append(QStringLiteral("cold-eyes"));
                          e.append(QStringLiteral("indie-review"));
                          e.append(QStringLiteral("test-audit"));
                          e.append(QStringLiteral("debt-sweep"));  // ANTS-3701
                          rkProp["enum"] = e; }
                        rkProp["description"] = QStringLiteral(
                            "Required. Which sweep dismissed it.");
                    QJsonObject claimProp; claimProp["type"] = "string";
                        claimProp["description"] = QStringLiteral(
                            "Required. One-line summary of the dismissed "
                            "claim (trimmed to 280 units).");
                    QJsonObject ratProp; ratProp["type"] = "string";
                        ratProp["description"] = QStringLiteral(
                            "Required. Why it's a false positive — cite the "
                            "file/line/system that makes it safe (trimmed to "
                            "1024 units). No secrets; flows into the brief.");
                    QJsonObject tsProp; tsProp["type"] = "string";
                        tsProp["description"] = QStringLiteral(
                            "Optional YYYY-MM-DD; defaults to today.");
                    QJsonObject laneProp; laneProp["type"] = "string";
                        laneProp["description"] = QStringLiteral(
                            "Optional lane/partition the finding belongs to "
                            "(empty = all lanes).");
                    QJsonObject topicProp; topicProp["type"] = "string";
                        topicProp["description"] = QStringLiteral(
                            "Optional short grouping tag "
                            "(e.g. rate-limit, unused-import).");
                    QJsonObject lbProp; lbProp["type"] = "string";
                        lbProp["description"] = QStringLiteral(
                            "Optional audit-trail tag "
                            "(user-confirmed / cc-session / external); "
                            "defaults to cc-session.");
                    props["review_kind"] = rkProp;
                    props["claim"]       = claimProp;
                    props["rationale"]   = ratProp;
                    props["timestamp"]   = tsProp;
                    props["lane"]        = laneProp;
                    props["topic"]       = topicProp;
                    props["logged_by"]   = lbProp;
                    props["dry_run"]     = makeDryRunProp();     // ANTS-2227
                    props["caller_cwd"]  = makeCallerCwdReadProp();
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append(QStringLiteral("review_kind"));
                    req.append(QStringLiteral("claim"));
                    req.append(QStringLiteral("rationale"));
                    req.append(QStringLiteral("caller_cwd"));
                    schema["required"] = req;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-1713 — audit_dismiss: record a learned-false-positive
                // verdict into the fingerprint ledger both audit_run and the
                // Audit dialog filter on. Deferred v2 of ANTS-1708.
                {
                    QJsonObject t;
                    t["name"] = "audit_dismiss";
                    t["description"] = QStringLiteral(
                        "Record learned false-positive verdicts in "
                        "<project>/.audit_cache/learned-fp.jsonl, the fingerprint "
                        "ledger audit_run and the Audit dialog both filter on, so a "
                        "finding stays suppressed on later sweeps. Pass `rule` plus "
                        "EITHER `fingerprint` OR `file`+`message`, or `entries[]` for "
                        "many. Re-dismissing changes nothing unless `replace_reason` "
                        "is set. Returns {ok, path, fingerprint, computed, rule, "
                        "reason, timestamp, outcome}; with entries {ok, path, "
                        "applied[], applied_count, skipped[], skipped_count, "
                        "changed}. Refusals: bad_args, no_project, write_failed. "
                        "caller_cwd required. Not audit_falsepos_log, which writes the "
                        "reviewers' prose ledger. Many files, one rule: "
                        ".audit_allowlist.json.");
                    t["detail"] = QStringLiteral(
                        "Record learned-false-positive verdicts into "
                        "<project>/.audit_cache/learned-fp.jsonl — the "
                        "fingerprint-keyed ledger BOTH `audit_run` and the "
                        "Audit dialog filter on, so a dismissed finding stays "
                        "suppressed on every later sweep (and survives edits "
                        "that shift its line: the fingerprint is "
                        "line-independent). Pass `rule` plus EITHER "
                        "`fingerprint` OR `file`+`message` (preferred — the "
                        "server hashes them the same way the engine will look "
                        "them up). Re-dismissing an already-recorded "
                        "fingerprint writes nothing and is not an error: "
                        "`outcome` says `already_recorded` and `reason` is the "
                        "stored one. `replace_reason:true` with a new `reason` "
                        "appends a row that supersedes it, keeping the stored "
                        "rule (`reason_replaced`). Returns {ok, path, "
                        "fingerprint, computed, rule, reason, timestamp, "
                        "outcome, hint?}. `entries[]` records many in one "
                        "write: each takes its own fingerprint or file+message, "
                        "and rule, reason and replace_reason fall back to the "
                        "call's. Returns {ok, path, applied[], applied_count, "
                        "skipped[], skipped_count, changed}; rows carry "
                        "`index`, and a bad entry costs only itself. "
                        "Refusals: `bad_args` (missing rule, or neither "
                        "fingerprint nor file+message, or a malformed "
                        "fingerprint, or replace_reason with no reason, or an "
                        "empty entries, or a finding's identity beside "
                        "entries, or a batch where every entry was refused), "
                        "`no_project` (caller_cwd unresolved), "
                        "`write_failed` (I/O error). caller_cwd required. "
                        "NOT the same as audit_falsepos_log, which writes the "
                        "prose ledger the review skills read. "
                        // ANTS-4966 — the class-wide form already exists, so
                        // point at it rather than add a second matcher.
                        "A false positive that is a property of the rule, "
                        "repeated across many files, needs no entry per site: "
                        "add ONE object to the \"allowlist\" array in "
                        "<project>/.audit_allowlist.json, "
                        "{\"rule\", \"path_glob\", \"line_regex\", \"reason\"}. "
                        "`path_glob` matches the file as the tool reports it "
                        "(`*` stays within a folder, `**` crosses folders); "
                        "`line_regex` matches the message. All three of rule, "
                        "path_glob and line_regex are required. audit_run and "
                        "the Audit dialog both apply it, and it also covers "
                        "files added later. This verb does not write that "
                        "file.");
                    t["selection_hint"] = QStringLiteral(
                        "Use when you have verified an audit finding is a "
                        "false positive and want future audits to stop "
                        "reporting it — previously only the Audit dialog "
                        "could teach the ledger.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    QJsonObject ruleProp; ruleProp["type"] = "string";
                        ruleProp["description"] = QStringLiteral(
                            "Required here or on each entry. The audit check "
                            "id the finding came from. For the line-based tools (cppcheck, clazy, "
                            "clang-tidy, mypy, shellcheck) this is the tool "
                            "name; JSON tools use their own check id.");
                    QJsonObject fileProp; fileProp["type"] = "string";
                        fileProp["description"] = QStringLiteral(
                            "The finding's file, as the tool spelled it. "
                            "Required unless `fingerprint` is given.");
                    QJsonObject msgProp; msgProp["type"] = "string";
                        msgProp["description"] = QStringLiteral(
                            "The finding's message. A leading "
                            "<path>:<line>[:<col>]: prefix is stripped before "
                            "hashing, so the verdict survives line shifts. "
                            "Required unless `fingerprint` is given.");
                    QJsonObject fpProp; fpProp["type"] = "string";
                        fpProp["description"] = QStringLiteral(
                            "Optional 16-lowercase-hex fingerprint, when you "
                            "already have it. Prefer file+message.");
                    QJsonObject reasonProp; reasonProp["type"] = "string";
                        reasonProp["description"] = QStringLiteral(
                            "Optional human note — why it isn't a real bug. "
                            "Recorded for the next reader; no secrets.");
                    QJsonObject replaceProp; replaceProp["type"] = "boolean";
                        replaceProp["description"] = QStringLiteral(
                            "Replace the reason already stored for this "
                            "finding. Needs a non-empty `reason`.");
                    props["rule"]           = ruleProp;
                    props["reason"]         = reasonProp;
                    props["replace_reason"] = replaceProp;
                    QJsonObject entryProps = props;
                    entryProps["file"]        = fileProp;
                    entryProps["message"]     = msgProp;
                    entryProps["fingerprint"] = fpProp;
                    QJsonObject entryItem;
                    entryItem["type"]       = "object";
                    entryItem["properties"] = entryProps;
                    QJsonObject entriesProp; entriesProp["type"] = "array";
                        entriesProp["items"] = entryItem;
                        entriesProp["description"] = QStringLiteral(
                            "Many findings in one write. Each takes fingerprint "
                            "or file+message; the rest fall back to the call's.");
                    props = entryProps;
                    props["entries"]     = entriesProp;
                    props["dry_run"]     = makeDryRunProp();
                    props["caller_cwd"]  = makeCallerCwdReadProp();
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append(QStringLiteral("caller_cwd"));
                    schema["required"] = req;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-1250: git_state — single tool, dispatches on
                // `op` (status / log / diff / rename_sweep / hooks).
                // Status, log and diff were collapsed from three
                // separate tools to save ~240 permanent schema tokens
                // per session-start (cold-eyes pass 2).
                QJsonObject gsTool;
                gsTool["name"] = "git_state";
                gsTool["description"] = QStringLiteral(
                    "Query git repo state as structured JSON instead of Bash git "
                    "status / log / diff. op: \"status\" (default; branch, "
                    "ahead/behind, files[] including untracked), \"log\" (n, default "
                    "10, cap 100; path; body), \"diff\" (path; range, omitted for "
                    "the working tree; hunks:true for per-file hunk headers, with "
                    "include_lines, context and staged; added_lines for only the "
                    "added lines), \"rename_sweep\" (where `old` still appears) "
                    "or \"hooks\" (push-gate wiring).");
                gsTool["detail"] = QStringLiteral(
                    "Query git repo state (status / log / diff) as "
                    "structured JSON. Replaces multiple Bash "
                    "invocations of `git status` / `git log` / "
                    "`git diff`. op (\"status\" / \"log\" / \"diff\") "
                    "defaults to \"status\" when omitted (ANTS-3365 — a "
                    "bare git_state{caller_cwd} returns the one-call "
                    "status+branch+ahead/behind read). Op-specific: n "
                    "(log only, default 10, "
                    "cap 100), path (log/diff filter), range (diff "
                    "only, e.g. HEAD~5..HEAD; omit for the working-tree "
                    "diff — ANTS-2074), body (log only, "
                    "include commit body). Saves ~14-300 tokens per "
                    "call vs Bash. op=\"status\" envelope (ANTS-1522): "
                    "`files[]` now includes untracked paths with "
                    "`index:\"?\"` + `worktree:\"?\"` for `git status "
                    "--porcelain` parity — one array, one shape. "
                    "`untracked[]` (DEPRECATED) is still emitted in "
                    "parallel. "
                    "op=\"diff\" (ANTS-3377): hunks=true emits per-file @@ "
                    "hunk headers {path, hunks:[{header, old_start, "
                    "old_count, new_start, new_count, lines?}]} for a clean "
                    "commit split (include_lines attaches hunk bodies, "
                    "context sets the -U width); staged=true diffs the index "
                    "vs HEAD. ANTS-5098: when git's output passes GitWrap's "
                    "stdout cap, status and diff set truncated:true and drop the "
                    "cut last line, so files[] and totals cover only what was "
                    "read. op=\"hooks\" (ANTS-5505) reports facts about the push "
                    "gate: core.hooksPath with its scope and origin, pre_push.kind "
                    "(none, machine_wide, hands_off or own), every ants.gate.* key "
                    "with its source, the gate script and the workflows calling "
                    "it, and whether HEAD's tree has a gate-passed record. It "
                    "gives no verdict; ~/.claude/tools/ci-gate does. ANTS-5508: "
                    "log and diff take paths[]; log grep keeps commits whose "
                    "message holds the text (any case) and stat adds each "
                    "commit's files[] {path, added, removed}; diff added_lines "
                    "returns files[{path, added:[{line, text}]}], narrowed by "
                    "added_match (case-sensitive), and refuses beside hunks; "
                    "op=\"rename_sweep\" lists hits[{path, line, text}] where "
                    "`old` still appears as a whole word in tracked files, at "
                    "range's last commit or in the working tree.");
                gsTool["selection_hint"] = QStringLiteral(
                    "Use for git status/log/diff in one structured "
                    "call (vs three Bash invocations). Pairs with "
                    "verify_changes after Edit/Write.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    QJsonObject opProp;     opProp["type"]    = "string";
                    QJsonArray opEnum;
                    opEnum.append("status");
                    opEnum.append("log");
                    opEnum.append("diff");
                    opEnum.append("rename_sweep");  // ANTS-5508
                    opEnum.append("hooks");         // ANTS-5505
                    opProp["enum"]    = opEnum;
                    opProp["default"] = "status";   // ANTS-3365
                    opProp["description"] = QStringLiteral(
                        "defaults to \"status\"; \"hooks\" reports hooksPath, the "
                        "pre-push hand-off, ants.gate.* keys, the gate script and its "
                        "passed record");
                    QJsonObject nProp;      nProp["type"]     = "integer";
                                            nProp["default"]  = 10;
                                            nProp["minimum"]  = 1;
                                            nProp["maximum"]  = 100;
                                            nProp["description"] =
                        QStringLiteral("log only");
                    QJsonObject pathProp;   pathProp["type"]  = "string";
                                            pathProp["description"] =
                        QStringLiteral("log/diff/rename_sweep: filter to "
                                       "repo-relative path");
                    QJsonObject rangeProp;  rangeProp["type"] = "string";
                                            rangeProp["description"] =
                        QStringLiteral("diff/rename_sweep: e.g. HEAD~5..HEAD. "
                                       "Omit for the working tree (diff: "
                                       "unstaged changes) — ANTS-2074.");
                    QJsonObject bodyProp;   bodyProp["type"]  = "boolean";
                                            bodyProp["default"] = false;
                                            bodyProp["description"] =
                        QStringLiteral("log: include commit body");
                    // ANTS-3377 — diff hunk-header mode + staged/index diff.
                    QJsonObject hunksProp;  hunksProp["type"] = "boolean";
                                            hunksProp["default"] = false;
                                            hunksProp["description"] =
                        QStringLiteral("diff: emit per-file @@ hunk headers "
                                       "{path, hunks:[{header, old_start, "
                                       "old_count, new_start, new_count, "
                                       "lines?}]} for a clean commit split, "
                                       "instead of the --numstat line counts");
                    QJsonObject stagedProp; stagedProp["type"] = "boolean";
                                            stagedProp["default"] = false;
                                            stagedProp["description"] =
                        QStringLiteral("diff: diff the index vs HEAD "
                                       "(git diff --cached); mutually "
                                       "exclusive with range");
                    QJsonObject incLProp;   incLProp["type"] = "boolean";
                                            incLProp["default"] = false;
                                            incLProp["description"] =
                        QStringLiteral("diff+hunks: attach each hunk's raw "
                                       "body lines");
                    QJsonObject ctxProp;    ctxProp["type"] = "integer";
                                            ctxProp["default"] = 3;
                                            ctxProp["minimum"] = 0;
                                            ctxProp["maximum"] = 10;
                                            ctxProp["description"] =
                        QStringLiteral("diff+hunks: unified-context width");
                    props["op"]    = opProp;
                    props["n"]     = nProp;
                    props["path"]  = pathProp;
                    props["range"] = rangeProp;
                    props["body"]  = bodyProp;
                    props["hunks"]         = hunksProp;
                    props["staged"]        = stagedProp;
                    props["include_lines"] = incLProp;
                    props["context"]       = ctxProp;
                    // ANTS-5508 — paths[], log grep / stat, diff added
                    // lines, rename_sweep's old name.
                    QJsonObject pathsProp;  pathsProp["type"] = "array";
                    QJsonObject pathsItems; pathsItems["type"] = "string";
                                            pathsProp["items"] = pathsItems;
                                            pathsProp["description"] =
                        QStringLiteral("log/diff/rename_sweep: several "
                                       "repo-relative paths; replaces path");
                    QJsonObject grepProp;   grepProp["type"] = "string";
                                            grepProp["description"] =
                        QStringLiteral("log: keep commits whose message "
                                       "contains this text, any case");
                    QJsonObject statProp;   statProp["type"] = "boolean";
                                            statProp["default"] = false;
                                            statProp["description"] =
                        QStringLiteral("log: add each commit's files[] "
                                       "{path, added, removed}");
                    QJsonObject addLProp;   addLProp["type"] = "boolean";
                                            addLProp["default"] = false;
                                            addLProp["description"] =
                        QStringLiteral("diff: only the added lines, "
                                       "{line, text} per file; not with hunks");
                    QJsonObject addMProp;   addMProp["type"] = "string";
                                            addMProp["description"] =
                        QStringLiteral("diff: keep added lines containing "
                                       "this text (case-sensitive); implies "
                                       "added_lines");
                    QJsonObject oldProp;    oldProp["type"] = "string";
                                            oldProp["description"] =
                        QStringLiteral("rename_sweep: the old name, matched "
                                       "as a whole word");
                    props["paths"]       = pathsProp;
                    props["grep"]        = grepProp;
                    props["stat"]        = statProp;
                    props["added_lines"] = addLProp;
                    props["added_match"] = addMProp;
                    props["old"]         = oldProp;
                    // ANTS-1391 — caller_cwd anchor.
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    props["etag_match"] = makeEtagMatchProp();   // ANTS-1499
                    props["fields"] = makeFieldsProp();          // ANTS-1720
                    props["compact"] = makeCompactProp();        // ANTS-2091
                    schema["properties"] = props;
                    // ANTS-3365: op no longer required — defaults to
                    // "status". caller_cwd is enforced by the dispatcher's
                    // CallerCwdContract (Required), not the JSON schema.
                    schema["required"] = QJsonArray();
                    gsTool["inputSchema"] = schema;
                }
                tools.append(gsTool);

                // ANTS-1251: subsystem — single tool, dispatches on
                // `op` (map / files / recent_changes). Pre-parses the
                // module map (docs/subsystems.md, else CLAUDE.md —
                // ANTS-1292) and serves per-lane chunks so /indie-review
                // reviewers don't each re-read the file.
                QJsonObject ssTool;
                ssTool["name"] = "subsystem";
                ssTool["description"] = QStringLiteral(
                    "Query the project's subsystem (lane) map parsed "
                    "from docs/subsystems.md (falling back to CLAUDE.md "
                    "when absent — ANTS-1292). Three ops: map (all lanes), "
                    "files (per-lane file list), recent_changes "
                    "(per-lane git log). Required: op. lane required "
                    "for files / recent_changes. n: recent_changes "
                    "only (default 10, cap 100). name: map only — "
                    "case-insensitive substring filter over lane names "
                    "(ANTS-3414). Saves ~24 K tokens per /indie-review run.");
                ssTool["selection_hint"] = QStringLiteral(
                    "Use as the first call on a 'where does feature "
                    "X live?' question — walks the docs/subsystems.md "
                    "module map. Collapses 3-5 grep rounds into one.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    QJsonObject opProp;     opProp["type"]    = "string";
                    QJsonArray opEnum;
                    opEnum.append("map");
                    opEnum.append("files");
                    opEnum.append("recent_changes");
                    opProp["enum"]    = opEnum;
                    QJsonObject laneProp;   laneProp["type"]  = "string";
                                            laneProp["description"] =
                        QStringLiteral("required for files / recent_changes");
                    QJsonObject nProp;      nProp["type"]     = "integer";
                                            nProp["default"]  = 10;
                                            nProp["minimum"]  = 1;
                                            nProp["maximum"]  = 100;
                                            nProp["description"] =
                        QStringLiteral("recent_changes only");
                    // ANTS-3414 — op:map optional `name` substring filter.
                    // Narrows the returned lane list to lanes whose name
                    // contains this needle (case-insensitive); empty/missing
                    // → the full map. Declared here so it is a real param
                    // (no longer flagged in ignored_args) — Vestige feedback.
                    QJsonObject nameProp;   nameProp["type"] = "string";
                                            nameProp["description"] =
                        QStringLiteral("op:map only — return only lanes whose "
                                       "name contains this substring "
                                       "(case-insensitive); omit for all lanes");
                    props["op"]   = opProp;
                    props["lane"] = laneProp;
                    props["n"]    = nProp;
                    props["name"] = nameProp;
                    // ANTS-1391 — caller_cwd anchor.
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    props["etag_match"] = makeEtagMatchProp();   // ANTS-1499
                    props["fields"] = makeFieldsProp();          // ANTS-1720
                    props["compact"] = makeCompactProp();        // ANTS-2091
                    schema["properties"] = props;
                    QJsonArray required;
                    required.append("op");
                    schema["required"] = required;
                    ssTool["inputSchema"] = schema;
                }
                tools.append(ssTool);

                // ANTS-1254: last_audit_summary — opens the latest
                // .audit_cache/audit-*.sarif and returns counts +
                // top_findings. ~5-15 K tokens → ~250 saved per fire.
                QJsonObject lasTool;
                lasTool["name"] = "last_audit_summary";
                lasTool["description"] = QStringLiteral(
                    "Read the latest audit report under {cwd}/.audit_cache and "
                    "return counts (error/warning/note/suppressed) plus "
                    "top_findings, instead of reading the HTML report. It reads "
                    "existing files and never re-runs scanners; call audit_run for "
                    "a fresh sweep. since_commit:<sha> reports fresh:false when the "
                    "cache is at another commit or older than 5 min. Refusal: "
                    "not_audited (no recognised report).");
                lasTool["detail"] = QStringLiteral(
                    "Read the latest audit summary under "
                    "{cwd}/.audit_cache and return a compact envelope: "
                    "counts (error/warning/note/suppressed) plus "
                    "top_findings (sorted by level desc, confidence "
                    "desc, file asc, line asc). Saves ~5-15 K tokens "
                    "vs reading the HTML report. Discovery order: "
                    "audit-*.sarif → cppcheck-*.xml (ANTS-1459) → "
                    "clang-tidy-*.txt → semgrep-*.json (ANTS-1494). "
                    "Returns {ok:false, code:\"not_audited\"} if no "
                    "recognised report is present. "
                    "Provenance (ANTS-1545): scrape-on-query against "
                    "disk artefacts that a previous `audit_run` (or "
                    "an out-of-band AuditDialog sweep / CI job) wrote "
                    "to .audit_cache. NOT paired with any specific "
                    "writer call — every invocation re-opens and "
                    "re-parses the newest matching file. If you need "
                    "a fresh sweep, call `audit_run` first; this tool "
                    "never re-runs the scanners. "
                    "Cppcheck gotcha: the default `--check-level=normal` "
                    "branch budget can silently truncate findings on "
                    "files > 5 K LoC; re-run cppcheck with "
                    "--check-level=exhaustive for full coverage. "
                    "ANTS-1406: pass `since_commit:<sha>` to short-"
                    "circuit redundant /audit re-runs — the response "
                    "carries `fresh:false` when the cached summary "
                    "is at a different commit or older than 5 min, "
                    "letting /close-phase skip a no-op audit "
                    "dispatch.");
                lasTool["selection_hint"] = QStringLiteral(
                    "Use when planning audit-touching work to see "
                    "what's already been flagged. Cheap; runs against "
                    "cached SARIF — no re-scan.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    QJsonObject topNProp;
                    topNProp["type"]    = "integer";
                    topNProp["default"] = 5;
                    topNProp["minimum"] = 0;
                    topNProp["maximum"] = 50;
                    topNProp["description"] = QStringLiteral(
                        "Cap on top_findings[]. Server-clamp [0, 50].");
                    QJsonObject floorProp;
                    floorProp["type"]    = "string";
                    floorProp["default"] = "warning";
                    QJsonArray floorEnum;
                    floorEnum.append("error");
                    floorEnum.append("warning");
                    floorEnum.append("note");
                    floorProp["enum"]    = floorEnum;
                    floorProp["description"] = QStringLiteral(
                        "SARIF level floor for top_findings[]. "
                        "Findings below this are still counted.");
                    // ANTS-1540 — `rule_ids: ["check-id-1", ...]`.
                    // Optional. When set, top_findings[] is filtered
                    // to only entries whose ruleId is in the list.
                    // Server expands the internal cap to 50 then
                    // re-applies top_n on the filtered slice so a
                    // rare rule below the default top-5 ranking
                    // still surfaces. Counts[] stays global.
                    QJsonObject ruleIdsProp;
                    ruleIdsProp["type"] = "array";
                    QJsonObject ruleIdsItems;
                    ruleIdsItems["type"] = "string";
                    ruleIdsProp["items"] = ruleIdsItems;
                    ruleIdsProp["description"] = QStringLiteral(
                        "Optional. Filter top_findings[] to only "
                        "those with ruleId in this list. Server "
                        "scans up to 50 raw findings under the hood "
                        "so a rare rule below the default top-5 "
                        "ranking still appears. Counts (error / "
                        "warning / note / suppressed) stay global. "
                        "Empty array equivalent to absent.");
                    // ANTS-1406 — `since_commit` short-circuit. When
                    // set, returns the cached snapshot only if it was
                    // produced at-or-after the supplied SHA (exact-match
                    // semantics) AND within a 5-minute freshness window.
                    // Otherwise returns {ok:true, fresh:false,
                    // since_commit, last_run_commit?, last_run_age_ms,
                    // reason:"commit_drift" | "stale_mtime" |
                    // "no_provenance"}. /close-phase uses this to skip
                    // a redundant /audit re-run when the prior pass is
                    // already cached at HEAD.
                    QJsonObject sinceCommitProp;
                    sinceCommitProp["type"] = "string";
                    sinceCommitProp["description"] = QStringLiteral(
                        "Optional. Short-circuit gate (ANTS-1406). "
                        "Pass your current HEAD SHA (full or shortened "
                        "≥7 hex chars). Server returns the cached "
                        "summary only if it was produced at this exact "
                        "commit AND the report mtime is within 5 min. "
                        "Otherwise responds {ok:true, fresh:false, "
                        "since_commit, last_run_commit?, "
                        "last_run_age_ms, reason} so callers can "
                        "decide whether to run a fresh audit. Saves "
                        "the ~45-50 K tokens /close-phase otherwise "
                        "burns re-running gates already known clean.");
                    props["top_n"]          = topNProp;
                    props["severity_floor"] = floorProp;
                    props["rule_ids"]       = ruleIdsProp;
                    props["since_commit"]   = sinceCommitProp;
                    // ANTS-1391 — caller_cwd anchor.
                    props["caller_cwd"]     = makeCallerCwdReadProp();
                    props["etag_match"]     = makeEtagMatchProp();   // ANTS-1499
                    schema["properties"]    = props;
                    lasTool["inputSchema"] = schema;
                }
                tools.append(lasTool);
}

}  // namespace claudeintegrationdetail
