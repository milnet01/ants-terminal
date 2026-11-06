// ANTS-1677 claudeintegration piece 3/7 — the code navigation tool descriptors
#include "claudeintegration_internal.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace claudeintegrationdetail {

void appendToolSchemasCodeNav(QJsonArray &tools) {
                // ANTS-1636 — find_sources: topic-to-files discovery.
                {
                    QJsonObject t;
                    t["name"] = "find_sources";
                    t["description"] = QStringLiteral(
                        "Map a free-text topic (\"audit cache invalidation\") to a ranked "
                        "list of source files under src/ + tests/. Use when you do not "
                        "know the symbol or filename; for an exact symbol use "
                        "workspace_search or find_definition. Returns {ok, "
                        "files:[{path, score, role, evidence}], files_count, "
                        "unmatched_terms, files_scanned, truncated}; role is "
                        "impl|header|test. Required: topic, caller_cwd. Optional "
                        "max_results (default 20, cap 100).");
                    t["detail"] = QStringLiteral(
                        "Map a free-text topic (\"audit cache "
                        "invalidation\", \"test audit fold-in\", "
                        "\"model auto switch actuator\") to a ranked "
                        "list of source files under the project's "
                        "src/ + tests/ trees. Use this when you don't "
                        "already know the symbol/filename and would "
                        "otherwise run 3-4 grep/find cycles. Returns "
                        "{ok, files:[{path, score, role, evidence}], "
                        "files_count, unmatched_terms, files_scanned, "
                        "truncated}. `role` ∈ impl|header|test; "
                        "`evidence` carries up to 3 short \"matched "
                        "filename …\" / \"\\\"…\\\" × N in body\" "
                        "explanations. Required: `topic`, `caller_cwd`. "
                        "Optional `max_results` (default 20, server "
                        "hard-cap 100). Compared to workspace_search: "
                        "this tool expects a topic, not an exact "
                        "symbol — it auto-expands snake/camel/dropped-"
                        "separator variants and ranks files (not "
                        "lines). Token-savings: typically replaces a "
                        "3-4 round-trip grep + read cycle with one "
                        "MCP call.");
                    // ANTS-3619 — lead with the language limit. The
                    // envelope already explains it on a zero-result, but
                    // that is after the fact: files_count:0 on a Python
                    // project is indistinguishable from a genuine
                    // "nothing calls this", which is the worst possible
                    // wrong answer when checking a change's blast radius.
                    // Must start with "Use " and stay under 240 chars
                    // (ANTS-1897 INV-7 / ANTS-1453 HINT-3) — the
                    // qualifier has to fit that budget, so it leads and
                    // the "prefer find_definition for known symbols"
                    // advice moves to the description above.
                    t["selection_hint"] = QStringLiteral(
                        "Use for topic discovery in C/C++ ONLY — on "
                        "Python/JS/Go it returns files_count:0, which "
                        "reads as \"no callers\" but is not an answer; "
                        "use codebase_index or workspace_search there. "
                        "find_definition IS multi-language — do not copy "
                        "its reach.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    {
                        QJsonObject p; p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "Free-text topic. Tokens shorter than 3 "
                            "chars and roadmap IDs (ANTS-NNNN) are "
                            "ignored.");
                        props["topic"] = p;
                    }
                    {
                        // ANTS-3415 — `symbol` alias for `topic`. A session
                        // reaching for the "who defines/uses X" verb by
                        // symbol name (natural from the catalog one-liner)
                        // was refused bad_args; accept it as an alias so the
                        // call succeeds. Both together refuse (ANTS-6365). For
                        // call-sites of a known symbol, prefer find_caller.
                        QJsonObject p; p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "Alias for `topic` (ANTS-3415). Sending both "
                            "refuses bad_op_combo. To list the "
                            "call-sites of a known symbol, prefer find_caller.");
                        props["symbol"] = p;
                    }
                    {
                        QJsonObject p; p["type"] = "integer";
                        p["default"] = 20;
                        p["description"] = QStringLiteral(
                            "Cap on files[] (default 20, hard-cap 100). "
                            "`files_count` carries the post-cap count; "
                            "`truncated` is true when the pre-cap list "
                            "was longer.");
                        props["max_results"] = p;
                    }
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    props["encoding"] = makeEncodingProp();      // ANTS-2090
                    schema["properties"] = props;
                    QJsonArray req; req.append(QStringLiteral("topic"));
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-3368 — co_change_family: the grouped edit-site
                // checklist for one settings-backed field.
                {
                    QJsonObject t;
                    t["name"] = "co_change_family";
                    t["description"] = QStringLiteral(
                        "Given one exemplar settings field, list every edit site you "
                        "must touch to mirror it, grouped by file, including derived "
                        "names (setX, m_X, XChanged) a whole-word search misses. "
                        "Returns {ok, stems, stem_words, min_run, files:[{path, "
                        "sites:[...]}], files_count, sites_count, truncated}; `role` is "
                        "lexical. Required: caller_cwd plus `stem` or `stems`. "
                        "Refusals: bad_args, no_project, rg_failed.");
                    t["detail"] = QStringLiteral(
                        "Given one exemplar settings field, list every edit "
                        "site you must touch to mirror it — grouped by file, "
                        "with the JSON string key and the derived names "
                        "(setX, m_X, XChanged) a whole-word symbol search "
                        "misses. Matches on the longest run of the stem's "
                        "words that is contiguous in both names, so "
                        "setMcpEnabled still belongs to claudeMcpEnabled. "
                        "Returns {ok, stems, stem_words, min_run, "
                        "files:[{path, sites:[{line, stem, name, role, run, "
                        "run_len, text}]}], files_count, sites_count, "
                        "truncated}. `role` is LEXICAL, one of "
                        "json_key|member|mutator|signal|type|reference. "
                        "Required: `caller_cwd` plus `stem` or `stems`. "
                        "Refusals: bad_args, no_project, rg_failed. "
                        "Scans the whole repo (minus .gitignore), unlike "
                        "find_sources."
                        " "
                        "min_run is the shortest accepted word-run, resolved "
                        "per stem against that stem's own word count: it "
                        "defaults to min(2, words) and clamps to 1..words. "
                        "Setting it to 1 widens the SCAN, not just the "
                        "filter — the default pattern alternates adjacent "
                        "word pairs and can never produce a one-word match, "
                        "so min_run:1 is what reaches a site sharing a "
                        "single word (audioLod from lodEnabled). That is "
                        "much more expensive on a common word. A run of "
                        "nothing but stopwords (get/set/is/has/on/off/"
                        "enabled/disabled/value/data/flag/count/size/index/"
                        "name/type/mode/m/p) is dropped, and a stem made "
                        "only of stopwords refuses bad_args rather than "
                        "returning a silent empty result. One row per "
                        "(path, line): a line matching several stems is "
                        "emitted once, owned by the longest run, ties by "
                        "position in stems. Files are ordered by their "
                        "maximum run_len descending then path; sites by "
                        "line. When max_sites binds, the sites retained are "
                        "those with the highest run_len — never the first N "
                        "in scan order — and truncated is set. truncated is "
                        "also set when the ripgrep budget is exhausted; "
                        "rg_failed is reserved for a scanner that did not "
                        "run at all. Contract: docs/specs/"
                        "ANTS-3368-co-change-family.md.");
                    t["selection_hint"] = QStringLiteral(
                        "Use when mirroring an existing settings field and "
                        "you need every file to touch. find_sources returns "
                        "ranked files under src/+tests/; this returns lines "
                        "repo-wide, including the JSON key and docs.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    {
                        QJsonObject p; p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "One exemplar name — an identifier "
                            "(claudeMcpEnabled) or a config key "
                            "(claude.mcp_enabled). Sugar for a one-element "
                            "`stems`; sending both refuses bad_op_combo. Must "
                            "match ^[A-Za-z0-9_.-]+$.");
                        props["stem"] = p;
                    }
                    {
                        QJsonObject p; p["type"] = "array";
                        QJsonObject items; items["type"] = "string";
                        p["items"] = items;
                        p["description"] = QStringLiteral(
                            "The field group; the union of each stem's "
                            "family. Each site carries the stem that owns "
                            "it. Required unless `stem` is given.");
                        props["stems"] = p;
                    }
                    {
                        QJsonObject p; p["type"] = "integer";
                        p["description"] = QStringLiteral(
                            "Shortest accepted word-run (default "
                            "min(2, stem words), clamped per stem to "
                            "1..words, never refused). 1 widens the scan "
                            "itself — see tool_info detail.");
                        props["min_run"] = p;
                    }
                    {
                        QJsonObject p; p["type"] = "integer";
                        p["default"] = 200;
                        p["description"] = QStringLiteral(
                            "Cap on sites (default 200, clamped to "
                            "1..1000). When it binds, the highest-run_len "
                            "sites are kept and `truncated` is true.");
                        props["max_sites"] = p;
                    }
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    props["etag_match"] = makeEtagMatchProp();
                    props["fields"]     = makeFieldsProp();
                    props["compact"]    = makeCompactProp();     // ANTS-4657
                    // No `encoding` prop: the columnar repack is for a
                    // top-level array of FLAT objects, and files[] carries a
                    // nested sites[] per row. ANTS-3368 § 2.4 does not
                    // declare it either.
                    schema["properties"] = props;
                    // `stem`/`stems` is an either-or that required[] cannot
                    // express, so it is a runtime check refusing bad_args.
                    QJsonArray req;
                    req.append(QStringLiteral("caller_cwd"));
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-1248: workspace_search — ripgrep wrapper. The
                // schema marks `pattern` as required. The description
                // names the alternative bash idiom + token-saving headline so Claude prefers
                // this over Bash/Read for code searches.
                QJsonObject wsTool;
                wsTool["name"] = "workspace_search";
                // On the wire, kEagerDescription below replaces this text.
                wsTool["description"] = QStringLiteral(
                    "Search the project for code matching a literal string "
                    "or regex — prefer over `Bash grep -r` (saves "
                    "250-4500 tokens). Returns {ok, matches:[{file, "
                    "line, text, also_at?, context_*?}], truncated, dedup, "
                    "...}. Args: pattern (required; alias query), regex, "
                    "lane, glob, max_results (cap 500), context [0,10], "
                    "case, respect_gitignore, include_hidden, dedup, "
                    "timeout_sec [1,30], max_match_bytes, headline_only, "
                    "enclosing_symbol, match_wrapped, multiline. "
                    "caller_cwd picks the tree: ANY directory. "
                    "The query is ONE pattern, not "
                    "AND-combined words — a multi-word query that hits 0 "
                    "matches returns an advisory `hint` (a single token, "
                    "or regex:true with .* between terms). A partial reply "
                    "says why in truncated_reason.");
                // ANTS-2079 — full per-arg reference in `detail` (stripped
                // from the tools/list wire; served by tool_info
                // {name:"workspace_search"}).
                wsTool["detail"] = QStringLiteral(
                    "Search the project for code matching a literal "
                    "string or regex. Returns {ok, matches:[{file, "
                    "line, text, also_at?:[{file,line}…], "
                    "context_before?:[{line,text}], "
                    "context_after?:[{line,text}]}], truncated, "
                    "dedup, dedup_collapsed, respect_gitignore, "
                    "include_hidden, timeout_sec, elapsed_ms}. Prefer "
                    "this over `Bash grep -r ...` — typically saves "
                    "250-4500 tokens per query and avoids round-trips "
                    "for no-match cases. Args: pattern (required; "
                    "alias `query` — ANTS-2041), "
                    // ANTS-4928 — the escape stated where the decision is
                    // made. A session audited every Bash grep it ran and
                    // found almost none needed to be one: another project's
                    // tree, a non-project directory, counts, distinct files,
                    // context, hidden files, ignoring gitignore all work
                    // here. What it could not tell from the description is
                    // that the verb is not confined to its own project —
                    // `lane`'s prose says lane cannot escape the root, which
                    // is true and is the sentence that misleads, because the
                    // escape is caller_cwd, one argument away.
                    "caller_cwd (REQUIRED) is what picks the tree: point it "
                    "at any directory and THAT tree is searched — another "
                    "project, or a path outside any repo entirely. Only "
                    "`lane` is confined to the root; caller_cwd is the "
                    "escape, and '~global' reaches ~/.claude/. What is NOT "
                    "covered is non-file input — `git log | grep`, `ls | "
                    "grep`, `command -v x | grep` are command output rather "
                    "than a corpus, and git_state covers the git ones. "
                    "regex (false), lane (subdir under project root), "
                    "glob, max_results (default 50, cap 500), context "
                    "(default 0, server-clamped to [0,10] — when > 0, "
                    "each match carries `context_before`/`context_after` "
                    "arrays with up to N surrounding lines per side; "
                    "ANTS-1304), case (smart/sensitive/insensitive), "
                    "respect_gitignore (default true — pass false for "
                    "stale-path audits across build outputs / "
                    "compile_commands.json / cache dirs), "
                    "include_hidden (default false — pass true to "
                    "search dotfile paths; .git/ stays excluded "
                    "regardless. ANTS-4753 — a `glob` naming a hidden "
                    "path while this is off returns a zero carrying "
                    "`hidden_paths_skipped:true` and a hint: ripgrep "
                    "prunes the directory during the walk, so the empty "
                    "result says nothing about the pattern. A `lane` "
                    "naming one is descended anyway and does not flag), "
                    "dedup (default true — collapses "
                    "near-duplicate excerpts into a single primary "
                    "match with `also_at` carrying the rest; "
                    "context is preserved on the primary only, pass "
                    "dedup:false for per-hit context; ANTS-1501), "
                    "timeout_sec (default 5, range [1,30] "
                    "— raise for mid-size projects > 2 k files; "
                    "ANTS-1565). ANTS-1876: opt-in payload knobs — "
                    "`max_match_bytes` (clip every `text` / "
                    "`headline` to N UTF-8 bytes, default 512, range "
                    "[50, 10000], 0 turns it off) and "
                    "`headline_only:true` (emit "
                    "`{file, line, headline}` triples without "
                    "`text` / `context_*` — pair with "
                    "`max_match_bytes` for ~10× wire reduction on "
                    "dense bundle sweeps). ANTS-2220: "
                    "`enclosing_symbol:true` annotates each match with "
                    "`enclosing:\"Foo::bar\"` (the function/method it "
                    "lives inside, via the file outline's "
                    "nearest-preceding symbol) — folds the post-search "
                    "\"which function?\" lookup into the search; costs "
                    "one outline scan per distinct matched file, so it "
                    "is off by default. ANTS-3537: `count_only:true` "
                    "returns {count, files_count, truncated} with "
                    "matches[] omitted — a rows-eliminated "
                    "existence/frequency mode; `count` is the true "
                    "total (uncapped by max_results). On hard-kill the "
                    "rg_failed envelope carries a `hint` field with "
                    "the three viable next steps. ANTS-5323: a partial "
                    "reply in any mode names why in `truncated_reason` — "
                    "timeout (wall budget; a `hint` names timeout_sec), "
                    "output_cap, max_results or bytes. ANTS-3549: "
                    "`files_only:true` returns {files:[{file,count}], "
                    "files_count} with no match rows — the distinct "
                    "matched-file set for \"which files reference X?\". "
                    "ANTS-3547: `offset` pages a truncated search — the reply "
                    "carries `next_offset`; pass it back as `offset` to "
                    "continue instead of re-running wider. ANTS-1390: pass "
                    "`caller_cwd: "
                    "\"~global\"` (alias `\"~claude-config\"`) to "
                    "search ~/.claude/ instead of a project root — "
                    "for sessions editing global Claude config "
                    "(skills, agents, the global CLAUDE.md).");
                wsTool["selection_hint"] = QStringLiteral(
                    "Use for vague-location queries ('where is the X "
                    "feature wired up?'). For known one-keyword bug "
                    "hunts, 3 Grep calls can still be cheaper.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    QJsonObject patternProp;  patternProp["type"]  = "string";
                                              patternProp["description"] =
                        QStringLiteral("Literal string (default) or "
                                        "regex if `regex=true`.");
                    QJsonObject regexProp;    regexProp["type"]    = "boolean";
                                              regexProp["default"] = false;
                    QJsonObject laneProp;     laneProp["type"]     = "string";
                                              laneProp["description"] =
                        QStringLiteral("Subdir under repo root (e.g. \"src\"). "
                                        "Empty = whole repo. ANTS-4569: lane "
                                        "cannot escape the project root — "
                                        "\"..\" refuses bad_path. To search a "
                                        "DIFFERENT tree, set caller_cwd to "
                                        "that tree's root and confine with "
                                        "glob.");
                    QJsonObject globProp;     globProp["type"]     = "string";
                                              globProp["description"] =
                        QStringLiteral("Ripgrep --glob filter (e.g. \"*.cpp\"). "
                                       "INCLUSION only — a leading \"!\" refuses "
                                       "bad_glob; use exclude_glob instead. "
                                       "ANTS-4672: ONE pattern, never an array "
                                       "— name several paths with a brace "
                                       "group, \"docs/{coding,testing}.md\". "
                                       "`exclude_glob` takes an array and this "
                                       "does not; that asymmetry is ripgrep's "
                                       "own shape, and a brace group works in "
                                       "both.");
                    // ANTS-3704 — the "everywhere EXCEPT here" half. Separate
                    // arg rather than a "!" in `glob`, so `glob` stays
                    // unambiguously positive at the call site.
                    QJsonObject exGlobProp;
                    { QJsonArray ty; ty.append(QStringLiteral("string"));
                                     ty.append(QStringLiteral("array"));
                      exGlobProp["type"] = ty; }
                    exGlobProp["description"] = QStringLiteral(
                        "Path glob(s) to EXCLUDE — a string or an array, each "
                        "rendered as ripgrep --glob '!<pattern>' after `glob` so "
                        "last-one-wins narrows rather than widens. Write the "
                        "pattern positively (\"docs/**\", not \"!docs/**\"). Use for "
                        "\"search everywhere but the prose\" on a doc-heavy repo, "
                        "which `lane` (one subdir) and `glob` (one positive "
                        "pattern) cannot express. ANTS-4568 — it is also how you "
                        "keep a COLD REVIEW LANE inside its permitted set. An "
                        "agent told which files it may read cannot honour that "
                        "through an unscoped search: one project-wide match "
                        "returns context lines from the very documents it was "
                        "forbidden, so a path-scoped prohibition does not survive "
                        "an unscoped search. Name those paths here rather than "
                        "relying on the instruction.");
                    QJsonObject maxProp;      maxProp["type"]      = "integer";
                                              maxProp["default"]   = 50;
                                              maxProp["maximum"]   = 500;
                    // ANTS-1304: context surfaces ±N surrounding
                    // lines on each match (context_before / context_after).
                    // Server clamp [0, 10] applied in cmdWorkspaceSearch.
                    QJsonObject ctxProp;      ctxProp["type"]      = "integer";
                                              ctxProp["default"]   = 0;
                                              ctxProp["minimum"]   = 0;
                                              ctxProp["maximum"]   = 10;
                                              ctxProp["description"] = QStringLiteral(
                        "Surrounding-lines window. When > 0 (server-"
                        "clamped to 10), each match carries "
                        "`context_before` and `context_after` arrays "
                        "of up to N `{line, text}` entries. When 0 "
                        "(default), the compact pre-1304 envelope "
                        "ships.");
                    QJsonObject caseProp;     caseProp["type"]     = "string";
                    QJsonArray caseEnum;
                    caseEnum.append("smart");
                    caseEnum.append("sensitive");
                    caseEnum.append("insensitive");
                    caseProp["enum"]    = caseEnum;
                    caseProp["default"] = "smart";
                    // ANTS-1452 — opt-ins for gitignored / hidden files.
                    // Both default to pre-1452 behaviour so existing
                    // callers are unaffected; the description points
                    // Claude at the stale-path-audit / cache-dir use
                    // case for `respect_gitignore=false`.
                    QJsonObject respectGitignoreProp;
                    respectGitignoreProp["type"]    = "boolean";
                    respectGitignoreProp["default"] = true;
                    respectGitignoreProp["description"] = QStringLiteral(
                        "When false, bypass .gitignore/.ignore "
                        "(--no-ignore-vcs --no-ignore). Use for "
                        "audits across generated files like "
                        "compile_commands.json, build/, or cache "
                        "dirs.");
                    QJsonObject includeHiddenProp;
                    includeHiddenProp["type"]    = "boolean";
                    includeHiddenProp["default"] = false;
                    includeHiddenProp["description"] = QStringLiteral(
                        "When true, search dotfile paths (--hidden). "
                        "Excludes .git/ itself regardless of this "
                        "flag.");
                    // ANTS-1501 — near-duplicate excerpt dedup. Default
                    // on; collapses identical excerpts into one primary
                    // match with `also_at: [{file, line}, …]` carrying
                    // the rest. Pass dedup:false on the rare query
                    // where the exact surrounding context per hit
                    // matters more than per-call token cost.
                    QJsonObject dedupProp;
                    dedupProp["type"]    = "boolean";
                    dedupProp["default"] = true;
                    dedupProp["description"] = QStringLiteral(
                        "When true (default), group identical "
                        "whitespace-normalised excerpts into one "
                        "primary match plus `also_at:[{file,line}, …]` "
                        "for the duplicates. Pass false to keep the "
                        "per-match verbatim output.");
                    // ANTS-1565 — per-call wall-clock budget. Default
                    // 5 s (raised from 2 s); accept integer in [1, 30].
                    // Out-of-range values fall back to the default. The
                    // effective value is echoed on every response so
                    // callers can see what they got.
                    QJsonObject timeoutSecProp;
                    timeoutSecProp["type"]    = "integer";
                    timeoutSecProp["default"] = 5;
                    timeoutSecProp["minimum"] = 1;
                    timeoutSecProp["maximum"] = 30;
                    timeoutSecProp["description"] = QStringLiteral(
                        "Wall-clock budget for the rg invocation in "
                        "seconds. Clamped to [1, 30] (out-of-range "
                        "falls back to default). Raise when a "
                        "mid-size project (> 2 k files) hard-kills "
                        "the default; fall back to `Bash rg` for "
                        "queries that need longer than 30 s.");
                    // ANTS-1293 — response byte cap. max_results bounds
                    // the count; this bounds total size. Trims matches[]
                    // from the tail and sets truncated + results_dropped.
                    QJsonObject maxBytesProp;
                    maxBytesProp["type"]    = "integer";
                    maxBytesProp["minimum"] = 1;
                    maxBytesProp["description"] = QStringLiteral(
                        "Cap on the serialized response in bytes "
                        "(default 512 KiB, server-clamped to 4 MiB). When "
                        "exceeded, trailing matches are dropped and the "
                        "envelope carries truncated:true + "
                        "results_dropped:<n> (+ bytes_cap_clamped:true if "
                        "the requested cap exceeded the ceiling). "
                        "ANTS-3543 — unless you passed mode=headline_only, "
                        "the server first downshifts the whole match set "
                        "to lean {file,line,headline} rows and re-caps, "
                        "emitting `downshifted:true` + `headline_only:true` "
                        "(so headline_only may appear on a response you "
                        "did not request) and clearing the drop signals "
                        "when the lean set is complete.");
                    // ANTS-1876 — per-match text clip. Out-of-range
                    // values clamp to [50, 10000], which guarantees at
                    // least the ellipsis + 47 bytes of payload.
                    QJsonObject mmbProp;
                    mmbProp["type"]    = "integer";
                    // ANTS-3548 — default-ON clip (512). `minimum` is 0
                    // (not 50) so the `0` opt-out sentinel is in-range /
                    // passable; the effective CLIP range is still
                    // [50, 10000] (server clamps 1..49 up to 50).
                    mmbProp["default"] = 512;
                    mmbProp["minimum"] = 0;
                    mmbProp["maximum"] = 10000;
                    mmbProp["description"] = QStringLiteral(
                        "Per-match `text` (or `headline`, if "
                        "`headline_only:true`) clip in UTF-8 bytes. "
                        "Every text-bearing field — primary `text`, "
                        "every `text` inside `context_before` / "
                        "`context_after` — is clipped to *exactly* this "
                        "many UTF-8 bytes (payload prefix + 3-byte "
                        "ellipsis \"\xE2\x80\xA6\"). Fields whose unclipped "
                        "form already fits are emitted verbatim. "
                        "ANTS-3548: **default-ON** — absent → 512 (a "
                        "token-saver clip on long/pathological lines); "
                        "pass **0 to opt out** (no clip). An explicit "
                        "value clamps to the [50, 10000] clip range. "
                        "Dedup runs BEFORE the clip so the key is "
                        "unaffected (ANTS-1876 INV-4). The effective clip "
                        "value echoes on the envelope (so a default call "
                        "carries `max_match_bytes:512`).");
                    // ANTS-1876 — headline_only summary shape:
                    // emit {file, line, headline} triples instead of
                    // the bullets-mode {file, line, text, context_*}
                    // shape. Drops context entirely; rename text →
                    // headline. Composes with `max_match_bytes` (the
                    // headline field gets clipped just like text
                    // would have).
                    QJsonObject hoProp;
                    hoProp["type"]    = "boolean";
                    hoProp["default"] = false;
                    hoProp["description"] = QStringLiteral(
                        "When true, each match emits as "
                        "`{file, line, headline}` (where `headline` "
                        "is the matched line, renamed from `text`) "
                        "plus optional `also_at` (unchanged). "
                        "`context_before` / `context_after` are "
                        "dropped even when `context > 0` was passed. "
                        "`also_at` entries remain `{file, line}` "
                        "(no text to clip). Pairs with "
                        "`max_match_bytes` for ~10× wire reduction "
                        "on dense bundle sweeps (ANTS-1876).");
                    // ANTS-2041 — `query` alias for `pattern`. Declared
                    // so the schema advertises it. `pattern` stays the
                    // canonical (required) arg; both together refuse.
                    QJsonObject queryProp;  queryProp["type"] = "string";
                    queryProp["description"] = QStringLiteral(
                        "Alias for `pattern`, the canonical, required arg. "
                        "Sending both refuses bad_op_combo.");
                    // ANTS-2220 — enclosing_symbol: annotate each match
                    // with the function/method it lives inside.
                    QJsonObject encProp;  encProp["type"] = "boolean";
                    encProp["default"] = false;
                    encProp["description"] = QStringLiteral(
                        "When true, annotate each match with "
                        "`enclosing:\"Foo::bar\"` — the function/method it "
                        "lives inside (nearest-preceding symbol from the "
                        "file outline) — folding the usual \"which function "
                        "is this in?\" follow-up into the search. Costs one "
                        "file-outline scan per distinct matched file, so it "
                        "is off by default. A match before the first symbol "
                        "(e.g. in includes) carries no `enclosing`. "
                        "ANTS-4901 — the envelope says what the annotation "
                        "managed: `enclosing_annotated` counts the rows that "
                        "got one, and `enclosing_symbol_unavailable` names the "
                        "files whose LANGUAGE has no outline (capped at 20, "
                        "with the true total in "
                        "`enclosing_symbol_unavailable_count`). Read that list "
                        "before concluding anything from a missing "
                        "`enclosing`: a row above the first symbol and a file "
                        "this verb cannot outline at all look identical on the "
                        "row, and in the second case NO row in that file can "
                        "ever be annotated. Both keys ride the opt-in only.");
                    // ANTS-3537 — count_only: rows-eliminated existence /
                    // frequency mode. Omits matches[] entirely; returns
                    // just the totals.
                    QJsonObject countOnlyProp;
                    countOnlyProp["type"] = "boolean";
                    countOnlyProp["default"] = false;
                    countOnlyProp["description"] = QStringLiteral(
                        "When true, run the search but return only "
                        "`{count, files_count, truncated, "
                        "count_only:true}` — matches[] is omitted "
                        "entirely (the rg scan still runs; the rows are "
                        "never serialised). A rows-ELIMINATED mode for "
                        "existence / frequency checks (\"is X referenced "
                        "anywhere?\", \"how many call-sites?\") that would "
                        "otherwise pay for row bodies they discard. "
                        "`count` is the TRUE total match count (uncapped "
                        "by max_results); `files_count` is the number of "
                        "files with a match. `truncated` is true only "
                        "when the scan was cut off (hard-kill / parse "
                        "budget), never merely because the row cap was "
                        "hit. Complements headline_only (row-shape trim) "
                        "and max_match_bytes (row-length trim).");
                    // ANTS-3549 — files_only: rows-eliminated "which files
                    // matched" mode. Distinct matched-file set + per-file
                    // counts; drops the match rows.
                    QJsonObject filesOnlyProp;
                    filesOnlyProp["type"] = "boolean";
                    filesOnlyProp["default"] = false;
                    filesOnlyProp["description"] = QStringLiteral(
                        "When true, return only `{files:[{file, count}], "
                        "files_count, count, truncated, files_only:true}` — "
                        "the DISTINCT set of files that matched, each with its "
                        "hit count, and NO match rows. A rows-ELIMINATED mode "
                        "for \"which files reference X?\" when you will open "
                        "the files next anyway; much smaller than "
                        "headline_only when a symbol recurs many times in one "
                        "file. `count` is the true total match count across "
                        "all files (uncapped by max_results); the file list "
                        "is complete (not capped by max_results). `truncated` "
                        "is true only when the scan was cut off (hard-kill / "
                        "parse budget). count_only (leaner still) takes "
                        "precedence if both are set. Complements count_only "
                        "(counts only) and headline_only (one line per "
                        "match).");
                    // ANTS-3547 — offset cursor: continue a truncated search
                    // instead of re-running it wider. (Named wsOffsetProp so
                    // the source-grep test can scope it to workspace_search.)
                    QJsonObject wsOffsetProp;
                    wsOffsetProp["type"] = "integer";
                    wsOffsetProp["default"] = 0;
                    wsOffsetProp["minimum"] = 0;
                    wsOffsetProp["description"] = QStringLiteral(
                        "Skip the first N matches, returning the window "
                        "[offset, offset+max_results). Use to CONTINUE a "
                        "truncated search (paging) instead of re-running it "
                        "wider — which re-scans from scratch and re-emits "
                        "every match already seen. When more matches remain, "
                        "the response carries `next_offset`; pass it back as "
                        "`offset` for the next page (mirrors roadmap_query). "
                        "Pairs with count_only (ANTS-3537): count first, then "
                        "page. Dedup applies within each page. Default 0.");
                    props["pattern"]     = patternProp;
                    props["enclosing_symbol"] = encProp;          // ANTS-2220
                    props["query"]       = queryProp;
                    props["regex"]       = regexProp;
                    props["lane"]        = laneProp;
                    props["glob"]        = globProp;
                    props["exclude_glob"] = exGlobProp;  // ANTS-3704
                    props["max_results"] = maxProp;
                    props["max_bytes"]   = maxBytesProp;
                    props["max_match_bytes"] = mmbProp;       // ANTS-1876
                    props["headline_only"]   = hoProp;        // ANTS-1876
                    props["count_only"]      = countOnlyProp; // ANTS-3537
                    props["files_only"]      = filesOnlyProp; // ANTS-3549
                    // ANTS-4388 — distinct-MATCH mode.
                    {
                        QJsonObject mo; mo["type"] = "boolean";
                                        mo["default"] = false;
                                        mo["description"] = QStringLiteral(
                            "Optional (ANTS-4388). Return the DISTINCT MATCHED "
                            "SUBSTRINGS instead of rows: {matches:[{text, "
                            "count, files_count}], distinct_count}. Every other "
                            "trim here is row-shaped (count_only drops rows, "
                            "files_only gives one row per file, headline_only "
                            "thins rows, max_match_bytes shortens rows), so "
                            "\"what is the SET of X in this tree\" had no "
                            "answer. `dedup` is NOT this — its key is the "
                            "whitespace-normalised LINE, so two lines carrying "
                            "the same token stay two rows and one line carrying "
                            "two tokens stays one. Nor is it count_only, which "
                            "counts occurrences of ONE pattern; this asks which "
                            "distinct strings that pattern matched. The cap "
                            "applies to DISTINCT VALUES, not occurrences, so a "
                            "3-string answer is never truncated by a 50-row "
                            "limit; `count` / `files_count` come from the full "
                            "uncapped scan. Best with regex:true. count_only "
                            "wins if both are set.");
                        props["matches_only"] = mo;
                    }
                    {
                        QJsonObject mw; mw["type"] = "boolean";
                                        mw["default"] = false;
                                        mw["description"] = QStringLiteral(
                            "Optional (ANTS-4547). Match a quotation that is "
                            "HARD-WRAPPED in the file: a run of whitespace in "
                            "`pattern` matches a run of whitespace and markdown "
                            "blockquote markers in the text, so a sentence "
                            "spanning a line break is found and `line` reports "
                            "where the span STARTS. Prose here wraps at ~70 "
                            "columns, so a line-oriented search misses text "
                            "that is present and exact — and for a review gate "
                            "that DISMISSES a finding whose quote it cannot "
                            "locate, that miss is indistinguishable from a "
                            "defect, so the real defect ships. Normalisation is "
                            "TWO-SIDED: paste the quotation with its own "
                            "newlines, indentation and `>` markers and it still "
                            "matches unmarked text. Each row's `text` is folded "
                            "to one line. LITERAL only — with `regex:true` it "
                            "refuses bad_args rather than re-flowing your "
                            "pattern. Off by default.");
                        props["match_wrapped"] = mw;
                    }
                    {
                        QJsonObject ml; ml["type"] = "boolean";
                                        ml["default"] = false;
                                        ml["description"] = QStringLiteral(
                            "Optional (ANTS-4851). Let a regex cross line breaks: "
                            "write the break yourself, as `\\n` or `\\s+`. `line` "
                            "reports where the match STARTS and `text` is folded "
                            "to one line. Needs `regex:true`, or it refuses "
                            "bad_args; for literal wrapped prose use "
                            "`match_wrapped`. Off by default.");
                        props["multiline"] = ml;
                    }
                    props["offset"]      = wsOffsetProp;      // ANTS-3547
                    props["context"]     = ctxProp;
                    props["case"]        = caseProp;
                    props["respect_gitignore"] = respectGitignoreProp;
                    props["include_hidden"]    = includeHiddenProp;
                    props["dedup"]             = dedupProp;
                    props["timeout_sec"]       = timeoutSecProp;
                    // ANTS-1391 — caller_cwd anchor.
                    props["raw"]         = makeRawProp();        // ANTS-2218
                    props["caller_cwd"]  = makeCallerCwdReadProp();
                    props["encoding"]    = makeEncodingProp();   // ANTS-2090
                    schema["properties"] = props;
                    QJsonArray required;
                    required.append("pattern");
                    schema["required"] = required;
                    wsTool["inputSchema"] = schema;
                }
                tools.append(wsTool);

                // ANTS-3716 — cited_by: which document cites which anchor, in
                // one call. Spec: docs/specs/ANTS-3716-cited-by-sweep.md.
                QJsonObject cbTool;
                cbTool["name"] = "cited_by";
                cbTool["description"] = QStringLiteral(
                    "Given the anchors a change touched (symbols, flags, config "
                    "keys, paths), report which documents cite each, in one call. "
                    "Returns {ok, cells:[{anchor, file, count, first_line}], "
                    "cells_count, anchors_matched, anchors_unmatched, files_count, "
                    "scope_resolved, truncated}. Anchors match literally. Refusals: "
                    "bad_args, bad_path, rg_failed. caller_cwd Required. "
                    "Reverse lookup: it starts from names you hold, "
                    "so it stays cheap; prefer it to doc_symbols when you have "
                    "them.");
                cbTool["detail"] = QStringLiteral(
                    "Given the anchors a change touched (symbols, flags, config "
                    "keys, paths), report which documents cite which of them — one "
                    "call in place of one search per anchor. Returns {ok, "
                    "cells:[{anchor, file, count, first_line}], cells_count, "
                    "anchors_matched, anchors_unmatched, files_count, "
                    "scope_resolved, truncated}. `count` is OCCURRENCES, not "
                    "matching lines. Anchors match LITERALLY — for a regex use "
                    "workspace_search. `anchors_unmatched` is a first-class "
                    "result: \"nothing cites this\" is the cheap half of a sweep. "
                    "`scope` defaults to the project's docs dir + README.md + "
                    "CLAUDE.md; an entry absent on disk is pruned and drops out of "
                    "`scope_resolved`. Refusals: bad_args (anchors absent, empty, "
                    "over 64, an empty-string anchor, scope over 256 entries, or "
                    "case \"smart\"), bad_path "
                    "(scope escapes the project root), rg_failed. caller_cwd "
                    "Required. Full detail via tool_info {name:\"cited_by\"}."
                    " "
                    "One rg run per anchor, in sorted anchor order, so every match "
                    "belongs to its anchor by construction and there is no "
                    "attribution step. A combined single pass was measured and "
                    "rejected: rg reports the matched TEXT rather than the pattern "
                    "(so a case-insensitive hit cannot be mapped back to its "
                    "anchor), and with several -F patterns a nested anchor loses "
                    "leftmost-first and is reported uncited. 64 sequential runs "
                    "over a docs tree cost ~0.45 s against a 5 s default budget, "
                    "and it is still one round-trip either way.\n"
                    "`case` is \"insensitive\" by default and there is NO \"smart\" "
                    "mode: rg resolves --smart-case over the combined pattern set, "
                    "so one anchor carrying a capital would silently flip every "
                    "other anchor in the request to case-sensitive.\n"
                    "`max_cells` (default 500, clamp 1-5000) caps cells[] AFTER "
                    "the (anchor, file) sort, so two calls over an unchanged tree "
                    "return byte-identical bodies. `cells_count` is the capped "
                    "length; `files_count` is the UNCAPPED number of distinct "
                    "matching files. anchors_matched / anchors_unmatched are "
                    "computed over every run and stay meaningful when truncated "
                    "is true. A 50,000-cell collection ceiling guards the "
                    "pathological case; past it collection stops but every run "
                    "still drains, so no anchor is misreported as uncited.\n"
                    "`timeout_sec` (default 5, clamp 1-30) is the budget for the "
                    "WHOLE anchor set, not per run. Any failed rg run refuses "
                    "rg_failed and discards the cells already tallied — a partial "
                    "cell set is indistinguishable from a complete one.\n"
                    "Overlapping scope entries are de-overlapped before the run "
                    "(rg searches a file once per positional path that reaches it, "
                    "which would double a cell's count). No line bodies: "
                    "first_line is enough to get there, and read_region is the "
                    "drill-in. ETag-304 supported.");
                cbTool["selection_hint"] = QStringLiteral(
                    "Use after a change to ask \"which documents mention any of "
                    "these?\" — one call instead of one workspace_search per "
                    "anchor, and it names the anchors nothing cites. Not a "
                    "replacement for workspace_search: no regexes, no line "
                    "bodies.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    {
                        QJsonObject p;
                        p["type"] = "array";
                        QJsonObject items;
                        items["type"] = "string";
                        p["items"] = items;
                        p["description"] = QStringLiteral(
                            "The changed names, 1-64 literal strings — symbol, "
                            "flag, config key, env var or path. Never regexes. An "
                            "empty array, over 64 entries, or an empty-string "
                            "element refuses bad_args.");
                        props["anchors"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "array";
                        QJsonObject items;
                        items["type"] = "string";
                        p["items"] = items;
                        p["description"] = QStringLiteral(
                            "Project-relative files and/or dirs to search. "
                            "Defaults to the project's docs dir, README.md and "
                            "CLAUDE.md. Entries absent on disk are pruned; "
                            "scope_resolved echoes what was actually searched. "
                            "Over 256 entries refuses bad_args.");
                        props["scope"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        QJsonArray e;
                        e.append("insensitive");
                        e.append("sensitive");
                        p["enum"]    = e;
                        p["default"] = "insensitive";
                        p["description"] = QStringLiteral(
                            "Match case. \"smart\" is deliberately absent — rg "
                            "resolves it over the whole pattern set, so one anchor "
                            "would change every other anchor's result.");
                        props["case"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"]        = "integer";
                        p["default"]     = 500;
                        p["minimum"]     = 1;
                        p["maximum"]     = 5000;
                        p["description"] = QStringLiteral(
                            "Cap on cells[] (default 500), applied after the sort. "
                            "files_count stays uncapped; truncated says so.");
                        props["max_cells"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"]        = "integer";
                        p["default"]     = 5;
                        p["minimum"]     = 1;
                        p["maximum"]     = 30;
                        p["description"] = QStringLiteral(
                            "Wall-clock budget in seconds for the WHOLE anchor "
                            "set, not per run (default 5).");
                        props["timeout_sec"] = p;
                    }
                    props["caller_cwd"]  = makeCallerCwdReadProp();
                    props["etag_match"]  = makeEtagMatchProp();   // ANTS-1499
                    schema["properties"] = props;
                    QJsonArray required;
                    required.append("anchors");
                    required.append("caller_cwd");
                    schema["required"]             = required;
                    schema["additionalProperties"] = false;
                    cbTool["inputSchema"] = schema;
                }
                tools.append(cbTool);

                // ANTS-1249: file_outline — regex-scanner outline for
                // a single file. ~13-39× compression on a typical
                // source file vs full Read.
                QJsonObject foTool;
                foTool["name"] = "file_outline";
                // On the wire, kEagerDescription below replaces this text.
                foTool["description"] = QStringLiteral(
                    "Return a structured outline of a file (header comment + "
                    "per-symbol {line, kind, name, signature}) instead of a full "
                    "Read. Languages by extension: cpp/c/glsl, py, md, json, html "
                    "and the brace family. paths:[...] outlines several files; "
                    "branch on files_found / files_missing, not ok. sizes:true adds "
                    "per-symbol bytes and lines; raw:true returns verbatim bytes. "
                    "caller_cwd required, or \"~global\". Does NOT satisfy the native "
                    "Edit tool's read-precondition.");
                foTool["detail"] = QStringLiteral(
                    "Return a structured outline of a file — header "
                    "comment + per-symbol {line, kind, name, "
                    "signature}. Prefer this over a full Read when "
                    "you only need to know what's IN a file (where "
                    "to grep, which class lives in what file). "
                    "Languages (auto-picked by extension): cpp/c (+ glsl), "
                    "py, md, json, and the brace family — rust, go, "
                    "javascript, typescript, java, csharp, kotlin, swift, "
                    "scala, php, ruby, and html. ANTS-4361: a single "
                    "self-contained HTML page — the normal shape for a small "
                    "local tool — outlines to structural LANDMARKS rather "
                    "than a DOM parse: each <style>/<script> as a region with "
                    "its start line, every element carrying an id= as an "
                    "anchor, and the top-level declarations inside each "
                    "JavaScript <script> via the brace-family parser, "
                    "reporting their real line in the FILE. A <script> whose "
                    "type is not JavaScript (application/json, text/template) "
                    "is data and is not parsed. ANTS-4090: in the brace family a "
                    "top-level `const`/`let`/`var NAME =` is emitted with "
                    "kind \"const\", so a file storing payloads in template "
                    "literals shows those regions instead of a gap. "
                    "ANTS-4520: in md mode a heading inside a BLOCKQUOTE "
                    "(`> ## You are here`) is a heading and is listed, at the "
                    "depth its `#` run gives it. Anything "
                    "else still reports total_lines/total_bytes for "
                    "orientation. "
                    "Typically 13-39× smaller than a full Read. "
                    "ANTS-1390: pass `caller_cwd: \"~global\"` "
                    "(alias `\"~claude-config\"`) to outline files "
                    "under ~/.claude/ — for sessions editing global "
                    "Claude config (skills, agents, the global "
                    "CLAUDE.md). "
                    "ANTS-2223: pass `paths:[...]` instead of `path` to "
                    "outline several related files (a header + its impl + a "
                    "consumer) in ONE call — returns a `files:[{path, "
                    "symbols, etag, …}]` array, each entry 304ing "
                    "independently via the optional `etags` map "
                    "({relPath: priorEtag}). "
                    "NOTE (ANTS-3383): outlining a file via this verb does "
                    "NOT satisfy the native Edit tool's read-precondition — "
                    "do a native Read before editing a file you intend to "
                    "modify. "
                    "ANTS-4349: in the `paths` form, top-level `ok` means the "
                    "CALL was well-formed, NOT that any path resolved — a "
                    "partial hit is a success. Branch on `files_found` / "
                    "`files_missing` (or each entry's own `ok`), because "
                    "ok:true with files_found:0 is a total miss. "
                    "ANTS-4384: pass `sizes:true` for per-symbol `bytes` + "
                    "`lines` — which section carries the weight, and where "
                    "the seam is. "
                    "ANTS-4365: pass `raw:true` for verbatim `header_doc` / "
                    "`signature` bytes; the default framing rewrites literal "
                    "`<!--`/`-->`, so a Markdown file whose header IS an HTML "
                    "comment cannot report its own first line truthfully "
                    "without it — and an Edit built from the mangled spelling "
                    "writes it back.");
                foTool["selection_hint"] = QStringLiteral(
                    "Use to map a large file's symbols before Read. "
                    "Prefer over `Read` when you only need a "
                    "structural overview (which class/function is "
                    "where).");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    QJsonObject pathProp;     pathProp["type"]     = "string";
                                              pathProp["description"] =
                        QStringLiteral("Repo-relative or absolute path. "
                                       "Must resolve under project root.");
                    QJsonObject modeProp;     modeProp["type"]     = "string";
                    QJsonArray modeEnum;
                    modeEnum.append("auto");
                    modeEnum.append("cpp");
                    modeEnum.append("py");
                    modeEnum.append("md");
                    modeEnum.append("json");
                    // ANTS-3800 — `generic` has been accepted by parseMode()
                    // since ANTS-2150 and was never advertised, and `glsl` is
                    // new here. Both are listed because the defect this fixes
                    // IS the schema disagreeing with what the verb accepts.
                    modeEnum.append("generic");
                    modeEnum.append("glsl");
                    // ANTS-4826 — shell, the third time this schema was the
                    // one left behind. find_definition's own `lang` enum
                    // (makeSymbolLangProp) has listed "sh" since ANTS-3558.
                    modeEnum.append("sh");
                    modeEnum.append("html");     // ANTS-4361
                    modeProp["enum"]    = modeEnum;
                    modeProp["default"] = "auto";
                    QJsonObject hdrProp;      hdrProp["type"]      = "boolean";
                                              hdrProp["default"]   = true;
                    QJsonObject maxSymProp;   maxSymProp["type"]   = "integer";
                                              maxSymProp["default"] = 200;
                                              maxSymProp["maximum"] = 1000;
                    // ANTS-4469 — the cap now says how much it withheld, so a
                    // caller can tell "I missed one" from "I missed four
                    // hundred" without a second call at a raised cap.
                                              maxSymProp["description"] =
                        QStringLiteral("Cap on symbols[] (default 200). When "
                            "it bites, the envelope carries truncated:true + "
                            "symbols_dropped:<n> — the same field the "
                            "max_bytes cap emits, so one caller branch "
                            "handles both.");
                    // ANTS-1293 — response byte cap. max_symbols bounds the
                    // count; this bounds total size. Trims symbols[] from
                    // the tail and sets truncated + symbols_dropped.
                    QJsonObject maxBytesProp; maxBytesProp["type"] = "integer";
                                              maxBytesProp["minimum"] = 1;
                                              maxBytesProp["description"] =
                        QStringLiteral("Cap on the serialized response in "
                            "bytes (default 512 KiB, server-clamped to "
                            "4 MiB). When exceeded, trailing symbols are "
                            "dropped and the envelope carries "
                            "truncated:true + symbols_dropped:<n> "
                            "(+ bytes_cap_clamped:true if the requested cap "
                            "exceeded the ceiling).");
                    // ANTS-2223 — multi-path batch. `paths` is the
                    // alternative to `path`; `etags` 304s unchanged entries.
                    QJsonObject pathsProp;    pathsProp["type"]    = "array";
                    {
                        QJsonObject items; items["type"] = "string";
                        pathsProp["items"] = items;
                    }
                    pathsProp["description"] = QStringLiteral(
                        "Repo-relative or absolute paths to outline in one "
                        "call (alternative to `path`; wins when both sent). "
                        "Returns files:[{path, symbols, etag, …}] — one entry "
                        "per path, each resolved + capped independently.");
                    QJsonObject etagsProp;    etagsProp["type"]    = "object";
                    etagsProp["description"] = QStringLiteral(
                        "Optional {project-relative-path: prior etag} map for "
                        "the `paths` form. Any file whose current etag matches "
                        "is returned as a compact {path, unchanged:true, etag} "
                        "stub instead of its full symbols — so a re-outline "
                        "after editing one file in the set re-sends only the "
                        "changed bodies.");
                    // ANTS-4384 — opt-in per-symbol extents.
                    QJsonObject sizesProp; sizesProp["type"] = "boolean";
                                           sizesProp["default"] = false;
                                           sizesProp["description"] =
                        QStringLiteral("Optional (ANTS-4384). When true, each "
                            "symbol also carries `bytes` and `lines` — its "
                            "extent from its start line to the line before the "
                            "next symbol at the SAME OR HIGHER level (EOF for "
                            "the last). So in md mode a `##` section's size "
                            "INCLUDES its `###` children, which is what makes "
                            "the answer \"where do I split this file\" rather "
                            "than \"how long is this paragraph\"; the flat "
                            "modes are sibling-scoped. Answers which section "
                            "carries the weight and where the natural seam is "
                            "without falling back to awk. Opt-in, so the "
                            "default envelope is byte-identical; composes with "
                            "compact / fields / etag_match and with the "
                            "`paths` form (uniform across the batch). A "
                            "truncated outline omits the LAST symbol's size "
                            "rather than reporting an extent that silently "
                            "absorbs everything the cap dropped.");
                    props["path"]                 = pathProp;
                    props["paths"]                = pathsProp;
                    props["etags"]                = etagsProp;
                    props["sizes"]                = sizesProp;
                    {   // ANTS-4396 — md heading-depth filter.
                        QJsonObject mh; mh["type"] = "integer";
                                        mh["minimum"] = 1;
                                        mh["maximum"] = 6;
                                        mh["description"] = QStringLiteral(
                            "Optional (ANTS-4396), md mode. Drop headings "
                            "deeper than this level — 2 keeps only `#`/`##`. "
                            "For a long append-only log (a feedback file, a "
                            "ROADMAP) the `###` entries are full sentences and "
                            "swamp the day headings that answer \"what is "
                            "here?\". NOT the same as `max_symbols`, which "
                            "truncates from the TOP and therefore keeps the "
                            "OLDEST entries — the opposite of what such a file "
                            "wants. The filter applies before the symbol "
                            "budget, so dropping deep headings frees room for "
                            "shallow ones further down. Out of range or "
                            "omitted = no filter.");
                        props["max_heading_level"] = mh;
                    }
                    {   // ANTS-3839 — symbol-name substring filter.
                        QJsonObject ft; ft["type"] = "string";
                                        ft["description"] = QStringLiteral(
                            "Optional (ANTS-3839). Keep only symbols whose "
                            "NAME contains this substring — the answer to "
                            "\"where is the roadmap stuff in this 6k-line "
                            "file?\" without paying for the whole outline. "
                            "Case-INSENSITIVE, so `outline` finds "
                            "`FileOutline`: a case-sensitive symbol filter "
                            "returns an empty list for a spelling difference, "
                            "and an unexplained empty list is what ANTS-4374 "
                            "forbids. Applied BEFORE `max_bytes` and "
                            "`max_symbols`, so it makes room rather than "
                            "competing with them. The response echoes "
                            "`filter` plus `symbols_considered` and "
                            "`symbols_filtered_out`, so zero matches is "
                            "distinguishable from a file with no symbols. "
                            "This argument was advertised by the verb's own "
                            "`leaner_call_hint` for a long time before it "
                            "existed; the hint is now true.");
                        props["filter"] = ft;
                    }
                    props["raw"]                  = makeRawProp();        // ANTS-4365
                    props["mode"]                 = modeProp;
                    props["include_doc_comment"]  = hdrProp;
                    props["max_symbols"]          = maxSymProp;
                    props["max_bytes"]            = maxBytesProp;
                    // ANTS-1391 — caller_cwd anchor.
                    props["caller_cwd"]           = makeCallerCwdReadProp();
                    props["etag_match"]           = makeEtagMatchProp();   // ANTS-1499
                    props["fields"]               = makeFieldsProp();      // ANTS-1720
                    props["compact"]              = makeCompactProp();     // ANTS-2091
                    props["encoding"]             = makeEncodingProp();    // ANTS-2090
                    schema["properties"] = props;
                    // `path` is no longer strictly required: the `paths` form
                    // (ANTS-2223) satisfies the verb without it. The handler
                    // returns bad_path when neither is usable.
                    foTool["inputSchema"] = schema;
                }
                tools.append(foTool);

                // ANTS-4398 — mutation_probe.
                {
                    QJsonObject mp;
                    mp["name"] = "mutation_probe";
                    mp["description"] = QStringLiteral(
                        "Apply a textual mutation to a source file, run a test command, "
                        "restore the file, and REFUSE a mutation that changed nothing. "
                        "Outcomes: killed | survived | broken | inert | timed_out | "
                        "command_not_found | write_failed | not_run. restored_clean is "
                        "verified against the baseline bytes. "
                        "require_green_baseline:true refuses a red or unreadable "
                        "baseline (baseline_unreadable) or one that collected nothing "
                        "(baseline_did_not_run); expect_occurrences refuses a "
                        "mismatch (occurrence_mismatch); transport_budget_sec stops the "
                        "batch before the transport times out. "
                        "A transport timeout leaves the outcome UNKNOWN: "
                        "re-read `path`; never hand-repair or commit it. "
                        "test_command is an argv array. caller_cwd required.");
                    mp["detail"] = QStringLiteral(
                        "Apply a textual mutation to a source file, run a test "
                        "command, restore the file — and REFUSE a mutation that "
                        "did not change anything. The mutate-and-watch-it-go-red "
                        "loop several projects' CLAUDE.md files mandate before "
                        "believing an invariant is held, which had no verb: "
                        "focused_test is ctest-only and does not mutate, "
                        "invariant_check reads specs and runs nothing. "
                        "`inert` is the field that matters: a mutation whose "
                        "`old` is absent, or whose replacement leaves the bytes "
                        "identical, is reported INERT and NO test is run — "
                        "running one would pass against unmutated code and read "
                        "as a weak test, which is the false conclusion this verb "
                        "exists to prevent. Outcomes: killed (the suite noticed) "
                        "| survived (it did not) | broken (ANTS-5360: a "
                        "non-zero exit with no readable pass/fail counts, "
                        "such as a syntax or collection error — no test is "
                        "known to have run, so it is not a kill) | inert | "
                        "timed_out | command_not_found | write_failed. "
                        "ANTS-4974: a killed result carries failed_tests and "
                        "killed_by — assertion, error, mixed or unknown, with "
                        "killed_by_counts. `error` means the tests crashed or "
                        "raised (a NameError, an uncaught exception, a "
                        "segfault): the mutant was not valid code, which is "
                        "not evidence about the suite. Prefer a mutation that "
                        "leaves the code valid and changes only what it "
                        "means. A bare ctest \"(Failed)\" names no cause and "
                        "reads `unknown`; run ctest with --output-on-failure "
                        "so the test's own output can be read. "
                        "ANTS-4975: a mutation may carry its own `path`, so a "
                        "fix spanning two files is probed in one batch with "
                        "one baseline run; each result echoes its `path`, "
                        "and every path is validated before anything is "
                        "written. "
                        "Each mutation is applied to a CLEAN baseline so two "
                        "cannot compound, and the restore is guaranteed even on "
                        "a failed or timed-out run — `restored_clean` is "
                        "VERIFIED against the baseline bytes, because a leaked "
                        "mutated file in a repo the session then commits is the "
                        "dangerous failure. Pass require_green_baseline:true to "
                        "refuse the batch when the suite is already red, since a "
                        "mutant \"dying\" proves nothing then. ANTS-4401: that "
                        "gate needs EVIDENCE of green, not merely the absence "
                        "of red — it also refuses `baseline_unreadable` when "
                        "the baseline exits 0 but its counts come back "
                        "unparsable (-1/-1) or empty (0/0), because a run that "
                        "could not be read, and a run that executed nothing, "
                        "both used to read as a pass and make every later "
                        "verdict unfalsifiable. "
                        "ANTS-6359: allow_exit_code_baseline:true lets an "
                        "unparsable baseline pass on exit 0 alone, for a "
                        "suite whose only signal is its exit code; the reply "
                        "then carries baseline_gate:\"exit_code_only\". An "
                        "empty run is still refused. "
                        "ANTS-4521: a mutation may carry "
                        "`expect_occurrences` — a mismatch refuses THAT "
                        "mutation (occurrence_mismatch) before any test runs, "
                        "because a mutation hitting more sites than intended "
                        "can be killed by a test covering a site you never "
                        "meant to probe. "
                        "ANTS-4736: the batch stops when the slowest run so "
                        "far would not fit in `transport_budget_sec` (default "
                        "60, matching the bridge's read timeout) — remaining "
                        "mutations come back `not_run` with "
                        "budget_exhausted:true and the measured arithmetic. "
                        "Without it the loop kept mutating the file after the "
                        "transport had given up, which reaches the leaked "
                        "mutant by the one route `restored_clean` cannot "
                        "report: you are no longer there to read it. "
                        "ANTS-5096: before each mutant the original is journaled "
                        "under the data dir's ants-terminal/mutation-journal, and "
                        "every call first restores a file a killed probe left "
                        "holding its mutant, listing it in `recovered` "
                        "({path, outcome}: restored | left_edited | "
                        "left_missing | restore_failed). A journal whose writer "
                        "is still running is left alone. "
                        "ANTS-4830: a transport timeout is the client giving "
                        "up, not the probe, and its error names no path. The "
                        "outcome is UNKNOWN, and the only file at risk is "
                        "`path`. The probe goes on and restores it when its "
                        "batch ends; if its process died, the next call in "
                        "any session restores it from the journal. So re-read "
                        "`path` (git diff) before acting, and never hand-repair "
                        "it: a repair races that restore, and a commit can "
                        "ship the mutant. "
                        "`test_command` is an ARGV ARRAY, never a shell string — "
                        "a deliberate narrowing, because this verb writes to a "
                        "source file and spawns a process. caller_cwd required.");
                    mp["selection_hint"] = QStringLiteral(
                        "Use to prove a test would actually catch a defect "
                        "before trusting it — and to find green tests that "
                        "measure nothing.");
                    {
                        QJsonObject schema;
                        schema["type"] = "object";
                        schema["required"] = QJsonArray{
                            QStringLiteral("caller_cwd"), QStringLiteral("path"),
                            QStringLiteral("mutations"),
                            QStringLiteral("test_command")};
                        QJsonObject props;
                        QJsonObject pathP; pathP["type"] = "string";
                            pathP["description"] = QStringLiteral(
                                "Project-relative source file to mutate.");
                        QJsonObject cmdP; cmdP["type"] = "array";
                            { QJsonObject it; it["type"] = "string";
                              cmdP["items"] = it; }
                            cmdP["description"] = QStringLiteral(
                                "ARGV array, e.g. [\"pytest\", \"-k\", "
                                "\"test_supervisor\"]. NOT a shell string.");
                        QJsonObject bldP; bldP["type"] = "array";
                            { QJsonObject it; it["type"] = "string";
                              bldP["items"] = it; }
                            bldP["description"] = QStringLiteral(
                                "ARGV array run before the baseline and each "
                                "test, and again after the batch. Counts against "
                                "the transport budget.");
                        props["build_command"] = bldP;
                        QJsonObject mutsP; mutsP["type"] = "array";
                            {
                                QJsonObject item; item["type"] = "object";
                                QJsonObject ip;
                                QJsonObject l; l["type"] = "string";
                                    l["description"] = QStringLiteral(
                                        "Your name for this mutant, echoed back.");
                                QJsonObject ov; ov["type"] = "string";
                                    ov["description"] = QStringLiteral(
                                        "Exact substring to replace. Absent from "
                                        "the file ⟹ inert.");
                                QJsonObject nv; nv["type"] = "string";
                                    nv["description"] = QStringLiteral(
                                        "Replacement; may be empty (a deletion "
                                        "is a mutation). Equal to `old` ⟹ inert.");
                                QJsonObject eo; eo["type"] = "integer";
                                    eo["minimum"] = 1;
                                    eo["description"] = QStringLiteral(
                                        "ANTS-4521, optional: how many "
                                        "occurrences of `old` you INTEND to "
                                        "mutate. A mismatch refuses this "
                                        "mutation (outcome "
                                        "\"occurrence_mismatch\", both counts "
                                        "reported) and runs no test. Absent ⟹ "
                                        "unchecked — every occurrence is "
                                        "mutated, which is often the intent, "
                                        "so this does NOT default to 1. Set it "
                                        "when the label names ONE site: a "
                                        "mutation hitting more can be killed "
                                        "by a test covering a site you never "
                                        "meant to probe while the one you did "
                                        "stays uncovered, and the verdict "
                                        "still reads `killed`.");
                                QJsonObject pp; pp["type"] = "string";
                                    pp["description"] = QStringLiteral(
                                        "ANTS-4975, optional: the file this "
                                        "mutation edits. Default: the top-level "
                                        "`path`. Each file keeps its own baseline.");
                                ip["label"] = l; ip["old"] = ov; ip["new"] = nv;
                                ip["expect_occurrences"] = eo; ip["path"] = pp;
                                item["properties"] = ip;
                                mutsP["items"] = item;
                            }
                            mutsP["description"] = QStringLiteral(
                                "1-50 mutations, each run separately against a "
                                "clean baseline. Capped because each is a full "
                                "test run.");
                        QJsonObject grP; grP["type"] = "boolean";
                            grP["default"] = false;
                            grP["description"] = QStringLiteral(
                                "Run the suite unmutated first and refuse the "
                                "batch (baseline_not_green) if it is red.");
                        QJsonObject toP; toP["type"] = "integer";
                            toP["default"] = 300;
                            toP["description"] = QStringLiteral(
                                "Per-run wall-clock budget in seconds, clamped "
                                "to [5, 1800].");
                        QJsonObject tbP; tbP["type"] = "integer";
                            tbP["default"] = 60;
                            tbP["description"] = QStringLiteral(
                                "ANTS-4736: batch-wide deadline in seconds, "
                                "clamped to [0, 1800]; 0 disables it. Defaults "
                                "to the bridge's own read timeout, so the "
                                "batch stops while you are still waiting "
                                "instead of mutating on unwatched. It is "
                                "MEASURED, not predicted from `timeout_sec`: "
                                "that is a worst-case cap almost never "
                                "reached, so predicting from it would refuse "
                                "batches that finish well inside the budget. "
                                "Raise it only alongside "
                                "ANTS_MCP_READ_TIMEOUT_S.");
                        props["transport_budget_sec"]   = tbP;
                        props["path"]                   = pathP;
                        props["test_command"]           = cmdP;
                        props["mutations"]              = mutsP;
                        props["require_green_baseline"] = grP;
                        QJsonObject ecP; ecP["type"] = "boolean";
                            ecP["default"] = false;
                            ecP["description"] = QStringLiteral(
                                "With require_green_baseline: accept exit 0 "
                                "when the counts are unreadable. Reply says "
                                "baseline_gate:\"exit_code_only\".");
                        props["allow_exit_code_baseline"] = ecP;
                        props["timeout_sec"]            = toP;
                        props["caller_cwd"] = makeCallerCwdReadProp();
                        schema["properties"] = props;
                        mp["inputSchema"] = schema;
                    }
                    tools.append(mp);
                }

                // ANTS-1855 — read_log: filter a log file, return only
                // matching lines + counts instead of Read-ing a whole
                // multi-MB log into context.
                QJsonObject rlTool;
                rlTool["name"] = "read_log";
                rlTool["description"] = QStringLiteral(
                    "Filter a log file and return only matching lines + "
                    "counts — instead of Read-ing a whole multi-MB log "
                    "into context. Default target (no `path`) is the Ants "
                    "debug log; a `path` reads any project log, resolved "
                    "under caller_cwd. Secrets and private-key lines are "
                    "blanked ([REDACTED:<kind>], count in `redacted`) "
                    "unless claude.mcp_redact_secrets is off. "
                    "Filters: include/exclude (regex), contains "
                    "(substring), since (keep lines whose leading "
                    "[yyyy-MM-ddTHH:mm:ss.zzz] prefix >= this), tail "
                    "(last N). Byte-capped (max_bytes, default 512 KiB, "
                    "4 MiB ceiling) keeping the NEWEST lines. Pass "
                    "since_cursor (a prior response's `cursor`) to read "
                    "only lines appended since — token-frugal polling; a "
                    "stale/rotated cursor soft-falls-back to a full "
                    "re-read (cursor_stale:true). caller_cwd required.");
                rlTool["selection_hint"] = QStringLiteral(
                    "Use to grep a log file (esp. the Ants debug log) for "
                    "relevant lines instead of Bash grep/tail or a full "
                    "Read.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    QJsonObject pathProp; pathProp["type"] = "string";
                        pathProp["description"] = QStringLiteral(
                            "Optional. Log file path resolved under "
                            "caller_cwd. Omit for the Ants debug log.");
                    QJsonObject incProp; incProp["type"] = "string";
                        incProp["description"] = QStringLiteral(
                            "Optional regex; keep only matching lines.");
                    QJsonObject excProp; excProp["type"] = "string";
                        excProp["description"] = QStringLiteral(
                            "Optional regex; drop matching lines.");
                    QJsonObject cntProp; cntProp["type"] = "string";
                        cntProp["description"] = QStringLiteral(
                            "Optional literal substring a line must "
                            "contain.");
                    QJsonObject sinceProp; sinceProp["type"] = "string";
                        sinceProp["description"] = QStringLiteral(
                            "Optional. Keep lines whose leading "
                            "[timestamp] prefix is lexically >= this "
                            "(local-time yyyy-MM-ddTHH:mm:ss.zzz).");
                    QJsonObject tailProp; tailProp["type"] = "integer";
                        tailProp["minimum"] = 1; tailProp["maximum"] = 10000;
                        tailProp["description"] = QStringLiteral(
                            "Optional. Return only the last N matching "
                            "lines (clamped to 10000).");
                    QJsonObject mbProp; mbProp["type"] = "integer";
                        mbProp["minimum"] = 1;
                        mbProp["description"] = QStringLiteral(
                            "Cap on the lines[] bytes (default 512 KiB, "
                            "server-clamped to 4 MiB). Oldest lines "
                            "dropped first; sets truncated + lines_dropped "
                            "(+ bytes_cap_clamped over ceiling).");
                    QJsonObject curProp; curProp["type"] = "string";
                        curProp["description"] = QStringLiteral(
                            "Optional. Byte-offset token from a prior "
                            "response's `cursor`; reads only lines "
                            "appended since. A stale/rotated cursor "
                            "soft-falls-back (cursor_stale:true).");
                    props["path"]         = pathProp;
                    props["include"]      = incProp;
                    props["exclude"]      = excProp;
                    props["contains"]     = cntProp;
                    props["since"]        = sinceProp;
                    props["tail"]         = tailProp;
                    props["max_bytes"]    = mbProp;
                    props["since_cursor"] = curProp;
                    props["caller_cwd"]   = makeCallerCwdReadProp();
                    props["fields"]       = makeFieldsProp();   // ANTS-1720
                    props["compact"]      = makeCompactProp();   // ANTS-2091
                    schema["properties"] = props;
                    rlTool["inputSchema"] = schema;
                }
                tools.append(rlTool);

                // ANTS-2021 — read_region: return a line range or a named
                // symbol's body from a project file (ETag-304 free re-read,
                // symbol-scoped under-read) instead of a full native Read.
                QJsonObject rrTool;
                rrTool["name"] = "read_region";
                // On the wire, kEagerDescription below replaces this text.
                rrTool["description"] = QStringLiteral(
                    "Return an exact slice of a project file instead of Read-ing "
                    "it: a line range (start_line/end_line), a named symbol's body "
                    "(symbol) or a markdown heading's body (section, by heading "
                    "text or slug). Exactly one selector. Byte-capped by max_bytes. "
                    "caller_cwd required, or \"~global\" for files under ~/.claude/. "
                    "Does NOT satisfy the native Edit tool's read-precondition.");
                rrTool["detail"] = QStringLiteral(
                    "Return an exact slice of a project file — a line range "
                    "(start_line/end_line, 1-based inclusive), a named "
                    "symbol's body (symbol), OR a markdown heading's body "
                    "(section) — instead of Read-ing the whole file. Exactly "
                    "one selector. "
                    "ANTS-4394: pass `caller_cwd: \"~global\"` (alias "
                    "`\"~claude-config\"`) to read files under ~/.claude/ — the "
                    "same sentinel file_outline (ANTS-1390) and doc_integrity "
                    "(ANTS-3719) accept. It has always worked here and was "
                    "simply undocumented, so a session reading a global "
                    "standard passed an absolute path with its project "
                    "caller_cwd, got the correct `bad_path` refusal, and fell "
                    "back to Bash sed. With the sentinel, `section=` answers "
                    "\"read § 5.4 of spec-format.md\" in one call with no line "
                    "arithmetic against a file that shifts as it is edited. Symbol mode resolves via the "
                    "flat file_outline (function/method, or a struct/class/"
                    "union aggregate whose FULL brace-matched body is returned "
                    "(ANTS-4735: brace matching is for BRACE-DELIMITED "
                    "languages only. In py mode the extent comes from the flat "
                    "outline -- the next symbol's line -- because `{` opens "
                    "nothing in Python, and brace-matching there closed on the "
                    "first dict literal and returned a short body while "
                    "reporting truncated:false) "
                    "— ANTS-2222; a namespace still resolves to its declaration "
                    "only, use line mode for one) and can see only the first "
                    "1000 outline symbols. Section mode (ANTS-2221, .md files) "
                    "returns one heading's body up to the next same-or-higher-"
                    "level heading; pass either the heading text or its slug "
                    "(\"4.2 Emission model\" and \"4-2-emission-model\" both "
                    "resolve) — the markdown analogue of symbol mode, so you "
                    "skip the file_outline→line-arithmetic dance. "
                    "Byte-capped (max_bytes, default 512 KiB, 4 MiB ceiling), "
                    "keeping the head. ETag-304: a matching etag_match "
                    "re-read is free. caller_cwd required. NOTE (ANTS-3383): "
                    "reading via this verb does NOT satisfy the native Edit "
                    "tool's read-precondition — do a native Read before "
                    "editing a file you intend to modify.");
                rrTool["selection_hint"] = QStringLiteral(
                    "Use after find_definition/file_outline to read one "
                    "function's body or any line range instead of a full Read — "
                    "and re-read it free when unchanged. Reading several slices? "
                    "Batch them in one `read_regions` call (worth it on first "
                    "reads too).");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    schema["additionalProperties"] = false;
                    QJsonObject props;
                    QJsonObject pathProp; pathProp["type"] = "string";
                        pathProp["description"] = QStringLiteral(
                            "Project file path resolved under caller_cwd.");
                    QJsonObject startProp; startProp["type"] = "integer";
                        startProp["minimum"] = 1;
                        startProp["description"] = QStringLiteral(
                            "Line-range mode: 1-based first line. Mutually "
                            "exclusive with `symbol`.");
                    QJsonObject endProp; endProp["type"] = "integer";
                        endProp["minimum"] = 1;
                        endProp["description"] = QStringLiteral(
                            "Line-range mode: 1-based last line (inclusive); "
                            "defaults to start_line. Clamps to EOF.");
                    QJsonObject symProp; symProp["type"] = "string";
                        symProp["description"] = QStringLiteral(
                            "Symbol-body mode: return this symbol's body. "
                            "Mutually exclusive with start_line/end_line and "
                            "section.");
                    QJsonObject sectionProp; sectionProp["type"] = "string";
                        sectionProp["description"] = QStringLiteral(
                            "Section-body mode (.md, ANTS-2221): return one "
                            "markdown heading's body — the heading line through "
                            "the line before the next same-or-higher-level "
                            "heading. Accepts the heading text or its slug "
                            "(both \"4.2 Emission model\" and "
                            "\"4-2-emission-model\" resolve). A short title "
                            "also resolves a heading with a trailing "
                            "parenthetical (\"7. Build order\" matches "
                            "\"7. Build order (cheapest-first)\") when it "
                            "uniquely prefixes one heading; ambiguous prefixes "
                            "refuse with section_ambiguous + candidates "
                            "(ANTS-2234). A section_not_found refusal ALSO "
                            "carries candidates (ANTS-4350) — the same field "
                            "and shape, so one code path handles both — "
                            "ranked by word overlap first and a shared "
                            "leading section number second, capped at 10. "
                            "ANTS-4520: a heading inside a BLOCKQUOTE "
                            "(`> ## You are here`, the standing convention for "
                            "run-state blocks in a plan file) is a heading and "
                            "resolves by name like any other; its body ends at "
                            "the next heading that is less deeply quoted, or "
                            "equally quoted at the same-or-higher level, so a "
                            "quoted block cannot swallow the plain section "
                            "after it. Mutually exclusive with the other "
                            "selectors. Echoes section + the resolved "
                            "section_slug.");
                    QJsonObject mbProp; mbProp["type"] = "integer";
                        mbProp["minimum"] = 1;
                        mbProp["description"] = QStringLiteral(
                            "Cap on the lines[] bytes (default 512 KiB, "
                            "server-clamped to 4 MiB). Keeps the head; sets "
                            "truncated (+ bytes_cap_clamped over ceiling).");
                    props["path"]       = pathProp;
                    props["start_line"] = startProp;
                    props["end_line"]   = endProp;
                    props["symbol"]     = symProp;
                    props["section"]    = sectionProp;   // ANTS-2221
                    props["max_bytes"]  = mbProp;
                    {
                        // ANTS-4700 — per-line clip.
                        QJsonObject mlb;
                        mlb["type"]    = "integer";
                        mlb["minimum"] = 0;
                        mlb["description"] = QStringLiteral(
                            "Optional (ANTS-4700). Clip each emitted LINE to "
                            "this many UTF-8 bytes, using the same "
                            "payload-prefix + U+2026 marker as "
                            "workspace_search's `max_match_bytes`, and clamped "
                            "to the same [50, 10000]. Applied BEFORE the "
                            "`max_bytes` cap is charged, so it makes room for "
                            "more LINES rather than competing with the cap — "
                            "which is the whole point on a region whose weight "
                            "sits in a few very long lines (a review loop-log "
                            "table, a hard-wrapped standard). Measured on the "
                            "reporting call: a 113-line region was 29,244 "
                            "bytes of which six table rows were 20,446, so the "
                            "call spilled and returned neither the structure "
                            "nor the tail. Off unless asked: read_region's job "
                            "is to return the bytes that are there, and a "
                            "default clip would silently change what a re-read "
                            "returns. When set, the envelope echoes "
                            "`max_line_bytes` and `lines_clipped` (+ "
                            "`line_cap_clamped` when the value was out of "
                            "range) — a zero there says nothing was long "
                            "enough to clip, which is not the same as the "
                            "argument having been ignored. Complements "
                            "file_outline `sizes:true`, which answers WHERE "
                            "the weight is; this reads past it.");
                        props["max_line_bytes"] = mlb;
                    }
                    {
                        QJsonObject tl; tl["type"] = "integer";
                                        tl["minimum"] = 1;
                                        tl["description"] = QStringLiteral(
                            "Optional (ANTS-5281). With `section`: return only "
                            "that section's last N lines, so the newest rows "
                            "of an append-only log need no line numbers. The "
                            "reply's `section_end_line` is where the section "
                            "ends, and it also appears on a truncated section "
                            "read. Without `section` it refuses bad_args.");
                        props["tail_lines"] = tl;
                    }
                    {
                        // ANTS-2157 — integration brief.
                        QJsonObject p;
                        p["type"]        = "boolean";
                        p["default"]     = false;
                        p["description"] = QStringLiteral(
                            "When true, also return `call_sequence` (the "
                            "ordered call-expressions inside the region — a "
                            "pipeline's STAGES, each {line, callee}; line = "
                            "the insertion point) and `accessors` (the m_ "
                            "members + get/is/has getters referenced — the "
                            "helpers a new stage usually needs). Answers "
                            "\"to add a step to this pipeline, what are the "
                            "existing steps in order and what does a new one "
                            "hook into?\" in one call. Best with a `symbol` "
                            "naming the pipeline driver function.");
                        props["call_sequence"] = p;
                    }
                    props["raw"]        = makeRawProp();         // ANTS-2218
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    props["etag_match"] = makeEtagMatchProp();   // ANTS-1499
                    props["fields"]     = makeFieldsProp();      // ANTS-1720
                    props["compact"]    = makeCompactProp();     // ANTS-2091
                    schema["properties"] = props;
                    QJsonArray required;
                    required.append("path");
                    schema["required"] = required;
                    rrTool["inputSchema"] = schema;
                }
                tools.append(rrTool);

                // ANTS-2219 — read_regions: batched multi-selector read.
                // Fetch several slices (symbol bodies / line ranges / md
                // sections, across one or many files) in ONE call.
                QJsonObject rrsTool;
                rrsTool["name"] = "read_regions";
                rrsTool["description"] = QStringLiteral(
                    "Batched read_region: fetch several file slices in one call. "
                    "`items` is an array of {path, one selector (symbol | "
                    "start_line[/end_line] | section), optional etag_match}; a "
                    "top-level `path` is the default for items without one. Returns "
                    "{ok, results:[...], count, truncated?}, one slice envelope per "
                    "item; a bad item fails alone. Shared max_bytes budget; max 64 "
                    "items (too_many_items). Aliases: requests / paths / regions. "
                    "caller_cwd required, or \"~global\". Does NOT satisfy the native "
                    "Edit tool's read-precondition.");
                rrsTool["detail"] = QStringLiteral(
                    "Batched read_region: fetch several file slices in ONE "
                    "call instead of N. `items` is an array of {path, + "
                    "exactly one selector: symbol | start_line[/end_line] | "
                    "section}, each with an optional per-item etag_match. "
                    "Returns {ok, results:[…], count, truncated?} — one slice "
                    "envelope per item, in order; an item whose etag_match "
                    "matches its current slice 304s to a compact {path, "
                    "ok:true, unchanged:true, etag} stub, so a re-read after "
                    "editing one file re-sends only the changed slices. A "
                    "bad/missing item path yields a per-item {ok:false, code} "
                    "without aborting the batch. One shared max_bytes budget "
                    "(default 512 KiB, 4 MiB ceiling) is consumed in item "
                    "order. Max 64 items (too_many_items over that). "
                    "caller_cwd required. The read-side mirror of apply_edits' "
                    "batched writes — collapses \"outline → read the 6 "
                    "interesting symbols\" from 7 calls to 2. NOTE (ANTS-3383): "
                    "reading via this verb does NOT satisfy the native Edit "
                    "tool's read-precondition — do a native Read before editing "
                    "a file you intend to modify. ANTS-3500: `requests` / "
                    "`paths` / `regions` are accepted as aliases for `items`. "
                    "ANTS-3589: pass an optional top-level `path` as the "
                    "default for any item that omits its own `path` (per-item "
                    "`path` still wins) — so reading N slices of ONE file is "
                    "just {path, items:[{start_line,end_line}, …]}. "
                    "ANTS-4394: accepts `caller_cwd: \"~global\"` (alias "
                    "`\"~claude-config\"`) for files under ~/.claude/, the same "
                    "sentinel read_region and file_outline take.");
                rrsTool["selection_hint"] = QStringLiteral(
                    "Use when one file_outline/find_definition pass surfaced "
                    "several symbols/sections to read together — batch them "
                    "here instead of one read_region per slice.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    schema["additionalProperties"] = false;
                    QJsonObject props;
                    QJsonObject itemsProp;
                    itemsProp["type"] = "array";
                    itemsProp["description"] = QStringLiteral(
                        "Slices to fetch (1-64). Each item: a `path` plus "
                        "exactly one selector — `symbol` (a function/aggregate "
                        "body), `start_line` (+ optional `end_line`) for a "
                        "line range, or `section` (a markdown heading "
                        "slug/text). Optional per-item `etag_match` 304s an "
                        "unchanged slice.");
                    QJsonObject itemSchema;
                    itemSchema["type"] = "object";
                    itemSchema["additionalProperties"] = false;
                    QJsonObject ip;
                    { QJsonObject p; p["type"] = "string";
                      p["description"] = QStringLiteral(
                          "Project file path resolved under caller_cwd.");
                      ip["path"] = p; }
                    { QJsonObject p; p["type"] = "string";
                      p["description"] = QStringLiteral(
                          "Symbol-body selector (function/method or "
                          "struct/class/union aggregate). Mutually exclusive "
                          "with start_line/end_line/section.");
                      ip["symbol"] = p; }
                    { QJsonObject p; p["type"] = "integer"; p["minimum"] = 1;
                      p["description"] = QStringLiteral(
                          "Line-range selector: 1-based first line.");
                      ip["start_line"] = p; }
                    { QJsonObject p; p["type"] = "integer"; p["minimum"] = 1;
                      p["description"] = QStringLiteral(
                          "Line-range selector: 1-based last line "
                          "(inclusive); defaults to start_line.");
                      ip["end_line"] = p; }
                    { QJsonObject p; p["type"] = "string";
                      p["description"] = QStringLiteral(
                          "Markdown section selector (.md): a heading slug or "
                          "text (ANTS-2221).");
                      ip["section"] = p; }
                    { QJsonObject p; p["type"] = "string";
                      p["description"] = QStringLiteral(
                          "Per-item ETag: when it equals this item's current "
                          "slice etag, the item 304s to an {unchanged:true} "
                          "stub.");
                      ip["etag_match"] = p; }
                    itemSchema["properties"] = ip;
                    QJsonArray itemReq; itemReq.append("path");
                    itemSchema["required"] = itemReq;
                    itemsProp["items"] = itemSchema;
                    props["items"] = itemsProp;
                    { QJsonObject p; p["type"] = "integer"; p["minimum"] = 1;
                      p["description"] = QStringLiteral(
                          "Shared cap on total results[] bytes across the set "
                          "(default 512 KiB, 4 MiB ceiling), consumed in item "
                          "order.");
                      props["max_bytes"] = p; }
                    props["raw"]        = makeRawProp();         // ANTS-2218
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    // ANTS-3568 — declare requests/paths/regions as schema
                    // properties so additionalProperties:false does not strip
                    // these documented aliases (ANTS-3500) before they reach
                    // cmdReadRegions' fallback. Same array shape as `items`
                    // (the handler uses the alias when `items` is absent).
                    // Two of these sent together refuse bad_op_combo
                    // (ANTS-4512), so the description says so.
                    for (const char *alias : {"requests", "paths", "regions"}) {
                        QJsonObject a = itemsProp;
                        a["description"] = QStringLiteral(
                            "Alias for `items` (ANTS-3500) — same array shape. "
                            "Send one name only: two together refuse "
                            "bad_op_combo.");
                        props[QLatin1String(alias)] = a;
                    }
                    // ANTS-3589 — optional top-level `path`: the per-item
                    // default for any item that omits its own `path`. Declared
                    // so additionalProperties:false keeps it (and so it is not
                    // reported in ignored_args). Per-item `path` still wins.
                    { QJsonObject p; p["type"] = "string";
                      p["description"] = QStringLiteral(
                          "Optional default path (ANTS-3589): any item that "
                          "omits its own `path` reads from this file instead, "
                          "so reading N slices of one file need not repeat the "
                          "filename. A per-item `path` still wins.");
                      props["path"] = p; }
                    schema["properties"] = props;
                    QJsonArray required;
                    required.append("items");
                    schema["required"] = required;
                    rrsTool["inputSchema"] = schema;
                }
                tools.append(rrsTool);

                // ANTS-2094 — read_spill: re-read a body the offload path
                // spilled to a content-addressed cache file, by its handle,
                // byte-paged. Pairs with the offload envelope's `hint`.
                QJsonObject rsTool;
                rsTool["name"] = "read_spill";
                rsTool["description"] = QStringLiteral(
                    "Re-read a result that was offloaded ({offloaded:true, handle, "
                    "head, ...}) by its `handle`, the 64-hex sha256. Byte paging: "
                    "offset + max_bytes (default 512 KiB, 4 MiB ceiling), advancing "
                    "by the RETURNED offset+bytes; returns {content, offset, bytes, "
                    "total_bytes, truncated}. Row paging over the envelope's array: "
                    "row_offset + row_count; returns {mode:\"rows\", key, rows, "
                    "row_offset, total_rows, population, truncated}. Refusals: "
                    "bad_args, not_found (evicted: re-issue the call), too_large, "
                    "not_array. caller_cwd optional.");
                rsTool["detail"] = QStringLiteral(
                    "Re-read a large result that was offloaded (observation "
                    "masking) — when a read verb returned {offloaded:true, "
                    "handle, head, ...} instead of the full body, fetch the "
                    "full body here by `handle`, byte-paged. Args: handle "
                    "(required, the 64-hex sha256 from the envelope), optional "
                    "offset (0-based byte offset, default 0) and max_bytes "
                    "(default 512 KiB, 4 MiB ceiling). Page by advancing to the "
                    "RETURNED offset+bytes (a UTF-8 char-boundary cut can "
                    "shorten a slice). Returns {content, offset, bytes, "
                    "total_bytes, truncated}. ANTS-3545 — for an offloaded body "
                    "whose preview showed head_rows_key (an array of rows), "
                    "pass row_offset/row_count instead to page that array by "
                    "ROW, parsed: returns a DIFFERENT envelope {mode:\"rows\", "
                    "key, rows, row_offset, total_rows, population, truncated} (no "
                    "content); page on via row_offset + rows.size(). "
                    "ANTS-4397: an offload envelope whose row BODIES do not "
                    "all fit carries `rows_preview` — one shape row per row, "
                    "{index, bytes, head} — instead of a prefix that shows one "
                    "row and says nothing about the rest. Use `bytes` to pick "
                    "what to fetch. ANTS-4474: when a full-width sample will "
                    "not cover every row the head is NARROWED before it is "
                    "dropped — the width steps 60 → 40 → 24 → 12 and the "
                    "narrowest that covers them all is used, reported as "
                    "`rows_preview_head_chars` (present only when below the "
                    "60 default). A row label, a Markdown heading, a table "
                    "cell name and a function signature all live in the first "
                    "~40 characters, so a narrowed head still says WHAT a row "
                    "is — the one question `bytes` cannot answer. ANTS-4519: "
                    "only when NO width covers every row is `rows_preview` "
                    "OMITTED rather than sent headless (a bare list of row "
                    "LENGTHS says no more than `row_count` + `bytes`); "
                    "`rows_preview_omitted:true` marks that case. `head_rows` "
                    "is unchanged and still the right preview for many small "
                    "rows. Refusals: "
                    "bad_args (bad handle, or negative "
                    "offset/max_bytes/row_offset/row_count), "
                    "not_found (never spilled or evicted — re-issue the "
                    "original call), too_large / not_array (row mode, on a "
                    "> 1 MiB or non-array body — byte-page it instead). The "
                    "spill store is global/content-"
                    "addressed, so caller_cwd is optional.");
                rsTool["selection_hint"] = QStringLiteral(
                    "Use only after a read verb returned an offloaded:true "
                    "envelope, to fetch the rest of the body via its handle — "
                    "byte-paged, or (if the preview showed head_rows_key) "
                    "row-paged with row_offset/row_count.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    schema["additionalProperties"] = false;
                    QJsonObject props;
                    QJsonObject hP; hP["type"] = "string";
                        hP["description"] = QStringLiteral(
                            "Content-addressed handle from the offload "
                            "envelope: a bare lowercase 64-char sha256.");
                    QJsonObject oP; oP["type"] = "integer";
                        oP["minimum"] = 0;
                        oP["description"] = QStringLiteral(
                            "0-based byte offset into the body (default 0). "
                            "Page by the RETURNED offset+bytes.");
                    QJsonObject mbP; mbP["type"] = "integer";
                        mbP["minimum"] = 1;
                        mbP["description"] = QStringLiteral(
                            "Cap on the returned slice in bytes (default "
                            "512 KiB, server-clamped to 4 MiB).");
                    // ANTS-3545 — row-paging args (row mode). integer,
                    // minimum:0 (matching byte-mode offset); a numeric value
                    // routes cmdReadSpill to readSpillRows.
                    QJsonObject roP; roP["type"] = "integer";
                        roP["minimum"] = 0;
                        roP["description"] = QStringLiteral(
                            "Row-paging: 0-based index of the first row of the "
                            "offloaded body's dominant array (the head_rows_key "
                            "array). Presence of row_offset or row_count "
                            "switches read_spill to row mode; page on by "
                            "row_offset + rows.size().");
                    QJsonObject rcP; rcP["type"] = "integer";
                        rcP["minimum"] = 0;
                        rcP["description"] = QStringLiteral(
                            "Row-paging: number of rows to return (default 100; "
                            "0/omitted = default). Row mode returns "
                            "{mode:\"rows\", key, rows, row_offset, total_rows, "
                            "truncated}. ANTS-4375: `total_rows` is the "
                            "rows IN the "
                            "spill file, which is what paging is over; "
                            "`population` is what the producing verb was "
                            "reporting on, and `rows_are_partial:true` fires "
                            "when the array was capped BEFORE spilling — then "
                            "paging to the end is NOT completeness, however "
                            "`truncated` reads.");
                    props["handle"]     = hP;
                    props["offset"]     = oP;
                    props["max_bytes"]  = mbP;
                    props["row_offset"] = roP;     // ANTS-3545
                    props["row_count"]  = rcP;     // ANTS-3545
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    schema["properties"] = props;
                    QJsonArray required;
                    required.append("handle");
                    schema["required"] = required;
                    rsTool["inputSchema"] = schema;
                }
                tools.append(rsTool);

                // ANTS-2022 — apply_edits: apply N {path, old, new} edits
                // across M project files in one atomic-per-file call,
                // instead of one native Edit round-trip per site.
                QJsonObject aeTool;
                aeTool["name"] = "apply_edits";
                aeTool["description"] = QStringLiteral(
                    "Apply a batch of {path, old, new} edits across project files "
                    "in one call. Without replace_all, `old` must occur exactly "
                    "once (else skipped: not_found or ambiguous, with candidates). "
                    "An edit may instead name start_line/end_line plus "
                    "expect_first_line/expect_last_line (range_mismatch, "
                    "range_out_of_bounds). Atomic per file; edits to one file apply "
                    "in order. Returns applied[] + skipped[] + counts. A path "
                    "escaping the root fails the call (bad_path). A transport "
                    "timeout means UNKNOWN: re-read before re-sending. A request "
                    "over 256 KiB is refused unapplied; split large "
                    "batches. caller_cwd required.");
                aeTool["detail"] = QStringLiteral(
                    "Apply a batch of {path, old, new} edits across one or "
                    "more project files in ONE call — instead of a native "
                    "Edit round-trip per site. Each edit replaces `old` with "
                    "`new` in `path`; without replace_all, `old` must occur "
                    "exactly once (else the edit is skipped: not_found for 0, "
                    "ambiguous for >1). Atomic per file (QSaveFile); multiple "
                    "edits to one file apply in array order. A path escaping "
                    "the project root fails the whole call (bad_path) — with "
                    "ONE deliberate exception (ANTS-3430/3616): a "
                    "`*_Ants_MCP_Feedback.md` file sitting directly in the "
                    "project root's PARENT directory is reachable, because "
                    "that shared cross-session file lives outside every "
                    "project by convention. A "
                    "missing file / absent-or-ambiguous old / >4 MiB file / "
                    "failed commit is a per-edit skip. Returns applied[] (one "
                    "per file) + skipped[] (one per edit) + counts. "
                    "ANTS-4733: a TRANSPORT TIMEOUT means UNKNOWN, never "
                    "failed. Measured -- a batch reported a transport "
                    "timeout, a check of the files immediately afterwards "
                    "showed NOTHING written, and the write then landed. So "
                    "the obvious recovery (look, see no change, re-send) "
                    "races a write still in flight. RE-READ before deciding, "
                    "and prefer the unique-`old` form: its not_found refusal "
                    "is what makes a duplicate re-send safe, and neither "
                    "`replace_all` nor a start_line/end_line range has an "
                    "equivalent guard. "
                    "ANTS-4418: a `not_found` skip whose text differs from the "
                    "file ONLY in spacing or punctuation (ANTS-4857: dashes, "
                    "quotes, ellipses) carries `candidates:[{line,text}]`, "
                    "`near_miss_line` and a `hint` — same field names as "
                    "read_region section-mode and roadmap_log's locators, so "
                    "one caller path handles all three. Retry with that line's "
                    "verbatim `text`, or with the start_line/end_line form for "
                    "that line; do NOT re-read the file and guess the spacing "
                    "again, which is the round-trip this exists to remove. "
                    "Reported only when the near miss is UNIQUE and "
                    "whitespace-class, and only for a single-line `old`. "
                    "ANTS-4473: an `ambiguous` skip carries the SAME "
                    "`candidates:[{line,text}]` naming where the occurrences "
                    "are, plus `match_count` (the true total) and a `hint`. "
                    "The two cases are mutually exclusive, so one caller path "
                    "covers both. The list is capped at 10 and sets "
                    "`candidates_truncated` when it bites — `match_count` is "
                    "always the real number. Recover by extending `old` with "
                    "surrounding context, by using the start_line/end_line "
                    "form for the occurrence you mean, or with "
                    "replace_all:true to change every one. "
                    "caller_cwd required. "
                    "LINE RANGES (ANTS-3711): an edit may instead name an "
                    "inclusive 1-based `start_line`/`end_line` range — use it "
                    "to replace or delete a large contiguous block without "
                    "re-emitting bytes you already read (`old` and a range "
                    "together → bad_args). A range edit MUST also carry "
                    "`expect_first_line`/`expect_last_line`, the verbatim text "
                    "of those two lines: `old` guards itself by being unique, "
                    "a line number guards nothing, so without them a stale "
                    "number silently replaces the wrong lines. Mismatch → a "
                    "`range_mismatch` skip; out-of-file coordinates → "
                    "`range_out_of_bounds`. read_region hands back both the "
                    "numbers and the text. Ranges resolve against the file as "
                    "EARLIER edits in the same call left it, so a batch that "
                    "shifts line counts will trip the guard — order "
                    "line-shifting edits last, or send them bottom-up. "
                    "BATCH SIZE (ANTS-3712): the 4 MiB above is a cap on the "
                    "FILE being edited, NOT on this call's arguments — the "
                    "request itself is far smaller-bounded by the client's "
                    "tool-call transport. A batch whose combined old+new runs "
                    "to several KB can fail BEFORE this server sees it, as a "
                    "client-side `could not be parsed as JSON` whose hints "
                    "blame escaping; the byte count it echoes is the real "
                    "clue. If you hit that, split edits[] into smaller calls "
                    "rather than auditing your escaping — identical content "
                    "applies cleanly in halves. dry_run cannot diagnose it "
                    "(the call never arrives). "
                    "ANTS-4856: a batch that applied some edits and skipped "
                    "others carries `partial:true` (`would_be_partial:true` "
                    "on a dry run). Its files hold neither the old text nor "
                    "the new, so read skipped[] before building on them.");
                aeTool["selection_hint"] = QStringLiteral(
                    "Use for a multi-site sweep (same change across N files) "
                    "to collapse N native Edit calls into one atomic batch.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    schema["additionalProperties"] = false;
                    QJsonObject props;
                    QJsonObject editsProp; editsProp["type"] = "array";
                        editsProp["description"] = QStringLiteral(
                            "Non-empty list of edits. Each is {path, new} plus "
                            "EITHER {old, replace_all?} OR (ANTS-3711) "
                            "{start_line, end_line, expect_first_line, "
                            "expect_last_line} — never both. ANTS-4089: "
                            "old_string/new_string are accepted as aliases for "
                            "old/new, so the native Edit tool's spelling works "
                            "here unchanged. ANTS-4936: so are "
                            "old_text/new_text, roadmap_log "
                            "op:\"amend_body\"'s spelling — and `path` may be "
                            "given ONCE at the top level instead of on every "
                            "edit. Canonical old/new/per-edit path win when "
                            "both are sent.");
                    QJsonObject items; items["type"] = "object";
                        items["additionalProperties"] = false;
                        QJsonObject ip;
                        QJsonObject pP; pP["type"] = "string";
                            pP["description"] = QStringLiteral(
                                "File path resolved under caller_cwd.");
                        QJsonObject oP; oP["type"] = "string";
                            oP["description"] = QStringLiteral(
                                "Substring to replace (non-empty; must be "
                                "unique unless replace_all). Omit when using "
                                "a start_line/end_line range instead.");
                        QJsonObject nP; nP["type"] = "string";
                            nP["description"] = QStringLiteral(
                                "Replacement text (may be empty for a "
                                "deletion).");
                        QJsonObject rP; rP["type"] = "boolean";
                            rP["description"] = QStringLiteral(
                                "Replace every occurrence of `old` (default "
                                "false → require a unique match).");
                        // ANTS-3711 — the line-range alternative to `old`.
                        QJsonObject slP; slP["type"] = "integer";
                            slP["minimum"] = 1;
                            slP["description"] = QStringLiteral(
                                "First line of the range to replace (1-based, "
                                "inclusive). Alternative to `old`; requires "
                                "end_line + both expect_* lines.");
                        QJsonObject elP; elP["type"] = "integer";
                            elP["minimum"] = 1;
                            elP["description"] = QStringLiteral(
                                "Last line of the range to replace (1-based, "
                                "inclusive; may equal start_line).");
                        QJsonObject efP; efP["type"] = "string";
                            efP["description"] = QStringLiteral(
                                "Verbatim text of line start_line. Mandatory "
                                "on a range edit — it is the range's only "
                                "staleness guard. Mismatch → range_mismatch "
                                "skip, no write.");
                        QJsonObject elnP; elnP["type"] = "string";
                            elnP["description"] = QStringLiteral(
                                "Verbatim text of line end_line. Mandatory on "
                                "a range edit; checked together with "
                                "expect_first_line so a range that shifted at "
                                "either end is caught.");
                        // ANTS-4089 — old_string/new_string as aliases for
                        // old/new, because that is the spelling every session
                        // arrives holding from the native Edit tool. They have
                        // to be DECLARED: additionalProperties is false, so an
                        // undeclared key is refused before the handler sees it.
                        QJsonObject osP; osP["type"] = "string";
                            osP["description"] = QStringLiteral(
                                "Alias for `old` — the native Edit tool's "
                                "spelling, accepted so this verb is a drop-in "
                                "for it. Sending two spellings refuses bad_op_combo.");
                        QJsonObject nsP; nsP["type"] = "string";
                            nsP["description"] = QStringLiteral(
                                "Alias for `new` — the native Edit tool's "
                                "spelling. Sending two spellings refuses bad_op_combo.");
                        // ANTS-4936 — old_text/new_text, the spelling
                        // roadmap_log op:"amend_body" uses. Declared for the
                        // same reason ANTS-4089's pair is: additionalProperties
                        // is false, so an undeclared key is refused before the
                        // handler ever sees it.
                        QJsonObject oldTextP; oldTextP["type"] = "string";
                            oldTextP["description"] = QStringLiteral(
                                "Alias for `old` — roadmap_log "
                                "op:\"amend_body\"'s spelling, accepted so a "
                                "caller arriving from that verb guesses "
                                "consistently. Sending two spellings refuses "
                                "bad_op_combo.");
                        QJsonObject ntP; ntP["type"] = "string";
                            ntP["description"] = QStringLiteral(
                                "Alias for `new` — roadmap_log "
                                "op:\"amend_body\"'s spelling. Sending two "
                                "spellings refuses bad_op_combo.");
                        ip["path"] = pP; ip["old"] = oP; ip["new"] = nP;
                        ip["old_string"] = osP; ip["new_string"] = nsP;
                        ip["old_text"] = oldTextP; ip["new_text"] = ntP;
                        ip["replace_all"] = rP;
                        {   // ANTS-4838 — the count guard replace_all lacks.
                            QJsonObject ecP; ecP["type"] = "integer";
                            ecP["minimum"] = 0;
                            ecP["description"] = QStringLiteral(
                                "Optional, `old` form only. How many times "
                                "`old` must match; a different count skips "
                                "the edit as count_mismatch with the real "
                                "match_count, leaving the file untouched. "
                                "Refused with a line range.");
                            ip["expect_count"] = ecP;
                        }
                        ip["start_line"] = slP; ip["end_line"] = elP;
                        ip["expect_first_line"] = efP;
                        ip["expect_last_line"]  = elnP;
                        items["properties"] = ip;
                        // Neither `old` nor `new` is unconditionally required:
                        // an edit satisfies the handler with either selector
                        // and either spelling, and both rules are enforced
                        // there (ANTS-3711, ANTS-4089). `new` left in required
                        // would refuse every new_string call before it landed.
                        // ANTS-4936 — `path` is no longer schema-required per
                        // edit: a top-level `path` may supply it. An edit that
                        // resolves to NO path is still refused, by the handler,
                        // whose message names both missing keys — a better
                        // answer than a schema rejection that names neither.
                        items["required"] = QJsonArray{};
                    editsProp["items"] = items;
                    props["edits"]      = editsProp;
                    {   // ANTS-4936 — batch-level default for `path`.
                        QJsonObject bp; bp["type"] = "string";
                        bp["description"] = QStringLiteral(
                            "Optional (ANTS-4936). Default `path` for every "
                            "edit in `edits[]` that omits one — a per-edit "
                            "`path` still wins. A single-file batch reads "
                            "naturally with the path stated once, and that "
                            "spelling used to be accepted, dropped into "
                            "ignored_args, and THEN refused for the missing "
                            "per-edit key. An edit left with no path either "
                            "way is refused.");
                        props["path"] = bp;
                    }
                    {   // ANTS-4834 — the shared dry_run text plus what THIS
                        // verb's preview envelope looks like. A caller cannot
                        // see from the generic wording that the past-tense
                        // keys are gone, and that is the whole point of the
                        // change: `files_written` is the natural key to
                        // branch on, so its absence has to be discoverable.
                        QJsonObject p = makeDryRunProp();
                        p["description"] = p.value("description").toString() +
                            QStringLiteral(
                                " ANTS-4834 — on a dry run this verb reports "
                                "in the FUTURE tense: `would_write` (the paths "
                                "the real call would touch), `would_apply` and "
                                "`would_apply_count`, with `files_written`, "
                                "`applied` and `edits_applied` ABSENT. Same "
                                "resolution ANTS-4463 applied to roadmap_log, "
                                "and for the same reason — a caller branching "
                                "on a past-tense key reads a preview as a "
                                "completed write. `skipped[]` and its "
                                "`candidates` / `near_miss_line` diagnostics "
                                "are unchanged: they are why a preview is "
                                "worth running.");
                        props["dry_run"] = p;                    // ANTS-2227
                    }
                    {   // ANTS-4723 — wrapped-match opt-in.
                        QJsonObject mw;
                        mw["type"] = "boolean";
                        mw["default"] = false;
                        mw["description"] = QStringLiteral(
                            "Optional (ANTS-4723). Apply an edit whose `old` "
                            "agrees with the file except in WHERE its line "
                            "breaks fall — a run of whitespace in `old` "
                            "matches a run of whitespace in the file. Prose "
                            "here is hard-wrapped, so an `old` copied from a "
                            "differently-wrapped rendering of the same text "
                            "is present and exact apart from one newline "
                            "position, and a verbatim search reports "
                            "not_found. In a MULTI-EDIT batch that is the "
                            "expensive miss: the edits before it have already "
                            "landed, so the file has moved under you while you "
                            "work out why. Consulted only where the verbatim "
                            "search found nothing, so it can only turn a "
                            "refusal into an edit and never changes a call "
                            "that already succeeds; uniqueness is enforced on "
                            "it exactly as on the verbatim pass, so a phrase "
                            "occurring twice under the looser rule is "
                            "`ambiguous` rather than a licence to take the "
                            "first. It DECLINES a span whose continuation "
                            "lines carry structure a re-flow would destroy — "
                            "column alignment, or the opening of a new list "
                            "item (ANTS-4612) — and reports the near miss "
                            "instead of writing. When it fires, the envelope "
                            "carries `wrapped_match:true` and "
                            "`wrapped_match_indices`, so a re-flowed span is "
                            "never silent. Ignored with `replace_all`: "
                            "\"every occurrence\" of a phrase whose "
                            "boundaries are approximate is not a set you can "
                            "predict. Off by default — this verb edits files "
                            "it did not author and must not quietly rewrite "
                            "line breaks. You do NOT need to guess when to "
                            "set it: a not_found whose cause is a wrap "
                            "mismatch already names this flag in its `hint`. "
                            "Same rule and same spelling as workspace_search's "
                            "`match_wrapped` (ANTS-4547).");
                        props["match_wrapped"] = mw;
                    }
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    schema["properties"] = props;
                    QJsonArray required;
                    required.append("edits");
                    schema["required"] = required;
                    aeTool["inputSchema"] = schema;
                }
                tools.append(aeTool);
}

}  // namespace claudeintegrationdetail
