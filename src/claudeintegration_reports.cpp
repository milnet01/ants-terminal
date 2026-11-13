// ANTS-1677 claudeintegration piece 8/9 — the report tool descriptors
#include "claudeintegration_internal.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace claudeintegrationdetail {

void appendToolSchemasReports(QJsonArray &tools) {
                // ANTS-5515 — dependency_inventory: what the project pins, offline.
                {
                    QJsonObject t;
                    t["name"] = "dependency_inventory";
                    t["selection_hint"] = QStringLiteral(
                        "Use instead of grepping for uses:, runs-on, FROM and runtime "
                        "versions; it lists them and looks nothing up.");
                    t["description"] = QStringLiteral(
                        "List what a project pins, each row with file and line: action "
                        "(repo, ref, version_comment), local_action, docker_action, "
                        "runner, container (image, tag, digest) and runtime (name, "
                        "version). Reads tracked files and asks no network, so it never "
                        "says what is latest. A runtime with no version source says "
                        "no_authoritative_source; a value holding $ is unresolved. Read "
                        "skipped[] and not_covered[]. caller_cwd Required.");
                    t["detail"] = QStringLiteral(
                        "ANTS-5515. Contract: tests/features/mcp_dependency_inventory/"
                        "spec.md. Lines are read by pattern, not by a YAML parser. Runtime "
                        "sources: .nvmrc, .node-version, .python-version, .ruby-version, "
                        ".tool-versions, rust-toolchain(.toml), go.mod, requires-python, "
                        "rust-version, engines.node, a workflow's *-version: and, in "
                        "CMakeLists.txt, cmake_minimum_required and a Qt find_package. "
                        "A version file counts at the project root only.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    schema["properties"] = props;
                    schema["required"] = QJsonArray{QStringLiteral("caller_cwd")};
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-6399 — ci_report: CI run history through gh, analysed.
                {
                    QJsonObject t;
                    t["name"] = "ci_report";
                    t["selection_hint"] = QStringLiteral(
                        "Use instead of reading GitHub Actions runs by hand: flaky tests, "
                        "jobs near their timeout, cache and queue problems.");
                    t["description"] = QStringLiteral(
                        "Read a project's GitHub Actions run history through gh and list "
                        "CI problems: flaky (failed then passed on one commit), "
                        "alternating (flips red/green across commits), near_budget, "
                        "budget_trend, scheduled_red, queue_wait, annotation, "
                        "runner_latest, cache_size, cache_key_shared, cache_save_skipped, "
                        "test_parallelism, repo_ambiguous (a fork it cannot tell apart). "
                        "Each finding names its runs, SHAs and the "
                        "standard it cites. source:\"saved\" reads the last report with no "
                        "network. op:\"checks\" with pr or ref instead returns one commit's "
                        "check counts, failing names and what also fails on the base "
                        "branch. caller_cwd Required.");
                    t["detail"] = QStringLiteral(
                        "ANTS-6399. Contract: docs/specs/ANTS-6399-ci-report-verb.md. A live "
                        "call is bounded at 120 s and 300 gh calls; a stopped fetch says "
                        "truncated_reason and still reports. No workflows or no GitHub "
                        "remote answers ci:\"none\". A run's budgets come from its commit's "
                        "workflow files, else today's; one it cannot read is listed in "
                        "unread[], never guessed.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    props["source"] = QJsonObject{
                        {"type", "string"},
                        {"enum", QJsonArray{QStringLiteral("live"), QStringLiteral("saved")}},
                        {"default", "live"},
                        {"description", QStringLiteral(
                             "live (default) asks GitHub and saves the report; saved returns "
                             "the last saved report, no network.")}};
                    props["days"] = QJsonObject{
                        {"type", "integer"}, {"minimum", 1}, {"maximum", 90},
                        {"default", 30},
                        {"description", QStringLiteral(
                             "Window by run creation time (default 30). Only the default "
                             "window is saved.")}};
                    // ANTS-5706 — op:"checks" and its three arguments.
                    props["op"] = QJsonObject{
                        {"type", "string"},
                        {"enum", QJsonArray{QStringLiteral("report"), QStringLiteral("checks")}},
                        {"default", "report"},
                        {"description", QStringLiteral(
                             "report (default): the history findings. checks: one commit's "
                             "check counts; needs pr or ref.")}};
                    props["pr"] = QJsonObject{
                        {"type", "integer"}, {"minimum", 1},
                        {"description", QStringLiteral(
                             "op:checks: a pull request number; compared with its base branch.")}};
                    props["ref"] = QJsonObject{
                        {"type", "string"},
                        {"description", QStringLiteral(
                             "op:checks: a branch, tag or SHA, in place of pr.")}};
                    props["base"] = QJsonObject{
                        {"type", "string"},
                        {"description", QStringLiteral(
                             "op:checks: the ref to compare with (default: a pr's base branch).")}};
                    props["workflow"] = QJsonObject{
                        {"type", "string"},
                        {"description", QStringLiteral(
                             "Keep this workflow only (exact name): its runs and its "
                             "file's checks. A filtered report is not saved.")}};
                    schema["properties"] = props;
                    schema["required"] = QJsonArray{QStringLiteral("caller_cwd")};
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-5510 — version_check: lockstep against the bump recipe.
                {
                    QJsonObject t;
                    t["name"] = "version_check";
                    t["selection_hint"] = QStringLiteral(
                        "Use before a release, or to ask whether every file holding "
                        "the version agrees with its source.");
                    t["description"] = QStringLiteral(
                        "Read .claude/bump.json and check each listed file holds the "
                        "version_source's version. Findings: version_drift, "
                        "stray_suffix, pattern_not_found, pattern_ambiguous, "
                        "file_missing, and unlisted_version_file, a candidate for a "
                        "tracked file holding the version that the recipe names "
                        "nowhere. Read skipped[]: no recipe or no git list is silence, "
                        "not a clean result. Refusals: bad_recipe, version_unreadable. "
                        "caller_cwd Required.");
                    t["detail"] = QStringLiteral(
                        "ANTS-5510. Contract: tests/features/mcp_version_check/spec.md. "
                        "The unlisted scan leaves out tests/, docs/, *.md, comment "
                        "lines, binary files and files over 1 MiB, and stops at 100 "
                        "findings with unlisted_truncated. Read each candidate: a grep "
                        "for a version finds lockstep fields and history alike.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    schema["properties"] = props;
                    schema["required"] = QJsonArray{QStringLiteral("caller_cwd")};
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-5502 — quotation_check: many quotations verified in one
                // call. Every property below is read by cmdQuotationCheck (ANTS-4621).
                {
                    QJsonObject t;
                    t["name"] = "quotation_check";
                    t["selection_hint"] = QStringLiteral(
                        "Use to verify a review's quotations in one call instead of "
                        "one quotation-check.sh run per quotation.");
                    t["description"] = QStringLiteral(
                        "Check that each quotation occurs in the file it names, "
                        "normalised as quotation-check.sh does (blockquote markers, "
                        "whitespace, [text](url), ** and backquotes). `items` is 1 to "
                        "500 {path, text, ref?, line?, tolerance?}. Each gets hit, "
                        "hit_displaced (found, but not within tolerance of `line`; "
                        "carries the found line), miss (with the nearest line), "
                        "not_run (with a reason; never a miss) or outside_allowed. "
                        "`findings` lists every non-hit and is never "
                        "trimmed; `check_errors` lists the not_run ones. Refusals: "
                        "bad_args. caller_cwd Required.");
                    t["detail"] = QStringLiteral(
                        "ANTS-5502. Contract: docs/specs/ANTS-5502-quotation-check.md. "
                        "not_run reasons: bad_path, not_found, not_a_file, unreadable, "
                        "too_large (subject over 16 MiB), text_too_long (text over "
                        "8 KiB), empty_text, bad_ref, ref_unreadable. `ref` reads the "
                        "blob via git cat-file at that revision. `allowed` globs: ** "
                        "crosses /, * and ? do not, all else literal; matched against "
                        "the path after symlinks resolve. max_bytes trims `results` "
                        "only. ANTS-5732: an item's `line` is the line its finding "
                        "cites, the first where it cites a range; an occurrence's line "
                        "is the one it starts on, and the nearest decides.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "Your $PWD. REQUIRED — the project the paths resolve in.");
                        props["caller_cwd"] = p;
                    }
                    {
                        QJsonObject item;
                        item["type"] = "object";
                        QJsonObject ip;
                        QJsonObject s;
                        s["type"] = "string";
                        ip["path"] = s;
                        ip["text"] = s;
                        ip["ref"] = s;
                        // ANTS-5732 — the cited line, and this item's tolerance.
                        QJsonObject lineP;
                        lineP["type"] = "integer";
                        lineP["minimum"] = 1;
                        ip["line"] = lineP;
                        QJsonObject tolP;
                        tolP["type"] = "integer";
                        tolP["minimum"] = 0;
                        ip["tolerance"] = tolP;
                        item["properties"] = ip;
                        QJsonArray req;
                        req.append(QStringLiteral("path"));
                        req.append(QStringLiteral("text"));
                        item["required"] = req;
                        QJsonObject p;
                        p["type"] = "array";
                        p["items"] = item;
                        p["minItems"] = 1;
                        p["maxItems"] = 500;
                        p["description"] = QStringLiteral(
                            "{path, text, ref?, line?, tolerance?}: path relative or "
                            "absolute; text as written; ref a git revision; line the "
                            "cited line.");
                        props["items"] = p;
                    }
                    {
                        QJsonObject s;
                        s["type"] = "string";
                        QJsonObject p;
                        p["type"] = "array";
                        p["items"] = s;
                        p["description"] = QStringLiteral(
                            "Globs over the project-relative path. An item matching "
                            "none is outside_allowed and never opened.");
                        props["allowed"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "integer";
                        p["minimum"] = 1;
                        p["description"] = QStringLiteral(
                            "Response cap in bytes (default 512 KiB). Trims `results` "
                            "only; `findings` is never trimmed.");
                        props["max_bytes"] = p;
                    }
                    {
                        // ANTS-5732 — default 3, measured (spec § 2.3).
                        QJsonObject p;
                        p["type"] = "integer";
                        p["minimum"] = 0;
                        p["description"] = QStringLiteral(
                            "Lines an occurrence may start from an item's `line` and "
                            "still be a hit (default 3). An item's own tolerance wins.");
                        props["tolerance"] = p;
                    }
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append(QStringLiteral("caller_cwd"));
                    req.append(QStringLiteral("items"));
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-5787 — function_sizes: the longest functions, ranked. Every
                // property below is read by cmdFunctionSizes (ANTS-4621).
                {
                    QJsonObject t;
                    t["name"] = "function_sizes";
                    t["selection_hint"] = QStringLiteral(
                        "Use to find where code is hardest to follow: the longest "
                        "functions in some paths, ranked, instead of outlining each file.");
                    t["description"] = QStringLiteral(
                        "Rank the longest functions in the given files or directories. "
                        "Each row: {id, file, symbol, lines, body_start, body_end, "
                        "nesting_depth}. `top_level` ranks shell and Python code outside any "
                        "function. A shortlist for review, not a finding. Tests and "
                        "generated files are left out unless asked for; `scanned` counts "
                        "files, functions measured and any unmeasured. Refusals: "
                        "bad_args, bad_path, no_project. caller_cwd Required.");
                    t["detail"] = QStringLiteral(
                        "ANTS-5787. Contract: tests/features/mcp_function_sizes/spec.md. "
                        "Functions are FileOutline's `func` symbols. A brace-family body "
                        "ends at its balancing brace; a Python body at its last line "
                        "indented deeper than the def. A declaration ending in `;` is "
                        "skipped. nesting_depth counts the body's own level as 1. paths "
                        "defaults to source_roots, else src, else the root.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    props["caller_cwd"] = makeWalkCallerCwdProp();
                    props["paths"] = makeWalkPathsProp(QStringLiteral(
                        "Files or directories, project-relative or absolute. Default: "
                        "source_roots, else src."));
                    {
                        QJsonObject p;
                        p["type"] = "integer";
                        p["minimum"] = 1;
                        p["maximum"] = 500;
                        p["description"] = QStringLiteral("How many rows to return (default 20).");
                        props["top"] = p;
                    }
                    props["include_tests"] = makeWalkIncludeProp(QStringLiteral("test files"));
                    props["include_generated"] = makeWalkIncludeProp(QStringLiteral("generated files such as moc_*"));
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append(QStringLiteral("caller_cwd"));
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-5602 — source_scrapes: the tests that read a file as text.
                // Every property below is read by cmdSourceScrapes (ANTS-4621).
                {
                    QJsonObject t;
                    t["name"] = "source_scrapes";
                    t["selection_hint"] = QStringLiteral(
                        "Use before a refactor: which tests read this file as text, and "
                        "which strings they look for, so a rename does not break them unseen.");
                    t["description"] = QStringLiteral(
                        "Name the tests that read a source file as text. Each scrape row: "
                        "{path, test, file, line, anchor, kind}; anchor is the searched "
                        "string as written, kind literal or regex. `readers` names each "
                        "test that reads the file, anchors found or not. Refusals: "
                        "bad_args, bad_path, no_project. caller_cwd Required.");
                    t["detail"] = QStringLiteral(
                        "ANTS-5602. Contract: tests/features/mcp_source_scrapes/spec.md. "
                        "A test reaches a file through a path definition in the root "
                        "CMakeLists.txt (SRC_*_PATH, a list such as ANTS_RC_SOURCES), a "
                        "directory definition plus a literal path, a helper whose return "
                        "reads one, or a variable assigned from any of these. Anchors are "
                        "literal arguments of find/contains/indexOf/count, of a call whose "
                        "first argument reads the file, or a regex used on it. test is "
                        "Suite.Name, for ctest -R. scanned.unresolved_anchors counts "
                        "searches by a variable; not_covered names what is not followed. "
                        "Python and shell tests give readers rows only (ANTS-6180); notices "
                        "warns when no test file was read.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    props["caller_cwd"] = makeWalkCallerCwdProp();
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "A source file or directory, project-relative or absolute.");
                        props["path"] = p;
                    }
                    props["paths"] = makeWalkPathsProp(QStringLiteral(
                        "Several files or directories; rows for any of them. Give path, "
                        "paths, or both."));
                    {
                        QJsonObject p;
                        p["type"] = "integer";
                        p["minimum"] = 1;
                        p["maximum"] = 5000;
                        p["description"] = QStringLiteral(
                            "Cap on scrapes and on readers (default 500); scanned holds the totals.");
                        props["limit"] = p;
                    }
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append(QStringLiteral("caller_cwd"));
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-5786 — comment_claims: comments whose claims can be checked.
                // Every property below is read by cmdCommentClaims (ANTS-4621).
                {
                    QJsonObject t;
                    t["name"] = "comment_claims";
                    t["selection_hint"] = QStringLiteral(
                        "Use to find comments that have gone false: a quotation its file "
                        "lacks, a see-reference to nothing, an unknown ticket id, a stale count.");
                    t["description"] = QStringLiteral(
                        "Check the claims code comments make. Two buckets of {id, file, line, "
                        "kind, quote, why}: findings, decided by lookup (missing_quotation, "
                        "dead_reference, unknown_ticket), and candidates for a reader "
                        "(census_number). Refusals: bad_args, bad_path, no_project. "
                        "caller_cwd Required.");
                    t["detail"] = QStringLiteral(
                        "ANTS-5786. Contract: tests/features/mcp_comment_claims/spec.md. "
                        "Comments: // and /* */ in C-family files, # in Python, shell, CMake "
                        "and YAML, -- in Lua. missing_quotation: a quoted text no file named "
                        "within 80 characters contains. dead_reference: `see X` naming a "
                        "path or a project symbol that does not exist. unknown_ticket: an id "
                        "with a roadmap prefix that the roadmap lacks. census_number: ~N, "
                        "N-line, N <plural noun>. Tests and generated files are left out "
                        "unless asked; paths defaults to source_roots, else src.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    props["caller_cwd"] = makeWalkCallerCwdProp();
                    props["paths"] = makeWalkPathsProp(QStringLiteral(
                        "Files or directories, project-relative or absolute. Default: "
                        "source_roots, else src."));
                    {
                        QJsonObject p;
                        p["type"] = "integer";
                        p["minimum"] = 1;
                        p["maximum"] = 5000;
                        p["description"] = QStringLiteral(
                            "Cap on each bucket (default 500); scanned holds the totals.");
                        props["limit"] = p;
                    }
                    props["include_tests"] = makeWalkIncludeProp(QStringLiteral("test files"));
                    props["include_generated"] = makeWalkIncludeProp(QStringLiteral("generated files such as moc_*"));
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append(QStringLiteral("caller_cwd"));
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-5785 — duplicate_blocks: code written out more than once.
                // Every property below is read by cmdDuplicateBlocks (ANTS-4621).
                {
                    QJsonObject t;
                    t["name"] = "duplicate_blocks";
                    t["selection_hint"] = QStringLiteral(
                        "Use to find code written out more than once, including copies that "
                        "drifted apart, as a shortlist instead of reading files side by side.");
                    t["description"] = QStringLiteral(
                        "Find blocks of code repeated across the given files. Each group: {id, "
                        "lines, copies, diverged, finding_shape, similarity, sites[{file, start, "
                        "end}], diff[{file, line, text}]}, largest first. Grades nothing. "
                        "Refusals: bad_args, bad_path, no_project. caller_cwd Required.");
                    t["detail"] = QStringLiteral(
                        "ANTS-5785. Contract: tests/features/mcp_duplicate_blocks/spec.md. "
                        "Compares lines in order: blank, comment, bracket-only and import lines "
                        "are dropped. A match may skip up to 3 edited lines per side while "
                        "matched lines stay at least `similarity` of the longer side; such a "
                        "group is diverged and diff lists the edited lines. finding_shape: "
                        "copies >= 3 or diverged (coding.md § 1.3). id: dup:<sha1 of the first "
                        "site's text, 12 hex>. Tests and generated files are left out unless "
                        "asked; paths defaults to source_roots, else src.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    props["caller_cwd"] = makeWalkCallerCwdProp();
                    props["paths"] = makeWalkPathsProp(QStringLiteral(
                        "Files or directories, project-relative or absolute. Default: "
                        "source_roots, else src."));
                    {
                        QJsonObject p;
                        p["type"] = "integer";
                        p["minimum"] = 2;
                        p["maximum"] = 200;
                        p["description"] = QStringLiteral(
                            "Shortest block reported, in compared lines (default 5).");
                        props["min_lines"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "number";
                        p["minimum"] = 0.5;
                        p["maximum"] = 1.0;
                        p["description"] = QStringLiteral(
                            "Least share of matched lines for a drifted copy (default 0.8); 1 "
                            "allows exact copies only.");
                        props["similarity"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "boolean";
                        p["description"] = QStringLiteral(
                            "Match copies whose names, numbers and strings differ (default false).");
                        props["normalise_names"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "integer";
                        p["minimum"] = 1;
                        p["maximum"] = 500;
                        p["description"] = QStringLiteral("How many groups to return (default 50).");
                        props["top"] = p;
                    }
                    props["include_tests"] = makeWalkIncludeProp(QStringLiteral("test files"));
                    props["include_generated"] = makeWalkIncludeProp(QStringLiteral("generated files such as moc_*"));
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append(QStringLiteral("caller_cwd"));
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-1548 — changelog_log: token-frugal CHANGELOG writer.
                {
                    QJsonObject t;
                    t["name"] = "changelog_log";
                    t["description"] = QStringLiteral(
                        "Append a Keep-a-Changelog entry under ## "
                        "[Unreleased] in CHANGELOG.md. "
                        "ops: add (default) | add_from_roadmap "
                        "(cite a ROADMAP id; reuses its headline + Layman "
                        "line) | add_batch (entries[], one atomic commit) | "
                        "normalize (put [Unreleased]'s ### blocks in canonical "
                        "order; fold stray prose into the bullet above) | "
                        "amend (replace or delete one [Unreleased] phrase). "
                        "Category from `category` or derived from `kind`. "
                        "Required: caller_cwd (+ summary for add, id for "
                        "add_from_roadmap, entries[] for add_batch). "
                        "dry_run:true previews without writing. "
                        "Refusals: not_unreleased, bad_category, "
                        "no_changelog, format_mismatch, id_not_in_roadmap, "
                        "feature_grouped_section, mixed_section, "
                        "missing_field, bad_args, "
                        "bad_summary, bad_op_combo.");
                    // ANTS-2079 — full per-op reference in `detail`
                    // (stripped from the tools/list wire; served by
                    // tool_info {name:"changelog_log"}).
                    t["detail"] = QStringLiteral(
                        "Append a Keep-a-Changelog entry under the "
                        "`## [Unreleased]` section of CHANGELOG.md "
                        "without re-emitting the file. Mode picked by "
                        "`op` (default \"add\"). op:\"add\" — render a "
                        "bullet `- **<summary>** (<id>)` (+ optional "
                        "`body` as indented continuation) under the "
                        "`### <category>` heading; the category is taken "
                        "from `category` or derived from `kind` "
                        "(fix/*-fix → Fixed, feature/implement/"
                        "enhancement → Added, security → Security, else "
                        "Changed). The `### <category>` heading is "
                        "created in canonical order if absent. "
                        "ANTS-4563: against a MIXED `[Unreleased]` \u2014 dated "
                        "`### <date> <Category>` topics leading, a legacy flat "
                        "category tail below \u2014 this op ROUTES to the "
                        "dated-topic form at the TOP of the section and sets "
                        "`routed_to_subsection:true`, so `bullet` echoes the "
                        "heading that landed rather than a bullet. It used to "
                        "append into that tail and report ok:true, measured at "
                        "~10,900 lines below the newest entry, because the "
                        "feature_grouped_section detector is a PRESENCE test "
                        "and one legacy `### Added` declassified the whole "
                        "section. Both guards now read one ORDER test. A "
                        "direct call to the flat writer refuses "
                        "`mixed_section` instead; op:\"add_batch\" routes each "
                        "entry as this op does, flagged per entry. "
                        "op:\"add_from_roadmap\" — cite a ROADMAP bullet "
                        "by `id`; its headline becomes the summary unless "
                        "`summary` overrides it (ANTS-4360), and "
                        "its `Layman:` line the body (reused verbatim, "
                        "not regenerated — keeps CHANGELOG + ROADMAP in "
                        "lockstep), category derived from the bullet's "
                        "`Kind:`. A wrapped multi-line headline is "
                        "collapsed to a single line (ANTS-1868) so the "
                        "rendered bullet stays a well-formed Markdown "
                        "list item. New bullets insert at the TOP of "
                        "their category (most-recent-first). "
                        "op:\"add_batch\" (ANTS-2044) — write N "
                        "`entries[]` in one read + one atomic commit; "
                        "each entry auto-detects add vs add_from_roadmap, "
                        "and per-entry failures land in "
                        "`skipped[]:[{index, code, error}]` while the rest "
                        "apply. **The RESULT reads in input order** "
                        "(ANTS-4854): entries[0] ends up ABOVE entries[1]. "
                        "They are written in reverse to achieve that, since "
                        "each insert prepends to the top of its category — "
                        "so this is NOT byte-identical to the same N "
                        "sequential calls, which put the LAST entry on top. "
                        "`applied[]` and `skipped[]` stay in input order "
                        "whatever order the writes ran in. "
                        "op:\"normalize\" (ANTS-3495) — reorder the "
                        "`### <category>` blocks under `## [Unreleased]` "
                        "into canonical Keep-a-Changelog order "
                        "(Added/Changed/Deprecated/Removed/Fixed/Security). "
                        "Non-destructive: each block's bullets and any "
                        "wedged prose move with its heading; a duplicate "
                        "or non-canonical heading keeps its relative "
                        "position; a preamble paragraph above the first "
                        "`### ` is untouched. No summary/body/id needed. "
                        "ANTS-3381 adds the other half of the ANTS-2125 "
                        "advisory in the same atomic write: a stray "
                        "flush-left prose line wedged between blocks is "
                        "FOLDED into the nearest preceding bullet as a "
                        "two-space continuation, reported line by line in "
                        "`moves[]:[{from_line, under_line, text}]` with a "
                        "`prose_moved` count. Preview it with dry_run "
                        "before writing — the failure mode this policy "
                        "accepts is a paragraph meant to stand alone being "
                        "absorbed by the entry above it. Prose separated "
                        "from its bullet by a heading is left alone and "
                        "keeps raising the `advisory`. "
                        "op:\"amend\" (ANTS-5246) — replace the one "
                        "occurrence of `old_text` under `## [Unreleased]` with "
                        "`new_text`; empty deletes it, and a line left blank "
                        "goes too. The match may span a hard wrap: the reply "
                        "then says `wrapped:true`, and the joined line is "
                        "wrapped again at the old width. `id` limits "
                        "it to the entries citing that id. Released sections "
                        "are frozen and never searched, so passing `version` "
                        "refuses `not_unreleased`. Refusals: missing_field, "
                        "id_not_in_unreleased, text_not_found, text_ambiguous, "
                        "wrapped_block, empty_bullet. Returns {ok, op, file, "
                        "section, line, changed, wrapped?, line_text or "
                        "line_removed, bytes_written, file_bytes}. "
                        "Returns {ok, op, file, changed, order_before, "
                        "order_after, prose_moved, moves?, bytes_written, "
                        "file_bytes} "
                        "(ANTS-3723: bytes_written is the ADDED-bytes delta, "
                        "matching roadmap_log — 0 for a pure reorder; "
                        "file_bytes is the whole file) (bytes + dry_run:true "
                        "under preview; no write when already canonical, "
                        "changed:false). Required: "
                        "caller_cwd (+ summary for add, or id for "
                        "add_from_roadmap, or entries[] for add_batch). "
                        "Atomic via QSaveFile. "
                        "Refusals: not_unreleased (no `## [Unreleased]` "
                        "heading), bad_category, no_changelog, "
                        "format_mismatch (YAML changelog), "
                        "id_not_in_roadmap, feature_grouped_section "
                        "(normalize: dated `### ` topics, not flat "
                        "categories), missing_field, bad_args "
                        "(empty entries[]), bad_summary (ANTS-4629 — the "
                        "`summary` already starts with `**` or ends with an "
                        "id in parentheses; this verb adds both, so it would "
                        "render `- ****...` or repeat the id. Pass the plain "
                        "text. Under add_batch it is a per-entry skip), "
                        "bad_op_combo. "
                        "Returns {ok, op, file, category, line, "
                        "bytes_written, file_bytes, created_category, "
                        "bullet, id?} — `bullet` is the rendered entry, "
                        "echoed on the real write as well as under dry_run "
                        "(ANTS-4629) "
                        "— or for "
                        "add_batch {ok, op, file, applied:[{index, id?, "
                        "category, line}], applied_count, skipped, "
                        "skipped_count, bytes_written, file_bytes} — or "
                        "{ok:false, error, code}. A non-blocking "
                        "`advisory` string (ANTS-2125) accompanies a "
                        "successful write when the `## [Unreleased]` "
                        "section already interleaves non-heading prose "
                        "(a stray footer/separator) between its `### ` "
                        "category blocks — the entry still inserts in "
                        "canonical order, but the section layout is "
                        "malformed. "
                        "dry_run:true (ANTS-2136) previews the resolved "
                        "insert (category routing, line, `bytes`, rendered "
                        "`bullet`; add_batch echoes applied/skipped) "
                        "without writing — envelope carries dry_run:true "
                        "and `bytes` replaces `bytes_written`.");
                    t["selection_hint"] = QStringLiteral(
                        "Use to add a CHANGELOG.md entry under "
                        "[Unreleased] instead of hand-editing it. "
                        "Mutates project state — caller_cwd required. "
                        "add_from_roadmap reuses a ROADMAP bullet's "
                        "prose by id.");

                    QJsonObject clCaller;
                    clCaller["type"] = "string";
                    clCaller["description"] = QStringLiteral(
                        "Your $PWD. REQUIRED — anchors the write to your "
                        "project's CHANGELOG.md.");

                    QJsonObject clOp;
                    clOp["type"] = "string";
                    QJsonArray clOpEnum;
                    clOpEnum.append("add");
                    clOpEnum.append("add_from_roadmap");
                    clOpEnum.append("add_batch");
                    clOpEnum.append("add_subsection");
                    clOpEnum.append("release");   // ANTS-4363
                    clOpEnum.append("normalize");
                    clOpEnum.append("amend");   // ANTS-5246
                    clOp["enum"] = clOpEnum;
                    clOp["description"] = QStringLiteral(
                        "Verb mode. Default \"add\" (summary + optional "
                        "body). \"add_from_roadmap\" reuses the cited "
                        "ROADMAP bullet's headline + Layman prose; pass "
                        "`summary` to rewrite the headline in changelog "
                        "voice and keep the rest (ANTS-4360). "
                        "\"add_batch\" (ANTS-2044) writes N `entries[]` "
                        "in one read + one atomic commit — each entry "
                        "auto-detects mode (a `summary` → add; an "
                        "`id`-only entry → add_from_roadmap); per-entry "
                        "failures land in `skipped[]` while the rest "
                        "apply (parity with roadmap_log append_batch). "
                        "\"release\" (ANTS-4363) CLOSES `## [Unreleased]` "
                        "into `## [<version>] - <date>` (the dash matches the "
                        "newest dated heading, ANTS-5388) and opens a fresh "
                        "empty `[Unreleased]` above it — the one changelog "
                        "edit every release makes, and previously the only one "
                        "this verb did not own. An optional one-line `theme` "
                        "is written as the section's **Theme:** line. "
                        "Takes `version` (required; "
                        "`date` defaults to today and the RESOLVED value is "
                        "echoed back as `date`, on the dry run as well as the "
                        "write — ANTS-4833, because it is the release path's "
                        "only unpredictable field and the heading date is what "
                        "a notes-extraction grep keys on), returns the closed "
                        "section as `released_body` so a caller has its "
                        "release notes without re-reading the file (capped at "
                        "16384 bytes by default and announced when truncated "
                        "\u2014 ANTS-4561; `max_body_bytes:0` lifts it), and "
                        "refuses `nothing_to_release` on an empty section or "
                        "`version_exists` when that heading already exists "
                        "(two `## [X.Y.Z]` blocks leave a notes-extraction "
                        "grep unable to choose). ANTS-5484 — `dated:true` "
                        "closes under `## <date>` instead, merging a second "
                        "close the same day. "
                        "\"add_subsection\" (ANTS-3584, guarded by "
                        "ANTS-4356 — it refuses `flat_section` when a flat "
                        "`### <category>` heading sits ABOVE every dated topic, "
                        "because writing a dated topic there produces a MIXED "
                        "section from which op:add and op:normalize BOTH start "
                        "refusing. ANTS-4562 — the test is ORDER, not presence: "
                        "a section whose first `### ` heading is a dated topic "
                        "is ACCEPTED however many flat category blocks trail "
                        "below it, because a top insert is safe there. So a "
                        "legacy tail does NOT block the migration it would "
                        "otherwise be protecting, and emptying the section is "
                        "not the remedy — leading with a dated topic is) "
                        "writes a DATED "
                        "feature-grouped block (`### <date> <Category> — "
                        "<headline>` + optional prose `body` + optional "
                        "`bullets[]`) at the TOP of [Unreleased], "
                        "newest-first — for changelogs grouped by dated "
                        "topic rather than flat categories (needs "
                        "`headline` + `category`|`kind`; `date` defaults "
                        "to today). "
                        "\"normalize\" (ANTS-3495) reorders the "
                        "`### <category>` blocks under [Unreleased] into "
                        "canonical Keep-a-Changelog order, and (ANTS-3381) "
                        "folds a stray flush-left prose line wedged between "
                        "blocks into the bullet above it as a two-space "
                        "continuation — `moves[]` reports each fold, so "
                        "preview with dry_run first (needs no "
                        "summary/body/id).");

                    QJsonObject clSummary;
                    clSummary["type"] = "string";
                    clSummary["maxLength"] = 300;
                    clSummary["description"] = QStringLiteral(
                        "Bold one-line entry summary. Under "
                        "add_from_roadmap it OVERRIDES the cited bullet's "
                        "headline while category, id and the Layman body "
                        "are still inherited (ANTS-4360) — a planned "
                        "bullet names the DEFECT, which under `### Fixed` "
                        "reads as if the bug is still there. Omit it to "
                        "reuse the headline verbatim.");

                    QJsonObject clCategory;
                    clCategory["type"] = "string";
                    QJsonArray clCatEnum;
                    clCatEnum.append("Added");
                    clCatEnum.append("Changed");
                    clCatEnum.append("Deprecated");
                    clCatEnum.append("Removed");
                    clCatEnum.append("Fixed");
                    clCatEnum.append("Security");
                    clCategory["enum"] = clCatEnum;
                    clCategory["description"] = QStringLiteral(
                        "Keep-a-Changelog category. Optional — derived "
                        "from `kind` (add) or the bullet's `Kind:` "
                        "(add_from_roadmap) when omitted.");

                    QJsonObject clKind;
                    clKind["type"] = "string";
                    clKind["description"] = QStringLiteral(
                        "Roadmap `Kind:` value used to derive `category` "
                        "when it is omitted (op:\"add\"). Same enum as "
                        "roadmap_log's kind.");

                    QJsonObject clBody;
                    clBody["type"] = "string";
                    clBody["maxLength"] = 4000;
                    clBody["description"] = QStringLiteral(
                        "Optional prose appended under the bullet as "
                        "2-space-indented continuation line(s). Under "
                        "add_from_roadmap, overrides the reused Layman "
                        "line. Under Fixed, a fix-kind item's Layman line "
                        "is not reused, since it describes the defect; "
                        "the reply hands it back as `layman_body_omitted` "
                        "(ANTS-5673). Pre-wrap to ~70 columns.");

                    QJsonObject clId;
                    clId["type"] = "string";
                    clId["description"] = QStringLiteral(
                        "[PROJ-NNNN] id. Appended as \"(<id>)\" on the "
                        "bullet under op:\"add\"; REQUIRED under "
                        "add_from_roadmap (the bullet to cite). "
                        "Case-sensitive.");

                    // ANTS-2044 — entries[] for op:"add_batch". Each
                    // item has the same shape as a single add /
                    // add_from_roadmap call (summary|id, category|kind,
                    // body?); mode is auto-detected per entry.
                    QJsonObject clEntryItem;
                    clEntryItem["type"] = "object";
                    QJsonObject clEntryProps;
                    clEntryProps["summary"]  = clSummary;
                    clEntryProps["category"] = clCategory;
                    clEntryProps["kind"]     = clKind;
                    clEntryProps["body"]     = clBody;
                    clEntryProps["id"]       = clId;
                    clEntryItem["properties"] = clEntryProps;
                    QJsonObject clEntries;
                    clEntries["type"]  = "array";
                    clEntries["items"] = clEntryItem;
                    clEntries["description"] = QStringLiteral(
                        "op:\"add_batch\" only — the entries to write in "
                        "one atomic commit. Each: {summary + category|"
                        "kind (+ id?, body?)} for an add, OR {id} alone "
                        "to pull from ROADMAP (add_from_roadmap). The result "
                        "READS in input order — entries[0] above entries[1] "
                        "(ANTS-4854); per-entry failures go to `skipped[]`, "
                        "which is keyed by `index` and emitted in input "
                        "order.");

                    // ANTS-3584 — op:"add_subsection" params: the dated
                    // topic headline, its date, and optional bullets rendered
                    // beneath the prose.
                    QJsonObject clHeadline;
                    clHeadline["type"] = "string";
                    clHeadline["maxLength"] = 300;
                    clHeadline["description"] = QStringLiteral(
                        "op:\"add_subsection\" — the dated topic headline; the "
                        "block heading is `### <date> <Category> — "
                        "<headline>`. Include any ids in the headline text "
                        "(e.g. \"Meadow trees cast shadows (PROJ-33)\").");

                    QJsonObject clDate;
                    clDate["type"] = "string";
                    clDate["description"] = QStringLiteral(
                        "op:\"add_subsection\" — the subsection date "
                        "(YYYY-MM-DD). Optional; defaults to today. The block "
                        "is inserted newest-first at the top of [Unreleased].");

                    QJsonObject clBulletItem;
                    clBulletItem["type"] = "object";
                    QJsonObject clBulletProps;
                    clBulletProps["summary"] = clSummary;
                    clBulletProps["body"]    = clBody;
                    clBulletProps["id"]      = clId;
                    clBulletItem["properties"] = clBulletProps;
                    QJsonObject clBullets;
                    clBullets["type"]  = "array";
                    clBullets["items"] = clBulletItem;
                    clBullets["description"] = QStringLiteral(
                        "op:\"add_subsection\" — optional bullets rendered "
                        "under the subsection prose, each `- **summary** (id)` "
                        "+ optional indented body. Each item is {summary "
                        "(required), body?, id?} (a bare string is accepted as "
                        "the summary).");

                    // ANTS-2136 — dry_run preview (parity with
                    // roadmap_log): resolve the would-be insert (id,
                    // category routing, rendered bullet, line) without
                    // writing CHANGELOG.md.
                    QJsonObject clDryRun;
                    clDryRun["type"] = "boolean";
                    clDryRun["description"] = QStringLiteral(
                        "Optional. When true, return the resolved insert "
                        "preview — `category`, `line`, `bytes`, and the "
                        "rendered `bullet` (add_batch: `applied`/`skipped` "
                        "with `bytes`) — WITHOUT writing CHANGELOG.md or "
                        "creating a category heading. A free pre-flight to "
                        "verify category routing and prose before "
                        "committing (envelope carries dry_run:true).");

                    QJsonObject clProps;
                    clProps["caller_cwd"] = clCaller;
                    clProps["op"]         = clOp;
                    {   // ANTS-4363
                        QJsonObject v; v["type"] = "string";
                                       v["description"] = QStringLiteral(
                            "op:\"release\" — the version to stamp, WITHOUT "
                            "brackets (they are added for you; a bracketed "
                            "value refuses bad_args rather than writing "
                            "`## [[1.2.3]]`, which a notes-extraction grep "
                            "would miss). The version STRING is otherwise "
                            "unconstrained, so a `-rc1` suffix or a "
                            "date-version is fine — what is fixed is the "
                            "heading shape.");
                        clProps["version"] = v;
                        // ANTS-5484
                        QJsonObject d; d["type"] = "boolean";
                                       d["description"] = QStringLiteral(
                            "op:\"release\" — close under `## <date>` for a "
                            "project with no versions (changelog-format.md "
                            "4.1); takes no `version`. A second close the same "
                            "day merges into that day's section category by "
                            "category, new bullets first, and replies "
                            "`merged:true`. Refuses `merge_unsupported` when "
                            "[Unreleased] holds text outside a category block.");
                        clProps["dated"] = d;
                        // ANTS-5522
                        QJsonObject th; th["type"] = "string";
                                        th["description"] = QStringLiteral(
                            "release: one line written as **Theme:** under the "
                            "new version heading. Not with dated.");
                        clProps["theme"] = th;
                    }
                    clProps["summary"]    = clSummary;
                    clProps["category"]   = clCategory;
                    clProps["kind"]       = clKind;
                    clProps["body"]       = clBody;
                    clProps["id"]         = clId;
                    clProps["entries"]    = clEntries;
                    clProps["headline"]   = clHeadline;
                    clProps["date"]       = clDate;
                    clProps["bullets"]    = clBullets;
                    clProps["dry_run"]    = clDryRun;
                    {   // ANTS-5246 — op:"amend" operands.
                        QJsonObject o;
                        o["type"] = "string";
                        o["description"] = QStringLiteral(
                            "amend: the exact phrase under [Unreleased]; it must "
                            "occur once, and may span a hard wrap.");
                        clProps["old_text"] = o;
                        QJsonObject n;
                        n["type"] = "string";
                        n["description"] = QStringLiteral(
                            "amend: the replacement; empty deletes the phrase.");
                        clProps["new_text"] = n;
                    }
                    {   // ANTS-4561
                        QJsonObject mb; mb["type"] = "integer";
                        mb["description"] = QStringLiteral(
                            "op:\"release\" — cap on the echoed "
                            "`released_body` in bytes (default 16384, clamped "
                            "to [2000, 1048576]; pass 0 for NO cap). The "
                            "closed section is returned so a caller has its "
                            "release notes without re-reading the file, but a "
                            "project that has been skipping the promotion "
                            "step accumulates an enormous `[Unreleased]` — one "
                            "measured at ~11,100 lines came back as 621,306 "
                            "characters, over the tool-result ceiling, so the "
                            "one changelog edit every release makes could not "
                            "be performed at all (dry_run renders the same "
                            "body, so it was no escape). A truncated body is "
                            "ANNOUNCED with `released_body_truncated` and "
                            "`released_body_bytes` and carries a marker "
                            "naming the read_region call that returns the "
                            "whole section \u2014 it is never silently clipped.");
                        clProps["max_body_bytes"] = mb;
                    }

                    // ANTS-4812 — reach a nested component changelog.
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "Optional. The changelog to act on, "
                            "project-relative. Omit for the usual behaviour: "
                            "discovery walks up from caller_cwd to the repo "
                            "boundary and finds the root CHANGELOG.md. Pass it "
                            "when a project ships a SECOND changelog for a "
                            "separately-versioned bundled component "
                            "(tools/audit/CHANGELOG.md), which discovery "
                            "cannot reach \\u2014 a nested caller_cwd resolves "
                            "back to the root file, so that file's only route "
                            "was a raw edit, losing the atomic write, category "
                            "routing, format validation and the [Unreleased] "
                            "guard on exactly the file most likely to drift. "
                            "It names an EXISTING changelog and never creates "
                            "one: a path that is not a file refuses "
                            "no_changelog. Validated under the project root, "
                            "so a root-escaping path refuses bad_path. The "
                            "Markdown/YAML probe still runs on whatever file "
                            "is handed over, so a nested file in a convention "
                            "this writer cannot parse refuses format_mismatch "
                            "as it does today. changelog_query takes the same "
                            "argument, or the read side cannot check what the "
                            "write side just wrote.");
                        clProps["path"] = p;
                    }

                    QJsonObject clSchema;
                    clSchema["type"] = "object";
                    clSchema["properties"] = clProps;
                    // Only caller_cwd is unconditionally required; the
                    // per-op needs (summary for add, id for
                    // add_from_roadmap) are enforced at runtime via
                    // missing_field, mirroring roadmap_log.
                    QJsonArray clReq;
                    clReq.append(QStringLiteral("caller_cwd"));
                    clSchema["required"] = clReq;
                    clSchema["additionalProperties"] = false;
                    t["inputSchema"] = clSchema;
                    tools.append(t);
                }
                // ANTS-3533 — changelog_query: read-only structured CHANGELOG
                // reader, the symmetric read side of changelog_log.
                {
                    QJsonObject t;
                    t["name"] = "changelog_query";
                    t["description"] = QStringLiteral(
                        "Read CHANGELOG.md as structured entries {version, date, "
                        "unreleased, category, text, ids[], body?} instead of the whole "
                        "file. Filters: version, category, query, id / ids[]. mode: "
                        "entries (default) | version_index | headline_only | lint "
                        "(read-only format check, findings[]). Opt-in: "
                        "include_body, offset/limit (1..500). caller_cwd Required. "
                        "Refusals: no_changelog, format_mismatch, bad_version, "
                        "bad_category, bad_mode, bad_mode_combo, bad_case, bad_args.");
                    t["detail"] = QStringLiteral(
                        "Read CHANGELOG.md as structured entries {version, date, "
                        "unreleased, category, text, ids[], body?} instead of "
                        "full-reading the file — the symmetric read side of "
                        "changelog_log, mirroring roadmap_query. Filters: "
                        "version=<token|Unreleased>, category=<Added|Changed|"
                        "Deprecated|Removed|Fixed|Security>, query=<keyword>, "
                        "id / ids[] (look up entries citing a [PROJ-NNNN]). "
                        "mode: entries (default) | version_index (version "
                        "skeleton + per-category counts, no entries) | "
                        "headline_only (~10x smaller) | lint (ANTS-5543: a "
                        "READ-ONLY format check returning findings[] — prose or "
                        "a bullet above a section's first ### category, an "
                        "unknown or out-of-order category, flush-left prose "
                        "inside a category block; pass version:\"Unreleased\" "
                        "from a push gate, and run it from a shell as "
                        "`ants-mcpd --call changelog_query '{\"mode\":\"lint\"}' "
                        "--exit-code`, which exits 3 on a finding). Opt-in: include_body, "
                        "compact, fields, etag_match, offset/limit (1..500). "
                        "caller_cwd Required. Refusals: no_changelog, "
                        "format_mismatch, bad_version, bad_category, bad_mode, "
                        "bad_mode_combo, bad_case, bad_args. Etag-304 aware.");
                    t["selection_hint"] = QStringLiteral(
                        "Use to look up CHANGELOG.md entries by version / id / "
                        "category / keyword without reading the whole file — "
                        "the read side of changelog_log (drift checks, "
                        "\"what shipped under ANTS-NNNN?\").");

                    QJsonObject cqVersion;
                    cqVersion["type"] = "string";
                    cqVersion["description"] = QStringLiteral(
                        "Filter to one version block. The bare version token "
                        "(\"0.7.100\") or \"Unreleased\" (case-insensitive). "
                        "Empty = all versions. Unknown → bad_version.");

                    // ANTS-3618 — `section` is an exact alias for `version`
                    // (a CHANGELOG's `## [X.Y.Z]` blocks ARE its sections).
                    // Declared so the dispatch layer stops flagging it in
                    // ignored_args and, more importantly, so it actually
                    // filters: callers arriving from roadmap_query reach for
                    // its vocabulary and passing `section` used to return
                    // every entry, which reads as a filter that matched
                    // everything rather than one never applied.
                    QJsonObject cqSection;
                    cqSection["type"] = "string";
                    cqSection["description"] = QStringLiteral(
                        "Alias for `version` — same values, same refusals. "
                        "Passing both with different values → bad_args.");

                    QJsonObject cqCategory;
                    cqCategory["type"] = "string";
                    QJsonArray cqCatEnum;
                    cqCatEnum.append("Added");
                    cqCatEnum.append("Changed");
                    cqCatEnum.append("Deprecated");
                    cqCatEnum.append("Removed");
                    cqCatEnum.append("Fixed");
                    cqCatEnum.append("Security");
                    cqCategory["enum"] = cqCatEnum;
                    cqCategory["description"] = QStringLiteral(
                        "Keep-a-Changelog category filter (case-sensitive). "
                        "Ignored in version_index mode. Unknown → bad_category.");

                    QJsonObject cqQuery;
                    cqQuery["type"] = "string";
                    cqQuery["description"] = QStringLiteral(
                        "Case-insensitive substring over each entry's text + "
                        "body (capped 200 chars). Composes with version/"
                        "category as a conjunction.");

                    QJsonObject cqId;
                    cqId["type"] = "string";
                    cqId["description"] = QStringLiteral(
                        "Look up entries citing a single [PROJ-NNNN] id. "
                        "Overrides version/category/query + pagination. A "
                        "genuinely-absent id → found:false (not a refusal); a "
                        "case-only mismatch → bad_case. Mutually exclusive with "
                        "ids. Cannot combine with mode:version_index.");

                    QJsonObject cqIds;
                    cqIds["type"] = "array";
                    QJsonObject cqIdsItem;
                    cqIdsItem["type"] = "string";
                    cqIds["items"] = cqIdsItem;
                    cqIds["description"] = QStringLiteral(
                        "Batch id lookup (array or comma/space string; deduped; "
                        ">100 → bad_args). A case-only mismatch is demoted to "
                        "missing_ids (no batch refusal). Mutually exclusive "
                        "with id.");

                    QJsonObject cqMode;
                    cqMode["type"] = "string";
                    QJsonArray cqModeEnum;
                    cqModeEnum.append("entries");
                    cqModeEnum.append("version_index");
                    cqModeEnum.append("headline_only");
                    cqModeEnum.append("lint");
                    cqMode["enum"] = cqModeEnum;
                    cqMode["description"] = QStringLiteral(
                        "Response mode. \"entries\" (default) returns filtered "
                        "entries; \"version_index\" returns the version "
                        "skeleton with per-category counts (no entries); "
                        "\"headline_only\" narrows each entry to {version, "
                        "category, text_oneline, ids} (~10x smaller); \"lint\" "
                        "checks the file's format and returns findings[] "
                        "(always present, empty when clean) without writing. "
                        "It honours `version` and refuses id / ids.");

                    QJsonObject cqIncludeBody;
                    cqIncludeBody["type"] = "boolean";
                    cqIncludeBody["description"] = QStringLiteral(
                        "entries mode only: attach each entry's continuation "
                        "body. Default false. Ignored in headline_only / "
                        "version_index.");

                    QJsonObject cqOffset;
                    cqOffset["type"] = "integer";
                    cqOffset["description"] = QStringLiteral(
                        "0-based pagination start over the post-filter list.");

                    QJsonObject cqLimit;
                    cqLimit["type"] = "integer";
                    cqLimit["description"] = QStringLiteral(
                        "Page size cap, clamped 1..500, still cut to the soft "
                        "cap (mirrors roadmap_query). "
                        "Omitted → auto soft-cap; truncation emits truncated + "
                        "next_offset. ANTS-3576 — in entries mode (not "
                        "headline_only / include_body), a would-be-truncated "
                        "page auto-downshifts to the lean headline_only entry "
                        "shape {version, category, ids, text_oneline} and "
                        "re-pages so the tail is kept; the response then carries "
                        "downshifted:true and its rows are headline-shaped.");

                    QJsonObject cqEncoding;
                    cqEncoding["type"] = "string";
                    QJsonArray cqEncEnum;
                    cqEncEnum.append("json");
                    cqEncEnum.append("tabular");
                    cqEncoding["enum"] = cqEncEnum;
                    cqEncoding["description"] = QStringLiteral(
                        "Optional (ANTS-2090). \"tabular\" packs the entries/"
                        "versions array columnar (30-60% smaller). Default "
                        "\"json\".");

                    QJsonObject cqProps;
                    cqProps["caller_cwd"]   = makeCallerCwdReadProp();
                    cqProps["version"]      = cqVersion;
                    cqProps["section"]      = cqSection;        // ANTS-3618
                    cqProps["category"]     = cqCategory;
                    cqProps["query"]        = cqQuery;
                    cqProps["id"]           = cqId;
                    cqProps["ids"]          = cqIds;
                    cqProps["mode"]         = cqMode;
                    cqProps["include_body"] = cqIncludeBody;
                    // ANTS-5507 — lint's release checks against the store and git.
                    QJsonObject cqRelease;
                    cqRelease["type"] = "boolean";
                    cqRelease["description"] = QStringLiteral(
                        "lint only: also check against the roadmap store and the newest "
                        "git tag (cited ids shipped, shipped ids cited, tag has a section).");
                    cqProps["release"] = cqRelease;
                    QJsonObject cqSince;
                    cqSince["type"] = "string";
                    cqSince["description"] = QStringLiteral(
                        "release:true only: YYYY-MM-DD window start for shipped ids "
                        "(default: the newest dated release section).");
                    cqProps["since"] = cqSince;
                    cqProps["offset"]       = cqOffset;
                    cqProps["limit"]        = cqLimit;
                    cqProps["etag_match"]   = makeEtagMatchProp();  // ANTS-1499
                    cqProps["fields"]       = makeFieldsProp();     // ANTS-1720
                    cqProps["compact"]      = makeCompactProp();    // ANTS-2091
                    cqProps["encoding"]     = cqEncoding;           // ANTS-2090
                    // ANTS-4812 — same `path` as changelog_log, or the read
                    // side cannot check what the write side just wrote.
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "Optional. The changelog to read, "
                            "project-relative. Omit to discover the root "
                            "CHANGELOG.md by walking up from caller_cwd. Pass "
                            "it to read a nested component changelog that "
                            "discovery cannot reach. Names an EXISTING file: a "
                            "path that is not a file refuses no_changelog, and "
                            "a root-escaping one refuses bad_path.");
                        cqProps["path"] = p;
                    }

                    QJsonObject cqSchema;
                    cqSchema["type"] = "object";
                    cqSchema["properties"] = cqProps;
                    QJsonArray cqReq;
                    cqReq.append(QStringLiteral("caller_cwd"));
                    cqSchema["required"] = cqReq;
                    cqSchema["additionalProperties"] = false;
                    t["inputSchema"] = cqSchema;
                    tools.append(t);
                }
                // ANTS-1399-INV-1 — tool_info(name) descriptor.
                // Cheaper than re-fetching tools/list when the assistant
                // only needs to refresh memory on one tool's schema.
                // Self-registers so tool_info({name:"tool_info"})
                // round-trips like every other listed tool.
                {
                    QJsonObject t;
                    t["name"] = "tool_info";
                    t["description"] = QStringLiteral(
                        "Fetch a single MCP tool's descriptor — name, "
                        "description, inputSchema. Cheaper than a full "
                        "tools/list refresh. "
                        "Unknown name returns code=unknown_tool plus "
                        "an `available` list of registered tool names. "
                        "Empty/missing name returns code=missing_name. "
                        "Cache cold (no prior tools/list) returns "
                        "code=tools_not_ready. ANTS-1985: pass "
                        "catalog:true (no name) to get the full verb "
                        "catalog — every registered tool grouped by "
                        "category with its one-line selection_hint, in "
                        "one call. See docs/specs/ANTS-1399-tool-info-name-mcp-verb.md + "
                        "ANTS-1985-tool-info-catalog-true-one.md.");
                    t["selection_hint"] = QStringLiteral(
                        "Use to fetch one tool's descriptor without "
                        "re-paying for the full tools/list snapshot, "
                        "or catalog:true for the "
                        "whole toolkit grouped by category (ANTS-1985). "
                        "Surfaces selection_hint field (ANTS-1453).");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject nameProp;
                    nameProp["type"] = "string";
                    nameProp["description"] = QStringLiteral(
                        "Registered tool name (e.g. "
                        "\"verify_changes\", \"file_outline\"). Omit "
                        "when catalog:true.");
                    // ANTS-1985 — catalog mode: return every verb
                    // grouped by category instead of one descriptor.
                    QJsonObject catalogProp;
                    catalogProp["type"] = "boolean";
                    catalogProp["description"] = QStringLiteral(
                        "When true, ignore `name` and return the full "
                        "verb catalog: {ok, catalog:{<category>:"
                        "[{name, selection_hint}]}, tool_count, "
                        "category_count}. One call replaces ~70 "
                        "per-tool probes (ANTS-1985).");
                    QJsonObject props;
                    props["name"] = nameProp;
                    props["catalog"] = catalogProp;
                    schema["properties"] = props;
                    // ANTS-1985 — neither arg is unconditionally
                    // required: name-mode needs `name`, catalog-mode
                    // needs `catalog:true`. The handler enforces the
                    // choice at runtime, so `required` is empty (was
                    // `["name"]` pre-1985).
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-1430 — project_layout. Pre-cached project file
                // layout (ROADMAP/CHANGELOG/specs/standards/decisions/
                // appstream/counter). Read verb; scans the project tree
                // when the cache is missing, expired (7-day TTL), or
                // any probed path's mtime has advanced. Persists via
                // session_memory under the well-known key
                // `project_layout`. See docs/specs/ANTS-1430-session-memory-project-layout-scan.md.
                {
                    QJsonObject t;
                    t["name"] = "project_layout";
                    t["description"] = QStringLiteral(
                        "Per-project file-layout cache: where the roadmap, changelog, "
                        "specs, standards, ADRs, AppStream metainfo and counter file "
                        "live. Returns {roadmap, changelog, specs_dir, standards_dir, "
                        "adr_dir, appstream_metainfo, counter_file, cached}. Cached for "
                        "7 days and invalidated by an mtime change; force_rescan:true "
                        "bypasses. caller_cwd required.");
                    t["detail"] = QStringLiteral(
                        "Per-project file-layout cache. Returns "
                        "{roadmap:{path,format,bullet_count_estimate,"
                        "size_bytes,mtime_ms},changelog:{path,"
                        "size_bytes,mtime_ms},specs_dir,standards_dir,"
                        "adr_dir,appstream_metainfo,counter_file,"
                        "cached:bool}. First call scans (cached:false), "
                        "subsequent within 7-day TTL return the cache "
                        "(cached:true). mtime change on any probed path "
                        "invalidates. Pass force_rescan:true to bypass. "
                        "caller_cwd required (Required contract — "
                        "tenant-hashed storage). ANTS-1435: reads "
                        "anchor to your caller_cwd directly — no "
                        "focused-tab match needed (cross-tab queries "
                        "work). ANTS-1493: probe set widened to also "
                        "cover docs/private/, docs/internal/, "
                        "docs/fork/ (fork-only doc trees) and "
                        "*.metainfo.xml at repo root + packaging/ + pkg/ + "
                        "data/ + share/applications/ — first hit per field "
                        "wins. ANTS-4439: also one level UNDER packaging/ and "
                        "pkg/ (packaging/obs/, packaging/flatpak/ — the "
                        "multi-distro layout), and the legacy *.appdata.xml "
                        "spelling, which is probed after every *.metainfo.xml "
                        "candidate so the current name always wins. "
                        "ANTS-5106: standards_files keeps a sorted prefix "
                        "of 5 KiB of paths so the cache entry fits; a cut "
                        "adds standards_files_truncated:true and "
                        "standards_files_total.");
                    t["selection_hint"] = QStringLiteral(
                        "Use as first call when you need to locate "
                        "ROADMAP/CHANGELOG/specs/standards in an "
                        "unfamiliar project layout. Cached.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject forceProp;
                    forceProp["type"] = "boolean";
                    forceProp["description"] = QStringLiteral(
                        "If true, bypasses TTL+mtime check and "
                        "re-scans unconditionally. Default false.");
                    QJsonObject callerProp;
                    callerProp["type"] = "string";
                    callerProp["description"] = QStringLiteral(
                        "Your $PWD. REQUIRED — Required contract "
                        "refuses with caller_cwd_required when empty.");
                    QJsonObject props;
                    props["force_rescan"] = forceProp;
                    props["caller_cwd"]   = callerProp;
                    props["etag_match"]   = makeEtagMatchProp();   // ANTS-1499
                    props["fields"]       = makeFieldsProp();      // ANTS-1720
                    props["compact"]      = makeCompactProp();     // ANTS-2091
                    schema["properties"]  = props;
                    QJsonArray req;
                    req.append(QStringLiteral("caller_cwd"));
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-1283 — session_memory KV. Per-cwd key-value
                // persistence backed by
                // ~/.cache/ants-terminal/mcp-state/<cwd-hash>.json.
                // Pure delegation to SessionMemoryEngine::execute.
                {
                    QJsonObject t;
                    t["name"] = "session_memory";
                    t["description"] = QStringLiteral(
                        "Per-cwd key-value store the MCP server backs "
                        "to ~/.cache/ants-terminal/mcp-state/"
                        "<cwd-hash>.json. Use for cross-session "
                        "caches: last audit timestamp, partition "
                        "snapshots, tool-detection results. 100 KiB "
                        "cap per cwd; 16 KiB cap per value. Required: "
                        "op (\"get\"/\"set\"/\"delete\"/\"list\"), "
                        "caller_cwd (your $PWD). ANTS-1435: read ops "
                        "(get, list) anchor to your caller_cwd "
                        "directly — cross-tab reads work. Write ops "
                        "(set, delete) require caller_cwd to match "
                        "the focused Ants tab's cwd (prevents "
                        "confused-deputy writes). key required for "
                        "get/set/delete; value required for set. See "
                        "docs/specs/ANTS-1283-session-memory-mcp-key-value.md + "
                        "docs/specs/ANTS-1336-session-memory-cwd-arg-cross.md + "
                        "docs/specs/ANTS-1435-session-memory-project-layout-reads.md.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject opProp;
                    opProp["type"] = "string";
                    QJsonArray   opEnum;
                    opEnum.append(QStringLiteral("get"));
                    opEnum.append(QStringLiteral("set"));
                    opEnum.append(QStringLiteral("delete"));
                    opEnum.append(QStringLiteral("list"));
                    opProp["enum"] = opEnum;
                    opProp["description"] = QStringLiteral(
                        "Operation: get / set / delete / list.");
                    QJsonObject keyProp;
                    keyProp["type"] = "string";
                    keyProp["description"] = QStringLiteral(
                        "Key, ^[A-Za-z0-9._-]{1,64}$. Required for "
                        "get/set/delete.");
                    QJsonObject valProp;
                    // No type constraint — any JSON value accepted.
                    valProp["description"] = QStringLiteral(
                        "Any JSON value (≤ 16 KiB serialised). "
                        "Required for set.");
                    // ANTS-1389 — surface the caller_cwd gate.
                    // ANTS-1336 promoted caller_cwd to required for
                    // every op (previously required only on
                    // set/delete; the read-side bypass was the
                    // tenancy leak).
                    QJsonObject callerProp;
                    callerProp["type"] = "string";
                    callerProp["description"] = QStringLiteral(
                        "Your $PWD, required on every op. get and list "
                        "read from it directly; set and delete refuse "
                        "on mismatch with the focused tab's cwd.");
                    t["selection_hint"] = QStringLiteral(
                        "Use to persist tiny per-project notes "
                        "across MCP calls (KB-grain). Self-scoped "
                        "per project; not for large data.");
                    QJsonObject props;
                    props["op"]         = opProp;
                    props["key"]        = keyProp;
                    props["value"]      = valProp;
                    props["caller_cwd"] = callerProp;
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append(QStringLiteral("op"));
                    req.append(QStringLiteral("caller_cwd"));
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
}

}  // namespace claudeintegrationdetail
