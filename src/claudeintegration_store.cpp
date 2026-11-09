// ANTS-1677 claudeintegration piece 7/9 — the roadmap and store tool descriptors
#include "claudeintegration_internal.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace claudeintegrationdetail {

void appendToolSchemasStore(QJsonArray &tools) {
                // ANTS-1424 — roadmap_log: append a new bullet to
                // ROADMAP.md. Mutates project state (counter +
                // markdown), so Required-contract gated and path-
                // validated. Schema mirrors the field set in
                // docs/standards/roadmap-format.md § 3.5.
                {
                    QJsonObject t;
                    t["name"] = "roadmap_log";
                    t["description"] = QStringLiteral(
                        "Append or flip/annotate ROADMAP.md bullets (GFM or "
                        "Ants-v1 emoji format, auto-detected). ops: see `op`, "
                        "default append. append needs caller_cwd, "
                        "section, status, headline, kind, source; flip needs "
                        "to_status + one of (id|anchor|headline). dry_run:"
                        "true previews. Refusals: bullet_not_found, "
                        "bullet_ambiguous, anchor_unsafe_context, "
                        "bad_op_combo, bad_case, missing_field, "
                        "unrecognised_format. SIZE NOTE: keep append bodies "
                        "small — large payloads can drop in transit "
                        "(ANTS-1853); use Edit for long prose. "
                        // ANTS-4465 — the amend_headline warning generalised.
                        // The hazard is not the headline; it is that on a
                        // migrated project the render owns the WHOLE file.
                        // INV-5 caps this string at 800 B on the tools/list
                        // wire, so the reasoning lives in `detail` and only
                        // the warning itself is spent here.
                        "MIGRATED: every op re-renders the WHOLE file, so a "
                        "hand-edit outside the store is reverted; the write "
                        "reports `publish_overwrote_file_text` (ANTS-4462). "
                        "caller_cwd Required.");
                    // ANTS-2079 — full per-op reference in `detail`
                    // (stripped from the tools/list wire; served by
                    // tool_info {name:"roadmap_log"}).
                    t["detail"] = QStringLiteral(
                        "Append a new bullet to ROADMAP.md, or flip "
                        "the status of an existing bullet on either "
                        "a GFM-task-list or Ants-v1 emoji-status "
                        "roadmap (auto-detected; ANTS-1441). Mode "
                        "picked by `op` (default \"append\"). "
                        "op:\"append\" (ANTS-1424) — allocates the "
                        "next stable ID from .roadmap-counter, "
                        "formats the bullet per the project's "
                        "roadmap-format spec, and inserts at the end "
                        "of `section` (slug from roadmap_query). "
                        "Required: caller_cwd, section, status "
                        "(planned/in-progress/shipped/considered), "
                        "headline, kind, source. Optional: body, "
                        "layman, lanes[], id_hint, id_prefix, dry_run. "
                        "ID prefix (ANTS-2076): counter IDs render as "
                        "[<PREFIX>-NNNN]; the prefix is sniffed from "
                        "existing IDs, else derived from caller_cwd's "
                        "leaf directory (uppercase first 4 chars, "
                        "DOOM_Ants → DOOM) — pass `id_prefix` to pin it "
                        "on a fresh roadmap. `dry_run:true` (ANTS-2077) "
                        "previews the id + bullet without writing. "
                        "Slugs / IDs are case-sensitive (off-case → "
                        "bad_case with the canonical form). ANTS-2043 — the "
                        "success envelope carries a non-blocking "
                        "`possible_duplicates:[{id, headline, score}]` "
                        "advisory (score 100 = exact normalised-headline "
                        "match, 60-99 = token overlap) when the new "
                        "headline resembles an existing bullet; the "
                        "bullet is still appended (append_batch attaches "
                        "it per accepted bullet as "
                        "`[{bullet_index, id, candidates[]}]`). "
                        // ANTS-4605 — declared, not fixed. The user
                        // settled the disposition 2026-08-21: a project
                        // is imported into the store and its ROADMAP.md
                        // is REGENERATED from it, so a mixed file is
                        // transient and appending ants-v1 is the
                        // direction of travel. What was missing was any
                        // statement that it happens at all.
                        "DIALECT (ANTS-4605): append always writes an "
                        "ants-v1 emoji bullet, whatever dialect the file "
                        "is in — so appending to a github-task-list "
                        "roadmap leaves it MIXED, and nothing warns. "
                        "That is the direction of travel, not a defect: "
                        "the store re-renders the file in one dialect. "
                        "op:\"flip\" does NOT do this — it applies per "
                        "the bullet's own format (ANTS-3565), so a flip "
                        "never converts a bullet. See roadmap-format.md "
                        "§ 3.10.2. ANTS-4606 — this verb SPELLS that "
                        "dialect `gfm`, where roadmap_query spells the "
                        "same one `github-task-list`. One format, two "
                        "vocabularies, so a caller comparing the two "
                        "envelopes reads a disagreement that is not one. "
                        "Both stand deliberately: unifying them would "
                        "silently break any caller already keying on "
                        "either, and the store is machine-global, so that "
                        "caller may be another project's session. Map both "
                        "spellings; never compare by string equality "
                        "across the two verbs. "
                        "op:\"flip\" (ANTS-1428) — flips a bullet's "
                        "status; injects an Obsidian-style "
                        "`^prefix-NNNN` anchor on first touch as the "
                        "durable handle. Required: caller_cwd, "
                        "to_status, and one of (id | anchor | "
                        "headline). Optional: prefix_hint, and `note` "
                        "to append a resolution line while flipping in "
                        "one call (ANTS-1793). "
                        "op:\"annotate\" (ANTS-1717) — appends `note` "
                        "to a located bullet's body and leaves status "
                        "unchanged (no flip, no anchor); for recording "
                        "partial progress on a still-open item. "
                        "Required: caller_cwd, note, and one of (id | "
                        "anchor | headline); rejects to_status with "
                        "bad_op_combo. Refusal codes: "
                        "bullet_not_found, bullet_ambiguous, "
                        "anchor_unsafe_context, bad_op_combo, "
                        "missing_field, unrecognised_format. Returns "
                        "{ok, id?, file, line, bytes_written, "
                        "note_appended?, note_line?, ...op-specific} "
                        "or {ok:false, error, code}. "
                        "NESTED SUB-BULLETS (ANTS-3403): flip/flip_batch/"
                        "annotate locators resolve TOP-LEVEL bullets only. "
                        "A multi-phase epic that tracks phases as nested "
                        "list items inside a narrator bullet's body "
                        "(`  - ✅ **Phase B — …**`) is out of scope — the "
                        "walker does not descend into a bullet's body, so a "
                        "per-phase flip returns bullet_not_found. Edit "
                        "ROADMAP.md directly to tick a nested phase line. "
                        "PASS-HEADINGS (ANTS-2126): on a `#### Pass N.M` "
                        "heading roadmap (RetroDB-style; reader synthesises "
                        "`PASS-N-M[-SUB]` ids), append/append_batch/flip/"
                        "flip_batch/annotate now WRITE instead of refusing "
                        "format_mismatch. FOUR ops still refuse on a "
                        "MARKDOWN one, with `unsupported_format`: "
                        "create_section, amend_body, amend_headline and "
                        "set_body — the last three because a pass-headings "
                        "roadmap stores bodies under its headings rather than "
                        "as indented continuation lines, so the body writer "
                        "refuses rather than mis-edit (ANTS-4863; the "
                        "description said only create_section refused, and "
                        "amend_body is the one concrete instance "
                        "mcp-tools.md § 6a gives of an unwritable route). On a "
                        "STORE-BACKED project those three take the store "
                        "route, which returns before this gate, so they write "
                        "there. op:\"append\" needs a `pass` arg "
                        "(e.g. \"43.5\" or \"43.5.B\", validated "
                        "^\\d+\\.\\d+(?:\\.[A-Za-z][A-Za-z0-9]*)?$); status "
                        "is required, kind/source/lanes/layman are ignored, "
                        "and .roadmap-counter is never touched. Flip/annotate "
                        "locate by the `PASS-N-M` id (from roadmap_query) or "
                        "`headline`; a missing required arg is `bad_args`. A "
                        "stray `pass` on a GFM/ants-v1 roadmap is ignored. "
                        "SIZE NOTE (ANTS-1853): keep op:\"append\" "
                        "calls small. Large multi-paragraph `body` "
                        "payloads (many embedded newlines/quotes) are "
                        "intermittently dropped in transit by the "
                        "tool-call transport — the arguments arrive "
                        "empty and the call refuses with "
                        "arguments_empty (NOT an Ants bug; the data "
                        "never reaches the server). Small bodies are "
                        "reliable. For a long entry, prefer a concise "
                        "`body` (≲1–2 short paragraphs) or write the "
                        "prose with the Edit tool directly into "
                        "ROADMAP.md. If a call refuses with "
                        "arguments_empty, just resend it. "
                        // ANTS-4465 / ANTS-4462 — the whole account, here
                        // rather than in the wire description, which INV-5
                        // caps at 800 B.
                        "MIGRATED PROJECT — what the re-render owns. Every op "
                        "re-renders the WHOLE file from the store, so ANY byte "
                        "the store does not model is reverted: the preamble "
                        "above the first heading is the sharpest case, since "
                        "no verb writes it and a hand-edit there is the only "
                        "way to change it. op:\"amend_headline\" refuses on a "
                        "migrated project for this reason and is an INSTANCE "
                        "of the rule, not a headline-only quirk. The write no "
                        "longer does it silently (ANTS-4462): it renders the "
                        "store as it stood BEFORE the mutation, diffs that "
                        "against the file, and reports "
                        "`publish_overwrote_file_text` (bool; it cannot tell "
                        "a hand edit from a stale render) plus "
                        "`discarded_edit_lines` on the true arm — "
                        "`would_discard_*` on a dry run, per ANTS-4463's tense "
                        "rule. ABSENT means nothing measured the file, which "
                        "is not the same as clean. ANTS-4729 — the BOOL asks "
                        "whether the publish overwrote content the FILE held, "
                        "so it is computed from the file's own classified "
                        "lines. A line the RENDER adds and the file lacked — "
                        "the format marker, on a project rendered before that "
                        "marker existed — is an addition, never a discard, and "
                        "no longer fires it. `discarded_edit_lines` keeps its "
                        "two-directional count. It reports and never "
                        "refuses, because one hand-edit would otherwise brick "
                        "every op on the project. Expect one true report per "
                        "project on the FIRST write after a migration: the "
                        "file still carries the author's bytes wherever the "
                        "store keeps a canonical form (a table separator, "
                        "ANTS-3832), and that publish really does overwrite "
                        "them. ANTS-4615 — the total alone could not be acted "
                        "on: one measured report of 84 was 24 bullets restyled "
                        "into the canonical id form plus ONE sentence that no "
                        "longer existed anywhere, and a number mixing those "
                        "trains you to wave the flag through. The true arm now "
                        "also carries `discarded_restyled_lines` (text "
                        "survives, styling differs) and "
                        "`discarded_text_lines` (text does NOT survive — THIS "
                        "is the one to act on), plus `discarded_text[]` naming "
                        "the lost lines, capped at 20 with "
                        "`discarded_text_truncated`. ANTS-4695 — a third, "
                        "`discarded_repunctuated_lines`, rides the same arm "
                        "when present: a file line differing from the render "
                        "ONLY in terminal punctuation. It is neither of the "
                        "other two, and it used to score as restyling because "
                        "the content key ignores punctuation entirely — so a "
                        "project whose Layman lines were hand-authored with "
                        "periods saw all of them change while "
                        "`discarded_text_lines` read 0, which is the field a "
                        "caller checks before allowing the overwrite. "
                        "ANTS-4957 — `discard_reason` names the worst at "
                        "stake: text_lost | structure | punctuation | "
                        "restyle_only (a stale render, safe to overwrite). "
                        "ANTS-4965 — likewise `discarded_structure_lines`: a "
                        "line differing ONLY in whitespace, i.e. an indent or "
                        "aligned column the render flattens. The "
                        "counters classify "
                        "the FILE's lines and so do not sum to the total, "
                        "which also counts lines the render restores that the "
                        "file had deleted. Nothing is suppressed: an "
                        "unclassifiable line counts as TEXT, because "
                        "over-reporting loss costs a look and under-reporting "
                        "hides data loss. ANTS-4947 — on the LOST arm the "
                        "overwritten file is COPIED first and "
                        "`discarded_backup_paths[]` names the copies, outside "
                        "the project so they never dirty git status. A lost "
                        "line is text the render reproduces in no styling, so "
                        "the file was its only copy and the capped echo above "
                        "was all a caller had to retype from. Restyled and "
                        "repunctuated drift keeps nothing: that text survives "
                        "in the render. No dry-run twin — a preview overwrites "
                        "nothing, so there is no copy to name. "
                        "op:\"replace_text\" (ANTS-5760) — replaces EVERY "
                        "exact occurrence of `old_text` with `new_text` in "
                        "every stored item body, in one commit. `section` "
                        "limits it to that section and its sub-sections; "
                        "`status` to items with that status. The match is "
                        "case-sensitive and not wrap-tolerant: a phrase "
                        "split across a hard wrap is not matched. Store-only. "
                        "Reply: `amended[]` {id, hits}, `amended_count`, "
                        "`replacements`. Refuses body_match_not_found when "
                        "nothing in scope matches, and too_many_matches above "
                        "500 items. Run it with dry_run:true first: the "
                        "preview lists every item it would change.");
                    t["selection_hint"] = QStringLiteral(
                        "Use to append a new bullet or flip an "
                        "existing one's status on ROADMAP.md. "
                        "Mutates project state — caller_cwd "
                        "required. Keep append bodies small — large "
                        "payloads can be dropped in transit "
                        "(ANTS-1853); use Edit for long prose.");
                    QJsonObject schema;
                    schema["type"] = "object";

                    QJsonObject callerProp;
                    callerProp["type"] = "string";
                    callerProp["description"] = QStringLiteral(
                        "Your $PWD. REQUIRED — anchors the write to "
                        "your project's ROADMAP.md.");

                    QJsonObject sectionProp;
                    sectionProp["type"] = "string";
                    sectionProp["description"] = QStringLiteral(
                        // ANTS-4385 — this used to say "from `roadmap_query`'s
                        // section echo". No field or mode is called that, so a
                        // caller searching the schema for "echo" found nothing
                        // and concluded the route did not exist — which is
                        // exactly what happened, costing one false finding and
                        // its retraction. Name the mode.
                        "Slug of a ## or ### heading "
                        "(e.g. \"performance-2\"). ANTS-4556 / ANTS-4591 — an "
                        "unknown slug does not just refuse: the envelope "
                        "carries `candidates[]`, the closest slugs ranked by "
                        "the shared ranker (capped at 10), plus "
                        "`sections_total` so an EMPTY list is readable as "
                        "\"no near miss\" rather than \"no sections\". Read "
                        "it before reaching for a section_index round-trip. "
                        "It is a HINT and the write still refuses — guessing "
                        "which section was meant is how a composed body lands "
                        "in the wrong one. A pure case mismatch still wins as "
                        "bad_case with `canonical_slug`. Get valid slugs from "
                        "`roadmap_query` mode:\"section_index\" (or its alias "
                        "mode:\"sections\"). New "
                        "bullet appends at the end of this section. Under "
                        "op:\"replace_text\" it limits the edit to this "
                        "section and its sub-sections (ANTS-5760).");

                    QJsonObject statusProp;
                    statusProp["type"] = "string";
                    // The five lifecycle statuses, shared by `status`,
                    // `to_status` and locators[].to_status.
                    const QJsonArray lifecycleEnum{
                        QStringLiteral("planned"), QStringLiteral("in-progress"),
                        QStringLiteral("shipped"), QStringLiteral("considered"),
                        QStringLiteral("dropped")};
                    statusProp["enum"] = lifecycleEnum;
                    statusProp["description"] = QStringLiteral(
                        "Lifecycle status. Mapped to 📋/🚧/✅/💭/🚫 "
                        "emoji by the verb. `dropped` (🚫) is closed, "
                        "deliberately not done. Under op:\"append_batch\" the "
                        "call-level value is the fallback for any bullet "
                        "that carries none, as `pass` is (ANTS-4982). Under "
                        "op:\"replace_text\" it limits the edit to items "
                        "with this status (ANTS-5760).");

                    QJsonObject headlineProp;
                    headlineProp["type"]      = "string";
                    headlineProp["maxLength"] = 200;
                    headlineProp["description"] = QStringLiteral(
                        "One-line bold headline ending in a period "
                        "(per roadmap-format.md § 3.5).");

                    QJsonObject kindProp;
                    kindProp["type"] = "string";
                    QJsonArray kindEnum;
                    kindEnum.append("implement");
                    kindEnum.append("fix");
                    kindEnum.append("audit-fix");
                    kindEnum.append("review-fix");
                    kindEnum.append("doc");
                    kindEnum.append("doc-fix");
                    kindEnum.append("refactor");
                    kindEnum.append("test");
                    kindEnum.append("chore");
                    kindEnum.append("release");
                    kindEnum.append("perf");
                    kindEnum.append("security");
                    kindEnum.append("feature");
                    kindEnum.append("enhancement");
                    kindEnum.append("investigate");
                    kindEnum.append("research");
                    kindEnum.append("accessibility");
                    kindEnum.append("optimize");
                    kindEnum.append("package");
                    kindEnum.append("marketing");
                    kindEnum.append("ux");
                    kindProp["enum"] = kindEnum;
                    kindProp["description"] = QStringLiteral(
                        "Work category (roadmap-format.md § 3.5.3). "
                        "Under op:\"append_batch\" the call-level value is "
                        "the fallback for a bullet without one.");

                    QJsonObject sourceProp;
                    sourceProp["type"]      = "string";
                    sourceProp["maxLength"] = 200;
                    sourceProp["description"] = QStringLiteral(
                        "Provenance (e.g. \"user-request-2026-05-16\"). "
                        "Under op:\"append_batch\" the call-level value is "
                        "the fallback for a bullet without one.");

                    QJsonObject bodyProp;
                    bodyProp["type"]      = "string";
                    bodyProp["maxLength"] = 4000;
                    bodyProp["description"] = QStringLiteral(
                        "Optional body prose. Written verbatim with "
                        "a 2-space indent; pre-wrap to ~70 columns.");

                    QJsonObject laymanProp;
                    laymanProp["type"]      = "string";
                    laymanProp["maxLength"] = 400;
                    laymanProp["description"] = QStringLiteral(
                        "Optional one-sentence summary for non-"
                        "technical readers. Renders on the card face "
                        "in the Roadmap dialog (ANTS-1154).");

                    QJsonObject lanesProp;
                    lanesProp["type"] = "array";
                    QJsonObject laneItem;
                    laneItem["type"] = "string";
                    lanesProp["items"] = laneItem;
                    lanesProp["description"] = QStringLiteral(
                        "Optional subsystem owners (a `Lanes:` line). "
                        "Under op:\"append_batch\" the call-level value is "
                        "the fallback for a bullet without one.");

                    // ANTS-3382 — optional evidence file paths for
                    // image/log-driven bullets (e.g. a bug diagnosed from
                    // screenshots). Emitted as an `Evidence: <paths>` line
                    // and echoed back by roadmap_query as an `evidence`
                    // array. Accepted on op:"append" / "append_batch".
                    QJsonObject evidenceProp;
                    evidenceProp["type"] = "array";
                    QJsonObject evidenceItem;
                    evidenceItem["type"]      = "string";
                    evidenceItem["maxLength"] = 500;
                    evidenceProp["items"] = evidenceItem;
                    evidenceProp["description"] = QStringLiteral(
                        "Optional file paths (screenshots, logs) that "
                        "evidence this item. Emitted as an `Evidence: "
                        "path1, path2` line and echoed by roadmap_query "
                        "as an `evidence` array so a later session can "
                        "re-locate the files. Commas/newlines inside a "
                        "path are folded to spaces. PATHS, not prose "
                        "(roadmap-format 3.5): the field is comma-split, so "
                        "a sentence written here is stored as several "
                        "fragments rather than as what you wrote — one "
                        "measured bullet holds `96`, `000 mutants across all "
                        "twelve parsers under ASan+UBSan` and `clean`. "
                        "ANTS-4527 — an element that is neither separator- "
                        "nor extension-shaped is reported back as an "
                        "`evidence_not_path_shaped` entry in the envelope's "
                        "`warnings` array. It is an ADVISORY on a successful "
                        "write, never a refusal, and it fires on the store "
                        "and markdown paths alike. Put the explanation in "
                        "`body` and leave this for paths.");

                    QJsonObject idHintProp;
                    idHintProp["type"]    = "integer";
                    idHintProp["minimum"] = 1;
                    idHintProp["description"] = QStringLiteral(
                        "Optional explicit ID under op:\"append\". "
                        "Must exceed the current .roadmap-counter "
                        "value or returns code=id_taken. Not accepted "
                        "under op:\"flip\" (bad_op_combo).");

                    // ANTS-1428 flip-mode fields.
                    QJsonObject opProp;
                    opProp["type"] = "string";
                    QJsonArray opEnum;
                    opEnum.append("append");
                    opEnum.append("append_batch");
                    opEnum.append("flip");
                    opEnum.append("flip_batch");
                    opEnum.append("annotate");
                    opEnum.append("annotate_batch");  // ANTS-4470
                    opEnum.append("amend_body");   // ANTS-3406
                    opEnum.append("amend_headline");  // ANTS-4372
                    opEnum.append("amend_field");  // ANTS-4667
                    opEnum.append("amend_field_batch");  // ANTS-5385
                    opEnum.append("amend_batch");        // ANTS-4669
                    opEnum.append("replace_text");       // ANTS-5760
                    // ANTS-4842 — set_body was dispatched and described for a
                    // release while absent here, so a caller that checked the
                    // enum concluded the op did not exist. That reasoning was
                    // right once: an earlier session correctly read its absence
                    // as a stale binary. The enum is the answer callers trust.
                    opEnum.append("set_body");  // ANTS-4808
                    opEnum.append("create_section");
                    opEnum.append("bundle_row");
                    opEnum.append("backfill_dates");  // ANTS-4501
                    opEnum.append("place_by_release");  // ANTS-5315
                    opEnum.append("repair_trailers");  // ANTS-4585
                    opEnum.append("render");  // ANTS-4614
                    opEnum.append("set_intro");     // ANTS-4949
                    opEnum.append("set_preamble");  // ANTS-4968
                    opEnum.append("amend_intro");   // ANTS-5373
                    opEnum.append("amend_preamble"); // ANTS-5523
                    opEnum.append("set_legend");     // ANTS-5615
                    opEnum.append("delete_section");  // ANTS-4958
                    opEnum.append("move_section");    // ANTS-4958
                    opEnum.append("retitle_section"); // ANTS-5557
                    opEnum.append("rotate_minor");    // ANTS-4081
                    opEnum.append("link");            // ANTS-4079
                    opEnum.append("unlink");          // ANTS-4079
                    opEnum.append("list_elements");    // ANTS-5379
                    opEnum.append("amend_element");    // ANTS-5379
                    opEnum.append("delete_element");   // ANTS-5379
                    opEnum.append("promote_element");  // ANTS-5379
                    // ANTS-5253 — convert shipped dispatched, described in the
                    // unknown-op refusal, and ABSENT here, which is ANTS-4842's
                    // defect repeated one op later. The guard that item left
                    // behind names the ops it knew about, so it cannot fail for
                    // the NEXT one; a mechanical enum-versus-dispatcher check is
                    // ANTS-5254.
                    opEnum.append("convert");  // ANTS-4491
                    opProp["enum"] = opEnum;
                    opProp["description"] = QStringLiteral(
                        "Verb mode. Default \"append\" (ANTS-1424). "
                        "\"set_intro\" (ANTS-4949) replaces a section's intro "
                        "prose: `section` + `new_text`, stored verbatim, "
                        "no `#` to `###` headings (`####` and deeper are "
                        "intro text). \"amend_intro\" (ANTS-5373) replaces "
                        "one unique `old_text` inside that intro with "
                        "`new_text`. \"set_preamble\" (ANTS-4968) replaces "
                        "the roadmap's title and preamble: `new_text` only, "
                        "one `# ` title line allowed; the render adds the "
                        "format marker and generated-file notice, so omit "
                        "them; \"amend_preamble\" replaces one unique "
                        "`old_text` in it. All four store-only, "
                        "dry_run previewable (echoing previous_intro), and "
                        "echo replaced_intro_chars (on amend_intro, the "
                        "matched old_text only). "
                        "\"rotate_minor\" (ANTS-4070, store-only) moves every "
                        "section of a closed `minor` (\"0.7\") into "
                        "docs/roadmap/<minor>.md; it refuses minor_not_closed "
                        "while one holds an open item. Run it on a minor or "
                        "major bump only, as the bump recipe does; a minor "
                        "with nothing open may still be the current one. "
                        "\"retitle_section\" (store-only) renames `section` to "
                        "`title` and returns its new `slug`; a clash refuses "
                        "bad_args. "
                        "\"link\" / \"unlink\" (ANTS-4079, store-only) write or "
                        "remove relationship rows from `id` to each of `targets`, "
                        "of one `type`; roadmap_query id: reads them back as "
                        "`links`. A same-type cycle refuses link_cycle naming it "
                        "in `cycle`; a repeat lands in `unchanged`. A flip to "
                        "shipped refuses open_parts while a part split from the "
                        "item is open; an open blocker adds `blocked_by_open`. "
                        "\"delete_section\" (ANTS-4958, store-only) removes a section by "
                        "`section`, refusing section_not_empty while it files "
                        "an item; its intro and narration come back in the "
                        "reply. \"move_section\" (store-only) moves `section` and its "
                        "subsections after `after_section` or before "
                        "`before_section`. "
                        "\"amend_field\" (ANTS-4667) writes ONE TRAILER "
                        "COLUMN after creation — `id` + `field` "
                        "(layman|kind|source|lanes|evidence) + `value`, "
                        "dry_run previewable. It is not a mode of "
                        "amend_body and takes no old_text: the trailer "
                        "lines are COMPOSED at render time from their own "
                        "columns (ANTS-4599), so they are not in the "
                        "stored body and amend_body cannot reach them — "
                        "while roadmap_query include_body:true RETURNS "
                        "them inside `body`, which is what made a caller "
                        "pass one back as old_text and get "
                        "body_match_not_found about a string it had just "
                        "read. Store-only (a markdown project keeps the "
                        "trailer line as body text; use amend_body there) "
                        "and id-only. Refuses field_shadowed_by_body when "
                        "the body declares that key at a line start, "
                        "because the declaration wins at render AND is "
                        "re-parsed into the column by the next body "
                        "write, so the column write would be invisible now "
                        "and reverted later. ANTS-4948 — `field:\"section\"` "
                        "MOVES the item: `value` is the destination slug, "
                        "and `locators[]` of {id} moves several in one "
                        "render. "
                        "\"render\" (ANTS-4614) PUBLISHES the store to "
                        "ROADMAP.md with NO semantic change — no locator, no "
                        "arguments, nothing written to the store. It exists "
                        "because roadmap_migrate honestly reports "
                        "markdown_rewritten:false and nothing owned the doing "
                        "half: the canonical re-render landed only on the next "
                        "unrelated write, so a wanted normalisation (two id "
                        "dialects in one file, say) sat undelivered and the "
                        "only route was to invent a semantic write purely as a "
                        "render trigger, polluting the roadmap with a bullet "
                        "nobody wanted. It also makes a migration verifiable "
                        "from the repo side, where a clean `git status` was "
                        "otherwise indistinguishable from the migration never "
                        "having run. Idempotent, `dry_run` previewable, and it "
                        "runs every gate the semantic ops run (the Layman "
                        "gate, ANTS-4141's divergence guard) because those "
                        "live in the shared write sequence. Reports "
                        "files_written / items_rendered / bytes_written plus "
                        "the ANTS-4462 drift fields; a dry run's "
                        "`would_change` says whether any bytes would change "
                        "(ANTS-5283). Store-backed projects "
                        "only: on a markdown project the file already IS the "
                        "source of truth, so it refuses "
                        "`project_not_registered` rather than pretending. "
                        "\"append_batch\" (ANTS-1879) appends N bullets "
                        "to one `section` in a single read + single "
                        "commit — pass `bullets[]` (each with the same "
                        "fields as op:\"append\"). Per-bullet validation "
                        "failures land in `skipped[]` while accepted "
                        "bullets still apply (semantic parity with "
                        "flip_batch). \"flip\" routes to the status-flip "
                        "path (ANTS-1428; works on GFM-task-list and "
                        "Ants-v1 emoji formats — ANTS-1441) and accepts "
                        "an optional `note` to append a resolution line "
                        "in the same call (ANTS-1793). \"flip_batch\" "
                        "(ANTS-1690) flips N bullets to one `to_status` "
                        "in a single read + single atomic commit — pass "
                        "`locators[]` (each {id|anchor|headline|"
                        "line_range} + optional per-locator `note` and "
                        "`no_anchor`); an unresolvable locator lands in "
                        "the response `skipped[]` while the rest apply, "
                        "and a locator naming a bullet an earlier one "
                        "claimed is skipped `duplicate_locator`. "
                        "`to_status` resolves op:\"flip\"'s synonyms "
                        "(done/wip/todo/maybe) too. "
                        "Use it to close a whole bundle without N "
                        "round-trips or racing the file watcher. "
                        "\"annotate\" "
                        "(ANTS-1717) appends a `note` to a located "
                        "bullet WITHOUT changing its status — for "
                        "recording partial progress on a still-open "
                        "item; no status flip, no anchor injection. "
                        "\"annotate_batch\" (ANTS-4470) is that op's "
                        "batch form and closes the asymmetry that left "
                        "annotate the only bullet-writing op without "
                        "one: N notes in a single read + single atomic "
                        "commit, where N separate annotates cost N "
                        "reads of a large ROADMAP.md, N commits, and "
                        "raced the file watcher the same way N separate "
                        "flips did before flip_batch existed. Takes "
                        "flip_batch's `locators[]` UNCHANGED — "
                        "{id|anchor|headline|line_range} plus the "
                        "per-locator `note` that array has carried "
                        "since ANTS-1690 — so there is no second shape "
                        "to learn; it is flip_batch with the status "
                        "write omitted, exactly as annotate is flip. "
                        "`to_status` is REFUSED (bad_op_combo), not "
                        "ignored, since a caller who passes one means "
                        "to change status. A locator with no `note` "
                        "writes nothing and is refused into `skipped[]` "
                        "per locator, so one noteless locator does not "
                        "cost the batch its other closures; when every "
                        "locator is noteless the all-failed refusal "
                        "still fires. The envelope keeps flip_batch's "
                        "keys — `flipped` / `flipped_count` / "
                        "`would_flip_count` — deliberately, so one "
                        "parser reads both ops; each row's `to_status` "
                        "is that item's OWN unchanged status, and the "
                        "batch-wide top-level `to_status` is ABSENT, "
                        "because a single value there could only be "
                        "wrong. "
                        "\"create_section\" (ANTS-1878) splices a new "
                        "## / ### heading after an existing section — "
                        "pass `after_section` (slug), `level` (2 or 3), "
                        "`title`, optional `intro_body`. Returns the "
                        "new slug so a follow-up op:\"append\" / "
                        "op:\"append_batch\" can key on it. "
                        "\"bundle_row\" (ANTS-1691) appends a row to a "
                        "Markdown progress table under a section heading — "
                        "pass `section` (heading text/slug) + `cells` "
                        "(non-empty array of cell strings), optional "
                        "`header` (creates the table if absent), `position` "
                        "(\"end\" default) and `sort_col` (keep the table "
                        "sorted by a column). Pipe-escapes each cell and "
                        "folds newlines to <br>, so a bundle-tracking table "
                        "no longer needs a hand-edit / sed. "
                        "\"set_body\" (ANTS-4808) REPLACES a bullet's WHOLE "
                        "body — locate by id/anchor/headline and pass "
                        "`new_text` alone. `old_text` is REFUSED "
                        "(bad_op_combo) because it scopes nothing, and an "
                        "empty `new_text` clears the body. It is the route "
                        "back for a body whose text cannot be expressed as a "
                        "match, which amend_body has no answer for: three "
                        "sessions reported ending there, one leaving a "
                        "kilobyte of garbled prose in a shipped item. "
                        "Store-backed projects only. It re-derives the "
                        "trailer columns and records the previous body in "
                        "history exactly as amend_body does, and the envelope "
                        "echoes `replaced_body_chars` so the reply says what "
                        "was destroyed. dry_run previewable. "
                        "\"amend_body\" (ANTS-3406) patches a bullet's "
                        "continuation prose in place — locate by "
                        "id/anchor/headline, then replace the EXACT "
                        "`old_text` with `new_text` (unique-match "
                        "guarded; status/headline out of scope; dry_run "
                        "previewable). Use it to fix a stale phrase inside an "
                        "existing bullet's body without a raw text edit. "
                        "ANTS-4550 — a phrase straddling the ~70-col hard "
                        "wrap matches and is amended in ONE call; see "
                        "`old_text`. "
                        "\"backfill_dates\" (ANTS-4501) is the ONE-OFF that "
                        "makes roadmap_query mode:\"report\" answer anything "
                        "about the PAST. The item table's `created` / `shipped` "
                        "columns are stamped going forward from 2026-08-20 and "
                        "are NULL on every row filed before it, so every "
                        "period figure is computed over that empty population "
                        "and the report's `coverage` block says so. This walks "
                        "the project's git history over its roadmap files "
                        "(author dates, oldest first) and fills them: an id's "
                        "`created` is the first commit its bullet appears in, "
                        "its `shipped` the first commit the bullet carries ✅. "
                        "Takes `caller_cwd` (one project per call, like "
                        "roadmap_migrate) and `dry_run`; returns "
                        "`revisions_walked`, `created_written`, "
                        "`shipped_written`, `undated_count` and a capped "
                        "`undated[]` sample of the ids it could not date, "
                        "`shipped_undated_count` (+ `shipped_undated[]`) for "
                        "shipped items the history never showed done, and "
                        "`dialect` (ANTS-5405: a `#### Pass` roadmap is walked "
                        "by the pass that encloses each change). It "
                        "NEVER overwrites a non-NULL date, so it is "
                        "re-runnable and a hand correction survives; it never "
                        "invents one for an id no commit showed; it skips the "
                        "`shipped` COLUMN for any id not currently shipped, so "
                        "a reopened item is not silently re-closed from git; "
                        "and it does not touch `last_modified` at all. "
                        "Refusals: `not_a_git_repo` (no history to walk), "
                        "`project_not_registered` (the store holds no row for "
                        "that root — run roadmap_migrate first), `git_failed` "
                        "(the walk could not complete; nothing was written). "
                        "~4 s over 1525 revisions on Ants Terminal — a one-off, "
                        "not a per-query cost, which is why it is an explicit "
                        "op and never a side effect of a read. "
                        "\"place_by_release\" (ANTS-5315) files each SHIPPED "
                        "item under the release it shipped in: the lowest "
                        "CHANGELOG `## [X.Y.Z]` citing its id, else the lowest "
                        "stable tag holding a commit whose subject begins with "
                        "the id. It reuses or creates one `## X.Y.Z` section "
                        "per release, moves a wave title's leading version to "
                        "its end, and never moves open items or ones a human "
                        "moved. Dry run first is enforced: send dry_run:true, "
                        "read the counts, section changes and sampled moves "
                        "(`sample_limit`), then apply its plan_id. "
                        "\"repair_trailers\" (ANTS-4585 phase 2) is the other "
                        "ONE-OFF: it recovers the `layman` / `source` / `lanes` "
                        "values MIGRATION CUT SHORT, at a hard line wrap "
                        "(ANTS-4542) or at the full stop inside `e.g.` / "
                        "`config.yaml` (ANTS-4596 / ANTS-4597). Those causes are "
                        "fixed; the short values are still stored. The repair is "
                        "a RE-PARSE: the item's legacy inline run was never "
                        "stripped from its body, so the author's full text "
                        "survives in its own prose, and the current parser reads "
                        "it correctly. The bullet is rebuilt from its head line "
                        "and its STORED body only, never from the render — "
                        "`bulletText()` composes a trailer line for every column "
                        "the prose does not declare (ANTS-4599), so re-parsing "
                        "that hands the column straight back and the pass "
                        "becomes a no-op reporting success. **A value is written "
                        "ONLY where the stored one is a strict prefix of the "
                        "re-parse**, so the op can only ever EXTEND a value with "
                        "more of the author's own adjacent prose; `lanes` "
                        "compares element-wise, since a joined string diverges "
                        "on separator spacing alone. Everything else is SKIPPED "
                        "and counted, and that guard is the whole design rather "
                        "than caution: seven measured items hold a column that "
                        "is NEWER than the prose, because a later roadmap_log "
                        "write updated it and left the legacy run alone — an "
                        "unguarded re-parse reverts those to superseded text "
                        "with no second copy to check against. Takes "
                        "`caller_cwd` (one project per call) and `dry_run`, "
                        "which decides every write before opening a transaction "
                        "so the preview reports the real run's counts. Returns "
                        "`items`, `items_with_run`, `repaired`, the per-column "
                        "`layman_repaired` / `source_repaired` / "
                        "`lanes_repaired`, `chars_recovered`, `skipped` and a "
                        "capped `skipped_ids[]`. Idempotent: a second run "
                        "repairs nothing. It CANNOT repair an item whose prose "
                        "no longer carries the run — that text is gone and no "
                        "re-parse recovers it. ANTS-4507 — `strip_runs:true` also "
                        "removes the trailer run a body stored before ANTS-4506 "
                        "still ENDS in, and only where it is redundant: every "
                        "value the run carries equals its column and the prose "
                        "left behind declares none of those keys. Any other "
                        "trailing run is left in place and listed, because the "
                        "file shows the run's value today. Adds `runs_stripped`, "
                        "`strip_skipped` and `strip_skipped_ids[]` (capped at "
                        "500, `strip_skipped_truncated` past it); each removed "
                        "run is recorded in history. ANTS-4543 — it also "
                        "removes a key declared at a line start more than once "
                        "mid-body, deleting each copy equal to its column under "
                        "the same test, counted in `repeats_stripped`. "
                        "Refusals: `project_not_registered` "
                        "(run roadmap_migrate first), `store_failed`. "
                        "ECHO COST (ANTS-5263). A store-served flip or "
                        "annotate echoes only `post_bullets` {id, status, "
                        "headline} by default; `return:\"full\"` echoes the "
                        "whole rendered bullet as `bullet` (`would_be_bullet` "
                        "on a dry run), which grows with every note. "
                        "`fields:[\"ok\",\"id\"]` trims any reply. "
                        "\"convert\" (ANTS-4491) re-imports a "
                        "`github-task-list` or (ANTS-6724) `pass-headings` "
                        "roadmap and republishes it as "
                        "canonical ants-v1, in ONE atomic commit+render — the "
                        "two halves cannot be written separately, because "
                        "every read dispatches on the live file's detected "
                        "dialect and refuses a disagreement with the stored "
                        "`source_format`, so a half-written convert leaves the "
                        "project unreadable. Takes `caller_cwd` alone (plus "
                        "`dry_run` and `backup_to`); it is NOT section-scoped and takes no "
                        "locator. Bullets already carrying an id keep it, and "
                        "an id-less one is ALLOCATED a synthesised "
                        "`<prefix>-S<NNNN>` from its own counter (ANTS-4500), "
                        "so a convert never renumbers what the file already "
                        "declares. The envelope carries `source_dialect`, "
                        "`target_dialect` and an `ids` block — `allocated`, "
                        "`parsed`, `bullets_total`, a capped "
                        "`allocated_ids[]`, and (ANTS-5252) `planned[]` — ONE "
                        "ROW PER BULLET carrying `id`, `origin`, `in_file`, "
                        "the source `line`, and the gated `id_inferred` / "
                        "`layman_missing` flags. On a dry run that table IS "
                        "the deliverable, since the op is a one-way bulk "
                        "rewrite of a version-controlled file that moves a "
                        "counter other documents cite by id: an aggregate "
                        "count cannot be checked against anything, a "
                        "per-bullet list can be read against the file. Scan "
                        "`in_file` (a boolean: true when the file already "
                        "carries the id) — a newly assigned id is visible and "
                        "fixable, a CHANGED one is invisible and permanent. "
                        "`origin` is what the FILE says: `absent` means the "
                        "bullet carries no id there and the load will either "
                        "issue one OR match an existing store item by "
                        "headline, so it deliberately does NOT assert an "
                        "allocation — `allocated_ids[]` names the ids "
                        "actually issued. Every capped list announces its cap "
                        "(`*_truncated` plus `*_shown`). An ALREADY-ants-v1 "
                        "source is accepted, not refused — the op is "
                        "idempotent per bullet, which is what makes it safe to "
                        "re-run over a part-converted file. Refuses "
                        "`project_not_registered` on an unmigrated project (it "
                        "will not migrate implicitly — run roadmap_migrate "
                        "first), `unrecognised_format` on a file carrying no "
                        "dialect signal at all, and (ANTS-6724) "
                        "`id_format_required` on a pass-headings source until "
                        "the project declares `id_format` with a `prefix` and "
                        "a `pattern` its ids match. A real run snapshots the "
                        "store first (`backup_path`) and refuses "
                        "`backup_failed` when it cannot. "
                        "ANTS-5326 — on a github-task-list source it also "
                        "refuses `orphans_present` (with `items_orphaned` and "
                        "`orphaned_ids`) when the store holds items the file "
                        "no longer carries, which the render would otherwise "
                        "publish back; deregister and re-migrate first. "
                        "ANTS-5286 — a real run refuses `text_lost` when the "
                        "rewrite would drop the file's text, unless "
                        "`accept_text_loss:true`. ANTS-5329 — and "
                        "`ambiguous_rematch` when bullets would be paired by "
                        "order, unless `accept_ambiguous_rematch:true`. "
                        "Nothing is written on any of them.");
                    QJsonObject toStatusProp;
                    toStatusProp["type"] = "string";
                    toStatusProp["enum"] = lifecycleEnum;
                    toStatusProp["description"] = QStringLiteral(
                        "Target lifecycle status for op:\"flip\". "
                        "Mapped to 📋/🚧/✅/💭/🚫 emoji by the verb.");
                    QJsonObject idProp;
                    idProp["type"] = "string";
                    idProp["description"] = QStringLiteral(
                        "Bold-ID locator for op:\"flip\" (e.g. "
                        "\"Sh4\", \"VEST-0042\"). Highest precedence "
                        "locator; wins over anchor when both are "
                        "passed (INV-12).");
                    QJsonObject anchorProp;
                    anchorProp["type"] = "string";
                    anchorProp["description"] = QStringLiteral(
                        "Caret-anchor locator for op:\"flip\" (e.g. "
                        "\"vest-0042\"). Used when no bold-ID is "
                        "available.");
                    QJsonObject prefixHintProp;
                    prefixHintProp["type"] = "string";
                    prefixHintProp["pattern"] = "^[A-Z][A-Z0-9_-]{0,15}$";
                    prefixHintProp["description"] = QStringLiteral(
                        "Optional prefix for newly-injected caret "
                        "anchors under op:\"flip\". Defaults to the "
                        "uppercase first 4 chars of caller_cwd's "
                        "leaf directory.");

                    // ANTS-1717/1793 — note to append to the located
                    // bullet's body. Required under op:\"annotate\";
                    // optional under op:\"flip\".
                    QJsonObject noteProp;
                    noteProp["type"]      = "string";
                    noteProp["maxLength"] = 4000;
                    noteProp["description"] = QStringLiteral(
                        "Prose appended as indented continuation "
                        "line(s) at the end of the located bullet's "
                        "body — the standard \"Resolved (date): …\" / "
                        "\"Progress (date): …\" close-or-update line. "
                        "Required under op:\"annotate\" (status "
                        "untouched); optional under op:\"flip\" (append "
                        "the resolution note while flipping in one "
                        "call). Scrubbed of leaked tool-call XML like "
                        "op:\"append\"'s body; pre-wrap to ~70 columns. "
                        "ANTS-4549 — a note is PROSE: it may name a trailer key "
                        "(`Kind:`, `Layman:`, `Source:`, `Lanes:`, `Evidence:`) "
                        "only with the label first on its line, which is how the "
                        "render writes a declaration. Mid-line the key would be "
                        "re-parsed as metadata and its following words written to "
                        "that column, so it refuses `body_shadowed`; wrap the key "
                        "in backticks to talk ABOUT it.");

                    // ANTS-3406 — op:"amend_body" operands: replace an
                    // exact single-line substring of the located bullet's
                    // continuation body.
                    QJsonObject oldTextProp;
                    oldTextProp["type"]      = "string";
                    oldTextProp["maxLength"] = 4000;
                    oldTextProp["description"] = QStringLiteral(
                        "op:\"amend_body\" (ANTS-3406) / "
                        "op:\"amend_headline\" (ANTS-4372) — the EXACT substring "
                        "to replace inside the located bullet's continuation "
                        "body. Must occur exactly once (0 → "
                        "body_match_not_found, >1 → body_match_ambiguous, so "
                        "it can't silently clobber unrelated prose). "
                        "Case-sensitive. ANTS-4550 — a phrase straddling the "
                        "~70-col hard wrap DOES match and is amended in one "
                        "call: a run of whitespace here matches a run of "
                        "whitespace in the body, so paste the sentence as it "
                        "reads. That pass runs only after an exact match finds "
                        "nothing, and the uniqueness guard above applies to "
                        "it unchanged; a wrapped match re-flows the lines it "
                        "spanned into one and the envelope says "
                        "`wrapped_match:true`; on a store-backed project the "
                        "joined line is re-wrapped to the body's width and "
                        "the reply adds `rewrapped:true` (ANTS-4970). The "
                        "headline is out of "
                        "scope — amend_body edits body prose only. "
                        "ANTS-4612 — the wrapped pass DECLINES a span whose "
                        "continuation lines carry structure a re-flow would "
                        "destroy: column alignment (an internal run of 2+ "
                        "spaces) or the opening of a new list item. Refuses "
                        "`body_match_wrapped_block`, nothing is written, and "
                        "the remedy is to amend ONE line of the block or "
                        "rewrite it so it does not depend on alignment. A "
                        "distinct code because `wrapped_match:true` is also "
                        "true on the benign case, so it cannot be branched on. "
                        "Differing INDENTATION is not the trigger — a bullet's "
                        "hanging indent differs and is an ordinary hard wrap. "
                        "op:\"amend_headline\" is the mirror: it edits the "
                        "HEADLINE text only, with the body out of scope. A "
                        "headline is not immutable metadata (a phase prefix, "
                        "a typo, a renamed subsystem), and without this op a "
                        "four-word change forced a native Read of the whole "
                        "ROADMAP.md — file_outline / read_region do NOT "
                        "satisfy the native Edit tool's read-precondition. It "
                        "works on BOTH backends, by different routes. On a "
                        "MARKDOWN project it patches the bullet line and "
                        "refuses bad_args on a `new_text` that would alter the "
                        "`- <emoji> [ID] **` prefix or the bold delimiters, "
                        "which makes it strictly safer than the hand-edit it "
                        "replaces. ANTS-4668 — on a STORE-BACKED project it "
                        "writes the `headline` COLUMN and re-renders, the same "
                        "path op:\"flip\" and op:\"annotate\" already take. "
                        "There is no prefix guard on that route because the "
                        "column holds the headline TEXT alone (the marker, id "
                        "brackets, emoji and bold delimiters are composed by "
                        "the render); the guard there is bad_args on a "
                        "`new_text` that would leave the headline EMPTY, which "
                        "no locate could find again. It formerly refused "
                        "unsupported_format on a store-backed project, leaving "
                        "a headline immutable for the life of the item while "
                        "body notes accumulated a correction the headline went "
                        "on contradicting. Refusals are "
                        "headline_match_not_found / headline_match_ambiguous; "
                        "the store route echoes the new `headline`. "
                        "ANTS-4097: the success "
                        "envelope echoes `body_paragraph` (the edited line "
                        "with its wrapped neighbours) so you can read the "
                        "joint result without re-reading the file. It was "
                        "written because a wrapped phrase then took N "
                        "single-line calls, each succeeding, each looking "
                        "right alone, with the paragraph they jointly produced "
                        "checked by nothing — ANTS-4550 removes that split "
                        "rather than mitigating it, and the echo stays. "
                        "op:\"replace_text\" (ANTS-5760) — the literal text "
                        "to replace at EVERY occurrence in every body in "
                        "scope; exact, so a phrase split by a wrap is missed.");
                    QJsonObject newTextProp;
                    newTextProp["type"]      = "string";
                    newTextProp["maxLength"] = 65536;   // ANTS-4841
                    newTextProp["description"] = QStringLiteral(
                        "op:\"amend_body\" (ANTS-3406) — replacement text for "
                        "the unique `old_text` match. Required (the key must "
                        "be present); an empty string deletes the matched "
                        "phrase. Scrubbed of leaked tool-call XML like a "
                        "`note`. ANTS-4576 — and guarded like one: naming a "
                        "trailer key mid-line (`Kind:`, `Source:`, `Lanes:`) "
                        "refuses body_shadowed, because the body this text "
                        "lands in is re-parsed for those keys. Backtick the "
                        "key to mention it; put it first on its own line to "
                        "DECLARE it. DELETING a declaration is safe and no "
                        "longer refuses: `kind` and `source` cannot be empty, "
                        "so the column keeps its value, the render re-emits "
                        "the line canonically below the body, and the envelope "
                        "says `trailer_columns_kept`; `lanes` / `evidence` "
                        "empty to `[]`; `layman`, the only nullable one, "
                        "clears. op:\"replace_text\" (ANTS-5760) takes it "
                        "with the same guard, for every match. "
                        "ANTS-4841 — capped at 4096 characters for "
                        "amend_body and amend_headline, which take a fragment, "
                        "and 65536 for set_body, which takes a whole body.");

                    // ANTS-1690 — flip_batch locators array. Each item
                    // carries one locator (id|anchor|headline|line_range)
                    // plus optional per-locator note + no_anchor opt-out.
                    QJsonObject locItem;
                    locItem["type"] = "object";
                    QJsonObject locItemProps;
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "Bold-ID / [PREFIX-NNNN] locator for this bullet.");
                        locItemProps["id"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "Caret-anchor locator (GFM only).");
                        locItemProps["anchor"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "Headline locator (hash-matched, like op:\"flip\").");
                        locItemProps["headline"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "array";
                        QJsonObject ip; ip["type"] = "integer";
                        p["items"] = ip;
                        p["minItems"] = 2;
                        p["maxItems"] = 2;
                        p["description"] = QStringLiteral(
                            "[start,end] 1-based line range; flips every "
                            "bullet whose line falls inside it.");
                        locItemProps["line_range"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["maxLength"] = 4000;
                        p["description"] = QStringLiteral(
                            "Optional per-locator resolution note appended "
                            "to this bullet's body (scrubbed like a flip "
                            "note). Lets each bundle bullet carry its own "
                            "closure line.");
                        locItemProps["note"] = p;
                    }
                    {
                        // ANTS-5358 — a mixed-status batch in one call.
                        QJsonObject p;
                        p["type"] = "string";
                        p["enum"] = lifecycleEnum;
                        p["description"] = QStringLiteral(
                            "op:\"flip_batch\" — this locator's own target "
                            "status; the call-level `to_status` is the "
                            "fallback, and may be omitted when every locator "
                            "carries one (ANTS-5358). Refused on "
                            "op:\"annotate_batch\" and on a pass-headings "
                            "roadmap.");
                        locItemProps["to_status"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "boolean";
                        p["description"] = QStringLiteral(
                            "If true, do NOT inject a caret anchor on a GFM "
                            "bullet that lacks an id/anchor (keeps a "
                            "narrator-format section anchor-free).");
                        locItemProps["no_anchor"] = p;
                    }
                    {   // ANTS-4669 — op:"amend_batch"'s per-item edit.
                        QJsonObject o; o["type"] = "string";
                        o["description"] = QStringLiteral(
                            "op:\"amend_batch\" — the exact text to replace in this "
                            "item's stored body, as amend_body's `old_text`. Each "
                            "locator is {id, old_text, new_text}; one read, one "
                            "commit; a refused locator lands in skipped[].");
                        locItemProps["old_text"] = o;
                        QJsonObject n; n["type"] = "string";
                        n["description"] = QStringLiteral(
                            "op:\"amend_batch\" — the replacement, as amend_body's "
                            "`new_text`.");
                        locItemProps["new_text"] = n;
                    }
                    locItem["properties"] = locItemProps;
                    QJsonObject locatorsProp;
                    locatorsProp["type"]  = "array";
                    locatorsProp["items"] = locItem;
                    locatorsProp["description"] = QStringLiteral(
                        "Required under op:\"flip_batch\": the bullets to "
                        "flip to `to_status`. Each is one locator "
                        "(id|anchor|headline|line_range) + optional `note` "
                        "and `no_anchor`. Single read + single atomic "
                        "commit across all of them. ANTS-4470 — also "
                        "required, in the SAME shape, under "
                        "op:\"annotate_batch\", where `note` stops being "
                        "optional: it is the whole operation, and a "
                        "locator without one is refused into `skipped[]`. "
                        "`no_anchor` is inert there (annotate injects no "
                        "anchor), as is any `to_status`, which that op "
                        "refuses at the top level.");

                    // ANTS-1879 — bullets[] array for op:"append_batch".
                    // Each element carries the same shape as the
                    // single-bullet path's top-level fields.
                    QJsonObject bulletItem;
                    bulletItem["type"] = "object";
                    QJsonObject bulletItemProps;
                    {
                        QJsonObject p = headlineProp;
                        bulletItemProps["headline"] = p;
                    }
                    {
                        QJsonObject p = statusProp;
                        bulletItemProps["status"] = p;
                    }
                    {
                        QJsonObject p = kindProp;
                        bulletItemProps["kind"] = p;
                    }
                    {
                        QJsonObject p = sourceProp;
                        bulletItemProps["source"] = p;
                    }
                    {
                        QJsonObject p = bodyProp;
                        bulletItemProps["body"] = p;
                    }
                    {
                        QJsonObject p = laymanProp;
                        bulletItemProps["layman"] = p;
                    }
                    {
                        QJsonObject p = lanesProp;
                        bulletItemProps["lanes"] = p;
                    }
                    {
                        QJsonObject p = evidenceProp;  // ANTS-3382
                        bulletItemProps["evidence"] = p;
                    }
                    {
                        QJsonObject p = idHintProp;
                        p["description"] = QStringLiteral(
                            "Optional explicit ID under op:\"append_batch\". "
                            "Honoured ONLY on the first bullet (later "
                            "bullets follow first_id+i to keep the "
                            "allocation contiguous). Counter strategy only.");
                        bulletItemProps["id_hint"] = p;
                    }
                    {
                        // ANTS-2078 — per-bullet stable id. Required on
                        // every bullet when the batch-wide id_strategy is
                        // "stable_prefix"; the counter is skipped and this
                        // string is written verbatim.
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "Full stable id string for THIS bullet when "
                            "the top-level id_strategy=\"stable_prefix\" "
                            "(e.g. \"Ts20-SP6\"). Must match "
                            "^[A-Za-z][A-Za-z0-9_-]+$ and be unique within "
                            "the batch. Ignored under the counter strategy "
                            "(ANTS-2078).");
                        bulletItemProps["stable_id"] = p;
                    }
                    {   // ANTS-5344 — per-bullet section.
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "This bullet's section slug; the call-level "
                            "`section` is the fallback, and may be omitted "
                            "when every bullet names one. Store-served "
                            "projects only: a markdown or pass-headings "
                            "roadmap refuses unsupported_format when a "
                            "bullet's section differs from the batch's.");
                        bulletItemProps["section"] = p;
                    }
                    {   // ANTS-4354 — per-bullet pass designator.
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "Pass designator for THIS bullet on a "
                            "pass-headings roadmap (e.g. \"43.5\"), mirroring "
                            "`stable_id`. This is what lets a batch of N "
                            "passes name N designators rather than sharing "
                            "one; the call-level `pass` is the fallback for "
                            "any bullet omitting it. Ignored on GFM / ants-v1 "
                            "roadmaps.");
                        bulletItemProps["pass"] = p;
                    }
                    bulletItem["properties"] = bulletItemProps;
                    QJsonObject bulletsProp;
                    bulletsProp["type"]  = "array";
                    bulletsProp["items"] = bulletItem;
                    bulletsProp["description"] = QStringLiteral(
                        "Required under op:\"append_batch\": the bullets "
                        "to append to `section`. Single read + single "
                        "atomic commit across all of them. Per-bullet "
                        "validation failures land in `skipped[]` while "
                        "accepted bullets still apply (semantic parity "
                        "with flip_batch). ANTS-4580 — `{{id:N}}` in a "
                        "headline, body or layman becomes the id bullet N "
                        "(0-based) receives, so siblings can cite each "
                        "other; one naming a skipped or missing bullet "
                        "refuses the whole call.");

                    // ANTS-1878 — create_section params.
                    QJsonObject afterSectionProp;
                    afterSectionProp["type"] = "string";
                    afterSectionProp["description"] = QStringLiteral(
                        "Required under op:\"create_section\": slug of "
                        "an existing ## / ### heading. The new heading "
                        "is inserted at this section's end. ANTS-5315 — "
                        "optional for a level-2 release title such as "
                        "\"0.10.0 — Theme\": omitted, it is placed after "
                        "the greatest lower version (before the first release "
                        "when it is the lowest) and the reply carries "
                        "after_section or before_section with "
                        "placed_by_version:true. Also "
                        "op:\"move_section\"'s destination (ANTS-4958).");
                    QJsonObject beforeSectionProp;
                    beforeSectionProp["type"] = "string";
                    beforeSectionProp["description"] = QStringLiteral(
                        "op:\"move_section\" — put the section before this "
                        "slug instead of after `after_section` (ANTS-4958).");
                    QJsonObject levelProp;
                    levelProp["type"] = "integer";
                    QJsonArray levelEnum;
                    levelEnum.append(2);
                    levelEnum.append(3);
                    levelProp["enum"] = levelEnum;
                    levelProp["description"] = QStringLiteral(
                        "Required under op:\"create_section\": heading "
                        "depth (2 for `##`, 3 for `###`).");
                    QJsonObject titleProp;
                    titleProp["type"] = "string";
                    titleProp["maxLength"] = 200;
                    titleProp["description"] = QStringLiteral(
                        "Required under op:\"create_section\": literal "
                        "heading text. The slug is computed from it via "
                        "the same transform RoadmapIndex uses.");
                    QJsonObject introBodyProp;
                    introBodyProp["type"]      = "string";
                    introBodyProp["maxLength"] = 4000;
                    introBodyProp["description"] = QStringLiteral(
                        "Optional under op:\"create_section\": intro "
                        "paragraph(s) for the new section. Single "
                        "newlines = paragraph breaks; hard-wrapped at "
                        "80 cols on word boundaries. Lines matching "
                        "^#{1,6}\\s (a Markdown heading) refuse with "
                        "bad_intro.");

                    // ANTS-1905 — id_strategy + stable_id for projects
                    // that use stable string IDs (Sh4, Ts20-FL1, MT8…)
                    // instead of [PROJ-NNNN]. Optional escape hatch
                    // around .roadmap-counter for op:"append" so a
                    // project that never had a counter can still drive
                    // the verb from MCP instead of falling back to Edit.
                    QJsonObject idStrategyProp;
                    idStrategyProp["type"] = "string";
                    {
                        QJsonArray e;
                        e.append("counter");
                        e.append("stable_prefix");
                        idStrategyProp["enum"] = e;
                    }
                    idStrategyProp["description"] = QStringLiteral(
                        "Allocator strategy under op:\"append\" / "
                        "\"append_batch\". \"counter\" (default) bumps "
                        ".roadmap-counter and renders an [ANTS-NNNN]-style "
                        "id. \"stable_prefix\" requires `stable_id` "
                        "(op:\"append\") or a per-bullet `stable_id` on "
                        "EVERY bullet (op:\"append_batch\", ANTS-2078) and "
                        "writes each bullet with that id verbatim — skip "
                        "the counter for projects that use stable string "
                        "IDs (Sh4, Ts20-FL1, MT8…) (ANTS-1905). When "
                        ".roadmap-counter is missing and stable IDs are "
                        "detected, the counter path's refusal envelope "
                        "(`stable_prefix_unsupported`) points the caller "
                        "at this strategy.");
                    QJsonObject stableIdProp;
                    stableIdProp["type"] = "string";
                    stableIdProp["description"] = QStringLiteral(
                        "Full stable id string when id_strategy="
                        "\"stable_prefix\" (e.g. \"Ts20-SP6\"). Must "
                        "match ^[A-Za-z][A-Za-z0-9_-]+$. Not accepted "
                        "under the default counter strategy.");

                    // ANTS-2076 — explicit counter-ID prefix override.
                    QJsonObject idPrefixProp;
                    idPrefixProp["type"]    = "string";
                    // ANTS-3492 — prefix may be digit-led if letter-containing.
                    idPrefixProp["pattern"] =
                        "^(?=[A-Za-z0-9_-]*[A-Za-z])[A-Za-z0-9][A-Za-z0-9_-]{0,15}$";
                    idPrefixProp["description"] = QStringLiteral(
                        "Optional counter-ID prefix for op:\"append\" / "
                        "\"append_batch\" (counter strategy). Overrides "
                        "BOTH the prefix sniffed from existing IDs and "
                        "the project-dir default. Use it on a fresh / "
                        "id-less roadmap to pin the project's prefix "
                        "(e.g. \"DOOM\" → DOOM-0001). When omitted, the "
                        "prefix is sniffed from existing IDs, else "
                        "derived from caller_cwd's leaf directory "
                        "(uppercase first 4 chars). Ignored under "
                        "id_strategy:\"stable_prefix\".");

                    // ANTS-2077 — dry_run preview flag; ANTS-2136 and
                    // ANTS-3798 extended it to every op. A new op must
                    // support it too, since the text below says "every op".
                    QJsonObject dryRunProp;
                    dryRunProp["type"] = "boolean";
                    dryRunProp["description"] = QStringLiteral(
                        "Optional. Supported on every op. Returns the "
                        "resolved preview WITHOUT "
                        "writing ROADMAP.md or bumping .roadmap-counter — a "
                        "free pre-flight to verify prefix / format / section / "
                        "locator before committing. Envelope carries "
                        "dry_run:true and reports `bytes` (would-be) in place "
                        "of bytes_written; append/append_batch add the "
                        "would-be id under `would_be_id` / `would_be_ids` "
                        "(ANTS-4508 — NOT `id`/`ids`, which a caller reading "
                        "one field takes for a reservation; nothing is "
                        "reserved and the id is wrong if any other write "
                        "intervenes), formatted bullet(s) and 1-based "
                        "insertion line(s), append_batch reporting "
                        "applied_count:0 + would_apply_count; bundle_row adds "
                        "the rendered `row` plus the row_index / columns / "
                        "created_table the write would have produced. "
                        // ANTS-4463 — the past-tense fields are ABSENT on a
                        // dry run rather than carrying would-be values under
                        // a name that asserts the write happened.
                        "ANTS-4463 — on a store-backed project a dry run emits "
                        "`would_write:[...]` and (where a note was supplied) "
                        "`note_would_append`; the past-tense `files_written` "
                        "and `note_appended` are ABSENT, so a caller that "
                        "branches on either cannot read a preview as a "
                        "completed write. "
                        // ANTS-4548 — measured: the preview RUNS the mutation.
                        "ANTS-4548 — on a store-backed project the preview is "
                        "not a simulation: the store mutation runs inside the "
                        "transaction and is rolled back, so a column CHECK, a "
                        "NOT NULL refusal, the render gate and the divergence "
                        "guard all reach a dry run exactly as they reach the "
                        "real call. What it does NOT cover is the COMMIT and "
                        "the file publish after it — a preview cannot tell you "
                        "the write to disk will land.");

                    // ANTS-2126 — pass designator for op:"append" on a
                    // `#### Pass N.M` heading roadmap.
                    QJsonObject passProp;
                    passProp["type"] = "string";
                    passProp["description"] = QStringLiteral(
                        "Pass designator for op:\"append\" / "
                        "op:\"append_batch\" on a pass-headings "
                        "(`#### Pass N.M`) roadmap (ANTS-2126), e.g. \"43.5\" "
                        "or \"43.5.B\"; validated "
                        "^\\d+\\.\\d+(?:\\.[A-Za-z][A-Za-z0-9]*)?$. REQUIRED "
                        "when the target is a pass-headings roadmap; ignored "
                        "on GFM / ants-v1 roadmaps. Flip/annotate locate a "
                        "pass by its synthesised `PASS-N-M` id (via the `id` "
                        "locator) or `headline`, not via `pass`. "
                        "ANTS-4354 — under op:\"append_batch\" a designator "
                        "may be set PER BULLET (a `pass` on the bullets[] "
                        "item, mirroring `stable_id`), so a batch of N passes "
                        "can name N designators; this call-level value is the "
                        "FALLBACK for any bullet that carries none. Previously "
                        "the batch read neither this nor an item `pass` from "
                        "the schema's point of view, so every bullet refused "
                        "bad_args and the op could write nothing at all on "
                        "this dialect. "
                        "ANTS-4117 — what append RENDERS on this format is "
                        "fixed: `#### Pass <pass> <headline>` + `- "
                        "**Status**: <keyword>` + `body` VERBATIM (not "
                        "indented for you), where <keyword> is the canonical "
                        "todo / in-progress / done / deferred, NOT the "
                        "`status` word you passed; `kind` / `source` / "
                        "`lanes` / `layman` have no slot and are ignored. A "
                        "trailing `---` is emitted only when the file already "
                        "separates its pass blocks that way. Pass "
                        "`dry_run:true` (or read the `bullet` field the write "
                        "envelope echoes) to see the exact block.");

                    // ANTS-2080 — confirm-after compact echo.
                    QJsonObject returnProp;
                    returnProp["type"] = "string";
                    {
                        QJsonArray e;
                        e.append("default");
                        e.append("headline_only");
                        e.append("full");   // ANTS-5263
                        returnProp["enum"] = e;
                    }
                    returnProp["description"] = QStringLiteral(
                        "Optional. \"headline_only\" on op:\"append\" / "
                        "\"append_batch\" / \"flip\" / \"flip_batch\" adds "
                        "`post_bullets` to the success envelope — the "
                        "just-touched bullet(s) in the compact {id, status, "
                        "headline_oneline} shape roadmap_query "
                        "mode:\"headline_only\" emits — so a confirm-after "
                        "read folds into the write (no follow-up "
                        "roadmap_query). \"default\" (omitted) keeps the "
                        "lean envelope (ANTS-2080 append; ANTS-2089 flip). "
                        "On a store-served flip or annotate the default "
                        "already carries `post_bullets`, and \"full\" "
                        "echoes the whole rendered bullet instead "
                        "(ANTS-5263).");

                    // ANTS-3432 — op:"bundle_row" params. The handler
                    // (cmdRoadmapLogBundleRow) has always read these, but
                    // they were never declared here, so with
                    // additionalProperties:false the client stripped them
                    // and `cells` reached the handler empty → missing_field.
                    // Declaring them completes the ANTS-1691 wiring.
                    QJsonObject cellsProp;
                    cellsProp["type"] = "array";
                    {
                        QJsonObject item;
                        item["type"] = "string";
                        cellsProp["items"] = item;
                    }
                    cellsProp["description"] = QStringLiteral(
                        "op:\"bundle_row\" — the row's cells, one string per "
                        "column (non-empty). Must match the table's column "
                        "count (or `header`'s length when creating the "
                        "table), else column_mismatch. Each cell is "
                        "pipe-escaped and its newlines folded to <br> so a "
                        "`|` inside a cell can't corrupt the column count.");

                    QJsonObject headerProp;
                    headerProp["type"] = "array";
                    {
                        QJsonObject item;
                        item["type"] = "string";
                        headerProp["items"] = item;
                    }
                    headerProp["description"] = QStringLiteral(
                        "op:\"bundle_row\" — optional column names. When the "
                        "section has no Markdown table yet, `header` creates "
                        "one (header row + separator + the first data row); "
                        "when a table exists it is validated against the "
                        "column count. Omit to append to an existing table.");

                    QJsonObject positionProp;
                    positionProp["type"] = "string";
                    {
                        QJsonArray e;
                        e.append("end");
                        e.append("sorted");
                        positionProp["enum"] = e;
                    }
                    positionProp["description"] = QStringLiteral(
                        "op:\"bundle_row\" — where to insert the new row. "
                        "\"end\" (default) appends after the last data row; "
                        "\"sorted\" inserts in ascending order by `sort_col` "
                        "(numeric-aware collation) to keep the table sorted.");

                    QJsonObject sortColProp;
                    sortColProp["type"]    = "integer";
                    sortColProp["minimum"] = 0;
                    sortColProp["description"] = QStringLiteral(
                        "op:\"bundle_row\" — 0-based column index used when "
                        "`position` is \"sorted\". Ignored otherwise.");

                    QJsonObject props;
                    props["caller_cwd"]    = callerProp;
                    props["op"]            = opProp;
                    props["cells"]         = cellsProp;     // ANTS-3432
                    props["header"]        = headerProp;    // ANTS-3432
                    props["position"]      = positionProp;  // ANTS-3432
                    props["sort_col"]      = sortColProp;   // ANTS-3432
                    props["locators"]      = locatorsProp;
                    // ANTS-5385 — amendments[] for op:"amend_field_batch".
                    {
                        QJsonObject entryProps;
                        QJsonObject idP;    idP["type"] = "string";
                        QJsonObject fieldP; fieldP["type"] = "string";
                        fieldP["enum"] = QJsonArray{"layman", "kind", "source",
                                                    "lanes", "evidence"};
                        QJsonObject valueP;
                        valueP["type"] = QJsonArray{"string", "array"};
                        entryProps["id"]    = idP;
                        entryProps["field"] = fieldP;
                        entryProps["value"] = valueP;
                        QJsonObject entry;
                        entry["type"]       = "object";
                        entry["properties"] = entryProps;
                        entry["required"]   = QJsonArray{"id", "field", "value"};
                        QJsonObject amendmentsProp;
                        amendmentsProp["type"]  = "array";
                        amendmentsProp["items"] = entry;
                        amendmentsProp["description"] = QStringLiteral(
                            "op:\"amend_field_batch\" — {id, field, value} "
                            "entries, each checked as op:\"amend_field\" "
                            "checks one, all written in ONE commit and render, "
                            "so the Layman gate judges the final state. A "
                            "refused entry lands in skipped[] with its index; "
                            "all refused writes nothing. At most 500. To move "
                            "items, use op:\"amend_field\" field:\"section\" "
                            "with locators[], one call per destination.");
                        props["amendments"] = amendmentsProp;
                    }
                    props["bullets"]       = bulletsProp;
                    props["section"]       = sectionProp;
                    props["after_section"] = afterSectionProp;
                    props["before_section"] = beforeSectionProp;  // ANTS-4958
                    props["level"]         = levelProp;
                    props["title"]         = titleProp;
                    props["intro_body"]    = introBodyProp;
                    {   // ANTS-4081 — op:"rotate_minor"'s one argument.
                        QJsonObject minorProp;
                        minorProp["type"]        = "string";
                        minorProp["pattern"]     = "^[0-9]+\\.[0-9]+$";
                        minorProp["description"] =
                            "op:\"rotate_minor\" — the closed minor, "
                            "<MAJOR>.<MINOR> (\"0.7\").";
                        props["minor"] = minorProp;
                    }
                    {   // ANTS-5379 — the element ops' address.
                        QJsonObject elemPosProp;
                        elemPosProp["type"]    = "integer";
                        elemPosProp["minimum"] = 0;
                        elemPosProp["description"] = QStringLiteral(
                            "op:\"amend_element\" / \"delete_element\" / "
                            "\"promote_element\" — the element's `position` "
                            "as op:\"list_elements\" reports it. list_elements "
                            "returns a section's items, tables and narration in "
                            "order; amend_element replaces a narration's text "
                            "(`new_text`); delete_element removes a narration or "
                            "table; promote_element files a narration as an item "
                            "at the same place, taking op:\"append\"'s fields "
                            "(headline defaults to the narration's first line). "
                            "An item element is refused element_kind_refused.");
                        props["element_position"] = elemPosProp;
                        QJsonObject preambleProp;
                        preambleProp["type"] = "boolean";
                        preambleProp["description"] = QStringLiteral(
                            "The element ops: address the elements under the "
                            "roadmap's preamble instead of a `section`.");
                        props["preamble"] = preambleProp;
                        // ANTS-5615 — op:"set_legend"'s operand.
                        QJsonObject legendProp;
                        legendProp["type"] = "object";
                        legendProp["description"] = QStringLiteral(
                            "op:\"set_legend\" — {status: wording} for the status "
                            "legend under the preamble (planned, in-progress, "
                            "shipped, considered, dropped). Named statuses change, "
                            "others keep theirs; an empty string removes one. "
                            "Wording is one line opening with Done, In progress, "
                            "Planned, Considered or Dropped. Store-only, dry_run "
                            "previewable; echoes legend and previous_legend.");
                        props["legend"] = legendProp;
                    }
                    {   // ANTS-4079 — op:"link" / op:"unlink" operands.
                        QJsonObject linkTypeProp;
                        linkTypeProp["type"] = "string";
                        linkTypeProp["enum"] = QJsonArray{
                            QStringLiteral("splits-from"), QStringLiteral("blocked-by"),
                            QStringLiteral("duplicate-of"), QStringLiteral("supersedes")};
                        linkTypeProp["description"] = QStringLiteral(
                            "op:\"link\" / \"unlink\" — the relationship `id` has "
                            "to each of `targets`. relates-to and specified-by are "
                            "not written here: they come from the body's "
                            "Dependencies: and Spec: lines.");
                        props["type"] = linkTypeProp;
                        QJsonObject linkTargetsProp;
                        linkTargetsProp["type"]     = "array";
                        linkTargetsProp["minItems"] = 1;
                        linkTargetsProp["items"]    = QJsonObject{{"type", "string"}};
                        linkTargetsProp["description"] = QStringLiteral(
                            "op:\"link\" / \"unlink\" — the target ids, in this or "
                            "any registered project. An unfiled id of this project "
                            "refuses link_target_not_found; one of another project "
                            "is stored for when it is filed.");
                        props["targets"] = linkTargetsProp;
                    }
                    props["status"]        = statusProp;
                    props["to_status"]     = toStatusProp;
                    props["headline"]      = headlineProp;
                    props["kind"]          = kindProp;
                    props["source"]        = sourceProp;
                    props["body"]          = bodyProp;
                    props["layman"]        = laymanProp;
                    props["lanes"]         = lanesProp;
                    props["evidence"]      = evidenceProp;   // ANTS-3382
                    props["id_hint"]       = idHintProp;
                    props["id"]            = idProp;
                    props["anchor"]        = anchorProp;
                    props["prefix_hint"]   = prefixHintProp;
                    props["note"]          = noteProp;
                    // ANTS-4667 — op:"amend_field" operands.
                    QJsonObject fieldProp;
                    fieldProp["type"] = QStringLiteral("string");
                    fieldProp["enum"] = QJsonArray{
                        QStringLiteral("layman"), QStringLiteral("kind"),
                        QStringLiteral("source"), QStringLiteral("lanes"),
                        QStringLiteral("evidence"), QStringLiteral("section")};
                    fieldProp["description"] = QStringLiteral(
                        "op:\"amend_field\" — which trailer COLUMN to "
                        "write, or `section` to move the item to the section "
                        "`value` names (ANTS-4948). `headline` is op:\"amend_headline\" and "
                        "`status` is op:\"flip\"; body prose is "
                        "op:\"amend_body\". Only `layman` is nullable: "
                        "kind and source are NOT NULL with no default, so "
                        "an empty value refuses bad_args rather than "
                        "reaching the caller as a raw constraint string.");
                    QJsonObject valueProp;
                    { QJsonArray ty; ty.append(QStringLiteral("string"));
                                     ty.append(QStringLiteral("array"));
                      valueProp["type"] = ty; }
                    valueProp["description"] = QStringLiteral(
                        "op:\"amend_field\" — the replacement value. The "
                        "key must be PRESENT; for `layman` an empty string "
                        "clears the column. `lanes` / `evidence` take an "
                        "array of strings, or the comma-separated form the "
                        "trailer line uses — both are accepted, and the "
                        "stored bytes are canonicalised either way.");
                    QJsonObject beforeIdProp;
                    beforeIdProp["type"] = QStringLiteral("string");
                    beforeIdProp["description"] = QStringLiteral(
                        "op:\"amend_field\" field:\"section\" only — file the "
                        "moved items, in locator order, directly before this "
                        "item, which must be filed in the destination. It "
                        "may be the items' own section, which reorders them. "
                        "Omitted, they go after the section's last element. "
                        "Any other field refuses bad_args (ANTS-5380).");
                    props["field"]         = fieldProp;      // ANTS-4667
                    props["value"]         = valueProp;      // ANTS-4667
                    props["before_id"]     = beforeIdProp;   // ANTS-5380
                    props["old_text"]      = oldTextProp;     // ANTS-3406
                    props["new_text"]      = newTextProp;     // ANTS-3406
                    props["id_strategy"]   = idStrategyProp;  // ANTS-1905
                    props["stable_id"]     = stableIdProp;    // ANTS-1905
                    props["id_prefix"]     = idPrefixProp;    // ANTS-2076
                    props["dry_run"]       = dryRunProp;      // ANTS-2077
                    {   // ANTS-5315
                        QJsonObject sampleLimitProp;
                        sampleLimitProp["type"] = QStringLiteral("integer");
                        sampleLimitProp["description"] = QStringLiteral(
                            "place_by_release: cap on each sample array in the "
                            "reply, 1-200, default 50. Counts are never capped.");
                        props["sample_limit"] = sampleLimitProp;
                    }
                    {   // ANTS-5704
                        QJsonObject planIdProp;
                        planIdProp["type"] = QStringLiteral("string");
                        planIdProp["description"] = QStringLiteral(
                            "Apply a dry run without resending it: a successful "
                            "dry run returns `plan_id`, and a call carrying only "
                            "caller_cwd and that id runs the previewed request "
                            "for real, re-running every check. Any other payload "
                            "argument refuses bad_args. A plan is used once, by "
                            "an apply that lands; it lasts 15 minutes and a "
                            "server restart forgets it, both refusing "
                            "plan_not_found.");
                        props["plan_id"] = planIdProp;
                    }
                    props["return"]        = returnProp;      // ANTS-2080
                    props["pass"]          = passProp;        // ANTS-2126
                    {   // ANTS-4487 § 4.5
                        QJsonObject forceProp;
                        forceProp["type"] = QStringLiteral("boolean");
                        forceProp["description"] = QStringLiteral(
                            "op:\"append\" / \"append_batch\" — file a bullet whose "
                            "headline exactly matches an existing item. Without it "
                            "that append refuses `duplicate_item` (a batch skips "
                            "the entry), naming the match in `duplicate_of`.");
                        props["force"] = forceProp;
                    }
                    QJsonObject stripRunsProp;
                    stripRunsProp["type"] = "boolean";
                    stripRunsProp["description"] = QStringLiteral(
                        "op:\"repair_trailers\" — also remove the legacy "
                        "trailer run a stored body ENDS in, where every value "
                        "it carries equals its column. A run that disagrees "
                        "is listed in strip_skipped_ids and never removed. "
                        "Default false.");
                    props["strip_runs"]    = stripRunsProp;   // ANTS-4507
                    QJsonObject clearPlaceholderProp;
                    clearPlaceholderProp["type"] = "boolean";
                    clearPlaceholderProp["description"] = QStringLiteral(
                        "op:\"repair_trailers\" — also blank a `source` of "
                        "\"planned\" that is asserted and undated (the "
                        "ANTS-1129 backfill), and remove a body line declaring "
                        "only it. Caller's project only. Reports "
                        "placeholder_sources_cleared, placeholder_lines_removed "
                        "and placeholder_cleared_ids. Default false.");
                    props["clear_placeholder_source"] = clearPlaceholderProp;   // ANTS-4595
                    QJsonObject acceptLossProp;
                    acceptLossProp["type"] = "boolean";
                    acceptLossProp["description"] = QStringLiteral(
                        "op:\"convert\" — proceed although the rewrite would "
                        "drop text the file holds. Without it a real convert "
                        "refuses `text_lost`, naming the lines, and writes "
                        "nothing. A dry run never refuses; it reports. Pass "
                        "only after inspecting the loss. Default false.");
                    props["accept_text_loss"] = acceptLossProp; // ANTS-5286
                    QJsonObject acceptAmbigProp;
                    acceptAmbigProp["type"] = "boolean";
                    acceptAmbigProp["description"] = QStringLiteral(
                        "op:\"convert\" — proceed although some bullets share "
                        "their section and headline with several stored "
                        "items and would be paired by order. Without it a "
                        "real convert refuses `ambiguous_rematch`, listing "
                        "each with `candidate_ids`. Default false.");
                    props["accept_ambiguous_rematch"] = acceptAmbigProp; // ANTS-5329
                    QJsonObject maxPlannedProp;
                    maxPlannedProp["type"] = "integer";
                    maxPlannedProp["minimum"] = 0;
                    maxPlannedProp["description"] = QStringLiteral(
                        "op:\"convert\" — cap on `ids.planned[]` rows "
                        "(default 200, clamped to 5000). 0 omits the rows and "
                        "keeps the counts. `planned_total` is the uncapped "
                        "count after `planned_filter`.");
                    props["max_planned"] = maxPlannedProp;       // ANTS-5328
                    QJsonObject plannedFilterProp;
                    plannedFilterProp["type"] = "string";
                    plannedFilterProp["enum"] =
                        QJsonArray{QStringLiteral("all"), QStringLiteral("needs_decision")};
                    plannedFilterProp["description"] = QStringLiteral(
                        "op:\"convert\" — `needs_decision` keeps only rows a "
                        "human must check: origin absent, id_inferred, or "
                        "ambiguous_rematch. Default `all`.");
                    props["planned_filter"] = plannedFilterProp; // ANTS-5328
                    QJsonObject backupToProp;
                    backupToProp["type"] = "string";
                    backupToProp["description"] = QStringLiteral(
                        "op:\"convert\" — the store snapshot's file path. "
                        "Default: pre-convert-<slug>-<UTC>.sqlite in the "
                        "snapshot folder.");
                    props["backup_to"] = backupToProp;           // ANTS-6724
                    schema["properties"]   = props;

                    // ANTS-1428 — only caller_cwd is unconditionally
                    // required across every op. op:"append" needs
                    // section+status+headline+kind+source; op:"flip"
                    // needs to_status + one locator. JSON Schema's
                    // top-level `required` can't express this without
                    // `oneOf`/`allOf`; the verb enforces the
                    // conditional logic at runtime via
                    // missing_field / bad_op_combo refusals.
                    QJsonArray req;
                    req.append(QStringLiteral("caller_cwd"));
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-3855 — roadmap_migrate: load one project's markdown
                // roadmap into the roadmap store. The only production entry
                // point into the migration engine (read half ANTS-3757, load
                // half ANTS-3765), so Required-contract gated like the other
                // whole-project write verbs.
                {
                    QJsonObject t;
                    t["name"] = "roadmap_migrate";
                    t["description"] = QStringLiteral(
                        "Load ONE project's markdown roadmap (ROADMAP.md + "
                        "docs/roadmap/ archives) into the machine-global roadmap store, "
                        "resolved from caller_cwd. dry_run:true plans, reports counts "
                        "and rolls back (it still opens the store). It never writes "
                        "ROADMAP.md. Read store_backed, updated_items[] and "
                        "notes_summary before acting on counts. A missing item is "
                        "DELETED (items_deleted). op:\"deregister\" removes "
                        "a project's roadmap and keeps its mailbox (confirm:true "
                        "while its root exists). Refusals: "
                        "no_project, no_roadmap, case_ambiguous, not_utf8, too_large, "
                        "format_mismatch, bad_args, slug_collision, store_failed, "
                        "migrate_failed, mass_deletion, confirm_required, "
                        "transient_root. caller_cwd "
                        "Required.");
                    t["detail"] = QStringLiteral(
                        "ANTS-4617 — op:\"deregister\" is the INVERSE and the "
                        "catalogue had none: migrating a scratch copy to test "
                        "something destructive in isolation is the careful "
                        "instinct, and it left a permanent row that "
                        "roadmap_query mode:\"report\" scope:\"all\" sums into "
                        "machine-wide figures forever. Keyed by caller_cwd's "
                        "root, or by `export_slug` when the files are already "
                        "gone. It REFUSES `confirm_required` while the root "
                        "still exists on disk, because the store is primary and "
                        "ROADMAP.md is its render — the rows are the only copy "
                        "of the history, relationships and citations the file "
                        "does not carry. Pass `confirm:true` to override; an "
                        "absent root needs no ceremony. `dry_run:true` reports "
                        "what would go WITHOUT deleting it. The delete is one "
                        "transaction in foreign-key order and rolls back whole "
                        "on any failure. ANTS-5366 — the project row and its "
                        "mail are KEPT, marked deregistered: mail still reaches "
                        "it, its export_slug stays taken, its id is never "
                        "reused, and migrating its root again revives it. "
                        "Distinct "
                        "from ANTS-4600's `transient_root` guard, which stops a "
                        "scratchpad under the system temp dir being registered "
                        "at all. "
                        "Load ONE project's markdown roadmap (ROADMAP.md + any "
                        "rotated archives under docs/roadmap/) into the roadmap "
                        "store, resolved from caller_cwd. One project per call. "
                        "`dry_run:true` plans every write, reports the counts and "
                        "rolls back — note it still OPENS the store, and creates "
                        "an EMPTY schema if none exists, because its counts are a "
                        "diff against existing rows. Optional project_name / "
                        "export_slug default from caller_cwd's leaf directory "
                        "(export_slug slugified); a supplied export_slug is "
                        "validated verbatim, never rewritten, and must match "
                        "[a-z0-9][a-z0-9-]*. Re-running over an unchanged project "
                        "is idempotent in INTENT — but parse(render(x)) is not "
                        "yet identity (ANTS-4507), so `items_updated` can count "
                        "a stored column that disagrees with its own body text "
                        "rather than drift in the file. Read `updated_items[]` "
                        "before acting on a non-zero count: each entry's "
                        "`values[]` gives every column's stored `from` and "
                        "planned `to` text around the first difference "
                        "(`diff_at`). Re-running "
                        "with a different name or slug "
                        "is refused rather than silently applied. ANTS-4482 — it "
                        "NEVER writes ROADMAP.md, it only reads: the file is "
                        "first re-rendered by the next roadmap_log write, so a "
                        "byte-identical file and a clean `git status` right after "
                        "a migration are EXPECTED and are not a sign it did not "
                        "run (query the store to confirm). ANTS-4490 — and only "
                        "an `ants-v1` roadmap is SERVED from the store "
                        "afterwards: a github-task-list or pass-headings project "
                        "migrates ok:true and roadmap_query keeps answering "
                        "source:\"markdown\". Both facts are now FIELDS a caller "
                        "can branch on rather than prose it must read: "
                        "`store_backed` (will roadmap_query serve this project "
                        "from the store afterwards — the source-dialect answer, "
                        "unchanged by dry_run) and `markdown_rewritten` (always "
                        "false). ANTS-4479 — `updated_items:[{id,fields,values}]` names "
                        "WHICH items `items_updated` counted and which columns "
                        "moved, so a dry run is a reviewable plan rather than a "
                        "bare count; capped at 200 with `updated_items_truncated`, "
                        "and `items_updated` stays the true total. ANTS-4490 — "
                        "`sections_unchanged` accompanies `sections_written`, so "
                        "`0 written` reads as an idempotent re-run rather than as "
                        "a dead counter. ANTS-4694 — `elements_written` counts "
                        "the NON-ITEM content carried: prose, older-dialect "
                        "bullets, tables and legends all become `narration` / "
                        "`table` elements under the section they sat in. The "
                        "item count is therefore NOT a coverage figure, and on "
                        "a prose-heavy roadmap it is small by construction — a "
                        "`coverage_hint` says so on the arm where that could "
                        "mislead (more elements than items). Nothing is "
                        "dropped: the line partition is total and tested as "
                        "one, so every non-blank source line lands in exactly "
                        "one item, element, section or legend. ANTS-4478 — "
                        "`project_id` is the real id "
                        "whenever a row for this root already exists, on the dry "
                        "path too; 0 means only that the root is not registered "
                        "yet, so `project_id > 0` on a preview reads as \"already "
                        "migrated\". ANTS-4559 — `notes[]`'s row bound is a "
                        "DEFAULT of 200 and `max_notes` moves it (clamped to "
                        "[1, 2000]; the envelope echoes the EFFECTIVE value). "
                        "`notes_summary` maps each note `code` to its total "
                        "count over ALL notes, tallied before the cap and "
                        "never truncated, so a caller holding a truncated "
                        "array can still name the population it did not "
                        "receive — `notes_count` is one scalar over every code "
                        "and cannot. Repetition collapses below the cap, so "
                        "what overflows it is a code whose `detail` is unique "
                        "per occurrence: `quarantined_id` carries the id "
                        "token. ANTS-4656 — a `field_conflict` note names the "
                        "item in `id` and only the column in `detail`, so one "
                        "column's conflicts collapse into one row carrying up "
                        "to three `sample_ids`; a merged row of line-less "
                        "notes omits `sample_lines`. ANTS-5394 — on a pass-headings source, a "
                        "`####` block with a Status line and no `Pass N.M` id "
                        "is prose, not an item; `unparsed_headings[]` names "
                        "each (heading, line, status_line) at the top level "
                        "when there is one; `unparsed_headings_count` is on "
                        "every reply, 0 included. The store is machine-global "
                        "(~/.local/share/ants-terminal/roadmap.sqlite), not "
                        "per-project. Refusals: "
                        "no_project, no_roadmap, case_ambiguous, not_utf8, "
                        "too_large (sources over 32 MiB), format_mismatch, bad_args, "
                        "slug_collision, store_failed, migrate_failed. caller_cwd Required.");
                    // ANTS-1453 — selection_hint: one sentence on WHEN to
                    // reach for this verb rather than what it does.
                    // ANTS-4480 (reported by AI Prompts and Fin Break, the same
                    // day, having each lost most of a session to it) — this read
                    // "Use ONCE per project", which is the line a session reads
                    // when deciding whether to call the verb at all. Both read it
                    // as one-shot-and-dangerous and did not re-run: one nearly
                    // stopped at "already migrated, nothing to do" (the re-run it
                    // almost skipped reconciled 3 drifted items), the other hand-
                    // edited a 616 KB generated ROADMAP.md instead. Re-ingesting
                    // IS the routine drift repair and the hint now says so.
                    // Two house rules bind this string and both are tested:
                    // ≤ 240 chars (McpSelectionHint HINT-3) and it must open
                    // with "Use " (McpOrientation_Inv7.SelectionHintFormat).
                    t["selection_hint"] = QStringLiteral(
                        "Use it once to move a project onto the roadmap store, "
                        "and again to reconcile real drift. dry_run:true first: "
                        "items_updated may be a re-parse artefact, not drift "
                        "(ANTS-4507), so read updated_items[] before acting. "
                        "Never writes ROADMAP.md.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "Your $PWD. REQUIRED — the project root to migrate.");
                        props["caller_cwd"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "Optional display name stored as project.name. "
                            "Defaults to caller_cwd's leaf directory verbatim. "
                            "Must be non-empty after trimming.");
                        props["project_name"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "Optional export key stored as project.export_slug "
                            "(UNIQUE across the store). Defaults to the "
                            "slugified leaf directory (Ants_Terminal → "
                            "ants-terminal). A SUPPLIED value is validated "
                            "verbatim and never slugified for you: it must start "
                            "with [a-z0-9] and contain only [a-z0-9-], else "
                            "bad_args.");
                        props["export_slug"] = p;
                    }
                    // ANTS-4621 — declared because the HANDLER READS THEM.
                    // ANTS-4617 shipped both documented in the description and
                    // in neither the properties nor the enum, and this schema
                    // sets additionalProperties:false — so a strictly
                    // validating client refuses the call before the handler is
                    // reached. The permissive path is worse: ANTS-2175 builds
                    // `ignored_args` from inputSchema.properties, so a live
                    // deregister answered "ignored_args":["op"] — telling the
                    // caller its argument did nothing, on the call that
                    // argument had just steered.
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        QJsonArray e;
                        e.append(QStringLiteral("migrate"));
                        e.append(QStringLiteral("deregister"));
                        e.append(QStringLiteral("init"));
                        e.append(QStringLiteral("reroot"));   // ANTS-3857
                        p["enum"] = e;
                        p["description"] = QStringLiteral(
                            "Verb mode. Default \"migrate\" (omit it) — load "
                            "this project's markdown roadmap into the store. "
                            "\"deregister\" (ANTS-4617) is the INVERSE: it "
                            "DELETES the project and every row that hangs off "
                            "it, keyed by caller_cwd's root or by "
                            "`export_slug` when the files are already gone. "
                            "It refuses `confirm_required` while the root "
                            "still exists on disk — pass `confirm:true` to "
                            "override. Preview it with `dry_run:true`, which "
                            "reports the per-table counts and deletes "
                            "nothing. \"init\" (ANTS-4740) is the BOOTSTRAP, "
                            "for a project with no roadmap at all: it writes a "
                            "conforming ants-v1 skeleton carrying one "
                            "`## Backlog` section, migrates it and renders it "
                            "(ANTS-5671), so "
                            "registration takes the same path a migrated "
                            "project takes. Add a release block with "
                            "roadmap_log op:\"create_section\" before the "
                            "first append: Backlog is never selected for "
                            "work. Until it existed, both routes into "
                            "the store required a parseable file to already "
                            "exist, so a greenfield project had to hand-author "
                            "one — and a mis-cased trailer label there is "
                            "silently invisible, so the bootstrap file could "
                            "migrate cleanly while dropping fields. Refuses "
                            "`roadmap_exists` when there is already one, since "
                            "that project wants the plain migrate. The id "
                            "prefix is stored nowhere: pass `id_prefix` on the "
                            "first roadmap_log op:\"append\" to pin it, or it "
                            "is derived from the directory name. Preview with "
                            "`dry_run:true`, which returns the skeleton and "
                            "writes nothing. \"reroot\" (ANTS-3857) points "
                            "the project `export_slug` names at caller_cwd's "
                            "root, after its tree moved; nothing else changes. "
                            "It refuses `confirm_required` while the old root "
                            "still exists (a copy, not a move) and `bad_args` "
                            "when another project holds the new root; "
                            "`dry_run:true` previews.");
                        props["op"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "boolean";
                        p["description"] = QStringLiteral(
                            "op:\"deregister\" and op:\"reroot\" only. Clears "
                            "the `confirm_required` refusal raised when the "
                            "project root (for reroot, the OLD root) still "
                            "EXISTS on disk: the store is primary and "
                            "ROADMAP.md is its render, so the rows are the "
                            "only copy of that project's history, "
                            "relationships and citations. An absent root needs "
                            "no confirmation. Ignored by the migrate op.");
                        props["confirm"] = p;
                    }
                    {
                        // ANTS-4559 — the escape hatch for a corpus whose note
                        // groups exceed the default. Declared here as well as
                        // read by the handler for ANTS-4621's reason: this
                        // schema sets additionalProperties:false, so an
                        // undeclared argument is a refusal on a strict client
                        // and lands in `ignored_args` on a permissive one.
                        QJsonObject p;
                        p["type"] = "integer";
                        p["minimum"] = 1;
                        p["maximum"] = 2000;
                        p["description"] = QStringLiteral(
                            "Row bound for `notes[]`. Default 200, clamped to "
                            "[1, 2000]; the envelope echoes the EFFECTIVE "
                            "value after the clamp, so a caller passing 5000 "
                            "learns it received 2000. Raise it when "
                            "`notes_truncated` is true and `notes_summary` "
                            "names a population you need: repetition is "
                            "collapsed before this cap, so what overflows it "
                            "is a code whose `detail` is unique per "
                            "occurrence — `quarantined_id` carries the id "
                            "token, and no two of those ever merge. Measured "
                            "on one project: 442 groups against the default, "
                            "of which 438 were quarantined ids at count 1, so "
                            "242 ids were dropped and named nowhere. At the "
                            "ceiling the reply is large enough to be offloaded "
                            "and read back with read_spill, which is expected "
                            "rather than a failure.");
                        props["max_notes"] = p;
                    }
                    {   // ANTS-4992 — scope the report to named ids.
                        QJsonObject p;
                        p["type"] = "array";
                        p["items"] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
                        p["description"] = QStringLiteral(
                            "Report only these ids in updated_items[] "
                            "(case-insensitive), so one item's answer is "
                            "reachable past the 200 cap. The migration still "
                            "runs whole and items_updated stays the full count. "
                            "Echoes only_ids and only_ids_not_updated, the ids "
                            "that do not move. Pair with dry_run:true to ask "
                            "\"does this item still change?\".");
                        props["only_ids"] = p;
                    }
                    // ANTS-4499 — the pre-migration snapshot. Declared for the
                    // reason the ANTS-4429 note below gives: this schema sets
                    // additionalProperties:false, so an undeclared argument
                    // never reaches the handler at all.
                    {
                        QJsonObject p;
                        p["type"] = QStringLiteral("boolean");
                        p["description"] = QStringLiteral(
                            "Snapshot the machine-global store before "
                            "migrating (default true). A FAILED snapshot "
                            "refuses the call `backup_failed` and migrates "
                            "nothing: this verb rewrites rows in a store every "
                            "project on the machine shares, with no undo. Pass "
                            "false to migrate without one. A dry run never "
                            "snapshots -- it commits nothing to protect. The "
                            "copy uses SQLite's own VACUUM INTO, so it is safe "
                            "while a live Ants holds a connection; a plain "
                            "`cp` of the .sqlite is NOT, because the store "
                            "runs in WAL and the copy silently loses the most "
                            "recent writes.");
                        props["backup"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = QStringLiteral("string");
                        p["description"] = QStringLiteral(
                            "Where that snapshot goes; a failure refuses. "
                            "Omitted: `pre-migrate.sqlite` in the folder "
                            "config key claude.roadmap_snapshot_dir names, "
                            "else the weekly snapshot's recorded folder, else "
                            "beside the store (ANTS-5247). A named folder must "
                            "exist; one that fails falls back beside the store "
                            "and says so in `backup_fallback`. "
                            "`backup_path_source` names which. It is "
                            "ROLLING -- each run overwrites it -- so name your "
                            "own path to keep a particular one. An existing "
                            "file there that is not a SQLite database is "
                            "never replaced: the call refuses `backup_failed` "
                            "naming it. NOT validated "
                            "against the project root, because the store is "
                            "machine-global and its backup belongs beside it "
                            "or on another drive. Deliberately not named "
                            "`roadmap-*.sqlite`: that glob is what "
                            "tools/roadmap-store-backup.sh prunes to its KEEP "
                            "limit, so a matching name would quietly cost the "
                            "weekly rotation one snapshot. The envelope echoes "
                            "the resolved path as `backup_path`.");
                        props["backup_to"] = p;
                    }
                    {
                        // ANTS-5287 — an item removed from the roadmap file is
                        // deleted on migrate; a large deletion needs this.
                        QJsonObject p;
                        p["type"] = QStringLiteral("boolean");
                        p["default"] = false;
                        p["description"] = QStringLiteral(
                            "An item missing from the roadmap file is DELETED "
                            "from the store (its history too; the snapshot is "
                            "the undo), and reported in `items_deleted` / "
                            "`deleted_ids`. A run that would delete more than "
                            "a quarter of the project's items refuses "
                            "`mass_deletion`, naming them; pass true to allow "
                            "it. An item another row references is kept.");
                        props["accept_deletions"] = p;
                    }
                    props["dry_run"] = makeDryRunProp();
                    // ANTS-4429 — declared for the same reason ANTS-4621
                    // declared `op` and `confirm`: this schema sets
                    // additionalProperties:false, so an undeclared argument is
                    // refused by a strict client before the dispatcher is
                    // reached, and on a permissive one ANTS-2175 reports it in
                    // `ignored_args` — telling the caller its argument did
                    // nothing on the call that argument had just steered.
                    // Consumed by the dispatcher rather than the handler, so
                    // they narrow the report and never the migration.
                    props["fields"]  = makeFieldsProp();         // ANTS-1720
                    props["compact"] = makeCompactProp();        // ANTS-2091
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append(QStringLiteral("caller_cwd"));
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-4622 — session_message: the cross-session mailbox.
                // Every property below is declared because the HANDLER READS
                // IT (INV-7). ANTS-4621 is why that is an invariant: this
                // schema sets additionalProperties:false, so an undeclared
                // argument is refused by a strict client before the handler
                // runs — and on the permissive path ANTS-2175 builds its
                // `ignored_args` advisory from these same properties, so an
                // undeclared argument is reported as ignored on the very call
                // it just steered.
                {
                    QJsonObject t;
                    t["name"] = "session_message";
                    t["selection_hint"] = QStringLiteral(
                        "Use to leave a message for another project's next "
                        "Claude session, read your own inbox, or ack mail "
                        "you have read. Addressed by project export_slug, "
                        "not by session. Ends in a receipt, not a roadmap "
                        "id.");
                    t["description"] = QStringLiteral(
                        "Leave a message for whoever next picks up another project, and "
                        "see when it has been read. Addressed to a project by "
                        "export_slug, never to a session. op:\"send\" (needs `to` + "
                        "`body`; to:\"*\" reaches every registered project and "
                        "returns sent[] and refused[]), op:\"inbox\" (default: your unacked mail, newest "
                        "first), op:\"outbox\" (mail you sent), op:\"ack\" (needs "
                        "`message_id`; idempotent), op:\"projects\" (every "
                        "registered slug, root and github_repo). An entry "
                        "with `acked_at` has been read. Refusals: no_project (run "
                        "roadmap_migrate first), unknown_project, inbox_full, "
                        "not_found. Distinct from the *_Ants_MCP_Feedback.md corpus, "
                        "which files work.");
                    t["detail"] = QStringLiteral(
                        "ANTS-4622 — leave a message for whoever picks up "
                        "another project next, and see when it has been read. "
                        "Addressed to a PROJECT by its export_slug, never to a "
                        "session: a session is its shell PID, which is "
                        "ephemeral and OS-reused, so mail addressed to one is "
                        "undeliverable the moment that session ends. "
                        "op:\"send\" (needs `to` + `body`), op:\"inbox\" (the "
                        "default — YOUR unacked mail, newest first) and "
                        "op:\"ack\" (needs `message_id`; idempotent, and the "
                        "FIRST ack is the fact). op:\"outbox\" (ANTS-5255) lists "
                        "the mail you sent, newest first, optionally for one "
                        "`to`, with a body preview; no `acked_at` means not acked "
                        "yet, and acked mail disappears when it is pruned. "
                        "op:\"projects\" (ANTS-6720) lists every registered "
                        "project's export_slug, name, root and, for a GitHub "
                        "origin, github_repo (owner/name), so a caller outside "
                        "Claude can find the slug for a repo. `dedupe:true` on "
                        "a send answers with the id of your identical unacked "
                        "message to that project instead of writing a second; "
                        "once that one is acked, the same body sends again. "
                        "Confirmation is two states in "
                        "one nullable column: an entry carrying `acked_at` has "
                        "been read. Every op needs the CALLING project "
                        "registered in the roadmap store, else `no_project` — "
                        "run roadmap_migrate first. An unresolvable `to` "
                        "refuses `unknown_project` rather than accepting mail "
                        "that could never be delivered, with `candidates[]` "
                        "naming the nearest registered slugs; a recipient already "
                        "holding 500 unacked messages refuses `inbox_full` "
                        "rather than dropping the oldest, because a silently "
                        "dropped message is indistinguishable from one never "
                        "sent. `ack` on a message addressed to another project "
                        "answers `not_found`, the same code an absent id gets, "
                        "so a probe cannot learn that an id exists. Acked mail "
                        "is pruned 30 days after `acked_at`, opportunistically "
                        "on ops that already write; UNACKED mail is never "
                        "auto-pruned at any age, because an old unread message "
                        "means nobody has picked that project up, which is "
                        "exactly when it still matters. DISTINCT from the "
                        "*_Ants_MCP_Feedback.md corpus and does not replace "
                        "it: that channel asks for work to be FILED and ends "
                        "in a roadmap id, this one asks for a person to KNOW "
                        "something and ends in an ack.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "Your $PWD. REQUIRED — resolves WHICH project's "
                            "mailbox this is, on every op including the reads.");
                        props["caller_cwd"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        QJsonArray e;
                        e.append(QStringLiteral("send"));
                        e.append(QStringLiteral("inbox"));
                        e.append(QStringLiteral("outbox"));
                        e.append(QStringLiteral("ack"));
                        e.append(QStringLiteral("projects"));
                        p["enum"] = e;
                        p["description"] = QStringLiteral(
                            "Default \"inbox\". \"outbox\": mail you sent. \"send\" "
                            "needs `to`, `body`; \"ack\" needs `message_id`. "
                            "\"projects\": every slug.");
                        props["op"] = p;
                    }
                    {
                        // ANTS-6720 — read only by op:"send" to one slug.
                        QJsonObject p;
                        p["type"] = "boolean";
                        p["default"] = false;
                        p["description"] = QStringLiteral(
                            "op:\"send\" — send nothing while your identical "
                            "unacked message to `to` exists; reply with its id "
                            "and deduplicated:true.");
                        props["dedupe"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "op:\"send\" — the recipient project's "
                            "export_slug (e.g. \"vestige\"). Resolved against "
                            "the store; an unregistered slug refuses "
                            "`unknown_project`. Self-addressed mail is allowed "
                            "on purpose: it is the note for the next session on "
                            "THIS project. \"*\" sends one copy to every "
                            "registered project but yours (ANTS-5741). "
                            "op:\"outbox\" — optional: only mail to this slug.");
                        props["to"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "op:\"send\" — the message. Non-empty, and at most "
                            "4096 BYTES. The cap is byte-valued, not "
                            "character-valued: SQLite's length() counts "
                            "characters on a TEXT value, so a character cap "
                            "would admit up to four times as much UTF-8.");
                        props["body"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "op:\"send\" — optional free-text provenance for "
                            "the sending session. At most 128 bytes. NOTHING "
                            "routes on it; it is there so a reader can tell who "
                            "was working, not so anything can address a reply.");
                        props["from_session"] = p;
                    }
                    {
                        // ANTS-5741 — read only on a `to:"*"` broadcast.
                        QJsonObject p;
                        p["type"] = "boolean";
                        p["default"] = false;
                        p["description"] = QStringLiteral(
                            "to:\"*\" only — also send a copy to your own "
                            "project. Default false.");
                        props["include_self"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "integer";
                        p["description"] = QStringLiteral(
                            "op:\"ack\" — the message to mark read. "
                            "op:\"inbox\" — return just this message, acked "
                            "or not.");
                        props["message_id"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "boolean";
                        p["default"] = false;
                        p["description"] = QStringLiteral(
                            "op:\"inbox\" — also return mail you have already "
                            "acked. Default false, which is the useful view: "
                            "what still needs reading.");
                        props["include_acked"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "integer";
                        p["default"] = 20;
                        p["description"] = QStringLiteral(
                            "op:\"inbox\" or \"outbox\" — cap on `messages[]`, at most 200; "
                            "0 or less asks for 200. `unacked_count` is the TRUE "
                            "total either way, so a capped page still reports "
                            "how much is waiting.");
                        props["limit"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "integer";
                        p["default"] = 0;
                        p["description"] = QStringLiteral(
                            "op:\"inbox\" or \"outbox\" — 0-based offset into the result, for "
                            "paging past `limit`.");
                        props["offset"] = p;
                    }
                    props["dry_run"] = makeDryRunProp();
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append(QStringLiteral("caller_cwd"));
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-6310 — know_how: how-to facts any project writes once
                // and any session finds. Every property below is read by
                // cmdKnowHow or KnowHowStore (ANTS-4621).
                {
                    QJsonObject t;
                    t["name"] = "know_how";
                    t["selection_hint"] = QStringLiteral(
                        "Use to look up, or record once, a lasting how-to fact about a "
                        "machine, tool or environment that helps more than one "
                        "project. Not for chat, status or one project's internals.");
                    t["description"] = QStringLiteral(
                        "A machine-global store of lasting how-to facts: how to reach a "
                        "machine, how to use a tool. op:\"find\" (default; `query` and/or "
                        "`tag`) returns snippets, op:\"get\" one topic's body, op:\"list\" "
                        "topics. op:\"add\", op:\"update\" and op:\"retire\" write, from a "
                        "registered project. op:\"purge\" permanently erases a topic or one "
                        "old version from the live file only: a backup, or a block the "
                        "disk's firmware remapped, is out of its reach. The secret check "
                        "catches known secret shapes only, so an entry says how to "
                        "authenticate, never the secret. Refusals: bad_args, missing_field, "
                        "no_project, io_error, not_found, topic_exists, conflict, "
                        "secret_detected, store_full.");
                    t["detail"] = QStringLiteral(
                        "ANTS-6310. Contract: docs/specs/ANTS-6310-know-how-store.md. "
                        "Belongs: how-to facts useful to more than one project. Not chat, "
                        "status or requests (session_message), not one project's internals, "
                        "and never a credential. find matches any token, so a sentence works, "
                        "and ranks a match in topic, title or tags above one in the body. "
                        "An entry not verified for over 90 days is flagged stale. update with "
                        "no field records that the entry was checked and is still true, and "
                        "it makes a retired entry active again. Pass the revision you read as "
                        "expected_revision, so a change someone made since refuses conflict. "
                        "Only purge and the 20-row history trim delete a row.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    const auto add = [&props](const char *name, const char *type, const char *text) {
                        QJsonObject p;
                        p["type"] = QString::fromLatin1(type);
                        p["description"] = QString::fromUtf8(text);
                        props[QString::fromLatin1(name)] = p;
                    };
                    add("caller_cwd", "string", "Your $PWD (required). A write records this project as the author.");
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        QJsonArray e;
                        e.append(QStringLiteral("find"));
                        e.append(QStringLiteral("get"));
                        e.append(QStringLiteral("list"));
                        e.append(QStringLiteral("add"));
                        e.append(QStringLiteral("update"));
                        e.append(QStringLiteral("retire"));
                        e.append(QStringLiteral("purge"));
                        p["enum"] = e;
                        p["description"] = QStringLiteral("Default find.");
                        props["op"] = p;
                    }
                    add("query", "string", "find: words or a sentence; any word may match.");
                    add("tag", "string", "find, list: keep entries holding this tag whole.");
                    add("limit", "integer", "find: default 5, max 20. list: default 50, max 200.");
                    add("offset", "integer", "list: 0-based start; a reply with more carries next_offset.");
                    add("topic", "string", "Slug, ^[a-z0-9-]+$, at most 64 bytes. Needed by all but find and list.");
                    add("history", "boolean", "get: also return the older versions.");
                    add("title", "string", "add, update: one line, at most 120 bytes.");
                    add("body", "string", "add, update: at most 16 KiB. How to authenticate, never the secret.");
                    {
                        QJsonObject item;
                        item["type"] = "string";
                        QJsonObject p;
                        p["type"] = "array";
                        p["items"] = item;
                        p["description"] = QStringLiteral(
                            "add, update: at most 8, each ^[a-z0-9-]+$, at most 32 bytes.");
                        props["tags"] = p;
                    }
                    add("expected_revision", "integer", "update: optional. purge: required. A stale value refuses conflict.");
                    add("revision", "integer", "purge: erase this old version; omit it to erase the topic and its history.");
                    add("reason", "string", "purge: required, at most 200 bytes. Kept in the purge log.");
                    props["dry_run"] = makeDryRunProp();
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append(QStringLiteral("caller_cwd"));
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-5299 — run_trace: a review run records its trace-index
                // row. Every property below is read by cmdRunTrace (ANTS-4621).
                {
                    QJsonObject t;
                    t["name"] = "run_trace";
                    t["selection_hint"] = QStringLiteral(
                        "Use to mint a review run's id and write its trace-index "
                        "row, detail file and cost row instead of hand-editing "
                        "gate-log.md. start before dispatching, finish after.");
                    t["description"] = QStringLiteral(
                        "Record a review run in the project's trace index "
                        "(`gate_log`, default docs/reviews/gate-log.md) as "
                        "`| Run | Date | Subject | Lanes | Outcome |`. op:\"start\" "
                        "(needs `kind` G|C|V|E|F|R|A|L + `subject`) mints "
                        "<K>-<YYYYMMDD>-<4 hex> and writes nothing tracked. "
                        "op:\"finish\" (needs `id`, `lanes` — 0 is a value, never a "
                        "default — and `outcome`) writes docs/reviews/<id>.md, the "
                        "optional `cost` row, then the index row. op:\"get\" reads "
                        "either state. Refusals: missing_field, bad_args, "
                        "format_mismatch, already_recorded, coverage_required, "
                        "not_found, bad_path. caller_cwd Required.");
                    t["detail"] = QStringLiteral(
                        "ANTS-5299 (with ANTS-5298's record half). Contract: "
                        "tests/features/run_trace/spec.md. The row format is the "
                        "v2 workflow's standards/documents.md § The header. "
                        "start parks a pending record in the user cache "
                        "(ants-terminal/run-trace/), outside the project, so a run "
                        "in flight dirties nothing git sees; it survives a "
                        "relaunch. It returns `env.CLAUDE_RUN_ID` for the caller to "
                        "export, because a verb cannot set the caller's shell. "
                        "finish writes the detail file, then the cost row, then "
                        "the index row LAST, since the row is what the gate-record "
                        "hook checks. A detail text with no Genre: line gets "
                        "`Genre: record` prepended; with no `detail`, an existing "
                        "<id>.md is accepted untouched and a missing one refuses "
                        "missing_field. An index whose table header is not the "
                        "fixed row refuses format_mismatch and is never rewritten. "
                        "`cost` needs `verified` (else coverage_required), `kind`, "
                        "`loop`, `executed`, `arm`, and `transcript` or "
                        "`session_id` — the transcript is NAMED, never guessed, "
                        "because two sessions in one project otherwise read each "
                        "other's usage. Usage counts each message.id once: the "
                        "harness writes one line per content block, each with the "
                        "whole message's usage. A cost with lanes 0 refuses: a "
                        "run that dispatched nobody records no cost. The root is "
                        "the nearest ancestor holding .git, where gate-record "
                        "looks. Every mutating op takes dry_run.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "Your $PWD. REQUIRED — the project whose "
                        "trace index this is, on every op.");
                        props["caller_cwd"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        QJsonArray e;
                        e.append(QStringLiteral("start"));
                        e.append(QStringLiteral("finish"));
                        e.append(QStringLiteral("get"));
                        p["enum"] = e;
                        p["description"] = QStringLiteral(
                            "start mints the id; finish writes the row; get reads "
                            "a run. No default.");
                        props["op"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        QJsonArray e;
                        for (const char *k : {"G", "C", "V", "E", "F", "R", "A", "L"})
                            e.append(QLatin1String(k));
                        p["enum"] = e;
                        p["description"] = QStringLiteral(
                            "op:\"start\" — the run kind: G gate, C code review, "
                            "V verify, E edit, F close-findings, R review-rules, "
                            "A adoption, L release.");
                        props["kind"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "op:\"start\" — what the run was against: a "
                        "path, a claim, a version. One line.");
                        props["subject"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "op:\"finish\" / op:\"get\" — the id start returned.");
                        props["id"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "integer";
                        p["minimum"] = 0;
                        p["description"] = QStringLiteral(
                            "op:\"finish\" — how many lanes read the subject. "
                            "REQUIRED, never defaulted: 0 is what marks a self-read.");
                        props["lanes"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "op:\"finish\" — one line: what the run concluded.");
                        props["outcome"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "op:\"finish\" — markdown for <index dir>/<id>.md. "
                        "Omit when you already wrote that file.");
                        props["detail"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "object";
                        p["description"] = QStringLiteral(
                            "op:\"finish\" — append a docs/reviews/run-costs.tsv row: "
                            "{verified (REQUIRED), dismissed, loop, kind, executed "
                            "yes|no|partial, outcome complete|stopped-early|open, "
                            "arm live|draft}. agents = lanes. Needs transcript or "
                            "session_id.");
                        props["cost"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "op:\"finish\" with cost — absolute path of "
                        "YOUR session's .jsonl under ~/.claude/projects.");
                        props["transcript"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "op:\"finish\" with cost — your session id; "
                        "resolves to ~/.claude/projects/<cwd slug>/<id>.jsonl.");
                        props["session_id"] = p;
                    }
                    props["dry_run"] = makeDryRunProp();
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append(QStringLiteral("caller_cwd"));
                    req.append(QStringLiteral("op"));
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-5698 — session_cost: a session's context rent, read from
                // its transcript. Every property below is read by cmdSessionCost
                // (ANTS-4621).
                {
                    QJsonObject t;
                    t["name"] = "session_cost";
                    t["selection_hint"] = QStringLiteral(
                        "Use to see what this session pays per request and what "
                        "holds its context, before deciding to /clear or start fresh.");
                    t["description"] = QStringLiteral(
                        "Report a Claude Code session's context rent from its "
                        "transcript: requests, context size at the first request, "
                        "now and at peak (tokens), token totals, the start-up "
                        "payload (system prompt and tool definitions, in bytes, "
                        "largest tools first) and the largest items the session "
                        "holds (tool results by tool, text blocks; bytes). Images "
                        "are counted, not sized. Also rent (bytes x requests held) "
                        "per tool, MCP server and hook. Without session_id it reads the "
                        "newest transcript for caller_cwd and says "
                        "session_inferred. session_id \"agent-<id>\" reads a helper "
                        "agent's transcript; \"self\" reads the newest session or "
                        "helper agent, the caller when it is the one writing "
                        "(also inferred). Refusals: bad_args, not_found, "
                        "no_project. caller_cwd Required.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "Your $PWD (required); picks the transcript folder.");
                        props["caller_cwd"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "Session to read: a transcript name, \"agent-<id>\" for a "
                            "helper agent, or \"self\". Default: the newest session.");
                        props["session_id"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "integer";
                        p["minimum"] = 1;
                        p["maximum"] = 50;
                        p["default"] = 8;
                        p["description"] = QStringLiteral("Cap on each list.");
                        props["top"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "boolean";
                        p["default"] = false;
                        p["description"] = QStringLiteral(
                            "Add the session's helper agents' summed requests, "
                            "token totals and rent (ANTS-6697).");
                        props["include_agents"] = p;
                    }
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append(QStringLiteral("caller_cwd"));
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
}

}  // namespace claudeintegrationdetail
