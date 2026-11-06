// ANTS-1677 claudeintegration piece 3/9 — the terminal and tab tool descriptors
#include "claudeintegration_internal.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace claudeintegrationdetail {

void appendToolSchemasTerminal(QJsonArray &tools) {
                QJsonObject scrollbackTool;
                scrollbackTool["name"] = "get_scrollback";
                scrollbackTool["description"] = QStringLiteral(
                    "Get the last N lines of terminal scrollback. "
                    "At most 10000 lines, byte-trimmed like get_text; "
                    "a reply cut by the line cap starts with "
                    "`<capped at L of R requested lines>` (ANTS-5219). "
                    "Pass `caller_cwd` (your $PWD) to anchor to your "
                    "tab; without it the call refuses "
                    "`tab_or_cwd_required` (ANTS-1415). "
                    "ANTS-1500: pass `since_cursor` (from a prior "
                    "response) for incremental-fetch mode — server "
                    "returns only the bytes appended since the cursor "
                    "(envelope: {ok, content, cursor, cursor_stale, "
                    "stale_reason?}) instead of the full window. "
                    "Saves 80-95% on rapid-polling loops. Stale "
                    "cursors (ring wrap, terminal restart) flip "
                    "cursor_stale:true and fall back to the full "
                    "window.");
                // ANTS-1453 — selection_hint: one-sentence
                // form-factor cue for the calling assistant.
                scrollbackTool["selection_hint"] = QStringLiteral(
                    "Use when you need recent terminal output (e.g. "
                    "the last build error). Prefer over Read/Grep "
                    "when the data lives in stdout, not on disk. "
                    "Polling loops: pass since_cursor for delta-only "
                    "responses (ANTS-1500).");
                // ANTS-1395 — scope props/schema inside a block so every
                // subsequent tool's matching declarations (each already
                // wrapped in `{ ... }`) no longer trigger
                // -Wshadow=compatible-local against this outer-scope pair.
                {
                    QJsonObject linesParam;
                    linesParam["type"] = "integer";
                    linesParam["default"] = 50;
                    QJsonObject props;
                    props["lines"] = linesParam;
                    // ANTS-1392 — caller_cwd anchor for the terminal-state
                    // verbs. Absent, the call refuses tab_or_cwd_required.
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    // ANTS-1500 — since_cursor opt-in for incremental
                    // mode. Absent → legacy raw-text return. Present →
                    // JSON envelope with content+cursor+stale flag.
                    QJsonObject sinceProp;
                    sinceProp["type"] = "string";
                    sinceProp["description"] = QStringLiteral(
                        "Optional. Opaque cursor token from a prior "
                        "response. When present, the server returns "
                        "only content appended since the cursor was "
                        "issued; cursor_stale:true falls back to the "
                        "full window (terminal restart, ring wrap). "
                        "Triggers JSON-envelope response shape; absent "
                        "preserves the legacy raw-text return.");
                    props["since_cursor"] = sinceProp;
                    QJsonObject schema;
                    schema["type"] = "object";
                    schema["properties"] = props;
                    scrollbackTool["inputSchema"] = schema;
                }
                tools.append(scrollbackTool);

                // MCP spec requires every tool to declare an inputSchema
                // (even zero-arg tools — empty object schema). Claude
                // Code's Zod validator rejects the whole tools/list
                // response if any entry omits it, silently registering
                // zero tools.
                QJsonObject emptySchema;
                emptySchema["type"] = "object";

                QJsonObject cwdTool;
                cwdTool["name"] = "get_cwd";
                cwdTool["description"] = QStringLiteral(
                    "Echo `caller_cwd` (your $PWD) back canonical "
                    "(ANTS-1391). Without it the call refuses "
                    "`tab_or_cwd_required` (ANTS-1415).");
                cwdTool["selection_hint"] = QStringLiteral(
                    "Use when you need the canonical form of your "
                    "$PWD (multi-tab disambiguation, symlink resolve). "
                    "Cheap (~100 B).");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    QJsonObject callerProp;
                    callerProp["type"] = "string";
                    callerProp["description"] = QStringLiteral(
                        "Your $PWD, echoed back canonical. Absent, the "
                        "call refuses `tab_or_cwd_required`.");
                    props["caller_cwd"] = callerProp;
                    schema["properties"] = props;
                    cwdTool["inputSchema"] = schema;
                }
                tools.append(cwdTool);

                QJsonObject sessionTool;
                sessionTool["name"] = "get_session_info";
                sessionTool["description"] = "Get terminal session metadata";
                sessionTool["selection_hint"] = QStringLiteral(
                    "Use once at session start to learn tab/process "
                    "layout. Control-plane (no caller_cwd anchor); "
                    "cheap.");
                sessionTool["inputSchema"] = emptySchema;
                tools.append(sessionTool);

                QJsonObject lastCmdTool;
                lastCmdTool["name"] = "get_last_command";
                lastCmdTool["description"] = QStringLiteral(
                    "Get the last command's exit code and output "
                    "(via shell integration). Optional `mode` — "
                    "\"full\" (default) returns {exit_code, output, "
                    "failed}; \"summary\" (ANTS-1503) returns "
                    "{exit_code, line_count, last_20[], ms, failed} "
                    "— skip the full body when you only need "
                    "exit + tail + duration. ") + callerCwdSuffix();
                lastCmdTool["selection_hint"] = QStringLiteral(
                    "Use after a shell command to inspect exit code "
                    "+ output without scraping scrollback. Requires "
                    "OSC 133 shell integration. Prefer mode:summary "
                    "for status checks (≤500 bytes vs full mode's "
                    "1-4 KiB).");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    QJsonObject modeProp;
                    modeProp["type"] = "string";
                    QJsonArray modeEnum;
                    modeEnum.append("full");
                    modeEnum.append("summary");
                    modeProp["enum"] = modeEnum;
                    modeProp["description"] = QStringLiteral(
                        "Response shape. \"full\" (default) — "
                        "{exit_code, output, failed}. \"summary\" — "
                        "{exit_code, line_count, last_20[], ms, "
                        "failed}.");
                    props["mode"] = modeProp;
                    schema["properties"] = props;
                    lastCmdTool["inputSchema"] = schema;
                }
                tools.append(lastCmdTool);

                QJsonObject envTool;
                envTool["name"] = "get_environment";
                envTool["description"] = QStringLiteral(
                    "Get shell environment info (PATH, virtualenv, "
                    "key env vars). ") + callerCwdSuffix();
                envTool["selection_hint"] = QStringLiteral(
                    "Use when env vars matter for the diagnosis "
                    "(PATH resolution, language venv, terminal "
                    "type). Read-only.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    props["etag_match"] = makeEtagMatchProp();   // ANTS-1499
                    props["fields"] = makeFieldsProp();          // ANTS-1720
                    props["compact"] = makeCompactProp();        // ANTS-2091
                    schema["properties"] = props;
                    envTool["inputSchema"] = schema;
                }
                tools.append(envTool);

                // ANTS-1244 — surface 3 existing remote-control verbs
                // as MCP tools so a Claude session in an Ants tab can
                // query terminal state via tool-call rather than
                // `Bash`/`Read`. See docs/specs/ANTS-1244.md.
                //
                // ANTS-1247-INV-8: `status` filter declared in inputSchema
                // with enum [all|active|shipped] + description citing
                // token counts so Claude prefers `active` for planning
                // queries; the description carries the measured figure.
                QJsonObject roadmapTool;
                roadmapTool["name"] = "roadmap_query";
                // ANTS-1287 — `section` arg added. Description cites
                // the partial-query saving so Claude prefers section
                // slices when only one block is needed.
                // ANTS-3409 — the wire one-liner overran its 800 B budget
                // (mcp_tool_detail_field INV-5) by 14 B; the `bundles` mode
                // gloss said "bundle" twice, so its redundant middle was
                // trimmed (full per-op detail is in `detail` below / via
                // tool_info {name:"roadmap_query"}).
                roadmapTool["description"] = QStringLiteral(
                    "Query ROADMAP.md as structured bullets; a list row is "
                    "{id, status, headline_oneline, section_slug}. Filters: "
                    "status (all|active|shipped), section=<slug>, "
                    "query=<keyword>, id / "
                    "ids[]. mode: "
                    "bullets (default) | section_index (all slugs; one "
                    "item's: fetch by id) | "
                    "headline_only | bundles | report | sources | "
                    "ledger. Opt-in: include_body, "
                    "compact, fields, etag_match. Refusals: bad_case, "
                    "bad_section, bad_mode_combo, no_roadmap_loaded. "
                    "caller_cwd Required. "
                    "`source` = the backend that answered, not `path`.");
                // ANTS-2079 — full per-op reference lives in `detail`,
                // stripped from the tools/list wire and served on demand
                // by tool_info {name:"roadmap_query"}.
                roadmapTool["detail"] = QStringLiteral(
                    "ANTS-4611 — `no_roadmap_loaded` IS AN ANSWER, not a "
                    "failed call: this project keeps no ROADMAP.md, which is "
                    "true of 9 of the 24 projects on this machine. Branch on "
                    "it rather than retrying or switching tab. Where you "
                    "passed caller_cwd the envelope carries `resolved_root` "
                    "and the message names it, so a project that keeps no "
                    "roadmap is distinguishable from a root that resolved "
                    "somewhere you did not mean; the active tab is named only "
                    "where the tab is what supplied the path. "
                    "ANTS-4402 — WHICH BACKEND ANSWERED. The envelope "
                    "carries `source`: \"store\" or \"markdown\". `path` "
                    "names ROADMAP.md on BOTH, so it does not tell you. On "
                    "a migrated project the store is authoritative and an "
                    "edit made to ROADMAP.md by hand is NOT reflected here "
                    "— it will not appear in bullets, and an id filed that "
                    "way comes back in `missing_ids`. "
                    "`file_ahead_of_store:true` (with `file_highest_id` and "
                    "`store_high_water`) means the file holds ids the store "
                    "has never seen, so the answer is PROVABLY stale. It is "
                    "one-directional: absence is not proof of freshness, "
                    "because a status flip or a body edit moves no id. "
                    "ANTS-4462 — `check_sync:true` answers the SAME question "
                    "in both directions and by CONTENT rather than by id, "
                    "which is what that one-directional gap costs: two ✅ "
                    "headlines were served as 📋 with an ok:true envelope and "
                    "no id anywhere out of range. It renders the store and "
                    "compares it against the live file — the same render and "
                    "the same comparison roadmap_log runs before it "
                    "overwrites, so \"is my file in sync?\" and \"what would a "
                    "write overwrite?\" can never disagree. Returns "
                    "`file_in_sync`, plus `drift_lines` / `drift_restyled` / "
                    "`drift_lost` / `drift_lost_text` on the drifted arm only; "
                    "ANTS-5382 adds `drift_sample` ({file, render} rows) and "
                    "`drift_header_only`; ANTS-5381 adds `drift_cause` "
                    "(file, store, file_and_store, renderer, unknown). "
                    "`sync_checked:false` means nobody looked (not "
                    "store-backed, or the render failed) and must NOT be read "
                    "as in-sync. ANTS-4730 — it reports on EVERY arm, so its "
                    "absence means only that check_sync was not requested. "
                    "OPT-IN and never cached, deliberately: one "
                    "render of a 2,267-item store measures ~204 ms, more than "
                    "this verb's whole 100 ms bullet-cache TTL, so it cannot "
                    "ride every query — and a cached answer to \"is my file in "
                    "sync RIGHT NOW\" would be worse than none. Ask once at "
                    "orientation. "
                    "`query=<keyword>` is a CASE-INSENSITIVE substring "
                    "match over headline AND body; `id` fetches one bullet "
                    "and `ids[]` a bundle, both by [PROJ-NNNN]. (Both "
                    "glosses moved here from the wire description when "
                    "ANTS-4402 added `source` — the tools/list budget is "
                    "800 B and every session pays it, so the wire keeps "
                    "the names and this keeps the semantics.) "
                    "ANTS-4575 — WHETHER AN ID WAS WRITTEN DOWN. A bullet "
                    "carries `id_inferred:true` when the reader ADOPTED its "
                    "id from a bold prose lead-in instead of reading a "
                    "bracket token the author wrote: `- [ ] **Terrain "
                    "System** — …` is reported with id \"Terrain System\". "
                    "Gated, so a roadmap whose ids are all bracketed sends "
                    "the same bytes as before. Two things it does NOT say. "
                    "It is not `synthetic`, which flags a content-hash id "
                    "manufactured when there was no token at all — the two "
                    "never both fire. And it describes THE TEXT THIS CALL "
                    "READ: a migrated project's ROADMAP.md is rendered from "
                    "the store with every id in brackets, so the flag goes "
                    "quiet there rather than reporting on how the id was "
                    "first obtained. Nothing in the text separates an "
                    "inferred id from a real multi-word one, which is why "
                    "two bullets sharing a lead-in silently collide; "
                    "ANTS-3771 (declare the project's id format) is the "
                    "fix for that, and this flag is only the warning. "
                    "Query the active tab's ROADMAP.md as structured "
                    "bullets. ANTS-5703 — a list row is {id, status, "
                    "headline_oneline, section_slug}; `include_body` or "
                    "`bullet_fields` gives the wide row {id, status, "
                    "headline, headline_oneline, kind, lanes, source, "
                    "section_slug}, and an id/ids fetch always does. "
                    "`headline_oneline` "
                    "(ANTS-1521) is `headline` with newlines + "
                    "whitespace runs collapsed to a single space — safe "
                    "to concatenate into a summary without post-"
                    "processing. When the parser capped a long headline at "
                    "120 chars, the bullet also carries `headline_full` "
                    "(ANTS-2075) — the untruncated text, usable as a "
                    "roadmap_log headline locator. Optional "
                    "`include_body:true` "
                    "(ANTS-1517) adds a `body` field (truncated to "
                    "~2000 chars, `body_truncated:true` set on "
                    "truncation; a truncated body keeps its head AND "
                    "its tail either side of an explicit elision "
                    "marker — ANTS-3736) — saves the 3-5 follow-up Reads a "
                    "session does to pick up Kind / Lanes / Source "
                    "prose from a dense bundle table. Optional "
                    "`status` filter — \"active\" "
                    "(📋+🚧, ~1.7 K tokens — recommended for planning "
                    "queries) / \"shipped\" (✅ only) / \"all\" (default, "
                    "~12 K tokens). Optional `section` slug — returns "
                    "only bullets within that ## or ### heading "
                    "(e.g. \"performance\", \"080\"); response carries "
                    "`section` echo. Optional `id` — fetch ONE bullet "
                    "by its [PROJ-NNNN] id (e.g. \"ANTS-1853\") in a "
                    "single call instead of paging; bypasses status + "
                    "pagination, includes the body by default, returns "
                    "{ok, bullets, count, id, found} (ANTS-1856). "
                    "Optional `ids` array — plural sibling of `id` for "
                    "N-bullet bundle fetches in one call; same bypass + "
                    "body-by-default, envelope adds matched_ids/"
                    "missing_ids accounting (ANTS-1726). Max 100 ids. "
                    "ANTS-1696 — section= empties also carry a "
                    "`section_shape` (\"table\"|\"prose\") + "
                    "`non_bullet_lines` hint when the slice has "
                    "non-bullet content (e.g. a planning table or prose "
                    "block); lets the caller skip a raw Read fallback. "
                    "Absent on bullet-rich and truly-empty sections "
                    "(back-compat). "
                    "Optional `mode` — \"bullets\" "
                    "(default) / \"section_index\" (returns a compact "
                    "{slug, headline, level, active_count, "
                    "shipped_count, total_count, active_count_id_only, "
                    "shipped_count_id_only, total_count_id_only}[] "
                    "index instead of bullets[] — use for slug discovery "
                    "before drilling in via section=; honours `status` so "
                    "status:\"active\" lists only sections with active work "
                    "(ANTS-1848)) / \"headline_only\" (ANTS-1881 — "
                    "bullets[] narrowed to id + status + headline_oneline "
                    "+ section_slug per bullet, ~10× smaller payload on "
                    "dense bundle sections; composes with section=, "
                    "status=, pagination, ETag). The `*_id_only` "
                    "parallels (ANTS-1622) count only bullets that "
                    "carry a [PROJ-NNNN] id, matching the default "
                    "bullets[] predicate — when `active_count > "
                    "active_count_id_only`, the section has legacy "
                    "narrator-prose entries that would be invisible to "
                    "a default `bullets[]` query; pass "
                    "include_narrator_bullets:true to retrieve them. "
                    "The envelope also surfaces a top-level "
                    "`legacy_format_sections[]` array listing slugs "
                    "where every direct bullet is narrator-only; the "
                    "same sections carry a per-section `legacy_format:"
                    "true` flag (ANTS-1714b) so you can spot them "
                    "without grepping the slug against the array. "
                    "ANTS-2052 — on a FULLY id-less roadmap status:active/"
                    "shipped would otherwise drop every section (all "
                    "*_id_only:0) and return sections:[]; the branch then "
                    "falls back to the raw emoji count so sections still "
                    "list, and the envelope carries top-level "
                    "`legacy_format:true` + `raw_active_count` + "
                    "`raw_shipped_count` (emoji-based) so a session_orient "
                    "bundle no longer reads as an empty queue. "
                    "ANTS-4964 — `parseable_bullets: 0` and its `warning` "
                    "appear only when the file holds no id-bearing bullet, "
                    "where count:0 means the format went unrecognised; their "
                    "absence means the roadmap parsed. A store-served read "
                    "(`source: store`) never carries them: every stored item "
                    "has an id, and a roadmap the store cannot recognise is "
                    "refused rather than served. "
                    "ANTS-1646 — every non-error response also carries "
                    "a top-level `duplicate_ids[]` field when the parser "
                    "saw the same id on more "
                    "than one bullet (each entry: `{id, occurrences:"
                    "[{section_slug, status}], truncated_count?}`). "
                    "ANTS-4546 — an AUTHORED id counts whatever its "
                    "shape: on a GFM roadmap two bullets leading with "
                    "the same bold span (`**Photo mode**`) BOTH carry "
                    "that id, and roadmap_log then refuses "
                    "`bullet_ambiguous` — so a silence here is paid for "
                    "at write time. Only the reader's own synthetic "
                    "content-hash nonces are excluded (ANTS-1688, where "
                    "they over-reported); `occurrences[]` "
                    "is capped at 3 with the dropped tail in "
                    "`truncated_count`. Absent when the roadmap is "
                    "clean. Read it as \"this id addresses more than one "
                    "bullet\" — which covers both hand-edited drift past "
                    "the `.roadmap-counter` guard and an inferred id two "
                    "bullets happen to share. "
                    "Envelope: {ok, bullets, path, "
                    "count, filter, section?, mode?, duplicate_ids?} or "
                    "{ok, sections, path, filter, mode, "
                    "legacy_format_sections?, legacy_format_hint?, "
                    "duplicate_ids?} for "
                    "section_index, or {ok:false, error, code} on error. "
                    "`bullets[].id` follows the shareable "
                    "docs/standards/roadmap-format.md § 3.5.1 spec — "
                    "any `[PROJ-NNNN]` token (letter-prefixed, "
                    "dash-then-digits) is recognised, e.g. "
                    "`[ANTS-1234]`, `[MAME-CURATOR-42]`, "
                    "`[mame-curator-7]` (ANTS-1405). "
                    "ANTS-4604 — MIXED-DIALECT ROADMAPS. `format` here means "
                    "\"the GFM adapter engaged, so kind/lanes/layman are "
                    "degraded\" — it is NOT a claim that the whole file is one "
                    "dialect, and roadmap_log's `format` answers a different "
                    "question again (the dialect of the bullet it touched, or "
                    "the one it wrote). On a file carrying BOTH shapes the "
                    "envelope adds `mixed:true` with `gfm_bullets` and "
                    "`non_gfm_bullets` counts, so a caller keying on "
                    "`format != \"ants-v1\"` can see it is not looking at a "
                    "uniform file. All three are ABSENT on a uniform roadmap, "
                    "so an envelope without them is not an older server. "
                    "`non_gfm_bullets` is deliberately not called a dialect: "
                    "the per-bullet tag is emitted only for github-task-list, "
                    "so the remainder is everything else. None of this decides "
                    "which backend serves the project — that is "
                    "migratedProject()'s own detector, which reads neither "
                    "field. ANTS-4606 — the two verbs also SPELL that dialect "
                    "differently: this verb reports `github-task-list` where "
                    "roadmap_log reports `gfm`. One format, two vocabularies, "
                    "so a caller comparing the two envelopes reads a "
                    "disagreement that is not one. Both spellings stand "
                    "deliberately: unifying them would silently break any "
                    "caller already keying on either, and this store is "
                    "machine-global, so that caller may be another project's "
                    "session. Compare dialects by mapping both spellings, "
                    "never by string equality across the two verbs.");
                roadmapTool["selection_hint"] = QStringLiteral(
                    "Use when you need to know which roadmap items "
                    "are active/shipped before quoting an ID. "
                    "Prefer over Read for triage queries; use Read "
                    "for full-text edits.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    QJsonObject statusProp;
                    QJsonArray statusEnum;
                    statusEnum.append("all");
                    statusEnum.append("active");
                    statusEnum.append("shipped");
                    statusEnum.append("planned");
                    statusEnum.append("in-progress");
                    statusEnum.append("considered");
                    statusEnum.append("dropped");   // ANTS-4977
                    // ANTS-5695 — the emoji the rendered roadmap shows.
                    for (const char *e : {"\xF0\x9F\x93\x8B", "\xF0\x9F\x9A\xA7",
                                          "\xE2\x9C\x85", "\xF0\x9F\x92\xAD",
                                          "\xF0\x9F\x9A\xAB"})
                        statusEnum.append(QString::fromUtf8(e));
                    // ANTS-5376 — one value, or an array that is their union.
                    {
                        QJsonObject one;
                        one["type"] = "string";
                        one["enum"] = statusEnum;
                        QJsonObject many;
                        many["type"]     = "array";
                        many["items"]    = one;
                        many["minItems"] = 1;
                        statusProp["anyOf"] = QJsonArray{one, many};
                    }
                    statusProp["description"] = QStringLiteral(
                        "ANTS-5376 — an ARRAY of these values is their union. "
                        "Filter by lifecycle. Aggregates: \"active\" = "
                        "planned + in-progress (📋+🚧, ~7× smaller payload), "
                        "\"shipped\" = ✅, \"all\". ANTS-3400 — the granular "
                        "roadmap_log lifecycle names are also accepted and "
                        "map to a single emoji: \"planned\"=📋, "
                        "\"in-progress\"=🚧, \"considered\"=💭, "
                        "\"dropped\"=🚫 (bullets / "
                        "section= path; section_index collapses granular "
                        "names to their aggregate). An unknown value refuses "
                        "with bad_status + an `accepted` list. ANTS-3698 — "
                        "`filter` is accepted as an alias (the envelope "
                        "echoes the applied lifecycle under that name, so it "
                        "is what a caller writing the next call by hand "
                        "sends); sending both refuses bad_op_combo.");
                    props["status"] = statusProp;
                    // ANTS-3698 — `filter` alias. Declared so the arg is not
                    // reported in ignored_args now that it is honoured, and so
                    // the echo field's name resolves to a real parameter.
                    QJsonObject filterAliasProp;
                    filterAliasProp["type"] = "string";
                    filterAliasProp["enum"] = statusEnum;
                    filterAliasProp["description"] = QStringLiteral(
                        "Alias for `status` (ANTS-3698) — same accepted "
                        "values. Exists because the response echoes the "
                        "applied lifecycle as `filter`; passing that name "
                        "used to be silently ignored and answered with the "
                        "full set. Sending both refuses bad_op_combo.");
                    props["filter"] = filterAliasProp;
                    // ANTS-1287 — `section` slug (optional). Unknown
                    // slug → ok:false with code=bad_section.
                    QJsonObject sectionProp;
                    sectionProp["type"] = "string";
                    sectionProp["description"] = QStringLiteral(
                        "Slug of a ## or ### heading (e.g. "
                        "\"performance-2\"). Returns the bullets "
                        "within that section AND within every section "
                        "nested inside it (ANTS-4819), so a level-2 "
                        "heading answers with its level-3 children's "
                        "items rather than nothing — the same "
                        "descendants mode:\"section_index\" rolls into "
                        "that slug's counts, on both backends; the reply "
                        "names those in `subsections`. Saves a "
                        "full reparse for partial queries. Slugs are canonically "
                        "lowercase; off-case spelling → code=bad_case "
                        "with `canonical_slug` surfaced (ANTS-1524). "
                        "Unknown slug → code=bad_section. ANTS-5285 — with "
                        "mode:\"headline_only\" and `status`, this is the "
                        "cheap way to list one section's open ids.");
                    props["section"] = sectionProp;
                    // ANTS-1856 — `id` single-item selector. The
                    // one-call answer to \"show me ANTS-NNNN\" without
                    // paging the whole roadmap.
                    QJsonObject idProp;
                    idProp["type"] = "string";
                    idProp["description"] = QStringLiteral(
                        "Fetch a single bullet by its [PROJ-NNNN] id "
                        "(e.g. \"ANTS-1853\") in one call instead of "
                        "paging. Returns {ok, bullets:[the item], "
                        "count, id, found}. Bypasses the `status` "
                        "filter + pagination (an id request wants THAT "
                        "item regardless of lifecycle), and includes "
                        "the body by default (pass include_body:false "
                        "to drop it). Case-sensitive exact match; a "
                        "case-only mismatch → code=bad_case with "
                        "`canonical_id`. Unknown id → {ok:true, "
                        "found:false, count:0}. Cannot combine with "
                        "`section` or mode:section_index "
                        "(bad_mode_combo). ANTS-4079 — on a store-backed "
                        "project the bullet carries `links` when it has any: "
                        "blocked_by / blocks, splits_from / parts, "
                        "duplicate_of / duplicated_by, supersedes / "
                        "superseded_by, relates_to, specified_by. `ids` "
                        "carries them too.");
                    props["id"] = idProp;
                    // ANTS-1726 — `ids` plural-selector. The N-call
                    // answer to "show me ANTS-1719..1724" in one go;
                    // pairs with the singular `id` selector for
                    // bundle-continuation sessions. Document-order
                    // result + matched/missing accounting.
                    QJsonObject idsProp;
                    idsProp["type"] = "array";
                    {
                        QJsonObject items;
                        items["type"] = "string";
                        idsProp["items"] = items;
                    }
                    idsProp["maxItems"] = 100;
                    idsProp["description"] = QStringLiteral(
                        "Fetch N bullets by their [PROJ-NNNN] ids "
                        "(e.g. [\"ANTS-1719\",\"ANTS-1721\"]) in one "
                        "call instead of N separate id= calls or a "
                        "status:all scan. Bypasses `status` filter + "
                        "pagination (same as singular `id`), keeps body "
                        "by default. Result is in DOCUMENT order, not "
                        "input order. Envelope: {ok, bullets, count, "
                        "ids, matched_ids, missing_ids, found}. "
                        "ANTS-4400: each bullet carries `input_index` — its "
                        "position in the `ids` array you sent — so a caller "
                        "can restore its own ordering. Results stay in "
                        "DOCUMENT order; zipping them against your input array "
                        "without re-keying silently mis-pairs. "
                        "ANTS-4387: an id ROTATED into a docs/roadmap/*.md "
                        "archive (roadmap-format.md § 3.9) is reported in "
                        "`archived_ids` with its source in `archived_sources`, "
                        "NOT in `missing_ids` — those two were previously "
                        "indistinguishable, and for a release pre-flight or a "
                        "changelog audit (which check ids that are by "
                        "definition old) the only safe reading of \"missing\" is "
                        "to stop. `all_ids_resolved` is the flag to branch on: "
                        "`found:true` is true of a PARTIALLY resolved set, so a "
                        "caller reading it alone never learns two of its fifty "
                        "did not resolve. "
                        "Duplicates de-duped (first occurrence wins). "
                        "Empty array (zero elements) → falls through to "
                        "the normal list path. A comma/whitespace-joined "
                        "STRING (\"ANTS-1719,ANTS-1721\") is coerced to "
                        "this array form; a present-but-otherwise-typed or "
                        "non-empty-but-all-malformed ids refuses bad_args "
                        "rather than silently dumping the full list "
                        "(ANTS-3541). Cannot combine with `id`, `section`, "
                        "or `mode:section_index` (bad_mode_combo). Max 100 "
                        "items (bad_args). Pairs with singular `id` "
                        "(ANTS-1856).");
                    props["ids"] = idsProp;
                    // ANTS-1398 — opt-in to retain section-rollup
                    // bullets (empty id/headline, status emoji only).
                    // Default false; the dropped rollups are visual
                    // chrome the dialog uses but actionable-item
                    // clients never want them in bullets[].
                    QJsonObject inclHeadersProp;
                    inclHeadersProp["type"] = "boolean";
                    inclHeadersProp["default"] = false;
                    inclHeadersProp["description"] = QStringLiteral(
                        "If true, retain section-rollup bullets "
                        "(empty id+headline, status emoji only) in "
                        "bullets[]. Default false — drops them so "
                        "clients get only actionable entries. "
                        "Opt-in for back-compat callers (ANTS-1398).");
                    props["include_section_headers"] = inclHeadersProp;
                    // ANTS-1425 — opt-in for narrator bullets (empty
                    // id, non-empty headline — section-summary prose).
                    QJsonObject inclNarratorsProp;
                    inclNarratorsProp["type"] = "boolean";
                    inclNarratorsProp["default"] = false;
                    inclNarratorsProp["description"] = QStringLiteral(
                        "If true, retain narrator bullets (empty id, "
                        "non-empty headline — section-summary prose "
                        "like \"Trust-model gaps in IPC sockets.\"). "
                        "Default false — roadmap-format.md § 3.5.1 "
                        "makes the [PROJ-NNNN] ID mandatory for every "
                        "actionable bullet, so empty-id is a "
                        "non-actionable marker. Opt-in for back-compat "
                        "callers (ANTS-1425).");
                    props["include_narrator_bullets"] = inclNarratorsProp;
                    // ANTS-1517 — include_body opt-in. Default false.
                    QJsonObject inclBodyProp;
                    inclBodyProp["type"] = "boolean";
                    inclBodyProp["default"] = false;
                    inclBodyProp["description"] = QStringLiteral(
                        "ANTS-4362 — an `id`/`ids` fetch returns bodies "
                        "WITHOUT this flag; a status- or section-filtered "
                        "query withholds them unless it is set, because a "
                        "filter can match the whole roadmap. A filtered reply "
                        "that withheld them carries `bodies_omitted:true` + "
                        "`bodies_omitted_reason`, so a missing body is never "
                        "mistaken for a bullet that has none. "
                        "If true, each bullet carries a `body` field "
                        "(continuation prose, truncated to ~2000 chars "
                        "with `body_truncated:true` on truncation). "
                        "ANTS-4557 — `body` is the bullet's CONTINUATION "
                        "lines only: the head line is `headline` / "
                        "`headline_oneline` in the same envelope and is no "
                        "longer repeated as the body's first line. The "
                        "trailer lines ARE body lines and stay, on both "
                        "backends. ANTS-4599 — and a trailer line may be "
                        "COMPOSED rather than quoted: the record is built by "
                        "rendering the item and re-parsing it, and the render "
                        "emits a line for every column whose key the stored "
                        "prose does not declare at a line start. So a body "
                        "that never wrote `Lanes:` reads back carrying one. "
                        "This field answers WHAT THIS BULLET SAYS IN "
                        "ROADMAP.md — truthfully, since the file is generated "
                        "from the store — and NOT whether the stored body "
                        "declares that field. ANTS-4813 — "
                        "`composed_trailers` names the keys the render wrote, "
                        "so the two ARE distinguishable from the reply. Read "
                        "it before passing a trailer line back as "
                        "op:\"amend_body\" old_text: a key named there is a "
                        "COLUMN, reachable only by op:\"amend_field\", and "
                        "amend_body refuses on it however plainly it appears "
                        "in `body`. Emitted only when non-empty and only "
                        "alongside a body, so its absence is not a withheld "
                        "answer. On the markdown backend it is always empty, "
                        "and that is the TRUE answer rather than a divergence: "
                        "there every line was written by an author, so nothing "
                        "was composed. "
                        "ANTS-4558 — and they keep their own "
                        "indentation: the block is dedented by its COMMON "
                        "leading whitespace, so a nested list or an indented "
                        "command line comes back with its depth, and text you "
                        "read here can be handed straight back to "
                        "op:\"amend_body\". "
                        "ANTS-3736 — a truncated body keeps BOTH its head "
                        "and its final ~1 KiB, joined by an explicit "
                        "`… [body elided — tail follows; refetch by id "
                        "with max_body_bytes for more] …` marker, so an "
                        "append-only progress-log body still reports its "
                        "CURRENT state and not just its oldest text. "
                        "ANTS-4904 — a truncated body also carries "
                        "`body_bytes`, the WHOLE body's size, so you can tell "
                        "what fraction you are holding instead of inferring "
                        "one; and `body_from_end:true` on a TARGETED id/ids "
                        "fetch keeps the END of the body rather than head + "
                        "tail, with the elision marker at the FRONT. Reach "
                        "for it when the item is a progress log and the "
                        "answer is its last paragraph: a 1 KiB tail is a "
                        "sliver of it, and raising max_body_bytes to get more "
                        "raises the head with it until the reply spills and "
                        "the preview drops the body altogether. It is inert "
                        "where nothing was truncated, so it can be passed "
                        "unconditionally. "
                        "Default false. Use when triaging dense bundle "
                        "tables where the rationale lives in the body, "
                        "not the headline (ANTS-1517). ANTS-3402 — on a "
                        "TARGETED id=/ids= fetch, raise the cap with "
                        "`max_body_bytes` to read a large multi-phase epic "
                        "body in one call (list queries stay at 2000). For "
                        "a large section body, read_region section= is the "
                        "recommended untruncated path.");
                    props["include_body"] = inclBodyProp;
                    // ANTS-3402 — opt-in higher body cap for id/ids fetches.
                    QJsonObject maxBodyProp;
                    maxBodyProp["type"] = "integer";
                    maxBodyProp["description"] = QStringLiteral(
                        "Max body bytes for a TARGETED id=/ids= fetch. "
                        "ANTS-4630 — the ceiling depends on how many ids you "
                        "named: a fetch naming ONE id clamps to [2000, "
                        "1048576] and so can read a body of any realistic "
                        "size WHOLE, middle included; a wider ids= fetch "
                        "still clamps to [2000, 16384] so one call cannot "
                        "pull N oversized bodies inline. Before that fix the "
                        "ceiling was 16384 everywhere, which is also where "
                        "the elision fired — so for a body over 16 KiB the "
                        "remedy the elision marker names could not reach "
                        "past the wall that produced it, and the middle was "
                        "unreadable by any argument (read_spill did not help: "
                        "the body was elided before the response was "
                        "serialised, so the spill held elided text). "
                        "ANTS-4091 — the targeted "
                        "DEFAULT is no longer 2000: it is 16384/N for an "
                        "N-id fetch, floored at 2000, so a single-id fetch "
                        "returns any body under 16 KiB whole (elision drops "
                        "the MIDDLE, where a resume plan sits) while a wide "
                        "id-set stays payload-bounded. Pass this to override. "
                        "ANTS-5467 — on a list or section query it may only "
                        "LOWER the 2000 cap, clamped to [400, 2000]; a clamp "
                        "is announced with `body_cap_clamped`. Ignored on "
                        "section_index. "
                        "ANTS-5477 — when a targeted reply would be offloaded, "
                        "its bodies are shortened with the elision marker until "
                        "it fits, and it carries `bodies_shrunk_to_fit` plus "
                        "`max_body_bytes_effective`. A single id asked for by "
                        "size is exempt. ANTS-5364 — this value also raises "
                        "the spill threshold for the call, to itself plus "
                        "4 KiB, at most 64 KiB; a larger reply still spills, "
                        "so read_spill reaches the middle.");
                    props["max_body_bytes"] = maxBodyProp;
                    // ANTS-4904 — read a progress-log bullet's TAIL.
                    QJsonObject fromEndProp;
                    fromEndProp["type"] = "boolean";
                    fromEndProp["default"] = false;
                    fromEndProp["description"] = QStringLiteral(
                        "TARGETED id/ids fetch only. Keep the END of an "
                        "over-cap body instead of ANTS-3736's head + 1 KiB "
                        "tail, with the elision marker at the FRONT. A "
                        "long-lived item is an append-only progress log, so "
                        "the sentence a resuming session wants is its LAST "
                        "paragraph — and raising `max_body_bytes` to reach "
                        "more of it raises the head too, until the reply "
                        "spills and the preview drops the body altogether. "
                        "With this, `max_body_bytes` sizes the TAIL, so one "
                        "small call answers 'where does this stand?'. Inert "
                        "where nothing was truncated, so it is safe to pass "
                        "unconditionally; ignored on list / section / "
                        "section_index paths, whose cap is a payload bound "
                        "across many rows rather than a question about one "
                        "item. Pair it with `body_bytes` (emitted on every "
                        "truncated body) to see how much you did not get.");
                    props["body_from_end"] = fromEndProp;
                    // ANTS-4836 — `kind` filter.
                    QJsonObject kindFilterProp;
                    kindFilterProp["type"] = "string";
                    kindFilterProp["description"] = QStringLiteral(
                        "Keep only bullets whose `kind` equals this, "
                        "case-folded — the one field that was settable and "
                        "readable but never selectable, so \"which review "
                        "fixes are still open?\" meant pulling every active "
                        "bullet and sorting by hand. Composes with `status`, "
                        "`section` and `query`. An unrecognised value REFUSES "
                        "with `bad_kind` and an `accepted` list, exactly as "
                        "`status` refuses `bad_status`: a silently ignored "
                        "filter returns the FULL set, which reads as \"nothing "
                        "matches that kind\" and is indistinguishable from a "
                        "real empty result. Applies under mode:"
                        "\"headline_only\" too — filtering runs before the "
                        "projection that drops the field.");
                    props["kind"] = kindFilterProp;
                    // ANTS-4985 — `source` provenance filter.
                    QJsonObject sourceFilterProp;
                    { QJsonArray ty; ty.append(QStringLiteral("string"));
                                     ty.append(QStringLiteral("array"));
                      sourceFilterProp["type"] = ty; }
                    sourceFilterProp["description"] = QStringLiteral(
                        "Keep only bullets whose `Source:` starts with this "
                        "PREFIX, case-folded. Prefix rather than equality "
                        "because the recognised values are DATED "
                        "(`indie-review-2026-05-13`), so equality answers "
                        "nothing a caller asks; and there is no `accepted` "
                        "list to refuse against the way `kind` has one, "
                        "because the column is free text (2599 distinct "
                        "values over 6706 items, measured 2026-09-09). "
                        "Accepts a STRING or an ARRAY of strings, matching if "
                        "ANY prefix hits \u2014 the standard keeps two live "
                        "spellings for one provenance (`code-quality-review-*` "
                        "was adopted 2026-08-12 and existing `indie-review-*` "
                        "bullets keep theirs), so a scalar-only filter cannot "
                        "ask \"from any review\" in one call. This is the "
                        "axis `kind` cannot carry: measured on this project's "
                        "active items, 36 are review-derived by source and "
                        "only 6 carry `review-fix`/`audit-fix` \u2014 the rest "
                        "are `perf`, `doc`, `refactor`, `marketing`, because a "
                        "review legitimately produces work of any kind. "
                        "Composes with `status`, `kind`, `section` and "
                        "`query`. Present-but-empty REFUSES `bad_args` rather "
                        "than returning the full set, which reads as "
                        "\"everything came from there\". The envelope echoes "
                        "the applied prefixes as `source` plus "
                        "`source_filtered_out`, so a zero-row answer is "
                        "distinguishable from a filter that did nothing. Each "
                        "bullet now carries `source` as a field, beside `kind` "
                        "and `lanes`.");
                    props["source"] = sourceFilterProp;
                    // ANTS-3391 — `query` keyword text-filter.
                    QJsonObject queryProp;
                    queryProp["type"] = "string";
                    queryProp["description"] = QStringLiteral(
                        "Case-insensitive keyword filter (substring). "
                        "Narrows the list to bullets whose headline (or "
                        "headline_full) OR body contains this text — or "
                        "whose id does, when the id holds a space (a "
                        "github-task-list title parsed as an id, ANTS-5257; "
                        "`query_fields` echoes what was searched), "
                        "composing with the status + section= filters — the "
                        "one-call \"find roadmap items mentioning X\". The "
                        "echoed `query` confirms it applied. Matches against "
                        "the same ~2000-char-capped body the list emits, so a "
                        "keyword only in a longer body's tail may be missed; "
                        "for an exact item use id / ids instead. Does NOT "
                        "combine with id / ids / mode:bundles (bad_mode_combo "
                        "— those are targeted or aggregate surfaces with no "
                        "per-bullet list to filter). ANTS-4610: "
                        "mode:section_index is the EXCEPTION — there `query` "
                        "narrows the INDEX by section name, matching the "
                        "headline or the slug (`query:\"dev experience\"` and "
                        "`query:\"dev-experience\"` both resolve), and the "
                        "reply carries `sections_considered` / "
                        "`sections_filtered_out` so zero matches is "
                        "distinguishable from a roadmap with no sections. It "
                        "is the cheap way to resolve the one slug "
                        "roadmap_log's `section` wants: unnarrowed that index "
                        "runs to tens of rows, and neither `fields` (top-level "
                        "keys, so [\"sections\"] keeps the array) nor "
                        "`compact` (drops empty values, not rows) cuts it. "
                        "ANTS-4367: a SHORT query needs narrowing — "
                        "`query:\"CI\"` matches \"decision\", \"efficiency\", "
                        "\"precision\" and \"facing\", and the damage is "
                        "REORDERING rather than padding, since genuine hits get "
                        "pushed off page one while truncated:true gives no clue "
                        "the answer is in the tail. Pass `whole_word:true` (or "
                        "`regex:true`) instead of raising `limit`, which grows "
                        "the payload at identical signal-to-noise.");
                    props["query"] = queryProp;
                    {   // ANTS-4927 — `q` as an alias for `query`. It must be
                        // DECLARED, not merely honoured: the dispatcher builds
                        // its ignored_args advisory from these properties, so
                        // an undeclared key is reported as dropped even once
                        // the handler reads it.
                        QJsonObject qAlias; qAlias["type"] = "string";
                        qAlias["description"] = QStringLiteral(
                            "Alias for `query` (ANTS-4927) — the short form a "
                            "caller naturally reaches for. Accepted for the "
                            "same reason `filter` aliases `status` and "
                            "`max_results` aliases `limit`: unrecognised, it "
                            "landed in ignored_args and the call returned an "
                            "UNFILTERED page, which is the one result shape "
                            "that cannot be told from an answer. Sending both "
                            "refuses bad_op_combo.");
                        props["q"] = qAlias;
                    }
                    // ANTS-4367 — the two narrowing knobs.
                    QJsonObject wwProp; wwProp["type"] = "boolean";
                                        wwProp["default"] = false;
                                        wwProp["description"] = QStringLiteral(
                        "Optional (ANTS-4367). Wrap `query` in word boundaries "
                        "so a short acronym stops matching inside longer words. "
                        "Still case-insensitive — it narrows the BOUNDARY, not "
                        "the casing, so \"CI\" still finds a bullet writing it "
                        "\"ci\". A hyphenated occurrence (`CI-parity`) DOES "
                        "match, because `-` is a word boundary. Default false, "
                        "so every existing caller is byte-identical.");
                    QJsonObject qReProp; qReProp["type"] = "boolean";
                                         qReProp["default"] = false;
                                         qReProp["description"] = QStringLiteral(
                        "Optional (ANTS-4367). Treat `query` as a regular "
                        "expression (case-insensitive) — the same knob "
                        "workspace_search spells the same way, so a caller who "
                        "knows one knows the other. Wins over `whole_word` when "
                        "both are sent. A malformed pattern refuses with "
                        "bad_args rather than matching nothing silently, since "
                        "zero hits with no explanation reads as \"the roadmap "
                        "has no such items\".");
                    props["whole_word"] = wwProp;
                    props["regex"]      = qReProp;
                    // ANTS-5468 — per-row body line filter.
                    QJsonObject bmProp; bmProp["type"] = "string";
                                        bmProp["description"] = QStringLiteral(
                        "ANTS-5468. Keep only the body lines that contain this "
                        "text, case-insensitively. `query` picks WHICH rows; "
                        "this bounds WHAT of each row. Matched against the "
                        "WHOLE body before any cap, on the list, section, id "
                        "and ids paths, and it turns bodies on. Each row gains "
                        "`body_lines_dropped`; the envelope echoes it. Refuses "
                        "bad_mode_combo with include_body:false and on "
                        "headline_only, section_index, bundles and report.");
                    QJsonObject bmReProp; bmReProp["type"] = "boolean";
                                          bmReProp["default"] = false;
                                          bmReProp["description"] = QStringLiteral(
                        "ANTS-5468. Read `body_match` as a case-insensitive "
                        "regular expression. A malformed one refuses bad_args.");
                    props["body_match"]       = bmProp;
                    props["body_match_regex"] = bmReProp;
                    // ANTS-5315 — a release, resolved to its section.
                    QJsonObject verProp; verProp["type"] = "string";
                                         verProp["description"] = QStringLiteral(
                        "ANTS-5315. A release, such as \"0.10.0\" or a patch "
                        "stream \"0.4.x\": selects the section whose title "
                        "starts with it and answers as section= does. The "
                        "version IS the section title. Unknown refuses "
                        "bad_version with `versions`; refuses bad_mode_combo "
                        "with section, id, ids, section_index, bundles and "
                        "report.");
                    props["version"] = verProp;
                    // ANTS-1437 — mode arg. Default "bullets" (legacy).
                    // "section_index" returns a compact section index
                    // instead of bullets — use to discover slugs cheaply.
                    QJsonObject modeProp;
                    modeProp["type"] = "string";
                    QJsonArray modeEnum;
                    modeEnum.append("bullets");
                    modeEnum.append("section_index");
                    // ANTS-1881 — third mode value. Narrow per-bullet
                    // shape for callers that only want the catalogue.
                    modeEnum.append("headline_only");
                    // ANTS-1922 — fourth mode value. Groups active items
                    // into thematic work-bundles for session triage.
                    modeEnum.append("bundles");
                    // ANTS-4501 — fifth mode value. One aggregate over the
                    // store: totals, lifecycle, and throughput per period.
                    modeEnum.append("report");
                    // ANTS-4119 / ANTS-4993 — the provenance vocabulary.
                    modeEnum.append("sources");
                    // ANTS-5516 — stale in-progress items and commits with no id.
                    modeEnum.append("ledger");
                    modeProp["enum"] = modeEnum;
                    modeProp["default"] = "bullets";
                    // ANTS-4501 § 2.4 — the report's own arguments. `scope`
                    // opts into the cross-project view; `since`/`until` replace
                    // the four standard buckets with one half-open window.
                    QJsonObject scopeProp;
                    scopeProp["type"] = "string";
                    QJsonArray scopeEnum;
                    scopeEnum.append("project");
                    scopeEnum.append("all");
                    scopeProp["enum"] = scopeEnum;
                    scopeProp["default"] = "project";
                    scopeProp["description"] = QStringLiteral(
                        "mode:\"report\" only. \"project\" (default) reports "
                        "the project at caller_cwd; \"all\" sums every "
                        "registered project in the machine-global store — the "
                        "one view no single ROADMAP.md can give.");
                    props["scope"] = scopeProp;
                    QJsonObject sinceProp;
                    sinceProp["type"] = "string";
                    sinceProp["description"] = QStringLiteral(
                        "mode:\"report\" only. YYYY-MM-DD. Replaces the four "
                        "standard buckets with a single `periods.since` entry "
                        "over [since, until) — half-open at the top like every "
                        "standard bucket, so two adjacent windows tile without "
                        "overlapping.");
                    props["since"] = sinceProp;
                    QJsonObject untilProp;
                    untilProp["type"] = "string";
                    untilProp["description"] = QStringLiteral(
                        "mode:\"report\" only. YYYY-MM-DD, exclusive upper "
                        "bound for `since`. Defaults to tomorrow's boundary, "
                        "so the default window includes today.");
                    props["until"] = untilProp;
                    QJsonObject shipSinceProp;
                    shipSinceProp["type"] = "string";
                    shipSinceProp["description"] = QStringLiteral(
                        "ANTS-4715. YYYY-MM-DD. Narrows the LIST path — "
                        "mode:\"bullets\" and mode:\"headline_only\" — to items "
                        "the STORE says shipped in [shipped_since, "
                        "shipped_until), so it answers WHICH ids closed in a "
                        "window where mode:\"report\" answers only how many. "
                        "That gap is why a release gate had to open "
                        "roadmap.sqlite directly. Composes with `status` and "
                        "with pagination; REFUSES bad_mode_combo on id / ids / "
                        "section= / section_index / bundles / report rather "
                        "than being ignored, because an ignored window returns "
                        "something that looks like a release list and is not "
                        "one. Store-backed projects only (project_not_"
                        "registered otherwise) — the roadmap FILE carries no "
                        "ship date. Day granularity: the column is a DATE, so "
                        "items cannot be ordered within a day. The envelope "
                        "echoes the resolved window plus `shipped_in_window`, "
                        "the store's own count — compare it against the rows "
                        "returned, since an id the store dated but the file "
                        "lacks is exactly what an audit is hunting. "
                        "`shipped_undated` appears when the store holds "
                        "shipped items with NO date: those cannot appear in "
                        "any window, so a complete-looking result may not be. "
                        "Dates are stamped going forward only; "
                        "roadmap_log op:\"backfill_dates\" fills the rest from "
                        "git history.");
                    props["shipped_since"] = shipSinceProp;
                    QJsonObject shipUntilProp;
                    shipUntilProp["type"] = "string";
                    shipUntilProp["description"] = QStringLiteral(
                        "ANTS-4715. YYYY-MM-DD, EXCLUSIVE upper bound for "
                        "`shipped_since` — half-open like mode:\"report\"'s "
                        "buckets, so two adjacent windows tile without "
                        "double-counting a boundary date. Defaults to "
                        "tomorrow, so the default window includes today. May "
                        "be passed alone, which reads as everything shipped "
                        "BEFORE that date.");
                    props["shipped_until"] = shipUntilProp;
                    modeProp["description"] = QStringLiteral(
                        "Response mode. \"bullets\" (default) returns "
                        "bullets[]. \"section_index\" returns "
                        "sections[{slug, headline, level, active_count, "
                        "shipped_count, total_count, "
                        "active_count_id_only, shipped_count_id_only, "
                        "total_count_id_only, direct_active_count, "
                        "direct_shipped_count, direct_total_count}] (the "
                        "`*_id_only` parallels match the default bullets[] "
                        "predicate; ANTS-1622; the plain counts include "
                        "subsections and `direct_*` do not, so sum "
                        "`direct_*` for a total; ANTS-4986) and no bullets — "
                        "use for slug discovery (response < 5 KB on a "
                        "500-bullet roadmap). ANTS-1848 — honours `status`: "
                        "status:\"active\"/\"shipped\" drops sections whose "
                        "matching *_count_id_only is 0 (status:\"active\" is "
                        "the lean planning call); status:\"all\" (default) "
                        "emits every section. ANTS-1729 — section_index "
                        "now auto-truncates the sections[] array under the "
                        "~20 KB soft cap (emitting truncated/next_offset) "
                        "and accepts offset/limit to page through a "
                        "many-section roadmap. Cannot combine with "
                        "section= (bad_mode_combo). ANTS-1881 — "
                        "\"headline_only\" returns bullets[] narrowed to "
                        "{id, status, headline_oneline, section_slug} per "
                        "bullet (skips body/lanes/kind). ANTS-4699 — those "
                        "four keys are the MODE's contract (ANTS-1881 INV-2: "
                        "extra keys fail, missing keys fail), NOT a `fields` "
                        "default, so `kind` is not obtainable here by any "
                        "argument and a caller needing it wants an "
                        "unfiltered id/ids fetch. Stated because a project "
                        "doc had recorded the wider claim that a filtered "
                        "call still returns kind. ANTS-4712 — the `ids` path "
                        "is the ONE documented exemption: each bullet there "
                        "also carries `input_index` (ANTS-4400), so it emits "
                        "those four keys plus that one. Deliberate, because "
                        "results come back in DOCUMENT order and a lean shape "
                        "is exactly where a caller zipping them against its "
                        "own array mis-pairs — the bug `input_index` exists "
                        "to stop. The singular `id` path emits four. ~10× smaller "
                        "payload on dense bundle sections; composes with "
                        "section=, status=, id=, pagination, and ETag. "
                        "ANTS-1922 — \"bundles\" groups the active subset "
                        "(📋+🚧) into thematic work-bundles by headline-token "
                        "similarity (the one-call \"what's the next bundle "
                        "of related to-dos?\" view). Active-only — a passed "
                        "`status` is ignored; cannot combine with section=, "
                        "id, or ids (bad_mode_combo). Returns "
                        "bundles[{bundle_label, lanes, size, items[{id, "
                        "status, headline_oneline, lanes, "
                        "possibly_resolved_by?, possibly_resolved_score?, "
                        "gate_note?, blocked?}]}] sorted by size desc, plus "
                        "active_total / bundle_count / truncated. "
                        "`possibly_resolved_by` flags an item a shipped ✅ "
                        "sibling may already cover; `gate_note`/`blocked` "
                        "surface a body gate/blocker marker.");
                    // ANTS-4501 — appended after the base text above. The
                    // report's gloss lives HERE and not in the wire
                    // description, which is byte-capped at 800
                    // (mcp_tool_detail_field INV-5). This property has no cap.
                    modeProp["description"] = modeProp["description"].toString()
                        + QStringLiteral(
                        " ANTS-4501 — \"report\" returns ONE aggregate over the "
                        "store and no bullets[]: totals (items / open / "
                        "in_progress / shipped), by_status, by_kind (chosen "
                        "kinds; import-defaulted ones count as "
                        "kind_not_recorded), throughput "
                        "per day / week / month / year, age_open and "
                        "time_to_close. `open` is planned + in-progress + "
                        "considered — an enumeration, NOT status != shipped, "
                        "because a `dropped` item is not outstanding work. "
                        "Every bucketed figure ships beside a `coverage` block "
                        "counting the rows that could not be bucketed, so a "
                        "figure computed over dated rows can never read as a "
                        "total — on an unbackfilled store that is most of them. "
                        "Refuses section / id / ids with bad_mode_combo, and "
                        "writes nothing. \"sources\" lists the `Source:` "
                        "values with their counts (see source_group). "
                        "\"ledger\" lists stale in-progress items and commits "
                        "naming no roadmap id (see stale_days and range).");
                    props["mode"] = modeProp;
                    // ANTS-1436 — offset/limit pagination args.
                    QJsonObject offsetProp;
                    offsetProp["type"] = "integer";
                    offsetProp["minimum"] = 0;
                    offsetProp["description"] = QStringLiteral(
                        "0-based start index into the post-filter "
                        "bullets array. Defaults to 0. Use with "
                        "`limit` to page through large responses. "
                        "Past-end returns empty bullets[] with "
                        "offset == total.");
                    props["offset"] = offsetProp;
                    QJsonObject limitProp;
                    limitProp["type"] = "integer";
                    limitProp["minimum"] = 1;
                    limitProp["maximum"] = 500;
                    limitProp["description"] = QStringLiteral(
                        "Cap on bullets[] length (1..500). If "
                        "omitted, server auto-truncates when the "
                        "response would exceed ~20 KB. ANTS-3543 — on "
                        "that auto path the server first downshifts the "
                        "whole list to headline-only rows ({id, status, "
                        "headline_oneline, section_slug}) so you keep "
                        "every id instead of losing the tail, emitting "
                        "`downshifted:true`; the drop signals "
                        "(`truncated`/`next_offset`) are cleared when the "
                        "lean list is complete, and only reappear if even "
                        "the lean list overflows. Explicit limit caps "
                        "rows, still cut by bytes; downshift only when omitted. "
                        "When pagination applies, envelope carries "
                        "offset/limit/total/truncated and next_offset "
                        "when truncated.");
                    props["limit"] = limitProp;
                    // ANTS-4701 — `max_results` alias, declared for the same
                    // reason ANTS-3698 declared `filter`: so the argument is
                    // not reported in ignored_args now that it is honoured.
                    // An undeclared-but-honoured arg is the worse of both
                    // states — it works, and the envelope says it was dropped.
                    QJsonObject maxResultsAliasProp;
                    maxResultsAliasProp["type"]    = "integer";
                    maxResultsAliasProp["minimum"] = 1;
                    maxResultsAliasProp["maximum"] = 500;
                    maxResultsAliasProp["description"] = QStringLiteral(
                        "Alias for `limit` (ANTS-4701) — same range and same "
                        "meaning; sending both refuses bad_op_combo. Accepted "
                        "because workspace_search, the other list verb a "
                        "session reaches for, spells this cap `max_results`, "
                        "so the name a caller naturally carries across used "
                        "to be dropped into ignored_args and answered with "
                        "the FULL set — on mode:\"headline_only\" that is "
                        "every row, which then spills. Learning after the "
                        "call that the cap applied to nothing costs most "
                        "exactly the caller who was trying to bound the "
                        "response.");
                    props["max_results"] = maxResultsAliasProp;
                    // ANTS-1391 — caller_cwd anchor.
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    props["etag_match"] = makeEtagMatchProp();   // ANTS-1499
                    // ANTS-4837 — the PER-ROW counterpart of `fields`, which
                    // reaches top-level envelope keys only.
                    {
                        QJsonObject p;
                        p["type"] = QStringLiteral("array");
                        QJsonObject items;
                        items["type"] = QStringLiteral("string");
                        p["items"] = items;
                        p["description"] = QStringLiteral(
                            "Keep only these keys on each bullet -- the "
                            "per-row counterpart of `fields`, which reaches "
                            "top-level envelope keys only. Closes the two "
                            "shapes neither argument could express: "
                            "mode:\"headline_only\" has a fixed four-key "
                            "contract that cannot carry `kind`, and the wide "
                            "shape emits `headline` and `headline_oneline` as "
                            "byte-identical strings on a single-line headline. "
                            "`bullet_fields:[\"id\",\"status\",\"kind\"]` is "
                            "the triage shape. A key absent from a row is not "
                            "emitted rather than emitted null, since these "
                            "rows are already gated (`evidence`, `anchor`, "
                            "`bold_id`, `headline_full`). A name no row "
                            "carries is reported in `bullet_fields_unmatched` "
                            "beside `bullet_fields_available`, so a "
                            "misspelling is visible rather than an empty row. "
                            "REFUSED with bad_mode_combo on headline_only / "
                            "section_index / bundles / report, each of which "
                            "owns its row shape. On the ids path "
                            "`input_index` survives a projection that does not "
                            "name it (ANTS-4712), because results come back in "
                            "DOCUMENT order and zipping without it mis-pairs. "
                            "It also suppresses the automatic downshift to the "
                            "lean shape, which would otherwise discard the "
                            "projection you asked for.");
                        props["bullet_fields"] = p;
                    }
                    props["fields"] = makeFieldsProp();          // ANTS-1720
                    props["compact"] = makeCompactProp();        // ANTS-2091
                    // ANTS-1907 — per-section ETag short-circuit (section=
                    // mode) + opt-in per-section etag emission
                    // (section_index mode). Independent of the dispatch-
                    // layer file-level etag; lets a /cold-eyes or
                    // /test-audit loop reading many sections per pass
                    // skip the re-emit on sections that didn't change.
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "Per-section ETag short-circuit (ANTS-1907). "
                            "On a section= query, pass back the "
                            "`section_etag` field returned by a previous "
                            "call; when the section's bytes are unchanged "
                            "the verb returns "
                            "{ok:true, unchanged:true, section, "
                            "section_etag, path} — saves the bullets[] "
                            "body. Independent of `etag_match` (file-"
                            "level); use this when /cold-eyes / /test-"
                            "audit loops touch many sections per pass "
                            "and most are stable across an edit to one. "
                            "ANTS-4732 — it is the `section` ARGUMENT that "
                            "selects this, never `mode`: the short-circuit "
                            "returns from inside the section branch before "
                            "any projection runs, so it IS honoured under "
                            "mode:\"headline_only\" (this row previously said "
                            "it was ignored there). mode:\"section_index\" and "
                            "mode:\"bundles\" take their own branches and "
                            "never reach it. The etag keys on the section's "
                            "BYTES and not on `mode`, so a caller that varies "
                            "`mode` between calls can be answered `unchanged` "
                            "for a payload shape it did not ask for.");
                        props["section_etag_match"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "boolean";
                        p["default"] = false;
                        p["description"] = QStringLiteral(
                            "If true, each section in section_index "
                            "carries a `section_etag` field (ANTS-1907) "
                            "— a SHA-256 hash of the section's byte slice "
                            "stable under edits to OTHER sections. Pair "
                            "with `section_etag_match` on follow-up "
                            "section= calls to avoid re-emitting "
                            "unchanged bullets. Default false (envelope "
                            "shape unchanged when omitted; opt-in pays "
                            "a one-time per-section slicing cost).");
                        props["include_section_etags"] = p;
                    }
                    // ANTS-4467 — the slug projection for section_index.
                    {
                        QJsonObject p; p["type"] = "boolean";
                        p["default"] = false;
                        p["description"] = QStringLiteral(
                            "mode:\"section_index\" only (ANTS-4467). Return "
                            "a flat `slugs` array of slug strings instead of "
                            "the section objects. section_index exists to hand "
                            "back the slugs roadmap_log's `section` argument "
                            "requires, and on a large roadmap the full objects "
                            "overflow the inline budget and spill — which "
                            "keeps whole bodies for a PREFIX of rows and "
                            "shape-only stubs for the rest, the worst split "
                            "for a reply whose caller needs one key from every "
                            "row. Measured: 84 sections returned slugs for 8. "
                            "The flat array is well under 2 KB at that size "
                            "and does not spill. Same filter, same drop rules, "
                            "same order — only the row shape changes.");
                        props["slugs_only"] = p;
                    }
                    {   // ANTS-5154 — titles beside the flat slug list.
                        QJsonObject p;
                        p["type"] = "boolean";
                        p["description"] = QStringLiteral(
                            "With slugs_only:true, also return `slug_titles`, "
                            "[{slug, headline}] in the same order, to find a "
                            "section by name without the full rows. Default "
                            "false.");
                        props["with_titles"] = p;
                    }
                    {   // ANTS-4119 / ANTS-4993 — mode:"sources" grouping.
                        QJsonObject p;
                        p["type"] = "string";
                        p["enum"] = QJsonArray{QStringLiteral("stem"), QStringLiteral("exact")};
                        p["description"] = QStringLiteral(
                            "mode:\"sources\" only. mode:\"sources\" lists the "
                            "`Source:` values with active_count, shipped_count and "
                            "total_count each, busiest first, so you can pick a "
                            "`source` prefix instead of guessing one. \"stem\" "
                            "(default) groups a value with its first date and "
                            "everything after it removed; \"exact\" keeps each "
                            "value. Counts the list `status`, `kind`, `source` and "
                            "`query` select; capped at `limit` (default 200) with "
                            "distinct_count and truncated. Refuses section, id and "
                            "ids.");
                        props["source_group"] = p;
                    }
                    {   // ANTS-5516 — mode:"ledger"'s two arguments.
                        QJsonObject p;
                        p["type"] = "integer";
                        p["minimum"] = 1;
                        p["description"] = QStringLiteral(
                            "mode:\"ledger\" only. An in-progress item untouched "
                            "this many days is stale. Default 14.");
                        props["stale_days"] = p;
                        QJsonObject r;
                        r["type"] = "string";
                        r["description"] = QStringLiteral(
                            "mode:\"ledger\" only. Git range whose commits are "
                            "checked for a roadmap id. Default: newest release "
                            "tag..HEAD.");
                        props["range"] = r;
                    }
                    {   // ANTS-4462 — opt-in file-vs-store staleness check.
                        QJsonObject p;
                        p["type"]    = "boolean";
                        p["default"] = false;
                        p["description"] = QStringLiteral(
                            "Optional (ANTS-4462). Answer \"is ROADMAP.md in "
                            "sync with the store?\" by rendering the store and "
                            "comparing it against the live file — the same "
                            "render and the same comparison roadmap_log runs "
                            "before it overwrites, so the two surfaces can "
                            "never disagree. Adds `file_in_sync`, plus "
                            "`drift_lines` / `drift_restyled` / `drift_lost` / "
                            "`drift_lost_text` on the drifted arm only; "
                            "ANTS-5382 adds `drift_sample` ({file, render} "
                            "rows) and `drift_header_only`. ANTS-5381 adds "
                            "`drift_cause`: which side moved since the last "
                            "publish (file, store, file_and_store, renderer, "
                            "unknown); re-render on renderer or store, "
                            "re-migrate only on file. "
                            "`sync_checked:false` means nobody looked — not "
                            "store-backed, or the render failed — and is NOT a "
                            "clean bill of health. ANTS-4730 — it is emitted "
                            "on EVERY arm, so absence means only that "
                            "check_sync was not requested. This is the "
                            "two-directional "
                            "answer `file_ahead_of_store` cannot give: that "
                            "flag fires only on an ID the store has never "
                            "seen, so a hand status flip or a body edit moves "
                            "nothing and reads as healthy. OPT-IN and never "
                            "cached: one render of a 2,267-item store measures "
                            "~204 ms, more than this verb's entire 100 ms "
                            "bullet-cache TTL, so it cannot ride every query "
                            "— and a cached answer to a question about RIGHT "
                            "NOW would be worse than no answer. Ask once at "
                            "orientation, not per call. ANTS-4818 — "
                            "answered on the LIST path and on mode:\"report\", "
                            "which is where a project-level question about the "
                            "file is most naturally asked. Under scope:\"all\" "
                            "it reports checked:false, because the question is "
                            "about ONE file and that scope spans every "
                            "registered project.");
                        props["check_sync"] = p;
                    }
                    props["encoding"] = makeEncodingProp();      // ANTS-2090
                    schema["properties"] = props;
                    roadmapTool["inputSchema"] = schema;
                }
                tools.append(roadmapTool);

                {
                    // ANTS-1735 — model_switch_stats: read-only effectiveness
                    // scorecard for the autonomous model switcher. Required
                    // caller_cwd; ETag + fields opt-in.
                    // ANTS-1889 — envelope carries the live switcher config
                    // (auto_model_switch_enabled / floor_tier / min_dwell_sec)
                    // and accepts an optional `scope:"global"` arg.
                    QJsonObject t;
                    t["name"] = "model_switch_stats";
                    t["description"] = QStringLiteral(
                        "Read-only effectiveness scorecard for the "
                        "autonomous model switcher (ANTS-1735): Opus turns "
                        "avoided vs regret/under-route rate — the trust "
                        "signal that auto-switching is helping. "
                        "mode: firings (default) | near_misses (per-blocker "
                        "breakdown). scope: project (default) | global. "
                        "Opt-in: etag_match, fields, compact. "
                        "caller_cwd Required.");
                    // ANTS-2079 — full envelope + headline + blocker
                    // reference in `detail` (stripped from the tools/list
                    // wire; served by tool_info {name:"model_switch_stats"}).
                    t["detail"] = QStringLiteral(
                        "Read-only scorecard for the autonomous model switcher "
                        "(ANTS-1735), scoped to the caller's project by default. "
                        "Aggregates the effectiveness ledger into Opus turns "
                        "avoided vs regret/under-route rate — the trust signal "
                        "that auto-switching is helping. Envelope: {ok, switches, "
                        "downgrades, upgrades, opus_turns_avoided, "
                        "opus_turns_routed_in, regret_count, regret_rate, "
                        "under_route_count, pending_count, by_tier, "
                        "inconclusive_count, clean_end_count, weighted_avoided, "
                        "measured_downgrades, headline_floor, "
                        "auto_model_switch_enabled, floor_tier, min_dwell_sec, "
                        "scope, headline}. ANTS-1891 — regret_count now folds "
                        "under-route harm into the numerator; measured_downgrades "
                        "excludes inconclusive 0-turn-zero-signal records "
                        "(reported as inconclusive_count instead). clean_end_count "
                        "+ weighted_avoided credit clean session-ends (no override / "
                        "correction / under-route within 10 min of session end) as "
                        "½ Opus turn avoided each, so end-of-task downgrades — the "
                        "dominant ledger shape — are no longer invisible. The "
                        "headline reads \"auto-switch OFF\" when disabled "
                        "(ANTS-2033 — the envelope then also carries "
                        "auto_model_switch_off_reason: \"never_enabled\" if the "
                        "first-run opt-in hasn't been accepted vs "
                        "\"user_disabled\" if it was shown and left off, plus a "
                        "human-readable auto_model_switch_off_detail; the switch "
                        "is a single global key, so there is no per-project "
                        "opt-out state), "
                        "\"auto-switch ON (floor=X, dwell=Ns) … no switches "
                        "yet\" when enabled with no records, \"calibrating "
                        "(N/F measured)\" (post-1909 — was \"insufficient "
                        "data\" pre-1909) until the headline floor (F=10) is "
                        "reached, and the full avoided/clean-end/regret "
                        "breakdown above the floor — so a caller can "
                        "distinguish \"feature dormant\" from \"feature "
                        "working quietly\" from \"feature "
                        "still in calibration.\" An absent ledger returns "
                        "{ok:true, switches:0, …}; pending records (switch near "
                        "session end, outcome not yet measured) are counted "
                        "separately, never as success. Envelope readers should "
                        "check measured_downgrades > 0 before treating "
                        "regret_rate as meaningful — the new inconclusive_count "
                        "makes the gap visible. Pass scope:\"global\" to "
                        "aggregate across all projects in the ledger instead of "
                        "filtering to the caller's project (ANTS-1889). "
                        "ANTS-1894 — envelope additionally carries a slim "
                        "`near_misses:{total_24h, dominant_blocker}` block "
                        "summarising auto-switch decisions that were "
                        "evaluated-but-blocked (composer_not_empty, "
                        "dwell_time_insufficient, override_cooldown_active, "
                        "etc.) — diagnostic for \"why doesn't it switch in "
                        "this project?\". Pass `mode:\"near_misses\"` for "
                        "the full per-blocker breakdown (24 h + all-time "
                        "windows, distinct_signatures count). ANTS-1909 — "
                        "the headline now carries the dwell parenthetical "
                        "(`floor=X, dwell=Ns`), uses \"calibrating\" in "
                        "place of \"insufficient data\" below the floor, "
                        "and — when the 24 h near-miss block is non-empty — "
                        "appends \"N near-misses in 24 h blocked by "
                        "<dominant_blocker>\" on both the no-switches and "
                        "calibrating branches so the trust signal reads "
                        "as \"evaluating but blocked\" rather than "
                        "\"feature did nothing\". Blocker-meanings (for "
                        "the dominant-blocker token): "
                        "`composer_not_empty` = focused-tab composer "
                        "carries text (hard veto; ANTS-1908 tracks a "
                        "stale-text soft-veto); "
                        "`dwell_time_insufficient` = haven't reached "
                        "min_dwell_sec on the current tier yet (transient, "
                        "self-resolving); "
                        "`override_cooldown_active` = user manually picked "
                        "the current tier within the cool-down window "
                        "(transient); "
                        "`target_equals_current` = recommender already on "
                        "the target tier — *expected steady-state*, NOT a "
                        "failure; "
                        "`ticks_target_stable_insufficient` = recommender "
                        "hasn't held the new target long enough to act on — "
                        "*expected steady-state*, NOT a failure. "
                        "ANTS-1949 — `by_trigger.manual` is structurally 0: "
                        "every ledger record carries trigger=\\\"auto\\\" because "
                        "user /model commands and chip-clicks are NOT appended "
                        "to the firing ledger (only the autonomous switcher "
                        "writes records). Implication: `opus_turns_routed_in` "
                        "counts turns on Opus after an autonomous upgrade only; "
                        "it cannot see human-forced Opus sessions.");
                    t["selection_hint"] = QStringLiteral(
                        "Use to check whether automatic model switching is "
                        "paying off before trusting it more widely — reports "
                        "avoided Opus turns vs regret rate. scope:\"global\" "
                        "aggregates across all projects.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    props["etag_match"] = makeEtagMatchProp();
                    props["fields"]     = makeFieldsProp();
                    props["compact"]    = makeCompactProp();     // ANTS-2091
                    {
                        QJsonObject scopeProp;
                        scopeProp["type"]        = QStringLiteral("string");
                        scopeProp["enum"]        = QJsonArray{
                            QStringLiteral("project"),
                            QStringLiteral("global")};
                        scopeProp["description"] = QStringLiteral(
                            "Optional aggregation scope (ANTS-1889). "
                            "\"project\" (default) filters the ledger to the "
                            "caller's project root; \"global\" aggregates "
                            "across all projects in the ledger.");
                        props["scope"] = scopeProp;
                    }
                    {
                        // ANTS-1894 — mode arm. Optional; defaults to "firings".
                        // Per mcp-tools.md § 10 schema-hygiene: not in required[];
                        // per-property description set.
                        QJsonObject modeProp;
                        modeProp["type"]        = QStringLiteral("string");
                        modeProp["enum"]        = QJsonArray{
                            QStringLiteral("firings"),
                            QStringLiteral("near_misses")};
                        modeProp["description"] = QStringLiteral(
                            "Optional mode arm (ANTS-1894). \"firings\" "
                            "(default) returns the firing envelope with a "
                            "slim near_misses block; \"near_misses\" returns "
                            "the full near-miss breakdown (24 h + all-time "
                            "windows, by_blocked_by counts, dominant_blocker, "
                            "distinct_signatures). Note: by_blocked_by values "
                            "can sum to MORE than window_24h.total because a "
                            "single near-miss may be blocked by several gates "
                            "simultaneously (ANTS-1947: total counts events, "
                            "by_blocked_by counts gate-hits). ANTS-1972 — the "
                            "firings-mode slim `near_misses.total_24h` EXCLUDES "
                            "idle-end-suppressed near-misses while near_misses-"
                            "mode `window_24h.total` INCLUDES them; at the same "
                            "instant firings.total_24h + "
                            "firings.idle_end_suppressed_24h == "
                            "near_misses-mode window_24h.total (not a bug — the "
                            "slim block reports actionable gaps only).");
                        props["mode"] = modeProp;
                    }
                    schema["properties"] = props;
                    schema["required"]   = QJsonArray{
                        QStringLiteral("caller_cwd")};
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                QJsonObject tabListTool;
                tabListTool["name"] = "tab_list";
                tabListTool["description"] = QStringLiteral(
                    "List all open terminal tabs in this Ants instance. Each tab: "
                    "{index, title, cwd, shell_pid, claude_running, color}, plus, "
                    "for a tracked Claude session, claude_state "
                    "(not_running|idle|thinking|tool_use|compacting), "
                    "awaiting_input, and plan_mode / auditing / tool when set. "
                    "Envelope: {ok:true, tabs:[...]}.");
                tabListTool["detail"] = QStringLiteral(
                    "List all open terminal tabs in this Ants instance. "
                    "Each tab: {index, title, cwd, shell_pid, "
                    "claude_running, color}. When a tab runs a tracked "
                    "Claude session it also carries (ANTS-1865) "
                    "`claude_state` (not_running|idle|thinking|tool_use|"
                    "compacting), `awaiting_input` (bool — permission prompt "
                    "active), and the lean overlays `plan_mode` / `auditing` "
                    "(emitted only when true) + `tool` (the active tool name "
                    "when tool_use) — so a session can verify the per-tab "
                    "Claude dot/prompt state programmatically instead of "
                    "eyeballing the tab strip. Envelope: {ok:true, tabs:[…]}.");
                tabListTool["selection_hint"] = QStringLiteral(
                    "Use when multiple Ants tabs may exist and you "
                    "need to pick the right one (e.g. cross-tab "
                    "queries). Cheap; no caller_cwd anchor.");
                // ANTS-1499 — tab_list opts into the etag pattern so
                // a session polling for "did a tab appear?" pays the
                // full envelope only on the change. Schema carries
                // `etag_match` only — no other inputs.
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    props["etag_match"] = makeEtagMatchProp();
                    props["fields"] = makeFieldsProp();          // ANTS-1720
                    props["compact"] = makeCompactProp();        // ANTS-2091
                    schema["properties"] = props;
                    tabListTool["inputSchema"] = schema;
                }
                tools.append(tabListTool);

                QJsonObject getTextTool;
                getTextTool["name"] = "get_text";
                getTextTool["description"] = QStringLiteral(
                    "Read trailing scrollback lines from a tab. Optional "
                    "`tab` (explicit index), `caller_cwd` (your $PWD — "
                    "ANTS-1392, anchors to your tab when `tab` is "
                    "omitted), and `lines` (default 100, capped at "
                    "10000). Returns {ok, text, lines, bytes} or "
                    "{ok:false, error} when the tab index is out of "
                    "range.");
                getTextTool["selection_hint"] = QStringLiteral(
                    "Use when you need a specific tab's trailing "
                    "scrollback (not your own). Prefer get_scrollback "
                    "for your own tab; this one targets `tab` index.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    QJsonObject tabProp;   tabProp["type"]   = "integer";
                    QJsonObject linesProp; linesProp["type"] = "integer";
                                           linesProp["default"] = 100;
                    props["tab"]   = tabProp;
                    props["lines"] = linesProp;
                    // ANTS-1392 — caller_cwd anchor for the no-`tab`
                    // path. Absent with no `tab`, the call refuses
                    // tab_or_cwd_required.
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    schema["properties"] = props;
                    getTextTool["inputSchema"] = schema;
                }
                tools.append(getTextTool);

                // ANTS-1301 — recent_errors: scan recent scrollback for
                // structured compile/test/runtime errors.
                {
                    QJsonObject t;
                    t["name"] = "recent_errors";
                    t["description"] = QStringLiteral(
                        "Scan the most recent N lines of a terminal's "
                        "scrollback for structured errors — GCC/clang "
                        "`error:`, ruff/flake8 `file:line:col: CODE`, "
                        "`lua: file:line:`, ctest `(Failed)`/`***Failed`, "
                        "and Python tracebacks. Returns {ok, errors:["
                        "{category, file?, line?, column?, message, "
                        "text}], errors_count, lines_scanned, "
                        "truncated}. `category` is compiler/lint/lua/"
                        "test/python. Use instead of re-running the "
                        "command or `get_text`-ing the whole buffer to "
                        "answer \"what just went wrong in this "
                        "terminal?\". Optional `tab` (explicit index), "
                        "`caller_cwd` (anchors to your tab when `tab` "
                        "is omitted), `lines` (default 500, cap 10000), "
                        "`max_results` (default 50, cap 1000 — keeps the "
                        "newest when capped). Refuses `no_window` when "
                        "no terminal resolves.");
                    t["selection_hint"] = QStringLiteral(
                        "Use right after a build/test/lint command "
                        "fails — far cheaper than get_text + eyeballing. "
                        "For the structured ctest summary of a recorded "
                        "run, prefer test_results.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    {
                        QJsonObject p; p["type"] = "integer";
                        p["description"] = QStringLiteral(
                            "Explicit tab index. Omit to use your own "
                            "tab (via caller_cwd).");
                        props["tab"] = p;
                    }
                    {
                        QJsonObject p; p["type"] = "integer";
                        p["default"] = 500;
                        p["description"] = QStringLiteral(
                            "Trailing scrollback lines to scan "
                            "(default 500, cap 10000).");
                        props["lines"] = p;
                    }
                    {
                        QJsonObject p; p["type"] = "integer";
                        p["description"] = QStringLiteral(
                            "Cap on errors[] (default 50, cap 1000); "
                            "the newest are kept when capped. "
                            "errors_count carries the pre-cap total.");
                        props["max_results"] = p;
                    }
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    schema["properties"] = props;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-1312 — last_selection: return the focused (or
                // routed) terminal's current selection text.
                {
                    QJsonObject t;
                    t["name"] = "last_selection";
                    t["description"] = QStringLiteral(
                        "Return your tab's (or `tab`'s) terminal's "
                        "current selection text — the text the user "
                        "highlighted with the mouse. Use this instead "
                        "of walking the scrollback to find the error / "
                        "stack trace / config snippet the user is "
                        "pointing at. Saves the equivalent of a "
                        "`get_text lines=500` round-trip whenever the "
                        "context Claude needs is the same text the "
                        "user just selected. Returns {ok, "
                        "has_selection, text, length, bytes, truncated}. When the "
                        "user has no active selection, `has_selection` "
                        "is false and `text` is the empty string. "
                        "Optional `tab` (explicit index), `caller_cwd` "
                        "(anchors to your tab when `tab` is omitted). "
                        "Refuses `no_window` when no terminal "
                        "resolves.");
                    t["selection_hint"] = QStringLiteral(
                        "Use when the user says \"this\" / \"that "
                        "error\" / \"the line I selected\" — far "
                        "cheaper than scanning scrollback. For a "
                        "structured error block from a recent build, "
                        "prefer recent_errors.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    {
                        QJsonObject p; p["type"] = "integer";
                        p["description"] = QStringLiteral(
                            "Explicit tab index. Omit to use your own "
                            "tab (via caller_cwd).");
                        props["tab"] = p;
                    }
                    {
                        // ANTS-5098 — the same cap and trim as get_text.
                        QJsonObject p; p["type"] = "integer";
                        p["description"] = QStringLiteral(
                            "Cap on the returned text in UTF-8 bytes, applied "
                            "after secret redaction, as get_text's max_bytes. "
                            "Past it the head is dropped behind a truncation "
                            "sentinel and truncated is true.");
                        props["max_bytes"] = p;
                    }
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    schema["properties"] = props;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
}

}  // namespace claudeintegrationdetail
