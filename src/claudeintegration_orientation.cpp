// ANTS-1677 claudeintegration piece 6/9 — the orientation and project tool descriptors
#include "claudeintegration_internal.h"
#include "testrunners.h"   // test_results, focused_test: the runner enum
#include "auditrunner.h"   // audit_run schema: the tool lists

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace claudeintegrationdetail {

void appendToolSchemasOrientation(QJsonArray &tools) {
                // ANTS-1883 — session_orient: bundle of current_state +
                // project_layout + roadmap_query (section_index, active).
                {
                    QJsonObject t;
                    t["name"] = "session_orient";
                    t["selection_hint"] = QStringLiteral(
                        "Use as the first read on a fresh /clear "
                        "session: bundles current_state + project_layout "
                        "+ roadmap_query section_index in one call "
                        "(one ETag).");
                    t["description"] = QStringLiteral(
                        "First call on a fresh session: one envelope under one ETag "
                        "composing current_state, project_layout, active roadmap "
                        "sections, active_bullets (top-20 headlines in file order, not "
                        "priority), server_build (running version, SHA, build date) and "
                        "a trimmed codebase_index summary. Top-level ok is true when "
                        "the first three upstreams succeed; a failing key carries that "
                        "upstream's refusal. A missing optional artifact is reported in "
                        "notices[], not as a failure.");
                    t["detail"] = QStringLiteral(
                        "Single-call session-orientation bundle: composes "
                        "current_state (project / git / audit state) + "
                        "project_layout (where docs / specs / roadmap "
                        "live) + roadmap_query mode:section_index "
                        "status:\"active\" (active roadmap sections) + "
                        "active_bullets (top-20 active item headlines — "
                        "ANTS-4399: carries an `active_bullets_hint` when that "
                        "slice is a minority of the queue, because the list is "
                        "ordered by POSITION IN THE FILE and not by priority; "
                        "do not plan from it alone, use roadmap_query "
                        "mode:\"bundles\" instead; "
                        "mode:headline_only — resolve the next work "
                        "bundle without a follow-up call) + server_build "
                        "(ANTS-2073 — the running server's version + git "
                        "SHA + build date, so a client self-diagnoses a "
                        "stale-binary deploy gap) + codebase_index "
                        "(ANTS-2140 — a trimmed codebase-map summary; the "
                        "call eagerly refreshes the index per ANTS-1637 so "
                        "it is fresh at session start; volatile "
                        "generated_at_ms / refreshed_files are stripped to "
                        "keep the bundle ETag stable) "
                        "into one envelope under a single ETag. Use as "
                        "the first read on a fresh /clear session — "
                        "saves four MCP round-trips and four ETag "
                        "misses vs the per-verb-call orientation "
                        "pattern. Top-level ok is true IFF the first "
                        "three upstreams succeeded (active_bullets "
                        "does not affect ok — absent roadmap is not "
                        "a failure); on any upstream failure "
                        "the failing key carries that upstream's "
                        "verbatim refusal envelope. ANTS-1883. "
                        "ANTS-3587: an upstream that refuses ONLY because "
                        "an optional artifact is absent (no ROADMAP.md yet — "
                        "no_roadmap_loaded) keeps ok:true and is surfaced in "
                        "a notices[] array, so a fresh project's first call "
                        "does not read as a failure. "
                        "Etag tip: cache the returned `etag` field and "
                        "pass it back via `etag_match` on subsequent "
                        "calls in the same session — saves a full "
                        "re-emit when the underlying file hasn't "
                        "changed (ANTS-1499 \"304 Not Modified\" pattern).");
                    QJsonObject schema;
                    schema["type"] = "object";
                    schema["additionalProperties"] = false;
                    const QJsonObject cwdProp = makeCallerCwdReadProp();
                    QJsonObject etagProp;
                    etagProp["type"] = "string";
                    etagProp["description"] = QStringLiteral(
                        "ETag from a previous session_orient call. "
                        "When it matches: "
                        "{ok:true,unchanged:true,etag:\"<same>\"}.");
                    QJsonObject props;
                    props["caller_cwd"] = cwdProp;
                    props["etag_match"] = etagProp;
                    // ANTS-4624 — session_orient joined the `fields=`
                    // allowlist in ANTS-4523 and never got the matching
                    // schema property, so the dispatcher was
                    // WILLING to project and the argument could not arrive in
                    // a projectable form: the ANTS-1720 block requires
                    // `fv.isArray()`, and with no declared property the client
                    // marshals `fields` as a string. The projection was then
                    // skipped in silence — ignored_args is barred from
                    // reporting `fields` (it is a universal dispatch arg), and
                    // passing it suppresses the leaner_call_hint that
                    // recommended it, so the one signal a caller might have
                    // noticed disappears on the call that needed it.
                    props["fields"] = makeFieldsProp();
                    props["compact"] = makeCompactProp();        // ANTS-4657
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append(QStringLiteral("caller_cwd"));
                    schema["required"] = req;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-1723 — workflow_state: superpowers skill step/phase store.
                {
                    QJsonObject t;
                    t["name"] = "workflow_state";
                    t["selection_hint"] = QStringLiteral(
                        "Use to persist skill step/phase across /clear. "
                        "Call op:\"get\" at session start to resume.");
                    t["description"] = QStringLiteral(
                        "Per-project, per-skill step/phase store for "
                        "superpowers skills. Survives /clear — call "
                        "op:\"get\" at session start to resume from the "
                        "last saved step. ops: get (returns {found,state?}), "
                        "set (stores {step,phase,notes}), clear (deletes). "
                        "Entries expire after 72 h of inactivity. "
                        "skill: ^[A-Za-z0-9_-]{1,32}$ "
                        "(e.g. \"tdd\", \"systematic-debugging\"). "
                        "Keys stored as wf.<skill> in session_memory "
                        "backing store. Write ops (set/clear) require "
                        "focused-tab cwd match (ANTS-1435). "
                        "ANTS-1723.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    schema["additionalProperties"] = false;
                    QJsonObject opProp;
                    opProp["type"] = "string";
                    QJsonArray opEnum;
                    opEnum.append(QStringLiteral("get"));
                    opEnum.append(QStringLiteral("set"));
                    opEnum.append(QStringLiteral("clear"));
                    opProp["enum"] = opEnum;
                    opProp["description"] = QStringLiteral("get/set/clear.");
                    const QJsonObject cwdProp = makeCallerCwdReadProp();
                    QJsonObject skillProp;
                    skillProp["type"] = "string";
                    skillProp["description"] = QStringLiteral(
                        "Skill identifier, ^[A-Za-z0-9_-]{1,32}$. "
                        "E.g. \"tdd\", \"systematic-debugging\".");
                    QJsonObject stepProp;
                    stepProp["type"] = "integer";
                    stepProp["description"] =
                        QStringLiteral("Current step number (required for set).");
                    QJsonObject phaseProp;
                    phaseProp["type"] = "string";
                    phaseProp["description"] =
                        QStringLiteral("Current phase label (required for set).");
                    QJsonObject notesProp;
                    notesProp["type"] = "array";
                    QJsonObject notesItems;
                    notesItems["type"] = "string";
                    notesProp["items"] = notesItems;
                    notesProp["description"] =
                        QStringLiteral("Optional carry-forward notes.");
                    QJsonObject props;
                    props["op"]         = opProp;
                    props["caller_cwd"] = cwdProp;
                    props["skill"]      = skillProp;
                    props["step"]       = stepProp;
                    props["phase"]      = phaseProp;
                    props["notes"]      = notesProp;
                    schema["properties"] = props;
                    QJsonArray reqArr;
                    reqArr.append(QStringLiteral("op"));
                    reqArr.append(QStringLiteral("caller_cwd"));
                    reqArr.append(QStringLiteral("skill"));
                    schema["required"] = reqArr;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-1309 — spec_query: parse a single spec file.
                // Returns {title, status, kind, invariants[]} without
                // reading the full 200-2000 line markdown body.
                {
                    QJsonObject t;
                    t["name"] = "spec_query";
                    t["description"] = QStringLiteral(
                        "Parse one spec file and return its metadata and invariant list "
                        "instead of reading it. Returns {ok, id, title, status, kind, "
                        "path, size_bytes, mtime_ms, invariants:[{id, body, "
                        "test_surface?}], invariants_count, source}. Target via `id` "
                        "(<PREFIX>-NNNN resolves docs/specs/<id>.md or <id>-*.md; "
                        "phase_<NN>_<topic> resolves docs/phases/) or a "
                        "project-relative `path`. No id or path lists the specs dir; "
                        "mode:\"gate_drift\" reports gated specs edited since their last "
                        "review loop. Refusals: bad_id, bad_path, not_found, "
                        "no_project.");
                    t["detail"] = QStringLiteral(
                        "Parse a single spec / design markdown file and "
                        "return its parsed metadata + invariant list. "
                        "Returns {ok, id, title, status, kind, path, "
                        "size_bytes, mtime_ms, invariants:[{id, body, "
                        "test_surface?}], invariants_count, source}. "
                        "Recognises both the table form (`| INV-N | "
                        "body | test surface |`) and the bullet form "
                        "(`- **INV-N** — body`). Use when you need "
                        "the contract list of a spec without reading "
                        "the full ~2 K-line body. ID routing: "
                        "<PREFIX>-NNNN → docs/specs/<id>.md (source=specs; "
                        "any project prefix, e.g. ANTS-1963 or DOOM-0009 "
                        "— ANTS-3356; a topic-suffixed file `<id>-*.md` "
                        "such as DOOM-0009-path-tracer.md also resolves); "
                        "phase_<NN>_<topic> → docs/phases/<id>.md "
                        "(source=phases, ANTS-1880). "
                        "GATE DRIFT (ANTS-4352): call with no `id`/`path` and "
                        "mode:\"gate_drift\" to answer \"which gated specs "
                        "have been EDITED since their last review loop?\" — "
                        "returns {stale, current, ungated} with "
                        "commits_since:[{sha, date, subject, same_day?}] on "
                        "each stale row. A Reviewed stamp does not survive an "
                        "edit made while closing a different item, and that "
                        "failure is self-concealing: the document asserts it "
                        "was reviewed, the next session trusts the assertion, "
                        "and the invalidating commit sits where nobody looks. "
                        "`commits_since` is what makes it actionable — the "
                        "subject distinguishes the gate's own fix pass from an "
                        "authoring edit. A spec whose only commits since the "
                        "loop land on the loop DATE is reported current with "
                        "same_day_commits_only:true, because that is the "
                        "gate's own pass and it does not re-arm the gate. "
                        "`ungated` (no loop log) is its own bucket: \"never "
                        "gated\" is a different answer from \"gated and "
                        "current\". "
                        "LIST MODE (ANTS-3360): "
                        "call with NEITHER `id` nor `path` to enumerate the "
                        "specs dir — returns {ok, mode:\"list\", specs_dir, "
                        "specs:[{id, title, status, path, size_bytes, "
                        "mtime_ms}], count, truncated}, the spec-side "
                        "analogue of roadmap_query mode:section_index "
                        "(use it to discover spec ids, not a shell `ls`). "
                        "ANTS-1906 — pass "
                        "an explicit project-relative `path` instead "
                        "of `id` for projects whose specs live "
                        "elsewhere (e.g. "
                        "`docs/phases/phase_22_threading_design.md`); "
                        "the response `source` becomes \"path\" and "
                        "`id` is auto-derived from the basename. Path "
                        "must be project-relative (no leading '/', no "
                        "'..' traversal). Refusals: `bad_id` (an `id` was "
                        "passed that doesn't match the recognised shape), "
                        "`bad_path` (path escaped the project root), "
                        "`not_found` (file absent), `no_project` "
                        "(caller_cwd unresolved).");
                    t["selection_hint"] = QStringLiteral(
                        "Use instead of `Read` when you only need a "
                        "spec's INV list (the contract surface), not "
                        "the full narrative. Typically 5-20× smaller "
                        "than a full Read. Pass `path` for non-Ants "
                        "spec layouts.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    QJsonObject idProp;
                    idProp["type"] = "string";
                    idProp["description"] = QStringLiteral(
                        "Spec ID. <PREFIX>-NNNN → docs/specs/<id>.md "
                        "(any project prefix, e.g. ANTS-1963 or "
                        "DOOM-0009 — ANTS-3356; topic-suffixed "
                        "`<id>-*.md` files also resolve); "
                        "phase_<NN>_<topic> → docs/phases/<id>.md "
                        "(ANTS-1880). Optional when `path` is set "
                        "(ANTS-1906); the explicit `id` then wins as "
                        "the response's display id. Omit BOTH `id` and "
                        "`path` for list mode (ANTS-3360 — enumerate "
                        "the specs dir).");
                    props["id"]         = idProp;
                    QJsonObject pathProp;
                    pathProp["type"] = "string";
                    pathProp["description"] = QStringLiteral(
                        "Optional project-relative path to a spec file "
                        "(e.g. `docs/phases/phase_22_threading_"
                        "design.md`). When set, bypasses id-shape "
                        "routing and reads that file directly; the "
                        "response `id` is auto-derived from the "
                        "basename. Mutually exclusive shape with `id` "
                        "(both are accepted; explicit `id` overrides "
                        "the basename-derived one). Must not start "
                        "with '/' or contain '..' (ANTS-1906).");
                    props["path"]       = pathProp;
                    // ANTS-4468 — `mode` was HONOURED by the handler and never
                    // DECLARED here, so the ANTS-2175 checker (which diffs the
                    // call's args against inputSchema.properties) correctly
                    // flagged it — and the envelope then asserted both that the
                    // argument was applied and that it was discarded, with the
                    // gate_drift payload proving the first. The reporter read
                    // it as a branch-ordering bug; the cause is the missing
                    // declaration, so the fix is to declare it rather than to
                    // suppress the advisory. Suppressing would have cost the
                    // signal its meaning, which is the harm they named: an
                    // ignored_args that fires on honoured arguments trains
                    // callers to disregard it, and then a genuine typo
                    // (mode:"gate-drift") reports nothing. Declaring it also
                    // makes both non-default modes discoverable from the
                    // schema instead of only from the description prose.
                    QJsonObject modeProp; modeProp["type"] = "string";
                    {
                        QJsonArray en;
                        en.append("gate_drift");
                        en.append("list");
                        modeProp["enum"] = en;
                    }
                    modeProp["description"] = QStringLiteral(
                        "Optional response mode. Omit (with `id` or `path`) "
                        "to parse one spec — the default. \"gate_drift\" "
                        "takes NEITHER id nor path and reports which gated "
                        "specs were edited since their last review loop "
                        "{stale, current, ungated}. \"list\" is the explicit "
                        "spelling of the enumerate-the-specs-dir behaviour "
                        "that omitting id AND path already selects.");
                    props["mode"]       = modeProp;
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    schema["properties"] = props;
                    QJsonArray req;
                    // ANTS-1906 — only caller_cwd stays unconditionally
                    // required; the verb enforces "id OR path" at
                    // runtime so a single JSON Schema entry can't
                    // express it cleanly without oneOf.
                    req.append("caller_cwd");
                    schema["required"]            = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-1963 — spec_log: write the three recurring spec
                // mutations (flip Status / append cold-eyes loop / append
                // INV) instead of hand-editing markdown. Write sibling of
                // spec_query; same id/path routing.
                {
                    QJsonObject t;
                    t["name"] = "spec_log";
                    t["description"] = QStringLiteral(
                        "Edit a spec's structured surface without hand-editing "
                        "markdown. op:\"set_status\" rewrites **Status:**; "
                        "op:\"append_loop\" appends a loop-log row in the section's "
                        "existing shape (a table needs `cells`); op:\"append_inv\" "
                        "appends an INV-N bullet, never renumbering. Target via `id` "
                        "(docs/specs/<id>.md, or phase_<NN>_<topic>) or a "
                        "project-relative `path`. dry_run:true previews. Refusals: "
                        "bad_mode, bad_id, bad_path, bad_args, no_project, not_found, "
                        "unrecognised_format, format_mismatch, write_failed. caller_cwd "
                        "required.");
                    t["detail"] = QStringLiteral(
                        "Edit a spec's structured surface without "
                        "hand-editing markdown. op:\"set_status\" rewrites "
                        "the whole **Status:** field, including any "
                        "continuation lines it wraps onto (ANTS-3785); "
                        "op:\"append_status\" adds `status` at the field's "
                        "end, rewriting nothing (ANTS-4959); "
                        "op:\"append_loop\" appends a "
                        "cold-eyes loop-log row, matching the SHAPE the "
                        "section already holds (ANTS-4364): a TABLE row when "
                        "it holds a table — pass `cells`, one string per "
                        "column in the header's order; a bullet from "
                        "`loop_label` + `body` when it holds bullets or does "
                        "not exist yet; a `### <loop_label>` heading with "
                        "`body` under it when its latest row is such a "
                        "heading (ANTS-6361). Against a table with no `cells` it "
                        "REFUSES format_mismatch, naming the columns it read, "
                        "rather than writing a bullet that corrupts the table. "
                        "The insertion END is INFERRED from the existing rows "
                        "(ANTS-4353), because loop logs run in opposite "
                        "directions across specs in one corpus and a row at "
                        "the wrong end reads as a different loop's result; the "
                        "envelope echoes `row_shape` and `row_order` "
                        "(oldest_first | newest_first | ambiguous, the last "
                        "when neither loop numbers nor, in a bullet log, "
                        "dates tell). op:\"append_inv\" appends an INV-N "
                        "bullet at the end of the Invariants section "
                        "(never renumbers). Target via `id` (<PREFIX>-NNNN → "
                        "docs/specs/<id>.md, any project prefix e.g. "
                        "DOOM-0009 — ANTS-3356, topic-suffixed `<id>-*.md` "
                        "also resolves; phase_<NN>_<topic> → "
                        "docs/phases/) or a project-relative `path`. "
                        "Atomic write. Returns {ok, op, id?, path, line, "
                        "bytes_written, file_bytes} plus `previous_status` "
                        "and `new_status` "
                        "on set_status and append_status (ANTS-4114 — the "
                        "value before the write, so a "
                        "vocabulary mismatch shows here, not at the project's "
                        "lint gate) (ANTS-3724: "
                        "bytes_written is the ADDED-bytes delta, matching "
                        "roadmap_log/changelog_log; file_bytes is the whole "
                        "file); dry_run:true previews line/`bytes` "
                        "without writing. Refusals: `bad_mode`, `bad_id`, "
                        "`bad_path`, `bad_args`, `no_project`, "
                        "`not_found` (file absent), `unrecognised_format` "
                        "(spec present but missing the section/line), "
                        "`write_failed`. caller_cwd required.");
                    t["selection_hint"] = QStringLiteral(
                        "Use to flip a spec's Status, log a cold-eyes "
                        "loop, or add an INV — cheaper than an Edit "
                        "round-trip on the markdown.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    QJsonObject opProp; opProp["type"] = "string";
                        { QJsonArray e; e.append(QStringLiteral("set_status"));
                          e.append(QStringLiteral("append_status"));  // ANTS-4959
                          e.append(QStringLiteral("append_loop"));
                          e.append(QStringLiteral("append_inv"));
                          opProp["enum"] = e; }
                        opProp["description"] = QStringLiteral(
                            "Required. set_status | append_status | "
                            "append_loop | append_inv.");
                    QJsonObject idProp; idProp["type"] = "string";
                        idProp["description"] = QStringLiteral(
                            "Spec id (<PREFIX>-NNNN, any project prefix "
                            "e.g. ANTS-1963 or DOOM-0009 — ANTS-3356, or "
                            "phase_<NN>_<topic>). Required unless `path` "
                            "is set.");
                    QJsonObject pathProp; pathProp["type"] = "string";
                        pathProp["description"] = QStringLiteral(
                            "Optional project-relative path to the spec "
                            "file (no leading '/', no '..'). Overrides "
                            "`id` routing.");
                    QJsonObject statusProp; statusProp["type"] = "string";
                        statusProp["description"] = QStringLiteral(
                            "set_status: the replacement value, written "
                            "VERBATIM as one line after \"**Status:** \". "
                            "Any continuation lines the previous value "
                            "wrapped onto are replaced too — pass "
                            "`preserve_body:true` to KEEP them (ANTS-4136). "
                            "append_status (ANTS-4959): the text to add after "
                            "a space at the field's END; nothing is rewrapped. "
                            "ANTS-4114 — this verb imposes NO vocabulary and "
                            "cannot validate one: each project's permitted "
                            "values live in its own spec standard (Ants uses "
                            "a dated lifecycle, others a fixed word list that "
                            "forbids history), so a value this verb accepts "
                            "may still fail the project's lint gate. Match "
                            "the project's standard; the envelope's "
                            "`previous_status` (on dry_run too) names the "
                            "value being replaced, which is the cheapest "
                            "read of the vocabulary actually in use.");
                    QJsonObject labelProp; labelProp["type"] = "string";
                        labelProp["description"] = QStringLiteral(
                            "append_loop: the loop label "
                            "(e.g. \"Loop 3 (2026-06-03)\").");
                    QJsonObject bodyProp; bodyProp["type"] = "string";
                        bodyProp["description"] = QStringLiteral(
                            "append_loop / append_inv: the bullet body "
                            "prose.");
                    // ANTS-4364 — the table form.
                    QJsonObject cellsProp; cellsProp["type"] = "array";
                    {
                        QJsonObject it; it["type"] = "string";
                        cellsProp["items"] = it;
                    }
                    cellsProp["description"] = QStringLiteral(
                        "append_loop, TABLE form: one string per column, in "
                        "the table header's own order. Required when the loop "
                        "log is a table (the shipped spec skeleton's is); a "
                        "wrong count refuses column_mismatch rather than "
                        "writing a ragged row. Pipes are escaped and newlines "
                        "folded to <br>, so a cell cannot break the column "
                        "count. Ignored for a bullet-shaped log.");
                    QJsonObject invProp; invProp["type"] = "string";
                        invProp["description"] = QStringLiteral(
                            "append_inv: the new INV id (^INV-[0-9]+$). "
                            "Refuses bad_args if already present.");
                    QJsonObject testProp; testProp["type"] = "string";
                        testProp["description"] = QStringLiteral(
                            "append_inv: optional *Test:* clause.");
                    QJsonObject dryRunProp; dryRunProp["type"] = "boolean";
                        dryRunProp["description"] = QStringLiteral(
                            "Optional (ANTS-2136). When true, return the "
                            "resolved landing `line` and `bytes` (would-be "
                            "file size) WITHOUT writing the spec — a free "
                            "pre-flight for the section-routed insert "
                            "(envelope carries dry_run:true).");
                    props["op"]         = opProp;
                    props["id"]         = idProp;
                    props["path"]       = pathProp;
                    props["status"]     = statusProp;
                    {   // ANTS-4136 — keep the Status field's wrapped prose.
                        QJsonObject pb; pb["type"] = "boolean";
                                        pb["default"] = false;
                                        pb["description"] = QStringLiteral(
                            "Optional (ANTS-4136), op:\"set_status\" only. "
                            "Keep the Status field's CONTINUATION LINES and "
                            "rewrite only the opener, instead of replacing the "
                            "whole extent. A spec's Status is a wrapped field "
                            "and this corpus routinely carries a paragraph "
                            "there — review-loop counts, whether the gate "
                            "converged, which sections were most revised — so "
                            "a call whose stated job is to change one word was "
                            "deleting a 490-byte review history. Recoverable "
                            "via `previous_status`, but only if the caller "
                            "notices; this makes the recovery the default. "
                            "It is a LINE rule, not a prose split: there is no "
                            "delimiter this corpus agrees on between the state "
                            "word and the prose after it, so a heuristic would "
                            "drop text on the shapes it guessed wrong — the "
                            "very failure being fixed. Prose on the SAME line "
                            "as the old value is still replaced. Default false, "
                            "so existing callers are unchanged.");
                        props["preserve_body"] = pb;
                    }
                    props["loop_label"] = labelProp;
                    props["cells"]      = cellsProp;  // ANTS-4364
                    props["body"]       = bodyProp;
                    props["inv_id"]     = invProp;
                    props["test"]       = testProp;
                    props["dry_run"]    = dryRunProp;
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append(QStringLiteral("op"));
                    req.append(QStringLiteral("caller_cwd"));
                    schema["required"] = req;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-1308 — invariant_check: which specs reference
                // a given set of source files, and what invariants do
                // they declare. Surfaces "what contracts could this
                // edit break?" before the edit, not after.
                {
                    QJsonObject t;
                    t["name"] = "invariant_check";
                    t["description"] = QStringLiteral(
                        "Given project-relative file paths you are about to edit, list "
                        "the docs/specs/*.md specs and docs/decisions/*.md ADRs whose body mentions them, with "
                        "invariant counts. Returns {ok, matched_specs:[{id, path, "
                        "title, matched_terms[], invariants_count}], specs_scanned, "
                        "matched_count, mode, invariants_included, fallback_match, "
                        "roadmap_scanned}. mode defaults to \"summary\" (no invariant "
                        "bodies); mode:\"full\" includes them. Matching is "
                        "substring-only; a path matching nothing is retried by suffix "
                        "(see matched_as). It never reads the ROADMAP.");
                    t["detail"] = QStringLiteral(
                        "Given a list of project-relative file paths "
                        "(typically what you're about to edit), scan "
                        "`docs/specs/*.md` for specs that mention any "
                        "of those paths in their body and return the "
                        "parsed invariant list per matching spec. "
                        "Returns {ok, matched_specs:[{id, path, title, "
                        "matched_terms[], invariants_count}], "
                        "specs_scanned, matched_count, mode, "
                        "invariants_included}. ANTS-3699 — mode defaults "
                        "to \"summary\", which OMITS each spec's "
                        "`invariants:[{id, body}]` bodies (omitted, never "
                        "truncated, so a short list can't read as a "
                        "complete one); `invariants_count` is still the "
                        "true count. That answers this verb's question "
                        "on its own — drill into one spec with "
                        "spec_query. mode:\"full\" restores the bodies, "
                        "which on a widely-referenced file like "
                        "remotecontrol.cpp can exceed the response cap "
                        "alone. Matching is substring-only (no symbol "
                        "resolution). ANTS-4644 — pass the project-relative "
                        "path (`src/foo.cpp`); if that matches NOTHING and "
                        "specs were scanned, the verb retries with the path's "
                        "suffixes, longest first, because a spec cites a "
                        "module the way its author writes it. `fallback_match` "
                        "is on EVERY reply; when true, `fallback_kind` is "
                        "\"path_suffix\" or \"basename\" and `matched_as` maps "
                        "each path you passed to the form that actually "
                        "matched — check it, since the basename tier (used "
                        "only when every fuller form failed) can collide "
                        "between two like-named modules. ANTS-4645 — "
                        "`roadmap_scanned` is always false: this reads "
                        "docs/specs + docs/phases and NEVER the ROADMAP, so "
                        "work already planned there does not appear here even "
                        "on a confident multi-spec answer — use roadmap_query "
                        "(query=<keyword>) or task_priors for that. Use "
                        "BEFORE editing files in `src/` to surface "
                        "documented contracts the edit might break.");
                    t["selection_hint"] = QStringLiteral(
                        "Use with `files:[...]` from `git diff "
                        "--name-only` (or your own changed-file list) "
                        "before editing source files with associated "
                        "specs. Pairs with spec_query for drilling "
                        "into a single matched spec.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    QJsonObject filesProp;
                    filesProp["type"] = "array";
                    QJsonObject itemProp;
                    itemProp["type"] = "string";
                    filesProp["items"] = itemProp;
                    filesProp["minItems"] = 1;
                    filesProp["description"] = QStringLiteral(
                        "Project-relative file paths to scan specs "
                        "for. Substring-matched against spec bodies; "
                        "pass `src/foo.cpp` not `foo.cpp` to avoid "
                        "false hits on basename collisions. ANTS-4644 — a "
                        "shorter form is tried automatically when the full "
                        "path matches nothing, so the collision risk is taken "
                        "only where the alternative is a confident zero, and "
                        "`matched_as` says when it was. ANTS-4566 — that "
                        "rescue fires ONLY on a zero, so a call that matched "
                        "one spec on an incidental line never learned that "
                        "the spec actually GOVERNING the file cites it by "
                        "basename. `basename_matches` (with its own count and "
                        "hint, emitted only when non-empty) names those "
                        "specs alongside `matched_specs`, which keeps its "
                        "exact meaning — read them before treating a "
                        "non-empty `matched_specs` as the contract. "
                        "ANTS-4742: a zero that is NOT `scanned_nothing` now "
                        "sets `path_match_only:true` with a hint. Every tier "
                        "matches the PATH, so a spec citing the module by "
                        "SYMBOL carries nothing this can match and the zero "
                        "does NOT establish that no spec governs the file — "
                        "which is the dangerous direction, since write-code's "
                        "Phase 0 makes this the first lookup and a false zero "
                        "sends the session to write against no contract. The "
                        "hint names the fallback: search the module's stem "
                        "across the specs dir with workspace_search. The "
                        "matching is deliberately unchanged — folding a bare "
                        "stem into `matched_specs` would hide a weaker match "
                        "among path hits, the same collision `basename_matches` "
                        "is reported-and-never-merged to avoid.");
                    props["files"]      = filesProp;
                    // ANTS-4744 — declared, not merely accepted. ANTS-2175
                    // builds `ignored_args` from inputSchema.properties, so an
                    // undeclared alias answers a working call with
                    // "ignored_args":["paths"] — telling the caller its
                    // argument did nothing, on the call that argument steered.
                    {
                        QJsonObject aliasProp = filesProp;
                        aliasProp["description"] = QStringLiteral(
                            "Alias for `files` — the spelling doc_integrity "
                            "and file_outline use, so the name a caller "
                            "carries across works here too. Sending both "
                            "refuses bad_op_combo. There is no files-versus-"
                            "directories distinction behind the two names.");
                        props["paths"] = aliasProp;
                    }
                    // ANTS-3699 — response-shape selector; summary by default.
                    QJsonObject modeProp;
                    modeProp["type"] = "string";
                    QJsonArray modeEnum;
                    modeEnum.append("summary");
                    modeEnum.append("full");
                    modeProp["enum"]    = modeEnum;
                    modeProp["default"] = "summary";
                    modeProp["description"] = QStringLiteral(
                        "\"summary\" (default) returns each matched spec's "
                        "id/path/title/matched_terms/invariants_count WITHOUT "
                        "the invariant bodies — a few hundred bytes, and the "
                        "whole answer to \"is any of this already under "
                        "contract?\". \"full\" adds `invariants:[{id, body}]`, "
                        "which over a widely-referenced file can exceed the "
                        "response cap. An unknown value refuses `bad_mode`.");
                    props["mode"]       = modeProp;
                    // ANTS-5414 — cap on matched_specs.
                    {
                        QJsonObject maxProp;
                        maxProp["type"]    = "integer";
                        maxProp["minimum"] = 1;
                        maxProp["maximum"] = 200;
                        maxProp["default"] = 50;
                        maxProp["description"] = QStringLiteral(
                            "Cap on matched_specs (default 50). matched_total "
                            "counts every match; truncated says the cap cut it.");
                        props["max_specs"] = maxProp;
                    }
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("files");
                    req.append("caller_cwd");
                    schema["required"]             = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-1306 — task_priors: bundle task-start context
                // (matching specs + ROADMAP cards + recent commits +
                // ADRs) for a free-text task description in one call.
                {
                    QJsonObject t;
                    t["name"] = "task_priors";
                    t["description"] = QStringLiteral(
                        "Given a free-text task description, return the project context "
                        "you would otherwise gather in several exploration round-trips: "
                        "matching specs, roadmap cards, recent commits touching named "
                        "paths, and ADRs. Returns {ok, terms[], ids[], paths[], "
                        "specs[], specs_count, roadmap_cards[], cards_count, commits[], "
                        "commits_count, adrs[], adrs_count}; each *_count is the "
                        "pre-cap total. Refusals: bad_args (empty description or no "
                        "searchable terms), no_project (caller_cwd unresolved).");
                    t["detail"] = QStringLiteral(
                        "Given a free-text task description, return the "
                        "project context you'd otherwise gather in 6-8 "
                        "exploration round-trips: matching `docs/specs/*.md` "
                        "(with one-line excerpts), matching ROADMAP cards, "
                        "recent commits touching the file paths named in the "
                        "description, and related ADRs. Returns {ok, terms[], "
                        "ids[], paths[], specs:[{id, path, title, excerpt, "
                        "score}], specs_count, roadmap_cards:[{id, status, "
                        "headline, score}], cards_count, commits:[{sha, "
                        "subject, date}], commits_count, adrs:[{path, title, "
                        "score}], adrs_count}. Ranking is case-insensitive "
                        "distinct-needle substring matching; each `*_count` "
                        "is the pre-cap total. Pure composer over "
                        "roadmap_query + git_state + the spec parser — no "
                        "cache. Refusals: `bad_args` (empty description or no "
                        "searchable terms after stopword removal), "
                        "`no_project` (caller_cwd unresolved).");
                    t["selection_hint"] = QStringLiteral(
                        "Use ONCE at task start, before proposing an "
                        "approach, with the user's task description. Cheaper "
                        "and more complete than ad-hoc grep + Read of specs / "
                        "ROADMAP / git log. Drill into a surfaced spec with "
                        "spec_query.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    QJsonObject descProp;
                    descProp["type"] = "string";
                    descProp["description"] = QStringLiteral(
                        "The free-text task description (the user's prompt "
                        "or a sub-task). Parsed into ANTS-NNNN ids, "
                        "file-path tokens, and >=4-char terms for ranking.");
                    props["description"] = descProp;
                    props["caller_cwd"]  = makeCallerCwdReadProp();
                    const struct { const char *name; const char *desc; } caps[] = {
                        {"max_specs",   "Max spec hits to return (default 5, clamp 1-20)."},
                        {"max_cards",   "Max ROADMAP cards to return (default 5, clamp 1-20)."},
                        {"max_commits", "Max commits to return (default 5, clamp 1-20)."},
                        {"max_adrs",    "Max ADRs to return (default 3, clamp 1-20)."},
                    };
                    for (const auto &cap : caps) {
                        QJsonObject p;
                        p["type"] = "integer";
                        p["minimum"] = 1;
                        p["maximum"] = 20;
                        p["description"] = QString::fromUtf8(cap.desc);
                        props[QString::fromUtf8(cap.name)] = p;
                    }
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("description");
                    req.append("caller_cwd");
                    schema["required"]             = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-1307 — project_conventions: the task_type-scoped
                // subset of project conventions, each {rule, source}.
                {
                    QJsonObject t;
                    t["name"] = "project_conventions";
                    t["description"] = QStringLiteral(
                        "Return the subset of project conventions relevant "
                        "to a stated task type, instead of re-reading the "
                        "whole CLAUDE.md + standards bundle. Returns {ok, "
                        "task_type, conventions:[{rule, source}], "
                        "conventions_count, sources:[{path, exists}], "
                        "sources_count}. Each `rule` is a one-line "
                        "convention; `source` is the repo-relative doc that "
                        "states it in full (read it for detail). `sources[]` "
                        "is the deduped source set with an existence flag "
                        "(a `false` flags a missing/renamed doc). Static "
                        "curated table; no file-body reads. Refusals: "
                        "`bad_args` (task_type not one of feature/bugfix/"
                        "refactor/docs/test), `no_project` (caller_cwd "
                        "unresolved).");
                    t["selection_hint"] = QStringLiteral(
                        "Use at task start with the kind of work you're "
                        "about to do (feature/bugfix/refactor/docs/test) to "
                        "get just the relevant rules + their authoritative "
                        "doc paths — cheaper than reading the full CLAUDE.md "
                        "preamble.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    QJsonObject typeProp;
                    typeProp["type"] = "string";
                    QJsonArray typeEnum;
                    typeEnum.append("feature");
                    typeEnum.append("bugfix");
                    typeEnum.append("refactor");
                    typeEnum.append("docs");
                    typeEnum.append("test");
                    typeProp["enum"] = typeEnum;
                    typeProp["description"] = QStringLiteral(
                        "The kind of work about to be done. Lower-case; one "
                        "of feature, bugfix, refactor, docs, test.");
                    props["task_type"]  = typeProp;
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("task_type");
                    req.append("caller_cwd");
                    schema["required"]             = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-1299 — build_status: record/read the most recent
                // build's outcome at <root>/.audit_cache/build.json.
                {
                    QJsonObject t;
                    t["name"] = "build_status";
                    t["description"] = QStringLiteral(
                        "Record or read the most recent build's outcome. op=record "
                        "takes {exit_code, output, started_at_ms?, finished_at_ms?}, "
                        "parses compiler output into errors[] + warnings_count and "
                        "writes <root>/.audit_cache/build.json. op=read (default) "
                        "returns the cached envelope, with stale:true when a compile "
                        "input is newer. Refusal: not_cached.");
                    t["detail"] = QStringLiteral(
                        "Record or read the most recent build's outcome. "
                        "op=record takes {exit_code, output, started_at_ms?, "
                        "finished_at_ms?} and parses GCC/clang/cppcheck-2.x "
                        "compiler output into errors[] + warnings_count; "
                        "writes <root>/.audit_cache/build.json atomically. "
                        "op=read (default) returns the cached envelope OR "
                        "{ok:false, code:\"not_cached\"} when no record "
                        "exists. Read also surfaces stale:true when any "
                        "compile-input file (src/, tests/, cmake/, "
                        "CMakeLists.txt) has mtime newer than the cache. "
                        "Saves ~3-10 K tokens per build cycle vs. shelling "
                        "`cmake --build` and reading the full output.");
                    t["selection_hint"] = QStringLiteral(
                        "Use op=record right after every `cmake --build` "
                        "you run, then op=read on subsequent steps "
                        "instead of re-shelling the build. Pairs with "
                        "test_results for the build → test → commit gate.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    {
                        QJsonObject opProp;
                        opProp["type"]        = "string";
                        QJsonArray opEnum;
                        opEnum.append("read");
                        opEnum.append("record");
                        opProp["enum"]        = opEnum;
                        opProp["default"]     = "read";
                        opProp["description"] = QStringLiteral(
                            "Verb. \"read\" (default) returns the "
                            "cached envelope; \"record\" parses the "
                            "supplied output and writes the cache.");
                        props["op"] = opProp;
                    }
                    {
                        QJsonObject p;
                        p["type"]        = "integer";
                        p["description"] = QStringLiteral(
                            "Build exit code (required for op=record).");
                        props["exit_code"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"]        = "string";
                        p["description"] = QStringLiteral(
                            "Full stdout+stderr of the build "
                            "(required for op=record).");
                        props["output"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"]        = "integer";
                        p["description"] = QStringLiteral(
                            "Wall-clock ms when the build started "
                            "(optional, op=record).");
                        props["started_at_ms"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"]        = "integer";
                        p["description"] = QStringLiteral(
                            "Wall-clock ms when the build finished "
                            "(optional, op=record).");
                        props["finished_at_ms"] = p;
                    }
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    props["etag_match"] = makeEtagMatchProp();
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("caller_cwd");
                    schema["required"]             = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-1300 — test_results: record/read the most recent
                // ctest --output-on-failure run.
                {
                    QJsonObject t;
                    t["name"] = "test_results";
                    t["description"] = QStringLiteral(
                        "Record or read the most recent test run's summary. op=record "
                        "takes {exit_code, output, runner?, started_at_ms?, finished_at_ms?, "
                        "duration_ms?}, parses ctest, pytest, jest, vitest, cargo test or "
                        "go test -v output into {runner, passed, failed, skipped, total, "
                        "nothing_ran, failing_tests:[{name, excerpt}]} and writes "
                        "<root>/.audit_cache/tests.json. op=read (default) returns it; "
                        "detail=<name> returns one failing test's full block. Refusals: "
                        "not_cached, detail_not_found.");
                    t["detail"] = QStringLiteral(
                        "Record or read the most recent test run's "
                        "summary. op=record takes {exit_code, output, "
                        "runner?, started_at_ms?, finished_at_ms?, "
                        "duration_ms?} and parses the output of ctest "
                        "--output-on-failure, pytest, jest, vitest, cargo "
                        "test or go test -v (ANTS-5503; `runner` names it, "
                        "else it is recognised from the output) into "
                        "{runner, passed, failed, skipped, total, "
                        "nothing_ran, collected?, failing_tests:"
                        "[{name, excerpt}]}; nothing_ran is true when no "
                        "test ran. Writes "
                        "<root>/.audit_cache/tests.json atomically. "
                        "op=read (default) returns the cached envelope. "
                        "op=read with detail=<name> returns one failing "
                        "test's full captured block (up to 8 000 chars) "
                        "instead of the truncated 20-line excerpt. "
                        "Refusals: not_cached (no record), "
                        "detail_not_found (cache exists, named test "
                        "wasn't in failing_tests[]). Saves ~3-15 K "
                        "tokens per test cycle.");
                    t["selection_hint"] = QStringLiteral(
                        "Use op=record after every test run (ctest, "
                        "pytest, jest, vitest, cargo, go -v), then op=read for "
                        "subsequent checks. Pass detail=<name> when you "
                        "need the full body of one failing test "
                        "without re-running the suite.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    {
                        QJsonObject opProp;
                        opProp["type"]        = "string";
                        QJsonArray opEnum;
                        opEnum.append("read");
                        opEnum.append("record");
                        opProp["enum"]        = opEnum;
                        opProp["default"]     = "read";
                        opProp["description"] = QStringLiteral(
                            "Verb. \"read\" (default) returns the "
                            "cached envelope; \"record\" parses the "
                            "supplied output and writes the cache.");
                        props["op"] = opProp;
                    }
                    {
                        QJsonObject p;
                        p["type"]        = "integer";
                        p["description"] = QStringLiteral(
                            "The runner's exit code (required for op=record).");
                        props["exit_code"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"]        = "string";
                        p["description"] = QStringLiteral(
                            "Full stdout+stderr of the test run "
                            "(required for op=record).");
                        props["output"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"]        = "string";
                        p["enum"]        = QJsonArray::fromStringList(TestRunners::names());
                        p["description"] = QStringLiteral(
                            "op=record: the runner that printed `output`. "
                            "Omitted, it is recognised from the output.");
                        props["runner"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"]        = "integer";
                        p["description"] = QStringLiteral(
                            "Wall-clock ms when the test run "
                            "started (optional, op=record).");
                        props["started_at_ms"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"]        = "integer";
                        p["description"] = QStringLiteral(
                            "Wall-clock ms when the test run "
                            "finished (optional, op=record).");
                        props["finished_at_ms"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"]        = "integer";
                        p["description"] = QStringLiteral(
                            "Total duration in ms (optional, "
                            "op=record). Caller-supplied wins over "
                            "the duration parsed from the output.");
                        props["duration_ms"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"]        = "string";
                        p["description"] = QStringLiteral(
                            "Failing-test name selector (optional, "
                            "op=read only). When present, response "
                            "shape is {ok, detail:{name, excerpt}} "
                            "with the full captured block as "
                            "excerpt. detail_not_found refusal "
                            "when the name isn't in failing_tests[].");
                        props["detail"] = p;
                    }
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    props["etag_match"] = makeEtagMatchProp();
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("caller_cwd");
                    schema["required"]             = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-1302 — focused_test: run only the ctest subset
                // touching the changed files, return the test_results
                // envelope shape.
                {
                    QJsonObject t;
                    t["name"] = "focused_test";
                    t["description"] = QStringLiteral(
                        "Run only the tests that exercise the changed files, under "
                        "ctest, pytest, jest, vitest, cargo or go (detected, or `runner`). "
                        "Maps `changed_files` (or git status) to the tests to run. Returns "
                        "the test_results envelope {ok, runner, passed, failed, skipped, "
                        "total, nothing_ran, failing_tests[]} plus selection, selector and "
                        "mapping fields. An unmapped file or empty selection falls back to "
                        "the full suite (downgraded_to_full). Refusals: no_project, "
                        "no_runner, no_build_dir, focused_test_in_flight, bad_args, "
                        "ctest_missing, ctest_failed, runner_missing, runner_failed, "
                        "unrecognised_output. To check a test would CATCH a defect, use "
                        "mutation_probe.");
                    t["detail"] = QStringLiteral(
                        "Run only the tests that exercise the changed "
                        "files, instead of the whole suite. Resolves "
                        "`changed_files` (or auto-derives from git "
                        "status) to ctest -R patterns via "
                        "`tests/coverage-map.json` (or a heuristic, or a "
                        "full-suite fallback), runs `ctest --test-dir "
                        "<build> -R <regex> --output-on-failure`, and "
                        "returns the test_results envelope {ok, passed, "
                        "failed, skipped, total, failing_tests[]} plus "
                        "{selection (map/heuristic/full), ctest_filter, "
                        "changed_files, mapped_files, unmapped_files, "
                        "ignored_files, selection_reason, build_dir, "
                        "duration_ms}. ANTS-5503 — pytest, jest, vitest, "
                        "cargo and go run the same way: the runner comes "
                        "from `runner` or the project's marker files "
                        "(reported as runner and runner_evidence); jest, "
                        "vitest, cargo and go pick related tests "
                        "themselves, pytest matches test file names; "
                        "`selector` lists the narrowing arguments; "
                        "ctest_filter and build_dir are ctest only. "
                        "nothing_ran is true when no test ran. "
                        "Conservative: an unmapped source "
                        "file or a 0-match selection falls back to the "
                        "full suite (sets downgraded_to_full) — it never "
                        "reports green from a run that matched no tests. "
                        "Runs against the EXISTING build (does not "
                        "rebuild). Refusals: no_project, no_build_dir "
                        "(no configured build/ with CMakeCache.txt), "
                        "focused_test_in_flight, bad_args, ctest_missing, "
                        "ctest_failed (timeout/crash), unrecognised_output; "
                        "for the other runners no_runner (no marker, no "
                        "`runner`), runner_missing (program would not "
                        "start), runner_failed (timeout/crash). "
                        "ANTS-4472 — SIBLING VERB: this one answers \"do the "
                        "tests pass?\". `mutation_probe` answers the other "
                        "half, \"would they CATCH the defect?\" — it mutates "
                        "a file, runs a selector, restores it, and REFUSES a "
                        "mutation that changed nothing, so a green run cannot "
                        "be misread as coverage the suite does not have. "
                        "Reach for it before believing a passing test holds "
                        "an invariant; it is deferred, so nothing surfaces it "
                        "at session start.");
                    // ANTS-4472 — the SessionStart prelude is at 1390 of its
                    // 1400-byte cap, so mutation_probe cannot be named there.
                    // It is named here instead — the contributor's own cheaper
                    // variant — because a session running tests is exactly the
                    // session that needs it: three consecutive sessions
                    // hand-rolled the loop in bash, one of them without an
                    // inert guard, because nothing surfaced the verb. The
                    // substantive sentence lives in `description`, which has no
                    // cap; `selection_hint` is capped at 240 chars by
                    // ANTS-1453 HINT-3 and takes the short pointer only.
                    t["selection_hint"] = QStringLiteral(
                        "Use instead of a full `ctest` to verify a focused "
                        "change (5x-50x faster on mapped files). Build "
                        "FIRST — focused_test does not rebuild. Omit "
                        "`changed_files` to use git status, or pass a set. "
                        "Sibling mutation_probe: would tests CATCH it?");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    {
                        QJsonObject p;
                        p["type"]        = "string";
                        p["enum"]        = QJsonArray::fromStringList(TestRunners::names());
                        p["description"] = QStringLiteral(
                            "The test runner. Omitted, it is detected from the "
                            "project's marker files (CMakeLists.txt first).");
                        props["runner"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "array";
                        QJsonObject item; item["type"] = "string";
                        p["items"] = item;
                        p["description"] = QStringLiteral(
                            "Project-relative changed file paths. When "
                            "omitted, derived from git working-tree "
                            "status. Mapped to the tests each runner "
                            "should run.");
                        props["changed_files"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"]    = "integer";
                        p["minimum"] = 10;
                        p["maximum"] = 1800;
                        p["description"] = QStringLiteral(
                            "ctest wall-clock budget in seconds "
                            "(default 300, clamp 10-1800).");
                        props["timeout_sec"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "Build dir relative to the project root "
                            "(default \"build\"). Must contain "
                            "CMakeCache.txt.");
                        props["build_dir"] = p;
                    }
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("caller_cwd");
                    schema["required"]             = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-3745 — build_target_for: which target owns a file,
                // and the two commands that answer is for.
                {
                    QJsonObject t;
                    t["name"] = "build_target_for";
                    t["description"] = QStringLiteral(
                        "Which build target owns a source file, read statically from "
                        "CMakeLists.txt, with the `cmake --build --target` line and, "
                        "for a gtest source, the `ctest -R` filter. Returns {ok, path, "
                        "cmake_path, targets:[{name, kind, command, line, source_count, "
                        "build_command}], targets_parsed, found, suites?, "
                        "ctest_filter?, ctest_command?, hint?, candidate_targets?}. "
                        "found:false is an answer: a header, or a new test source not "
                        "yet in a bundle's SOURCES, where candidate_targets suggests "
                        "bundles. Read-only. caller_cwd required.");
                    t["detail"] = QStringLiteral(
                        "Which build target owns this source file, "
                        "read statically from CMakeLists.txt — plus the "
                        "`cmake --build --target` line and, for a gtest "
                        "source, the `ctest -R` filter its suites imply. "
                        "Replaces the two fallbacks this project kept "
                        "reaching for: an awk walking backwards to the "
                        "nearest ants_add_*_bundle, and running every "
                        "build/test_* with --gtest_list_tests to find which "
                        "binary carried a suite. The mapping is NOT "
                        "guessable from the path — tests/features/"
                        "cold_eyes_engine builds into test_audit, and "
                        "tests/features/spec_conformance into test_claude — "
                        "so recall does not substitute. Returns {ok, path, "
                        "cmake_path, targets:[{name, kind, command, line, "
                        "source_count, build_command}], targets_parsed, "
                        "found, suites?, ctest_filter?, ctest_command?, "
                        "hint?}. `found:false` is a real answer rather than "
                        "a refusal: a header is usually not listed (build "
                        "the target owning its .cpp), and a NEW test source "
                        "is not listed until it is added to a bundle's "
                        "SOURCES — the trap where the build then succeeds "
                        "silently and runs the old binary, so check `found` "
                        "after adding one. For such a source (ANTS-5251), "
                        "`candidate_targets:[{name, build_command, "
                        "headers_covered, headers_missing}]` lists up to three "
                        "bundles whose sources already include this file's "
                        "`#include \\\"...\\\"` headers, best first. It is a "
                        "suggestion read from includes, not the link line, "
                        "and is absent when the file does not exist yet or "
                        "no bundle covers a header. A source named through a CMake "
                        "variable, a generator expression or "
                        "target_sources() is not resolved and reports "
                        "unowned rather than being attributed to the wrong "
                        "target. Multiple owners are possible and all are "
                        "returned. Read-only; opens the CMake file and (for "
                        "suites) the source, plus each bundle's sources when "
                        "suggesting candidates. caller_cwd required.");
                    t["selection_hint"] = QStringLiteral(
                        "Use right after editing or adding a test source and "
                        "before building — it answers what to build and what "
                        "to run in one call.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "Project-relative (or absolute in-root) source "
                            "path to look up. Required.");
                        props["path"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "CMake file to parse, project-relative "
                            "(default \"CMakeLists.txt\").");
                        props["cmake_path"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "Build dir used when composing build_command / "
                            "ctest_command (default \"build\"). Nothing is "
                            "run and the directory is not probed.");
                        props["build_dir"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"]    = "boolean";
                        p["default"] = true;
                        p["description"] = QStringLiteral(
                            "Read the source for TEST/TEST_F/TEST_P suite "
                            "names and emit suites[] + ctest_filter. Pass "
                            "false to skip the second file read when only "
                            "the target name is wanted.");
                        props["suites"] = p;
                    }
                    schema["properties"] = props;
                    QJsonArray req2;
                    req2.append("caller_cwd");
                    req2.append("path");
                    schema["required"]             = req2;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-1303 — find_definition: tree-wide regex scan for
                // a symbol's definition/declaration line(s).
                {
                    QJsonObject t;
                    t["name"] = "find_definition";
                    // On the wire, kEagerDescription below replaces this text.
                    t["description"] = QStringLiteral(
                        "Find where a symbol is defined across the project, without a "
                        "full LSP: a regex-anchored scan over C++/Python/Lua/Shell/GLSL "
                        "source. Use instead of grep + Read cycles for \"where is Foo "
                        "defined?\". Returns {ok, symbol, lang, definitions:[{file, "
                        "line, signature, lang, kind}], definitions_count, "
                        "files_scanned, truncated, walk_capped (+ walk_unreached)}; `kind` is definition, "
                        "declaration or local (inside a function body or parameter "
                        "list; ranked last). Refusals: bad_args (symbol missing or not a "
                        "valid identifier), no_project (caller_cwd unresolved).");
                    t["detail"] = QStringLiteral(
                        "Find where a symbol is defined across the "
                        "project, without a full LSP. Regex-anchored "
                        "scan over C++/Python/Lua/Shell/GLSL source. Returns "
                        "{ok, symbol, lang, definitions:[{file, line, "
                        "signature, lang, kind}], definitions_count, "
                        "files_scanned, truncated, walk_capped}. `kind` "
                        "is \"definition\" or \"declaration\" (C++ "
                        "header decls end in `;`), or \"local\" (ANTS-5313) "
                        "for a C++/GLSL name declared inside a function "
                        "body or a parameter list — still reported, never "
                        "claimed as the symbol's home, ranked after every "
                        "definition and declaration. When there are zero "
                        "definitions but the query exactly matches a "
                        "source file's base name, adds `file_stem_hint` "
                        "(the rel path) + a ready-to-read `hint` string "
                        "(\"no symbol named X; did you mean the file "
                        "X.cpp?\") — ANTS-1950, saves a follow-up when the "
                        "query was a filename, not a symbol. Use instead "
                        "of 4-6 grep + Read cycles for \"where is Foo "
                        "defined?\". Refusals: bad_args (symbol missing "
                        "or not a valid identifier), no_project "
                        "(caller_cwd unresolved).");
                    t["selection_hint"] = QStringLiteral(
                        "Use instead of grep+Read when you need the "
                        "definition site of one named symbol. Pairs "
                        "with find_caller (who uses it) and file_outline "
                        "(what's in one file).");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    {
                        QJsonObject p;
                        p["type"]        = "string";
                        p["description"] = QStringLiteral(
                            "Identifier to locate "
                            "(^[A-Za-z_][A-Za-z0-9_]{0,127}$).");
                        props["symbol"] = p;
                    }
                    props["lang"] = makeSymbolLangProp();
                    {
                        QJsonObject p;
                        p["type"]        = "integer";
                        p["description"] = QStringLiteral(
                            "Cap on definitions[] (default 50). "
                            "definitions_count carries the pre-cap "
                            "total.");
                        props["max_results"] = p;
                    }
                    {
                        // ANTS-2087 — opt-in symbol body inline.
                        QJsonObject p;
                        p["type"]        = "boolean";
                        p["default"]     = false;
                        p["description"] = QStringLiteral(
                            "When true, each definition carries its body "
                            "inline (`body`, `body_start_line`, "
                            "`body_end_line`, `body_truncated?`) via "
                            "read_region's symbol-body extractor — answers "
                            "\"where is Foo AND what does it do\" in one "
                            "call instead of a follow-up read_region. Off "
                            "by default (lean envelope). A def whose body "
                            "the file outline can't resolve "
                            "(declaration-only, overload ambiguity) is "
                            "returned without a body (ANTS-2087).");
                        props["include_body"] = p;
                    }
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("symbol");
                    req.append("caller_cwd");
                    schema["required"]             = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-1303 — find_caller: tree-wide regex scan for the
                // call sites of a symbol, plus its best-guess definition.
                {
                    QJsonObject t;
                    t["name"] = "find_caller";
                    t["description"] = QStringLiteral(
                        "Find what calls a symbol across the project, without a full "
                        "LSP: a regex-anchored scan over C++/Python/Lua/Shell/GLSL source. "
                        "Returns {ok, symbol, lang, callers:[{file, line, context, "
                        "lang}], callers_count, definition?, files_scanned, truncated, "
                        "walk_capped}; the definition line is excluded from callers. "
                        "files_only:true returns files:[{file, count, lines[]}] "
                        "instead; `lane` scopes the scan to one subdirectory. Refusals: "
                        "bad_args (symbol missing or not a valid identifier), "
                        "no_project (caller_cwd unresolved). Reverse lookup from "
                        "one name: cheaper than an unscoped workspace_search.");
                    t["detail"] = QStringLiteral(
                        "Find what calls a symbol across the project, "
                        "without a full LSP. Regex-anchored scan over "
                        "C++/Python/Lua/Shell/GLSL source. Returns {ok, "
                        "symbol, lang, callers:[{file, line, context, "
                        "lang}], callers_count, definition?, "
                        "files_scanned, truncated, walk_capped}. The "
                        "symbol's own definition line is excluded from "
                        "callers; `definition` carries the best-guess "
                        "definition so you get \"where + who\" in one "
                        "call. With `files_only:true` the `callers[]` "
                        "array is replaced by `files:[{file, count, "
                        "lines[]}]` (the matched-file manifest, no "
                        "quoted context windows). `lane` scopes the scan "
                        "to one subdirectory (ANTS-3805) — reach for it on "
                        "a short generic name like update/render/init, "
                        "which otherwise matches every class that has one. "
                        "Refusals: bad_args "
                        "(symbol missing or not a valid identifier), "
                        "no_project (caller_cwd unresolved).");
                    t["selection_hint"] = QStringLiteral(
                        "Use instead of grep+Read for \"who calls Foo?\". "
                        "Pairs with find_definition; shell matches are "
                        "coarser (no call-paren convention).");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    {
                        QJsonObject p;
                        p["type"]        = "string";
                        p["description"] = QStringLiteral(
                            "Identifier to locate "
                            "(^[A-Za-z_][A-Za-z0-9_]{0,127}$).");
                        props["symbol"] = p;
                    }
                    props["lang"] = makeSymbolLangProp();
                    {
                        QJsonObject p;
                        p["type"]        = "integer";
                        p["description"] = QStringLiteral(
                            "Cap on callers[] (default 200). "
                            "callers_count carries the pre-cap total.");
                        props["max_results"] = p;
                    }
                    {
                        // ANTS-3805 — scope filter. Without one, a generic
                        // method name (update/render/init/reset — most of a
                        // real class API) returns hundreds of unnarrowable
                        // rows and the verb silently fails at its headline
                        // question. Reported by Vestige: 360 truncated callers
                        // for `update`, none in the class they changed.
                        QJsonObject p;
                        p["type"]        = "string";
                        p["description"] = QStringLiteral(
                            "Optional (ANTS-3805). Restrict the scan to this "
                            "project-relative subdirectory (e.g. \"src/render\"). "
                            "Scopes the WALK, so callers_count and truncated "
                            "describe the scoped set rather than a filtered "
                            "view of a wider one; the applied value is echoed "
                            "back as `lane`. Use it whenever the symbol is a "
                            "short generic method name — a bare `update` "
                            "matches every class that has one. A lane that "
                            "does not resolve to a directory under the root is "
                            "IGNORED (you get the whole-project scan), never a "
                            "silent empty result.");
                        props["lane"] = p;
                    }
                    {
                        // ANTS-2087 — opt-in body of the called symbol's
                        // definition (not the call sites).
                        QJsonObject p;
                        p["type"]        = "boolean";
                        p["default"]     = false;
                        p["description"] = QStringLiteral(
                            "When true, the `definition` (the called "
                            "symbol's own definition) carries its body "
                            "inline (`body`, `body_start_line`, "
                            "`body_end_line`, `body_truncated?`) via "
                            "read_region's symbol-body extractor. The "
                            "call sites in `callers[]` already carry "
                            "context lines and are unaffected. Off by "
                            "default (ANTS-2087).");
                        props["include_body"] = p;
                    }
                    {
                        // ANTS-3555 — files_only: manifest mode. Drop the
                        // quoted per-call `context` windows; return the
                        // matched-file set + exact line numbers instead.
                        QJsonObject p;
                        p["type"]    = "boolean";
                        p["default"] = false;
                        p["description"] = QStringLiteral(
                            "When true, return {files:[{file, count, "
                            "lines[]}], files_count, callers_count, "
                            "files_only:true, definition?, ...} — the "
                            "DISTINCT set of files that call the symbol, each "
                            "with its call count and the exact line numbers, "
                            "and NO quoted `context` windows. A "
                            "rows-ELIMINATED manifest for \"which files call "
                            "X?\" when you will read_region the sites next. "
                            "`count` is the returned (possibly "
                            "max_results-capped) call count for that file and "
                            "equals lines[].size(); top-level callers_count "
                            "carries the true pre-cap total. Off by default "
                            "(ANTS-3555).");
                        props["files_only"] = p;
                    }
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    props["encoding"] = makeEncodingProp();      // ANTS-2090
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("symbol");
                    req.append("caller_cwd");
                    schema["required"]             = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-1305 — similar_code: tree-wide shape matcher.
                // Ranks existing class/function signatures by token-set
                // similarity to a free-text shape query.
                {
                    QJsonObject t;
                    t["name"] = "similar_code";
                    t["description"] = QStringLiteral(
                        "Find the project's existing examples of a code "
                        "shape before you write a new one (CLAUDE.md §3 "
                        "reuse-before-rewriting). Walks C++/Python source, "
                        "extracts class/function signatures, and ranks "
                        "them by token-set Jaccard similarity to your "
                        "`shape` query. Returns {ok, shape, lang, "
                        "matches:[{file, line, signature, kind, lang, "
                        "score}], matches_count, files_scanned, "
                        "truncated, walk_capped}, top matches first "
                        "(default 3, max 20). The query is tokenised, "
                        "never run as a regex. Refusals: bad_args (shape "
                        "missing/empty/over-512-chars/no usable tokens), "
                        "no_project (caller_cwd unresolved).");
                    t["selection_hint"] = QStringLiteral(
                        "Use before writing a new dialog / IPC verb / "
                        "test to copy the project convention. Phrase the "
                        "query like the signature you are about to write "
                        "(e.g. \"class FooDialog : public QDialog\"), not "
                        "as English prose. Pairs with file_outline.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    {
                        QJsonObject p;
                        p["type"]        = "string";
                        p["description"] = QStringLiteral(
                            "Free-text code shape to match, phrased like "
                            "a signature (e.g. \"void cmdBar(const "
                            "QJsonObject&)\"). Tokenised, not a regex.");
                        props["shape"] = p;
                    }
                    {
                        // ANTS-4951 — declared because the HANDLER READS IT,
                        // on ANTS-4621's reasoning: this schema sets
                        // additionalProperties:false, so an undeclared
                        // argument is refused by a strict client before the
                        // handler is reached, and on a permissive one
                        // ANTS-2175 reports it in ignored_args — telling the
                        // caller its argument did nothing, on the call that
                        // argument had just steered.
                        QJsonObject p;
                        p["type"]        = "string";
                        p["description"] = QStringLiteral(
                            "Alias for `shape` (ANTS-4951) — same meaning, "
                            "and sending both refuses bad_op_combo. Accepted "
                            "because workspace_search takes `query` as its "
                            "own alias for `pattern`, so the two "
                            "search-shaped verbs in one family took "
                            "different names for one idea and the natural "
                            "guess was a refusal.");
                        props["query"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        QJsonArray e;
                        e.append("auto"); e.append("cpp"); e.append("py");
                        p["enum"]        = e;
                        p["default"]     = "auto";
                        p["description"] = QStringLiteral(
                            "Restrict the scan to one language family. "
                            "\"auto\" (default) scans C++ and Python.");
                        props["lang"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"]        = "integer";
                        p["description"] = QStringLiteral(
                            "Cap on matches[] (default 3, max 20). "
                            "matches_count carries the pre-cap total.");
                        props["max_results"] = p;
                    }
                    {
                        // ANTS-2156 — return the FULL enclosing definition
                        // per match so the idiom is copyable in one call.
                        QJsonObject p;
                        p["type"]        = "boolean";
                        p["default"]     = false;
                        p["description"] = QStringLiteral(
                            "When true, each match also carries the FULL "
                            "enclosing definition (`symbol`, `body` lines, "
                            "`body_start_line`/`body_end_line`, "
                            "`body_truncated`) extracted via read_region — "
                            "so you copy the canonical in-repo idiom in ONE "
                            "call instead of opening each file. A match whose "
                            "signature doesn't resolve to an outline symbol "
                            "carries `body_unavailable:true` (file:line still "
                            "given). Pair with a small max_results (the "
                            "default 3).");
                        props["include_bodies"] = p;
                    }
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("shape");
                    req.append("caller_cwd");
                    schema["required"]             = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-1112 — five `indie_review_*` tools that lift the
                // mechanical halves of /indie-review out of orchestrator
                // context. Engine: src/indiereviewengine.{h,cpp}.
                {
                    QJsonObject t;
                    t["name"] = "indie_review_partition";
                    t["description"] = QStringLiteral(
                        "Return the subsystem (lane) partition for indie-review, from "
                        ".indie-review/partition.json when present, else "
                        "docs/subsystems.md or CLAUDE.md's module map, with per-lane "
                        "file lists. Each lane carries file_count and total_lines; "
                        "too_coarse and oversized flag lanes over budget, and "
                        "kind:\"tests\" labels test lanes. suggested_merges[] names "
                        "near-duplicate lanes. Files in no lane are reported as "
                        "unassigned_count / unassigned_reason; their absence means full "
                        "coverage.");
                    t["detail"] = QStringLiteral(
                        "Return the subsystem (lane) partition for "
                        "indie-review. Reads the module map from "
                        "`docs/subsystems.md` (or CLAUDE.md `## Module map "
                        "(src/)` when un-migrated; or the "
                        "`.indie-review/partition.json` override if "
                        "present) and computes per-lane source-file lists. "
                        "Saves N file-walk passes the orchestrator would "
                        "otherwise do. The envelope also carries "
                        "`suggested_merges:[{lanes:[a,b],rationale}]` "
                        "(ANTS-1288) — lanes whose summaries duplicate each "
                        "other (e.g. a multi-name module-map bullet) so you "
                        "can fold them into one review unit rather than "
                        "dispatching two near-identical briefs. ANTS-4100 — "
                        "each lane carries `file_count` (a real walk; a lane "
                        "may name a directory), and a lane above 30 files is "
                        "marked `too_coarse:true`, mirrored by envelope "
                        "`too_coarse` / `too_coarse_lanes` / "
                        "`too_coarse_hint`. The verb mirrors the module map's "
                        "granularity and never splits a lane for you: treat "
                        "`too_coarse` as a prompt to partition by cohesion "
                        "and commit `.indie-review/partition.json`. "
                        "ANTS-4804 — every lane also carries `total_lines`, "
                        "and a lane over the line budget is `oversized:true` "
                        "with envelope `oversized` / `oversized_lanes` / "
                        "`oversized_hint`. Read BOTH: `too_coarse` counts "
                        "files and is silent on a lane that is one large "
                        "module, which is the shape most likely to be swept "
                        "as a single unit. `total_lines` is reported whether "
                        "or not the threshold trips, so apply your own budget "
                        "if it differs; `total_lines_capped` means the figure "
                        "is a FLOOR, not a total. "
                        "ANTS-4806 — a lane over a declared or detected "
                        "test root carries `kind`:\"tests\". review-code's "
                        "scope excludes tests and review-tests' is exactly "
                        "them, so DROP what you do not want rather than "
                        "expecting the verb to choose: it labels and never "
                        "omits, because a silently omitted tree is the failure "
                        "ANTS-4771 was filed about. An absent `kind` is the "
                        "absence of a label, not a claim the lane is "
                        "production code. ANTS-4805 — .github is walked as "
                        "an explicit root, so the CI and release surface "
                        "appears in a lane or in the coverage fields below "
                        "rather than vanishing. ANTS-4816 — a lane whose "
                        "files the count cannot admit (packaging, YAML, "
                        "shell) carries `uncounted_files` and "
                        "`counted_nothing`, so a `file_count` of 0 is "
                        "distinguishable from an empty directory; both "
                        "`too_coarse` and `total_lines` key on that count, so "
                        "neither fires on such a lane. "                        "ANTS-4771/4786 — files under the walked source roots "
                        "that landed in NO lane are reported as "
                        "`unassigned_count` / `unassigned_by_suffix` / "
                        "`unassigned_reason` / `unassigned_hint`, on BOTH the "
                        "declared and the computed path, plus a bounded "
                        "`unassigned_sample` of paths on the declared one. "
                        "`unassigned_reason` names the cause: "
                        "`suffix_filter` (a `derived:true` reply — the "
                        "computed walk admits only suffixes the codebase "
                        "index can outline, which is NARROWER than "
                        "\"source\", and shell in particular is dropped) or "
                        "`incomplete_partition` (the project's declared "
                        "partition does not name them). Read either as a "
                        "coverage gap rather than a formatting note — a "
                        "review driven by this partition will not look at "
                        "those files, and `file_count` counts only what a "
                        "lane covers. The fields are ABSENT when nothing is "
                        "uncovered, so their absence is a clean bill of "
                        "health rather than a question nobody asked. "
                        "ANTS-4846 — when .indie-review/partition.json exists "
                        "but is rejected, `map_rejected:[{path, reason}]` says "
                        "why, and `path` names the source the lanes came from.");
                    t["selection_hint"] = QStringLiteral(
                        "Use to split source files for multi-reviewer "
                        "indie-review dispatch. Pairs with "
                        "indie_review_brief + indie_review_dispatch.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    // ANTS-1391 — caller_cwd anchor.
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    schema["properties"] = props;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                {
                    QJsonObject t;
                    t["name"] = "indie_review_corroborate";
                    t["description"] = QStringLiteral(
                        "Cross-lane corroboration filter: returns findings cited by at "
                        "least min_lanes (default 2) distinct lanes at the same "
                        "file:line. Provide exactly one of `reports` (inline {lane: "
                        "text}) or `reports_dir` (project-relative *.md directory; "
                        "allow_outside_project for an absolute one). Pure regex, no "
                        "LLM. Read citations_seen / citations_resolved and near_misses "
                        "before concluding from a zero; unresolved_citations:true means "
                        "nothing resolved. line_slop > 0 groups nearby citations.");
                    t["detail"] = QStringLiteral(
                        "Cross-lane corroboration filter. Input: "
                        "EITHER `reports` (inline map of "
                        "{lane: report_text}, v1) OR `reports_dir` "
                        "(project-relative directory of *.md files, "
                        "v2 — saves parent context by reading from "
                        "disk server-side; ANTS-1282). Returns "
                        "findings cited by >= min_lanes distinct "
                        "lanes at the same (file, line). Pure regex "
                        "pass; no LLM. Provide exactly one of "
                        "`reports` or `reports_dir`. Optional: "
                        "min_lanes (default 2); "
                        "allow_outside_project (ANTS-3713) to point "
                        "reports_dir at an absolute path such as the "
                        "session scratchpad. A citation is matched as "
                        "`path/to/file.ext:LINE` (bold wrapping and a "
                        "`423-424` range both parse; the range's first "
                        "number is used); ANTS-4095 — a citation naming only "
                        "a basename resolves when that basename is UNIQUE in "
                        "the tree, and stays dropped when it is ambiguous. "
                        "The envelope reports `citations_seen` / "
                        "`citations_resolved` (+ `citations_by_basename`), "
                        "and sets `unresolved_citations:true` when reports "
                        "parsed but nothing resolved — that case is NOT the "
                        "same as no two lanes agreeing. `total_input_bytes` "
                        "is 0 by design on the reports_dir path and is not a "
                        "parse-failure signal. "
                        "ANTS-4817 — the envelope also carries `near_misses` "
                        "when lanes cited the SAME FILE within a few lines of "
                        "one another without agreeing on a line. Two readers "
                        "quoting one statement rarely pick the same line (a "
                        "multi-line call, a decorator or a docstring puts the "
                        "quotable line somewhere different for each), so "
                        "exact matching makes the highest-value agreements "
                        "the least likely to be reported \\u2014 two "
                        "independent reports measured runs where EVERY real "
                        "agreement was missed. These are ADVISORY: they are "
                        "not findings, are not counted in `total_findings`, "
                        "and do not change what corroboration means. Read "
                        "them before concluding from a zero that the lanes "
                        "did not agree. Absent when there are none. "
                        "`line_slop` (default 0) is the opt-in other half: "
                        "when > 0, citations in one file within that many "
                        "lines are GROUPED and a group with enough distinct "
                        "lanes becomes one finding carrying `line_to` (the "
                        "span's end). The default stays 0 deliberately \\u2014 "
                        "corroboration is a claim about agreement, and a "
                        "tolerance that shipped on would redefine what every "
                        "existing report means for callers who never asked. A "
                        "group promoted this way is NOT also listed as a near "
                        "miss.");
                    t["selection_hint"] = QStringLiteral(
                        "Use to cross-check two+ reviewers' findings "
                        "against shared evidence. Reduces false "
                        "positives in N-reviewer fan-out.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject reportsProp;
                    reportsProp["type"] = "object";
                    reportsProp["description"] = QStringLiteral(
                        "Map of {lane_name: report_markdown}. "
                        "Mutually exclusive with reports_dir.");
                    QJsonObject reportsDirProp;
                    reportsDirProp["type"] = "string";
                    reportsDirProp["description"] = QStringLiteral(
                        "Project-relative path to a directory of "
                        "*.md files — or, with "
                        "allow_outside_project:true, an absolute one "
                        "(ANTS-3713). Lane name = filename stem. "
                        "Top level only; sub-directories not "
                        "recursed. Files >64 KiB truncated. "
                        "Mutually exclusive with reports.");
                    QJsonObject minLanesProp;
                    minLanesProp["type"]    = "integer";
                    minLanesProp["default"] = 2;
                    minLanesProp["minimum"] = 1;
                    minLanesProp["description"] = QStringLiteral(
                        "Minimum distinct lanes citing a (file, line) "
                        "for it to count as corroborated.");
                    // ANTS-3713 — same opt-in name and posture as
                    // test_audit_synthesis_prompt's (ANTS-1455).
                    QJsonObject aopProp;
                    aopProp["type"] = "boolean";
                    aopProp["description"] = QStringLiteral(
                        "ANTS-3713 — when true, reports_dir may resolve "
                        "outside the project root (session scratchpad, "
                        "/tmp), so lane reports need not be written into "
                        "the working tree. Still NFC + control-char "
                        "checked and canonicalised. Default false.");
                    // ANTS-4817 — opt-in proximity tolerance.
                    QJsonObject slopProp;
                    slopProp["type"]    = "integer";
                    slopProp["default"] = 0;
                    slopProp["minimum"] = 0;
                    slopProp["description"] = QStringLiteral(
                        "Group citations in one file within this many lines "
                        "of each other, so two lanes quoting one defect at "
                        "different lines count as agreeing. A grouped finding "
                        "carries `line_to` (the span's end). Default 0 = "
                        "exact (file, line) matching, unchanged \\u2014 and it "
                        "stays the default deliberately, because a tolerance "
                        "that shipped on would change what every existing "
                        "report means for callers who never asked for it. "
                        "Measured useful at 2-3 by the two projects that "
                        "reported the gap. At 0 the same groups are still "
                        "reported, as advisory `near_misses`.");
                    QJsonObject props;
                    props["reports"]     = reportsProp;
                    props["reports_dir"] = reportsDirProp;
                    props["min_lanes"]   = minLanesProp;
                    props["line_slop"]   = slopProp;
                    props["allow_outside_project"] = aopProp;
                    // ANTS-1391 — caller_cwd anchor.
                    props["caller_cwd"]  = makeCallerCwdReadProp();
                    schema["properties"] = props;
                    // INV-1 XOR is enforced at the handler layer
                    // (cmdIndieReviewCorroborate), not the schema —
                    // JSON Schema's oneOf is verbose and Claude
                    // Code's schema validator handles oneOf poorly.
                    // The handler returns bad_args if both or
                    // neither is provided.
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-1352 — indie_review_dispatch (server-side
                // reviewer fan-out).
                {
                    QJsonObject t;
                    t["name"] = "indie_review_dispatch";
                    // The tools/list pass below appends a sentence to this description.
                    t["description"] = QStringLiteral(
                        "Server-side multi-agent independent code "
                        "review: dispatches N parallel HTTP POSTs to "
                        "the project's configured AI endpoint "
                        "(Settings → AI), one per lane from "
                        "indie_review_partition, saves each response "
                        "as <reports_dir>/<lane>.md, returns a "
                        "manifest. Required: caller_cwd, "
                        "reports_dir. Optional: lanes (subset of "
                        "partition; default all), concurrency [1-8] "
                        "default 4, max_tokens [4096-128000] default "
                        "64000, model (default = Config::aiModel(), "
                        "which is \"llama3\" out-of-box — see "
                        "docs/specs/ANTS-1352.md § 3.3), "
                        "system_extras (≤ 4 KiB append to system "
                        "prompt). See docs/specs/ANTS-1352.md.");
                    t["selection_hint"] = QStringLiteral(
                        "Use to run the review lanes on the configured "
                        "LOCAL AI endpoint instead of Claude subagents — "
                        "a weaker reviewer, chosen for cost, not parity. "
                        "For a Claude-subagent plan use "
                        "indie_review_orchestrate.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject callerProp;
                    callerProp["type"] = "string";
                    callerProp["description"] = QStringLiteral(
                        "Your $PWD. Anchors lane partition lookup "
                        "and reports_dir. caller_cwd Required "
                        "(ANTS-1404).");
                    QJsonObject rdProp;
                    rdProp["type"] = "string";
                    rdProp["description"] = QStringLiteral(
                        "Project-relative directory under caller_cwd "
                        "where <lane>.md files are written. Created "
                        "if missing. Refuses on collision (delete "
                        "stale files or pick a fresh dir).");
                    QJsonObject lanesProp;
                    lanesProp["type"] = "array";
                    lanesProp["description"] = QStringLiteral(
                        "Optional subset of lane names from "
                        "indie_review_partition. Empty = all.");
                    QJsonObject concProp;
                    concProp["type"] = "integer";
                    concProp["description"] = QStringLiteral(
                        "Max parallel in-flight requests, [1, 8]. "
                        "Default 4.");
                    QJsonObject mtProp;
                    mtProp["type"] = "integer";
                    mtProp["description"] = QStringLiteral(
                        "Per-lane response cap forwarded to upstream "
                        "API, [4096, 128000]. Default 64000.");
                    QJsonObject modelProp;
                    modelProp["type"] = "string";
                    modelProp["description"] = QStringLiteral(
                        "Model name passed verbatim to upstream. "
                        "\"auto\" or empty → Config::aiModel().");
                    QJsonObject extrasProp;
                    extrasProp["type"] = "string";
                    extrasProp["description"] = QStringLiteral(
                        "Appended to system prompt with a `---` "
                        "separator. Useful for project-specific "
                        "threat-model hints. ≤ 4 KiB.");
                    QJsonObject props;
                    props["caller_cwd"]    = callerProp;
                    props["reports_dir"]   = rdProp;
                    props["lanes"]         = lanesProp;
                    props["concurrency"]   = concProp;
                    props["max_tokens"]    = mtProp;
                    props["model"]         = modelProp;
                    props["system_extras"] = extrasProp;
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("caller_cwd");
                    req.append("reports_dir");
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-1289 — verify_changes
                {
                    QJsonObject t;
                    t["name"] = "verify_changes";
                    t["description"] = QStringLiteral(
                        "Run the project's build / test / lint gates and "
                        "return pass/fail with log tails. Reads "
                        ".ants/verify.json (or auto-detects CMakePresets/"
                        "package.json/Cargo.toml/pyproject.toml). Args: "
                        "gates (subset of build|tests|lint), max_log_lines, "
                        "timeout_sec [10,1800], force_refresh, cache_only "
                        "(read-only, bypasses the cwd gate). Results cached "
                        "5 min keyed on git state; responses carry "
                        "cache_hit. Two-tier timeouts (tool-side per-gate "
                        "vs transport ~60s); for builds >60s fall back to "
                        "Bash cmake/make. Required: caller_cwd.");
                    // ANTS-2079 — full timeout / phase-timing reference in
                    // `detail` (stripped from the tools/list wire; served
                    // by tool_info {name:"verify_changes"}).
                    t["detail"] = QStringLiteral(
                        "Run the project's build / test / lint gates "
                        "and return pass/fail with log tails. Replaces "
                        "the verification-before-completion skill's "
                        "mechanical loop. Reads .ants/verify.json (or "
                        "auto-detects from CMakePresets/package.json/"
                        "Cargo.toml/pyproject.toml). Pure shell-out. "
                        "ANTS-1359 — results are cached in-process for "
                        "up to 5 minutes keyed on git state + options; "
                        "responses carry cache_hit. Pass force_refresh "
                        "to bypass; pass cache_only to probe without "
                        "running. Required: caller_cwd (string — your "
                        "$PWD; ANTS-1372 cross-project gate). ANTS-1497 "
                        "(also ANTS-1621): `cache_only:true` is a "
                        "read-only call that **bypasses** the cross-"
                        "project cwd gate, so a session in project B "
                        "can probe its own cache while Ants happens to "
                        "focus a different project's tab. Full runs "
                        "(non-cache-only) still require the focused-tab "
                        "match; for those, fall back to Bash → "
                        "`cmake/make` when Ants is focused elsewhere. "
                        "**Timeouts are two-tier (ANTS-1525/1579):** "
                        "the **tool-side** per-gate budget kills the "
                        "gate process and surfaces a structured "
                        "envelope (`tool_timed_out:true` + "
                        "`timed_out_gate` + `per_gate_timeout_sec` + "
                        "`timeout_hint`). The **transport-side** cap "
                        "is the MCP client's request timer (Claude "
                        "Code's is ~60 s) — when that fires, it "
                        "closes the socket before any envelope can "
                        "arrive and the caller sees `MCP error "
                        "-32000: transport: timed out` *outside* the "
                        "response. For builds > 60 s, fall back to "
                        "`Bash → cmake/make` rather than raising "
                        "`timeout_sec`; this tool's [10, 1800] clamp "
                        "is independent of the transport cap. "
                        "**Phase timing (ANTS-1628):** every successful "
                        "envelope carries `wall_clock_ms`, `pre_gate_ms`, "
                        "and `gate_ms`. A large `pre_gate_ms` with a "
                        "tiny `gate_ms` means the wrapper work (config "
                        "load, cache lookup, trust probe) is consuming "
                        "the transport budget before the build starts — "
                        "an actionable signal distinct from a long build.");
                    t["selection_hint"] = QStringLiteral(
                        "Use after Edit/Write to verify build / "
                        "tests / lint gates still pass. Cheap "
                        "regression-prevention step; 5-min cache.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject gatesProp;
                    gatesProp["type"] = "array";
                    gatesProp["description"] = QStringLiteral(
                        "Subset of [\"build\",\"tests\",\"lint\"] to "
                        "run. Empty / omitted = all configured gates.");
                    QJsonObject linesProp;
                    linesProp["type"] = "integer";
                    linesProp["description"] = QStringLiteral(
                        "Per-gate log-tail line cap. Server-clamped "
                        "[10, 500]; default 50.");
                    QJsonObject timeoutProp;
                    timeoutProp["type"] = "integer";
                    timeoutProp["description"] = QStringLiteral(
                        "Total wall-clock budget in seconds, split "
                        "evenly across the gates that will actually "
                        "run (post-`gates` filter, ANTS-1492). Server-"
                        "clamped [10, 1800]; default 1200. Caveat: "
                        "most MCP clients (incl. Claude Code) apply "
                        "their own request-level transport timeout "
                        "independent of this budget — if your build "
                        "exceeds ~60 s and you see `MCP error -32000: "
                        "transport: timed out`, that is the client "
                        "closing the connection, not the tool. Fall "
                        "back to running the same command via Bash "
                        "for long builds.");
                    // ANTS-1389 — caller_cwd schema surfacing.
                    QJsonObject callerProp;
                    callerProp["type"] = "string";
                    callerProp["description"] = QStringLiteral(
                        "Your $PWD. Mutating verbs refuse on mismatch "
                        "with the focused tab's cwd (ANTS-1372).");
                    // ANTS-1359 — cache-control args.
                    QJsonObject forceProp;
                    forceProp["type"] = "boolean";
                    forceProp["description"] = QStringLiteral(
                        "Bypass the in-process build-cache (ANTS-1359) "
                        "and force a fresh run. Useful after a system "
                        "update or any change to a `.gitignore`d build "
                        "input. Default false.");
                    QJsonObject probeProp;
                    probeProp["type"] = "boolean";
                    probeProp["description"] = QStringLiteral(
                        "Return the cached response if present; else "
                        "return {ok:true, cache_miss:true} without "
                        "running gates. Default false. Mutually "
                        "exclusive with force_refresh. ANTS-1497: "
                        "treated as a read-only call — the ANTS-1372 "
                        "focused-tab cwd gate is bypassed so a session "
                        "in project B can probe its own cache while "
                        "Ants happens to focus a different tab.");
                    QJsonObject props;
                    props["gates"]          = gatesProp;
                    props["max_log_lines"]  = linesProp;
                    props["timeout_sec"]    = timeoutProp;
                    props["caller_cwd"]     = callerProp;
                    props["force_refresh"]  = forceProp;
                    props["cache_only"]     = probeProp;
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("caller_cwd");
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-1351 — audit_run (server-side audit runner v1).
                {
                    QJsonObject t;
                    t["name"] = "audit_run";
                    // The tool names come from AuditRunner's own lists, so
                    // this text cannot drift from what the runner runs.
                    const QStringList &known = AuditRunner::kKnownTools();
                    t["description"] = QStringLiteral(
                        "Run the project's external audit tools (")
                        + known.join(QStringLiteral(", ")) + QStringLiteral(") "
                        "server-side and return a structured envelope + SARIF path. "
                        "Args: tools (auto-detect when empty), scope (auto / files / "
                        "since-tag:X / branch-diff / full), cap_per_tool_seconds "
                        "(default 30, 5-300), top_findings_count. Read partial, "
                        "incomplete_tools, paths_given_total and tools_with_no_files "
                        "before trusting a zero. async:true returns a job for "
                        "audit_poll; a long sync run can outlive the transport, then "
                        "read last_audit_summary. caller_cwd Required.");
                    t["detail"] = QStringLiteral(
                        "Run the project's external audit tools (")
                        + known.join(QLatin1Char('/')) + QStringLiteral(") "
                        "server-side and return "
                        "a structured envelope + SARIF path instead of "
                        "shipping each tool's raw output through parent "
                        "context. v1 ships infrastructure: tools run "
                        "with scrubbed env, absolute-path resolution, "
                        "per-tool wall-clock cap (default 30 s, [5, 300]; "
                        "ANTS-3585 raised the ceiling for big C/C++ sweeps — "
                        "a high cap is meant for the async path), "
                        "aggregate cap min(N*cap*1.5, 900 s). Caller "
                        "supplies tools list (auto-detect when empty), "
                        "scope (auto / files / since-tag:X / branch-diff), "
                        "and optional top_findings_count for inline "
                        "result preview. Required: caller_cwd (ANTS-1404 "
                        "Required gate). ANTS-1555: SARIF now lands in "
                        "`<root>/.audit_cache/audit-<iso>-<sha>.sarif` "
                        "(same dir AuditDialog GUI writes to), and the "
                        "envelope carries `cache_path` + `prior_run` "
                        "{iso_timestamp, commit, sarif} for the prior "
                        "sweep — anchor for ANTS-1504 since-last-run "
                        "mode. /tmp fallback when the root is read-only. "
                        "ANTS-2032: the envelope carries `partial` (true "
                        "when a tool timed out / crashed but the rest of "
                        "the run still produced results + the SARIF "
                        "artifact) and `incomplete_tools[]` naming the "
                        "offenders, so a single tool blowing its cap never "
                        "yields an all-or-nothing empty result. "
                        "ANTS-4371: every zero ships with its denominator — "
                        "per tool `paths_given` + `scanned_whole_project` (and "
                        "`no_files:true` when a NARROWED scope matched "
                        "nothing), plus top-level `paths_given_total` / "
                        "`scanned_whole_project` / `tools_with_no_files[]`. "
                        "Read them before treating total_raw:0 as clean: a "
                        "zero-finding audit is the most consequential result "
                        "this verb returns. They are deliberately NOT folded "
                        "into `partial` / `incomplete_tools` — a narrowed "
                        "scope that legitimately matched nothing is not a "
                        "failed run. "
                        "ANTS-3585: `incomplete_tools_detail[]` says WHY each "
                        "is incomplete ({tool, status, elapsed_ms, truncated}: "
                        "truncated=true ⇒ cut off at the cap, false ⇒ crashed) "
                        "so a caller need not scan by_tool[]; and "
                        "`parse_failures[]` lists source files a tool (cppcheck) "
                        "could not PARSE — a C++23 TU its frontend chokes on "
                        "gets ZERO coverage and would otherwise be silently "
                        "absent from the findings. "
                        "ANTS-3706: `parse_failures_detail[]` carries the same "
                        "set as {file, tool, reason} where reason is the "
                        "check-id + first diagnostic — so a missing include "
                        "path (fixable config) is distinguishable from a "
                        "frontend limitation (route around) without re-running "
                        "the tool by hand. `parse_failures[]` keeps its "
                        "bare-path shape. "
                        "ANTS-2183 — **long sweeps & the transport cap:** a "
                        "full-tree (scope:\"full\") run can take minutes and "
                        "exceed the MCP client's request timer (Claude "
                        "Code's is ~60 s), surfacing `MCP error -32000: "
                        "transport: timed out` *outside* the response. The "
                        "run is NOT lost — it completes server-side and "
                        "writes its SARIF; read the result back via "
                        "`last_audit_summary` (or this envelope's "
                        "`cache_path`). To stay under the cap, narrow with "
                        "`scope` / `tools` / a smaller `cap_per_tool_seconds`; "
                        "otherwise expect the `last_audit_summary` fallback "
                        "for full sweeps. ANTS-3396 — for a slow sweep, pass "
                        "`async:true` to get a job handle back instantly and "
                        "poll `audit_poll {job_id}`; the async path never "
                        "trips the transport cap at all. "
                        "See docs/specs/ANTS-1351.md + ANTS-1555.md + "
                        "ANTS-3396.md.");
                    t["selection_hint"] = QStringLiteral(
                        "Use to run the static-analysis sweep without "
                        "leaving Ants. Pairs with last_audit_summary "
                        "for the compact read afterwards.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject toolsProp;
                    toolsProp["type"] = "array";
                    // ANTS-3622 — auto-detect drops only mypy, so a
                    // default sweep includes clang-tidy.
                    toolsProp["description"] = QStringLiteral("Subset of {")
                        + known.join(QStringLiteral(", "))
                        + QStringLiteral("} (%1 known). Empty / omitted = auto-detect "
                        "every runnable tool EXCEPT mypy — %2 tools, "
                        "clang-tidy included. mypy runs deps-less here so "
                        "a full-sweep mypy is import-not-found noise — CI "
                        "/ pre-commit run the real deps-installed mypy "
                        "(ANTS-3418); request it explicitly to run it. "
                        "clang-tidy resolves its compile DB automatically "
                        "(ANTS-2182); pair it with `paths` + `checks` for "
                        "a scoped sweep (ANTS-1512).")
                        .arg(known.size()).arg(AuditRunner::kAutoDetectTools().size());
                    QJsonObject scopeProp;
                    scopeProp["type"] = "string";
                    scopeProp["description"] = QStringLiteral(
                        "Audit scope. \"auto\" (default) = full-tree "
                        "minus exclusions (some tools degrade to "
                        "changed-since-fork-point on a clean working "
                        "tree — ANTS-1456 — so a no-diff tree can read "
                        "as total_raw:0 \"clean\" when the truth is "
                        "\"nothing to audit\"). The narrowing scopes "
                        "(ANTS-1504) run each file-oriented tool against "
                        "ONLY the changed files; gitleaks/trivy skip "
                        "(reason not_file_scoped) and a tool with no "
                        "matching changed file skips "
                        "(no_changed_files_for_languages): "
                        "\"since-last-run\" = changed since the prior "
                        "cached run's commit + working tree (demotes to "
                        "a full scan with scope_demoted:\"full\" when "
                        "the prior commit is absent/unreachable); "
                        "\"files\" = changed since the merge-base with "
                        "origin/<default-branch> + working tree; "
                        "\"since-tag:<tag>\" = git diff <tag>..HEAD + "
                        "working tree (tag sanitised, INV-15); "
                        "\"branch-diff\" = git diff main..HEAD. The "
                        "envelope adds scope_resolved / "
                        "changed_files_count / scope_anchor_commit (+ "
                        "no_changes when nothing changed). Under "
                        "\"since-last-run\" it also returns a precise "
                        "delta:{added,removed,added_count,removed_count,"
                        "carried_forward_count} vs the prior run's findings "
                        "(or delta_unavailable_reason when no readable, "
                        "untruncated baseline exists) — ANTS-1870. For a "
                        "deterministic full sweep use \"full\" "
                        "(ANTS-2015): the whole tracked src/ tree, git-"
                        "diff-independent, so clazy/clang-tidy get real "
                        "source positionals instead of the empty list "
                        "\"auto\" hands them on a clean tree. No tool is "
                        "skipped under \"full\".");
                    QJsonObject capProp;
                    capProp["type"] = "integer";
                    capProp["description"] = QStringLiteral(
                        "Per-tool wall-clock cap in seconds, [5, 300]. "
                        "Caps above ~60 s are meant for the async path "
                        "(async:true), which survives the transport timeout. "
                        "Default 30. Out-of-range → bad_args.");
                    QJsonObject suppProp;
                    suppProp["type"] = "string";
                    // ANTS-3615 — describes what the headless engine actually
                    // does. `.audit_suppress` is GUI-only (line-grain dedup
                    // keys the runner never materialises); the headless
                    // sources are the learned-FP ledger + the allowlist.
                    suppProp["description"] = QStringLiteral(
                        "\"auto\" (default) applies the learned-FP ledger "
                        "(.audit_cache/learned-fp.jsonl) + "
                        ".audit_allowlist.json; \"none\" applies neither; "
                        "\"path:<file>\" reads the allowlist from the named "
                        "file instead (must resolve under the project root, "
                        "else bad_path). Any other value → bad_args.");
                    QJsonObject formatsProp;
                    formatsProp["type"] = "array";
                    formatsProp["description"] = QStringLiteral(
                        "Subset of [\"sarif\",\"html\"]. Default "
                        "[\"sarif\"]; HTML opt-in.");
                    QJsonObject topProp;
                    topProp["type"] = "integer";
                    topProp["description"] = QStringLiteral(
                        "When > 0, inline first N findings into envelope "
                        "as top_findings[] (avoids the SARIF-Read round "
                        "trip). [0, 100]; out-of-range → bad_args.");
                    // ANTS-1512 — scoped-check mode.
                    QJsonObject pathsProp;
                    pathsProp["type"] = "array";
                    // ANTS-3622 — second stale string in this schema. It
                    // named cppcheck + clang-tidy only; eight of the ten
                    // tools append the scoped paths (verified against the
                    // toolArgv branches). Under-claiming here is not
                    // harmless: a caller who believes narrowing does not
                    // reach ruff/bandit/semgrep will avoid `paths` and pay
                    // for a full sweep it did not need.
                    pathsProp["description"] = QStringLiteral(
                        "Project-relative paths constraining the "
                        "tool's invocation (e.g. [\"menu/drivers/\", "
                        "\"gfx/\"]). Honoured as positional args by "
                        "every file-scoped tool: cppcheck, clazy, "
                        "clang-tidy, ruff, bandit, semgrep, shellcheck, "
                        "mypy. gitleaks and trivy are repo-global and "
                        "ignore them (they skip entirely under a "
                        "narrowing scope — not_file_scoped). Each "
                        "entry is sanitised through isAuditArgSafe; "
                        "any failing entry rejects the whole call "
                        "with code:\"bad_args\".");
                    // ANTS-3710 — the negative counterpart to `paths`.
                    // Narrowing is not excluding: `paths` also drops the
                    // repo-global tools and the project's own sibling lanes.
                    QJsonObject exclProp;
                    exclProp["type"] = "array";
                    exclProp["description"] = QStringLiteral(
                        "Project-relative path prefixes to EXCLUDE (e.g. "
                        "[\"mingw-deps\", \"third_party\"]) — for code that "
                        "is present but not yours: a vendored dependency "
                        "tree, a staged cross-build SDK, a single-header "
                        "library. Unlike `paths` (which narrows, and so also "
                        "skips gitleaks/trivy and your own sibling lanes), "
                        "this keeps the sweep whole and removes only the "
                        "named subtrees. Applied two ways: every positional "
                        "file list is prefix-filtered at a path-segment "
                        "boundary, and each tool with a native exclusion "
                        "flag gets them on its whole-tree run (cppcheck -i, "
                        "ruff --extend-exclude, bandit -x, semgrep "
                        "--exclude, trivy --skip-dirs, mypy --exclude). "
                        "gitleaks, clazy, clang-tidy and shellcheck have "
                        "neither and CANNOT honour it — when any of them ran, "
                        "the envelope's `exclude_paths_ignored_by` names "
                        "them, alongside `exclude_paths_applied`. Each entry "
                        "is sanitised through isAuditArgSafe; any failing "
                        "entry rejects the whole call with "
                        "code:\"bad_args\".");
                    QJsonObject checksProp;
                    checksProp["type"] = "array";
                    checksProp["description"] = QStringLiteral(
                        "Check IDs constraining the tool's enabled "
                        "rule set (e.g. [\"bugprone-integer-division\"]). "
                        "Currently honoured by clang-tidy only "
                        "(rendered as `--checks=-*,<joined>`); "
                        "other tools refuse with code:\"bad_args\" "
                        "rather than silently ignore. Each entry "
                        "must match ^-?[A-Za-z0-9_*.,-]+$ "
                        "(length ≤ 128).");
                    // ANTS-3396 — opt-in async mode.
                    QJsonObject asyncProp;
                    asyncProp["type"] = "boolean";
                    asyncProp["description"] = QStringLiteral(
                        "When true, start the sweep detached and return a "
                        "job handle immediately ({async:true, job_id, "
                        "status:\"running\", poll_with:\"audit_poll\"}) "
                        "instead of blocking until it finishes. Poll "
                        "audit_poll {job_id} for completion. Default "
                        "false (synchronous, unchanged). Use it for "
                        "scope:\"full\" or slow tool sets (cold semgrep / "
                        "trivy / mypy) that can exceed the MCP client's "
                        "~60 s request timer — the async path never trips "
                        "the `transport: timed out` cap. Results are "
                        "written to .audit_cache regardless.");
                    QJsonObject callerProp;
                    callerProp["type"] = "string";
                    callerProp["description"] = QStringLiteral(
                        "Your $PWD. Mutating verbs refuse on mismatch "
                        "with the focused tab's cwd (ANTS-1372).");
                    QJsonObject props;
                    props["tools"]                = toolsProp;
                    props["scope"]                = scopeProp;
                    props["cap_per_tool_seconds"] = capProp;
                    props["suppressions"]         = suppProp;
                    props["formats"]              = formatsProp;
                    props["top_findings_count"]   = topProp;
                    props["paths"]                = pathsProp;
                    props["exclude_paths"]        = exclProp;   // ANTS-3710
                    props["checks"]               = checksProp;
                    props["async"]                = asyncProp;
                    props["caller_cwd"]           = callerProp;
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("caller_cwd");
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-3396 — audit_poll (async audit_run companion).
                {
                    QJsonObject t;
                    t["name"] = "audit_poll";
                    t["description"] = QStringLiteral(
                        "Poll an async audit_run job by its job_id (from an "
                        "audit_run {async:true} call). Read-only, cheap, "
                        "never blocks. Branch on `status`, not `ok` (ok "
                        "reports whether the POLL succeeded): "
                        "\"running\" (with elapsed_ms), \"done\" (with "
                        "cache_path + total_raw/total_actionable/partial/"
                        "incomplete_tools (+ incomplete_tools_detail/"
                        "parse_failures, ANTS-3585); no_changes on an empty "
                        "changeset; "
                        "read_full_with:last_audit_summary), \"error\" (the "
                        "run's own code/error), or \"expired\" (job_id "
                        "unknown or evicted from the bounded registry — the "
                        "result is still on disk; read last_audit_summary). "
                        "Prefer the done envelope's cache_path over "
                        "last_audit_summary for this job's exact SARIF. "
                        "Required: caller_cwd. See docs/specs/ANTS-3396.md.");
                    t["selection_hint"] = QStringLiteral(
                        "Use after audit_run {async:true} to check whether "
                        "the detached sweep has finished.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject jobProp;
                    jobProp["type"] = "string";
                    jobProp["description"] = QStringLiteral(
                        "The job_id returned by audit_run {async:true} "
                        "(e.g. \"audit-7\"). Missing / non-string → "
                        "bad_args.");
                    QJsonObject callerProp;
                    callerProp["type"] = "string";
                    callerProp["description"] = QStringLiteral(
                        "Your $PWD. Required (ANTS-1404 parity with "
                        "audit_run).");
                    QJsonObject props;
                    props["job_id"]     = jobProp;
                    props["caller_cwd"] = callerProp;
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("caller_cwd");
                    req.append("job_id");
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-1397 — test_audit_partition (v1 trio start)
                {
                    QJsonObject t;
                    t["name"] = "test_audit_partition";
                    t["description"] = QStringLiteral(
                        "Phase 1 of the test_audit trio: detect the test "
                        "framework, walk test files, pack into chunks "
                        "(size 12, [4,30]), run a pre-pass regex scan, "
                        "return paginated chunks + partition_token. "
                        "dimensions: auto (default) | csv:<d1,d2>. scope: "
                        "auto | path:<sub> | files:<csv>. Seed the subagent "
                        "with envelope-level dimensions_active[] "
                        "(auto omits 5 style dimensions — opt in via csv:; "
                        "dimensions_skipped[] says which and why). Chunks "
                        "are capped by chunk_byte_budget as well as "
                        "chunk_size. "
                        "(pre_pass_dimensions[] is a prioritisation hint, "
                        "not a coverage allow-list). Required: caller_cwd. "
                        "Pairs with test_audit_brief.");
                    // ANTS-2079 — full pagination / resume / polyglot
                    // reference in `detail` (stripped from the tools/list
                    // wire; served by tool_info
                    // {name:"test_audit_partition"}).
                    t["detail"] = QStringLiteral(
                        "Phase 1 of the test_audit trio. Detect "
                        "test framework, walk test files, pack into "
                        "chunks (size 12 default, [4,30]), run "
                        "pre-pass regex scan, return paginated chunks "
                        "+ partition_token. v1 uses a hardcoded "
                        "pre-pass pattern set; v2 ships a project "
                        "JSON resource. dimensions=\"auto\" (default) "
                        "or \"csv:<d1,d2>\". scope=\"auto\"/\"path:<sub>\"/"
                        "\"files:<csv>\". Required: caller_cwd. "
                        "Per-chunk `pre_pass_dimensions[]` (ANTS-1487 "
                        "renamed from dimension_hints; ANTS-1461) "
                        "reflects which audit dimensions the pre-pass "
                        "regex matched on chunk source — it is NOT a "
                        "finding-density predictor. Real finding "
                        "distribution is only known after the "
                        "subagent reviews the chunk. Always seed the "
                        "subagent with the envelope-level "
                        "`dimensions_active[]` (full lane list); "
                        "`pre_pass_dimensions[]` is a prioritisation "
                        "hint, not a coverage allow-list. Envelope "
                        "also carries `pre_pass_chunk_ids[]` "
                        "(ANTS-1489) — the subset of chunk IDs the "
                        "pre-pass found anything on, so callers can "
                        "skip per-chunk brief() calls for empties. "
                        "**ANTS-2070:** the inlined "
                        "`pre_pass_findings_by_chunk` map is omitted from "
                        "the envelope when it would exceed ~24 KiB (a large "
                        "suite would otherwise overflow the tool-result "
                        "cap); the response then sets `pre_pass_omitted:true` "
                        "+ `pre_pass_omitted_bytes` and `pre_pass_cached:"
                        "true`, and the caller fetches each chunk's findings "
                        "via `test_audit_brief` (guided by "
                        "`pre_pass_chunk_ids[]`). "
                        "**ANTS-1623 polyglot:** when scope is "
                        "`path:<sub>`, the framework probe runs "
                        "inside `<sub>` FIRST (then falls back to "
                        "root) so a pytest-backend + vitest-frontend "
                        "project routes correctly to the scoped tree "
                        "instead of pinning to the root's first-"
                        "match. The envelope also carries "
                        "`additional_frameworks[]` — "
                        "`[{name, root_rel}]` pairs listing every "
                        "other framework whose signal file was "
                        "detected at the project root OR a top-level "
                        "subdir but was not picked as the primary. "
                        "Empty on single-framework projects (the "
                        "common case). Caller can fan out a second "
                        "partition call with `scope: \"path:<root_rel>\"` "
                        "to cover each extra tree. "
                        "**ANTS-1580 resume:** the returned "
                        "`partition_token` is an in-process LRU handle "
                        "(not durable on disk and not auto-saved into "
                        "session_memory). To survive a session bounce, "
                        "store it explicitly via "
                        "`session_memory(op:\"set\", "
                        "key:\"test_audit_partition_token.<scope_id>\", "
                        "value:{token, scope, dimensions, saved_at_ms})` "
                        "right after partition; resume with a matching "
                        "`op:\"get\"` and fall back to re-running this "
                        "verb if `test_audit_brief` refuses with "
                        "`partition_token not found`. Full recipe at "
                        "docs/standards/test-audit-resume.md. "
                        "See docs/specs/ANTS-1397.md.");
                    t["selection_hint"] = QStringLiteral(
                        "Use to split a test corpus for multi-"
                        "reviewer test-audit dispatch. Phase 1 of "
                        "the trio; pairs with test_audit_brief.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject scopeProp; scopeProp["type"] = "string";
                    QJsonObject dimensionsProp; dimensionsProp["type"] = "string";
                    // ANTS-1519 — chunk_size default + bounds surfaced on
                    // the property itself so a session reading the JSON
                    // schema (not the prose description) still knows the
                    // default. Server-side clamp [4, 30] lives in
                    // testauditengine.cpp; default 12 lives in
                    // testauditengine.h PartitionRequest::chunkSize.
                    QJsonObject chunkSizeProp;
                    chunkSizeProp["type"] = "integer";
                    chunkSizeProp["default"] = 12;
                    chunkSizeProp["minimum"] = 4;
                    chunkSizeProp["maximum"] = 30;
                    chunkSizeProp["description"] = QStringLiteral(
                        "Files per chunk. Default 12; clamped to [4, 30] "
                        "server-side. Override via "
                        "<projectPath>/.test-audit/partition.json.");
                    // ANTS-3630 — the `quick` property is gone. It was
                    // accepted, forwarded and never read, so `quick:true`
                    // ran an identical full walk + pre-pass. Its schema
                    // text ("skip pre-pass regex scan") also contradicted
                    // the /test-audit skill, where the grep pre-pass IS
                    // the whole audit under --quick and only the subagent
                    // phase is skipped — which is caller-side. With
                    // additionalProperties:false a stale caller now gets a
                    // loud schema refusal instead of a silent no-op.
                    QJsonObject offsetProp;
                    offsetProp["type"] = "integer";
                    offsetProp["default"] = 0;
                    offsetProp["minimum"] = 0;
                    offsetProp["description"] = QStringLiteral(
                        "0-based start index into chunks[]. Default 0.");
                    QJsonObject limitProp;
                    limitProp["type"] = "integer";
                    limitProp["default"] = -1;
                    limitProp["description"] = QStringLiteral(
                        "Cap on chunks[] length. Default -1 (no caller "
                        "limit; server may still auto-truncate large "
                        "envelopes).");
                    QJsonObject callerCwdProp; callerCwdProp["type"] = "string";
                    QJsonObject props;
                    props["scope"] = scopeProp; props["dimensions"] = dimensionsProp;
                    props["chunk_size"] = chunkSizeProp;
                    props["offset"] = offsetProp; props["limit"] = limitProp;
                    props["caller_cwd"] = callerCwdProp;
                    schema["properties"] = props;
                    QJsonArray req; req.append("caller_cwd");
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-1397 — test_audit_brief
                {
                    QJsonObject t;
                    t["name"] = "test_audit_brief";
                    t["description"] = QStringLiteral(
                        "Phase 2 of the test_audit trio. Return "
                        "structured per-chunk manifest "
                        "(source_paths, dimensions, framework_context, "
                        "pre_pass_findings) — NO `brief` string field "
                        "(caller composes the subagent prompt from "
                        "structured siblings). Requires "
                        "partition_token from a prior partition call. "
                        "**Rate limit (ANTS-1643):** BriefAssembly "
                        "tier — 30 calls / 60 s per (tool, "
                        "caller_cwd). A canonical `/test-audit` "
                        "Phase-2 fan-out dispatches 12-16 briefs in "
                        "one parallel batch, which fits comfortably "
                        "under this cap. Higher than the 10/min "
                        "Expensive tier the sibling `test_audit_*` "
                        "verbs sit in.");
                    t["selection_hint"] = QStringLiteral(
                        "Use to assemble the brief for one "
                        "test-audit chunk. Phase 2 of the trio; "
                        "run after test_audit_partition.");
                    QJsonObject schema; schema["type"] = "object";
                    QJsonObject chunkIdProp; chunkIdProp["type"] = "string";
                    QJsonObject partitionTokenProp; partitionTokenProp["type"] = "string";
                    QJsonObject callerCwdProp; callerCwdProp["type"] = "string";
                    QJsonObject props;
                    props["chunk_id"] = chunkIdProp;
                    props["partition_token"] = partitionTokenProp;
                    props["caller_cwd"] = callerCwdProp;
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("caller_cwd");
                    req.append("chunk_id");
                    req.append("partition_token");
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-1284 — token_usage
                {
                    QJsonObject t;
                    t["name"] = "token_usage";
                    t["description"] = QStringLiteral(
                        "Reports per-tool token-saving telemetry for "
                        "this MCP server's current session. Returns "
                        "{since, calls:[...], total_saved, "
                        "total_wrap_bytes, tools_called}; per-tool "
                        "entry adds {wrap_bytes, duration_us_min/"
                        "max/mean}. Sorted by est_tokens_saved "
                        "descending. `mcpd` counts calls served by "
                        "ants-mcpd processes (ANTS-5311). Pure read "
                        "by default; pass reset:true to read-and-clear "
                        "in one round-trip. No required args.");
                    t["selection_hint"] = QStringLiteral(
                        "Use to inspect this session's MCP-call "
                        "cost. Control-plane (no caller_cwd); "
                        "read-only by default.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject resetProp;
                    resetProp["type"] = "boolean";
                    resetProp["description"] = QStringLiteral(
                        "If true, clears all counters AFTER building "
                        "the report. Default false.");
                    QJsonObject zeroProp;
                    zeroProp["type"] = "boolean";
                    zeroProp["description"] = QStringLiteral(
                        "If true, include tools with "
                        "est_tokens_saved == 0 in calls[]. Default "
                        "false.");
                    QJsonObject props;
                    props["reset"]        = resetProp;
                    props["include_zero"] = zeroProp;
                    schema["properties"] = props;
                    schema["required"] = QJsonArray();
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-1360 — mcp_trace: ring-buffer slice of last
                // tools/call dispatches. Privacy-first: shape +
                // lengths + hashes only, no raw arg values.
                {
                    QJsonObject t;
                    t["name"] = "mcp_trace";
                    t["description"] = QStringLiteral(
                        "Return the last N tool/call records observed "
                        "by this Ants Terminal MCP server. Useful for "
                        "debugging third-party MCP integrations. "
                        "Records are structured (shape + lengths + "
                        "hashes only) — raw argument values are never "
                        "stored. ANTS-1360.");
                    t["selection_hint"] = QStringLiteral(
                        "Use when debugging MCP tool behaviour. "
                        "Returns recent dispatcher trace events; "
                        "read-only ring-buffer.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject sinceProp;
                    sinceProp["type"] = "integer";
                    sinceProp["minimum"] = 0;
                    sinceProp["description"] = QStringLiteral(
                        "Return only records with id >= since "
                        "(inclusive). Use 0 to start from the oldest "
                        "record currently in the ring.");
                    QJsonObject limitProp;
                    limitProp["type"] = "integer";
                    limitProp["minimum"] = 1;
                    limitProp["maximum"] = 200;
                    limitProp["default"] = 50;
                    limitProp["description"] = QStringLiteral(
                        "Maximum records returned. Hard cap = ring "
                        "cap (200). Out-of-range values are clamped.");
                    QJsonObject props;
                    props["since"] = sinceProp;
                    props["limit"] = limitProp;
                    schema["properties"] = props;
                    schema["required"] = QJsonArray();
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-1319 — cold_eyes_*. Pure delegation to
                // ColdEyesEngine.
                {
                    QJsonObject t;
                    t["name"] = "cold_eyes_partition";
                    t["description"] = QStringLiteral(
                        "Return the doc-lane partition for a cold-eyes review: walks "
                        "docs/ + root contracts and groups them by topic (contracts / "
                        "standards / decisions / plugins / per-spec). An override at "
                        "<project>/.cold-eyes/partition.json replaces the default; a "
                        "malformed one falls back and reports `override_warning`. "
                        "Optional: scope (\"default\" / \"docs_only\" / \"contracts_only\").");
                    t["detail"] = QStringLiteral(
                        "Return the doc-lane partition for /cold-eyes. "
                        "Walks docs/ + root contracts, groups by topic "
                        "cohesion (contracts / standards / decisions / "
                        "plugins / per-spec). Spec lanes (ANTS-1411): "
                        "every `*.md` under `docs/specs/` is a "
                        "candidate; `ANTS-NNNN.md` files are gated on "
                        "ROADMAP active set (legacy filter), other "
                        "filename shapes (e.g. `DS01.md`, `P04.md`) "
                        "are unconditionally surfaced. Spec lanes "
                        "capped at 12 (most-recently-modified). "
                        "Override (ANTS-1412): "
                        "`<projectPath>/.cold-eyes/partition.json` with "
                        "shape `{\"version\":1,\"lanes\":[{\"name\":"
                        "\"<id>\",\"summary\":\"<text>\",\"doc_paths\":"
                        "[\"<project-rel>\",...]}]}`. Each doc_path "
                        "must resolve inside the project root; "
                        "absolute paths and symlink escapes are "
                        "dropped per INV-13. Malformed overrides "
                        "fall back to the default partition and "
                        "surface the cause in `override_warning`. "
                        "Optional: scope (\"default\" / \"docs_only\" "
                        "/ \"contracts_only\").");
                    t["selection_hint"] = QStringLiteral(
                        "Use to split docs/specs for multi-reviewer "
                        "cold-eyes dispatch. Pairs with "
                        "cold_eyes_brief.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject scopeProp;
                    scopeProp["type"] = "string";
                    QJsonArray scopeEnum;
                    scopeEnum.append("default");
                    scopeEnum.append("docs_only");
                    scopeEnum.append("contracts_only");
                    scopeProp["enum"] = scopeEnum;
                    scopeProp["default"] = "default";
                    scopeProp["description"] = QStringLiteral(
                        "Partition scope. \"default\" emits the full "
                        "lane set; \"docs_only\" omits the root "
                        "contract lane; \"contracts_only\" emits ONLY "
                        "the contract lane.");
                    QJsonObject props;
                    props["scope"] = scopeProp;
                    // ANTS-1391 — caller_cwd anchor.
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    schema["properties"] = props;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                {
                    QJsonObject t;
                    t["name"] = "cold_eyes_brief";
                    t["description"] = QStringLiteral(
                        "Return a brief manifest for one doc-review lane: brief text, "
                        "doc_paths, cross_reference_docs, cited_code_paths, "
                        "cited_code_regions, stale_citations, doc_integrity, input_hash "
                        "and section_index. Doc bodies are not inlined. input_hash lets "
                        "a caller skip a lane whose input is unchanged. Required: lane. "
                        "Optional doc_paths[] builds the brief from those "
                        "project-relative paths; optional prior_loop_fixes[] renders a "
                        "previously-fixed block. Rate limit: BriefAssembly tier, 30 "
                        "calls / 60 s per (tool, caller_cwd).");
                    t["detail"] = QStringLiteral(
                        "Return a brief manifest for one lane: brief "
                        "text + doc_paths + cross_reference_docs + "
                        "cited_code_paths + stale_citations + "
                        "doc_integrity + input_hash + section_index. "
                        "ANTS-3740: `section_index` is a per-doc map of the "
                        "lane's OWN docs — one entry per ATX heading with "
                        "{heading, slug, level, start_line, end_line}. Cite "
                        "findings against a section anchor rather than a line "
                        "number (an anchor survives an edit above it), and "
                        "fetch any listed section with `read_region "
                        "section=<slug>` — the slug is that verb's own key. "
                        "Capped per doc; `truncated:true` marks a doc that hit "
                        "the cap. ANTS-3718: "
                        "`input_hash` is a SHA-256 over this lane's FULL "
                        "review input — the assembled brief, the lane's "
                        "docs, the small cross-reference contracts, and "
                        "every resolved cited code file. Cache it per "
                        "lane and skip re-reviewing a lane whose hash is "
                        "unchanged since the loop it last passed clean; "
                        "the server hashes the bytes it already resolves, "
                        "so the caller never reads the input to hash it. "
                        "The large append-only logs "
                        "(large_cross_reference_docs) are excluded — they "
                        "are searched, not read, and would bust every "
                        "lane on every commit. Doc "
                        "bodies are NOT inlined (ANTS-1319 INV-3, "
                        "mirrors ANTS-1281); the subagent reads each "
                        "doc via its Read tool. The `cited_code_paths` "
                        "regex (ANTS-1633) covers `src/<path>.{h,cpp}` "
                        "(Ants-shaped) plus language-agnostic "
                        "`<path>:<line>` citations (.c/.cpp/.h/.hpp/"
                        ".py/.ts/.tsx/.js/.jsx/.go/.rs/.lua/.java/.kt/"
                        ".swift/.m/.mm/.sh) — works for non-Ants doc "
                        "lanes supplied via `doc_paths[]`. Paths the "
                        "regex matched but the filesystem could not "
                        "resolve under projectPath surface as "
                        "`stale_citations[]` — per-lane reviewers treat "
                        "non-empty entries as accuracy-dimension "
                        "findings (cited file moved/deleted). "
                        "ANTS-3522: `cited_code_regions[]` ({path, "
                        "lines[]}) carries the exact lines from "
                        "`<path>:<line>` citations so a reviewer reads a "
                        "window around each (outline + read_region) "
                        "instead of the whole file — additive alongside "
                        "cited_code_paths; empty when no line citation "
                        "resolved. "
                        "ANTS-3526: `large_cross_reference_docs[]` is the "
                        "subset of cross_reference_docs that are large "
                        "append-only logs (ROADMAP.md / CHANGELOG.md, "
                        ">100 KB); the brief routes them to a \"SEARCH, do "
                        "NOT full-read\" section so a drift-check greps the "
                        "log for the specific ID/feature/version instead of "
                        "reading tens of thousands of lines. "
                        "Required: lane (string). Optional (ANTS-1508): "
                        "doc_paths[] — when `lane` is not in the "
                        "auto-partition, the brief is synthesised "
                        "from these caller-supplied paths instead. "
                        "Paths must be project-relative and resolve "
                        "inside the project root (INV-13 enforced); "
                        "absolute paths and symlink escapes are "
                        "rejected silently. Optional (ANTS-1634b): "
                        "prior_loop_fixes[] — orchestrator-supplied "
                        "list of {title, summary} records the engine "
                        "renders as a 'Previously fixed in loop 1' "
                        "block before the Instructions section, so "
                        "loop-2 reviewers don't re-raise items "
                        "already closed. **Rate limit (ANTS-1629):** "
                        "BriefAssembly tier — 30 calls / 60 s "
                        "per (tool, caller_cwd). A canonical "
                        "`/cold-eyes` Phase-2 fan-out dispatches "
                        "12-16 briefs in one parallel batch, which "
                        "fits comfortably under this cap. Higher than "
                        "the 10/min Expensive tier the other "
                        "`cold_eyes_*` verbs sit in.");
                    t["selection_hint"] = QStringLiteral(
                        "Use to assemble the brief for one "
                        "cold-eyes chunk. Run after "
                        "cold_eyes_partition. Pass doc_paths[] for "
                        "ad-hoc lanes the auto-partition didn't "
                        "surface.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject laneProp;
                    laneProp["type"] = "string";
                    laneProp["description"] = QStringLiteral(
                        "Lane name. May be a partition-derived name "
                        "(see cold_eyes_partition) or an ad-hoc label "
                        "paired with `doc_paths[]`.");
                    QJsonObject docPathsProp;
                    docPathsProp["type"] = "array";
                    QJsonObject docPathsItems;
                    docPathsItems["type"] = "string";
                    docPathsProp["items"] = docPathsItems;
                    docPathsProp["description"] = QStringLiteral(
                        "Optional. Project-relative doc paths for the "
                        "lane. Used only when `lane` is absent from "
                        "the auto-partition. Each entry is anchored "
                        "inside the project root.");
                    // ANTS-1634(b) — orchestrator-supplied prior-loop
                    // fix list. Loop-2 dispatches inject one entry per
                    // item closed by loop-1 so the reviewer doesn't
                    // re-flag the same finding. Engine renders a
                    // "Previously fixed in loop 1 (do not re-raise)"
                    // section before the Instructions block.
                    QJsonObject priorFixesProp;
                    priorFixesProp["type"] = "array";
                    QJsonObject priorFixesItem;
                    priorFixesItem["type"] = "object";
                    QJsonObject priorFixesItemProps;
                    QJsonObject priorFixesTitleP;
                    priorFixesTitleP["type"] = "string";
                    priorFixesTitleP["description"] = QStringLiteral(
                        "Short headline for the closed item (e.g. "
                        "\"ANTS-1500 missing stale_reason on "
                        "counter_regressed\").");
                    QJsonObject priorFixesSummaryP;
                    priorFixesSummaryP["type"] = "string";
                    priorFixesSummaryP["description"] = QStringLiteral(
                        "One-sentence summary of what loop-1 changed. "
                        "Empty allowed but at least one of "
                        "title/summary must be non-empty (entries with "
                        "both empty are silently dropped).");
                    priorFixesItemProps["title"]   = priorFixesTitleP;
                    priorFixesItemProps["summary"] = priorFixesSummaryP;
                    priorFixesItem["properties"]   = priorFixesItemProps;
                    priorFixesItem["additionalProperties"] = false;
                    priorFixesProp["items"] = priorFixesItem;
                    priorFixesProp["description"] = QStringLiteral(
                        "Optional (ANTS-1634b). Orchestrator-supplied "
                        "list of {title, summary} records describing "
                        "items that were already fixed in loop 1. The "
                        "engine renders a 'Previously fixed in loop 1 "
                        "(do not re-raise)' section before the "
                        "reviewer Instructions block. Saves the "
                        "orchestrator from hand-rolling the block into "
                        "each subagent prompt. Empty/missing entries "
                        "are skipped; an empty array omits the "
                        "section entirely.");
                    QJsonObject props;
                    props["lane"]             = laneProp;
                    props["doc_paths"]        = docPathsProp;
                    props["prior_loop_fixes"] = priorFixesProp;
                    // ANTS-1391 — caller_cwd anchor.
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("lane");
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-1400 — caller_cwd_info diagnostic verb.
                // Surfaces the ResolvedRoot::Source enum so callers
                // can dispatch on which branch fired without
                // invoking a tab-anchored tool first.
                {
                    QJsonObject t;
                    t["name"] = "caller_cwd_info";
                    t["description"] = QStringLiteral(
                        "Diagnose how Ants would resolve your "
                        "caller_cwd. Returns "
                        "{ok:true, source:'ExplicitMatch'|"
                        "'EmptyFallback'|'NoMatch'|'Unresolvable', "
                        "resolved_cwd:'...', tab_index:N?}. No side "
                        "effects — does not read scrollback, run "
                        "git, or write any state. Pass caller_cwd "
                        "to test resolution; omit it to see the "
                        "empty-fallback behaviour (focused tab). "
                        // ANTS-1578 — discoverability hint mirrored
                        // into the description so callers that only
                        // see the description text (not the separate
                        // selection_hint field) still find the verb
                        // when chasing no_roadmap_loaded / cwd_mismatch.
                        "Use this FIRST when a project-scoped read "
                        "returns `no_roadmap_loaded` or any tool "
                        "returns `cwd_mismatch` — confirms which "
                        "project's data the tool would have been "
                        "operating on. "
                        "See docs/specs/ANTS-1400.md.");
                    t["selection_hint"] = QStringLiteral(
                        "Use FIRST when a read tool returns "
                        "no_roadmap_loaded, cwd_mismatch, or any "
                        "*-not-found refusal — confirms which "
                        "project's data the tool would have read. "
                        "No side effects.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject callerProp;
                    callerProp["type"] = "string";
                    callerProp["description"] = QStringLiteral(
                        "Your $PWD or an arbitrary path you want "
                        "to test. Empty/absent → EmptyFallback "
                        "(returns focused-tab info).");
                    QJsonObject props;
                    props["caller_cwd"] = callerProp;
                    schema["properties"] = props;
                    schema["required"] = QJsonArray();
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
}

}  // namespace claudeintegrationdetail
