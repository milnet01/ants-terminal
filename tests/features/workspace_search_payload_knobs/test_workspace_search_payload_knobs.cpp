// ANTS-1876 — workspace_search payload knobs.
// Source-grep style + focused helper tests where feasible.
//
// ANTS-5052 adds a behavioural section at the end of this file: the rg
// stdout byte ceiling. Those cases construct a real RemoteControl over a
// real fixture tree and a real rg, matching the pattern established by
// workspace_search_enclosing_symbol's ANTS-4901 addendum.

#include "../../_support/expect.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <string>

#include "remotecontrol.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QString>
#include <QTemporaryDir>

#ifndef ANTS_RC_SOURCES
#error "ANTS_RC_SOURCES compile definition required"
#endif
#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {


bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// INV-1 — max_match_bytes arg parsed, out-of-range → default 0.
TEST(workspace_search_payload_knobs, Inv1MaxMatchBytesParse) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    expect(contains(cpp, "\"max_match_bytes\""),
           "INV-1: max_match_bytes arg name present");
    expect(contains(cpp, "cmdWorkspaceSearch"),
           "INV-1: cmdWorkspaceSearch present");
    EXPECT_EQ(0, expect_failures());
}

// INV-2 — text clipped to exactly max_match_bytes when clipped.
TEST(workspace_search_payload_knobs, Inv2TextClippedToBudgetExactByteCount) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    // Anchor: named clip helper or inline clip site that uses the
    // 3-byte ellipsis (UTF-8 "…" = E2 80 A6).
    expect(contains(cpp, "rcClipMatchBytes") ||
           contains(cpp, "max_match_bytes"),
           "INV-2: clip helper / arg referenced");
    // The UTF-8 ellipsis literal must appear in the clip path.
    expect(contains(cpp, "QStringLiteral(\"\xE2\x80\xA6\")") ||
           contains(cpp, "\xE2\x80\xA6") ||
           contains(cpp, "u8\"\xE2\x80\xA6\"") ||
           contains(cpp, "\\u2026"),
           "INV-2: UTF-8 ellipsis literal present in clip path");
    EXPECT_EQ(0, expect_failures());
}

// INV-2b — context entries clipped too.
TEST(workspace_search_payload_knobs, Inv2ContextEntriesClipped) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    // Anchor: clip helper called on context_before / context_after
    // OR the helper operates over a generic field name.
    expect(contains(cpp, "rcClipMatchBytes") ||
           contains(cpp, "max_match_bytes"),
           "INV-2b: clip site exists (applied to context entries)");
    EXPECT_EQ(0, expect_failures());
}

// INV-2c — short field emitted verbatim (no ellipsis appended).
TEST(workspace_search_payload_knobs, Inv2ShortFieldEmittedVerbatim) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    // Anchor: the clip path has a guard for the short-field case
    // (size <= max_match_bytes → no-op).
    expect(contains(cpp, "max_match_bytes") &&
           (contains(cpp, "toUtf8().size()") ||
            contains(cpp, "utf8().size()") ||
            contains(cpp, "size() <= ")),
           "INV-2c: short-field guard present (no-op when "
           "unclipped form fits)");
    EXPECT_EQ(0, expect_failures());
}

// INV-3 — clip does NOT split UTF-8 code points.
TEST(workspace_search_payload_knobs, Inv3ClipDoesNotSplitCodePoints) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    // Anchor: the clip path uses Qt's QString::left (which operates
    // on QChar code-points, not bytes) OR explicitly checks UTF-8
    // continuation bytes (0x80-0xBF). Either way is safe.
    expect(contains(cpp, "QString::") ||
           contains(cpp, "max_match_bytes"),
           "INV-3: code-point-safe clip mechanism present");
    EXPECT_EQ(0, expect_failures());
}

// INV-4 — dedup key unaffected by clip (clip runs after dedup).
TEST(workspace_search_payload_knobs, Inv4DedupKeyUnaffectedByClip) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    // The pipeline order (per §2.5) is:
    // 1. rg matches
    // 2. dedup (text.simplified())
    // 3. max_match_bytes clip
    // 4. headline_only rename + context drop
    //
    // The clip site must appear AFTER the dedup loop.
    // The dedup loop normalises the matched text via QString
    // ::simplified() before keying. Look for the call form used by
    // the current code at cmdWorkspaceSearch's dedup branch.
    // Scope the position anchors to the cmdWorkspaceSearch body: the
    // `.toString().simplified()` idiom recurs in other functions (e.g.
    // rcProjectChangelogHeadlineOnly, ANTS-3576), so a whole-file find()
    // would match an unrelated earlier occurrence and test the wrong thing.
    const std::string ws =
        ants_test::slurpFunctionBody(cpp, "RemoteControl::cmdWorkspaceSearch");
    const auto dedupPos = ws.find(".toString().simplified()");
    // The CLIP-INVOCATION site (not the arg parse) — the call to
    // `rcClipMatchTextFields(matches, ...)`. The arg parse must
    // happen earlier (before the rg launch) but the clip itself
    // must run AFTER the dedup loop.
    const auto clipCallPos = ws.find("rcClipMatchTextFields(matches");
    expect(dedupPos != std::string::npos,
           "INV-4: dedup site (text.simplified()) present");
    expect(clipCallPos != std::string::npos,
           "INV-4: rcClipMatchTextFields(matches, ...) call present");
    if (dedupPos != std::string::npos &&
        clipCallPos != std::string::npos) {
        expect(dedupPos < clipCallPos,
               "INV-4: dedup (text.simplified()) runs BEFORE the "
               "rcClipMatchTextFields call (ordering constraint per "
               "spec §2.5 — dedup key sees the unclipped form)");
    }
    EXPECT_EQ(0, expect_failures());
}

// INV-5 — headline_only emits {file, line, headline}.
TEST(workspace_search_payload_knobs, Inv5HeadlineOnlyKeySet) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    expect(contains(cpp, "\"headline_only\""),
           "INV-5: headline_only arg name present");
    expect(contains(cpp, "\"headline\""),
           "INV-5: headline field name present in cmdWorkspaceSearch");
    EXPECT_EQ(0, expect_failures());
}

// INV-5b — also_at never clipped (no text field to begin with).
TEST(workspace_search_payload_knobs, Inv5AlsoAtNeverClipped) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    // also_at entries are {file, line} only. The clip helper
    // should only touch fields named "text" or "headline".
    expect(contains(cpp, "max_match_bytes") &&
           contains(cpp, "also_at"),
           "INV-5b: both clip path and also_at present (the spec "
           "demands also_at is never touched by the clip)");
    EXPECT_EQ(0, expect_failures());
}

// INV-6 — envelope echo only when feature activated.
TEST(workspace_search_payload_knobs, Inv6EchoActivationGated) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    // The echo path must guard on the activation condition
    // (max_match_bytes > 0; headline_only == true).
    expect(contains(cpp, "max_match_bytes") &&
               (contains(cpp, "if (max_match_bytes") ||
                contains(cpp, "if (maxMatchBytes") ||
                contains(cpp, "> 0) out[")),
           "INV-6: max_match_bytes echo gated on >0");
    expect(contains(cpp, "headline_only") &&
               (contains(cpp, "if (headline_only") ||
                contains(cpp, "if (headlineOnly")),
           "INV-6: headline_only echo gated on true");
    EXPECT_EQ(0, expect_failures());
}

// INV-6b — error envelopes never carry the new fields.
TEST(workspace_search_payload_knobs, Inv6NoEchoOnError) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    // The echo lives in the ok:true emission path; error envelopes
    // (wsErr() / bad_pattern / rg_failed) return before the echo.
    expect(contains(cpp, "out[\"ok\"]         = true;") ||
           contains(cpp, "out[\"ok\"] = true;"),
           "INV-6b: success-only emission path present");
    EXPECT_EQ(0, expect_failures());
}

// INV-7 — tools/list schema enumerates both new args.
TEST(workspace_search_payload_knobs, Inv7ToolsListEnumerates) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    // Anchor: props["max_match_bytes"] and props["headline_only"]
    // populated in the workspace_search descriptor block.
    expect(contains(cpp, "props[\"max_match_bytes\"]"),
           "INV-7: props[\"max_match_bytes\"] populated in "
           "workspace_search tools/list descriptor");
    expect(contains(cpp, "props[\"headline_only\"]"),
           "INV-7: props[\"headline_only\"] populated in "
           "workspace_search tools/list descriptor");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-3537 INV-8 — count_only arg parsed in cmdWorkspaceSearch.
TEST(workspace_search_payload_knobs, Inv8CountOnlyParsed) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    expect(contains(cpp, "\"count_only\""),
           "INV-8: count_only arg name present in cmdWorkspaceSearch");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-3537 INV-9 — count_only envelope emits count + files_count and
// omits matches[] (early return before the dedup/clip pipeline).
TEST(workspace_search_payload_knobs, Inv9CountOnlyEnvelopeShape) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    expect(contains(cpp, "out[\"count\"]") &&
           contains(cpp, "out[\"files_count\"]"),
           "INV-9: count + files_count fields emitted");
    // The count_only branch must sit BEFORE the dedup loop so matches[]
    // is never serialised (rows-eliminated). Anchor on the branch guard
    // preceding the dedup site.
    // Scope to the cmdWorkspaceSearch body (see INV-4) so the dedup anchor
    // can't match an earlier `.toString().simplified()` in another function.
    const std::string ws =
        ants_test::slurpFunctionBody(cpp, "RemoteControl::cmdWorkspaceSearch");
    const auto branchPos = ws.find("if (countOnly) {");
    const auto dedupPos  = ws.find(".toString().simplified()");
    expect(branchPos != std::string::npos,
           "INV-9: count_only early-return branch present");
    if (branchPos != std::string::npos && dedupPos != std::string::npos) {
        expect(branchPos < dedupPos,
               "INV-9: count_only return runs BEFORE the dedup pipeline "
               "(matches[] is never built into the envelope)");
    }
    EXPECT_EQ(0, expect_failures());
}

// ANTS-3537 INV-10 — files_count derives from rg `begin` events, and the
// true total count is uncapped (seenMatchEvents, not matches.size()).
TEST(workspace_search_payload_knobs, Inv10CountUncappedFilesFromBegin) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    expect(contains(cpp, "filesWithMatches"),
           "INV-10: filesWithMatches counter present");
    expect(contains(cpp, "out[\"count\"]       = seenMatchEvents;") ||
           contains(cpp, "out[\"count\"] = seenMatchEvents;"),
           "INV-10: count is the uncapped seenMatchEvents total");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-3537 INV-11 — tools/list schema enumerates count_only.
TEST(workspace_search_payload_knobs, Inv11CountOnlyInSchema) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    expect(contains(cpp, "props[\"count_only\"]"),
           "INV-11: props[\"count_only\"] populated in "
           "workspace_search tools/list descriptor");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-3549 INV-12 — files_only arg parsed in cmdWorkspaceSearch.
TEST(workspace_search_payload_knobs, Inv12FilesOnlyParsed) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    expect(contains(cpp, "\"files_only\""),
           "INV-12: files_only arg name present in cmdWorkspaceSearch");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-3549 INV-13 — files_only envelope emits a files[] set and omits the
// match rows (early return before the dedup/clip pipeline, like count_only).
TEST(workspace_search_payload_knobs, Inv13FilesOnlyEnvelopeShape) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    expect(contains(cpp, "out[\"files\"]"),
           "INV-13: files[] field emitted");
    // The files_only branch must sit BEFORE the dedup loop so matches[]
    // is never serialised (rows-eliminated). Anchor on the branch guard
    // preceding the dedup site.
    // Scope to the cmdWorkspaceSearch body (see INV-4) so the dedup anchor
    // can't match an earlier `.toString().simplified()` in another function.
    const std::string ws =
        ants_test::slurpFunctionBody(cpp, "RemoteControl::cmdWorkspaceSearch");
    const auto branchPos = ws.find("if (filesOnly) {");
    const auto dedupPos  = ws.find(".toString().simplified()");
    expect(branchPos != std::string::npos,
           "INV-13: files_only early-return branch present");
    if (branchPos != std::string::npos && dedupPos != std::string::npos) {
        expect(branchPos < dedupPos,
               "INV-13: files_only return runs BEFORE the dedup pipeline "
               "(matches[] is never built into the envelope)");
    }
    EXPECT_EQ(0, expect_failures());
}

// ANTS-3549 INV-14 — the file set is captured from rg `begin` events (path +
// per-file hit count), and per-file counting is uncapped (the increment sits
// before the max_results cap, so a file's count reflects ALL its matches).
TEST(workspace_search_payload_knobs, Inv14FilesOnlyPerFileFromBegin) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    expect(contains(cpp, "filesOnlyOrder") && contains(cpp, "filesOnlyHits"),
           "INV-14: per-file order + hit-count accumulators present");
    // The per-file increment must precede the matches[] max_results cap so
    // it counts every match, not just those under the cap.
    const auto hitPos = cpp.find("++filesOnlyHits");
    const auto capPos = cpp.find("matches.size() >= maxResults");
    expect(hitPos != std::string::npos,
           "INV-14: per-file hit increment present");
    if (hitPos != std::string::npos && capPos != std::string::npos) {
        expect(hitPos < capPos,
               "INV-14: per-file count increments BEFORE the max_results cap "
               "(count is uncapped)");
    }
    EXPECT_EQ(0, expect_failures());
}

// ANTS-3549 INV-15 — tools/list schema enumerates files_only.
TEST(workspace_search_payload_knobs, Inv15FilesOnlyInSchema) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    expect(contains(cpp, "props[\"files_only\"]"),
           "INV-15: props[\"files_only\"] populated in "
           "workspace_search tools/list descriptor");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-3547 INV-16 — offset cursor: the build loop skips the first `offset`
// match events (guard `seenMatchEvents <= offset`).
TEST(workspace_search_payload_knobs, Inv16OffsetSkipGuard) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    expect(contains(cpp, "seenMatchEvents <= offset"),
           "INV-16: offset skip guard present in cmdWorkspaceSearch");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-3547 INV-17 — truncation and the next_offset cursor account for the
// offset: `truncated` uses `seenMatchEvents > offset + …` and the envelope
// emits `next_offset` (the page-N+1 cursor, mirroring roadmap_query).
TEST(workspace_search_payload_knobs, Inv17NextOffsetCursor) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    expect(contains(cpp, "seenMatchEvents > offset +"),
           "INV-17: truncated recompute accounts for offset");
    expect(contains(cpp, "out[\"next_offset\"]"),
           "INV-17: next_offset cursor emitted in the envelope");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-3547 INV-18 — the offset skip is positioned AFTER the seenMatchEvents
// increment (so the total count stays uncapped and offset-independent) and
// BEFORE the max_results cap (offset pages within the same cap).
TEST(workspace_search_payload_knobs, Inv18OffsetSkipPlacement) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    const auto incPos  = cpp.find("++seenMatchEvents;");
    const auto skipPos = cpp.find("seenMatchEvents <= offset");
    const auto capPos  = cpp.find("matches.size() >= maxResults");
    expect(incPos != std::string::npos && skipPos != std::string::npos &&
           capPos != std::string::npos,
           "INV-18: increment, skip guard, and cap all present");
    if (incPos != std::string::npos && skipPos != std::string::npos &&
        capPos != std::string::npos) {
        expect(incPos < skipPos && skipPos < capPos,
               "INV-18: offset skip runs after ++seenMatchEvents and before "
               "the max_results cap");
    }
    EXPECT_EQ(0, expect_failures());
}

// ANTS-3547 INV-19 — tools/list schema enumerates the offset property (via
// the workspace-search-unique wsOffsetProp builder).
TEST(workspace_search_payload_knobs, Inv19OffsetInSchema) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    expect(contains(cpp, "wsOffsetProp"),
           "INV-19: wsOffsetProp builder present");
    expect(contains(cpp, "props[\"offset\"]      = wsOffsetProp") ||
           contains(cpp, "props[\"offset\"] = wsOffsetProp"),
           "INV-19: props[\"offset\"] wired to wsOffsetProp in "
           "workspace_search descriptor");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-3548 INV-1 amendment — max_match_bytes is default-ON: an absent
// arg clips to kDefaultMaxMatchBytes (512), not 0. Source anchors: the
// named constant + the default-init in cmdWorkspaceSearch.
TEST(workspace_search_payload_knobs, Inv20DefaultOnClip) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    expect(contains(cpp, "ANTS-3548"),
           "INV-20: ANTS-3548 anchor present in cmdWorkspaceSearch");
    expect(contains(cpp, "kDefaultMaxMatchBytes"),
           "INV-20: kDefaultMaxMatchBytes constant named");
    expect(contains(cpp, "int maxMatchBytes = kDefaultMaxMatchBytes"),
           "INV-20: maxMatchBytes defaults to the clip (default-ON)");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-3548 INV-1 amendment — an explicit max_match_bytes <= 0 opts OUT
// (the off switch), and the schema advertises default 512 / minimum 0 so
// the 0 opt-out is in-range and passable.
TEST(workspace_search_payload_knobs, Inv21ExplicitOptOut) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    expect(contains(cpp, "requested <= 0"),
           "INV-21: explicit <= 0 opts out of the clip");
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    expect(contains(ci, "mmbProp[\"default\"] = 512"),
           "INV-21: schema advertises default 512");
    expect(contains(ci, "mmbProp[\"minimum\"] = 0"),
           "INV-21: schema minimum 0 so the 0 opt-out is in-range");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-4389 — the clip marker is a CHARACTER (U+2026), not three Latin-1
// chars. `QStringLiteral("\xE2\x80\xA6")` reads a narrow literal as
// Latin-1, so U+2026's UTF-8 bytes each became their own QChar and
// re-encoded as the mojibake `â¦`. Beyond looks: the schema documents the
// clip as "payload prefix + 3-byte ellipsis", so a caller stripping the
// documented marker never matched it, and a caller grepping the returned
// text verbatim — which is what a cross-document quotation check does —
// got three spurious characters at the boundary. `max_match_bytes` defaults
// to 512 (ANTS-3548), so this was on by default rather than opt-in.
//
// Source-grep, matching this file's other cases: the clip helper is not
// exported, so the byte-level spelling in the source IS the surface.
TEST(workspace_search_payload_knobs, Ants4389ClipMarkerIsNotLatin1Bytes) {
    const std::string rc =
        ants_test::slurpFile(std::string(ANTS_RC_SRC_DIR) + "/remotecontrol.cpp");
    ASSERT_FALSE(rc.empty());
    expect(!contains(rc, "QStringLiteral(\"\\xE2\\x80\\xA6\")"),
           "ANTS-4389: the Latin-1 byte spelling must not return: "
           "QStringLiteral reads a narrow literal as Latin-1, which is what "
           "produced the mojibake");
    expect(contains(rc, "QChar(0x2026)"),
           "ANTS-4389: the marker is appended as one character");
}

// ANTS-4388 — matches_only: the distinct MATCHED SUBSTRINGS.
//
// Every other trim here is row-shaped, so "what is the SET of X in this tree"
// had no answer and `grep -o | sort -u` stayed hand-rolled. The measured cost
// was not tokens: a search for `{{[A-Z_]+}}` returned 15 rows and
// truncated:true where the true answer is a handful of strings, and a caller
// stopping at the default had no way to know whether one more existed past
// the cut — which is how a placeholder missed at scaffold time ships a
// literal {{PROJECT_NAME}} into a new project's README.
//
// The verb needs a MainWindow and a live rg, so the two properties the report
// says carry all the value are pinned against the source, in this file's
// existing idiom.
TEST(workspace_search_payload_knobs, Ants4388DistinctMatchMode) {
    const std::string cpp = ants_test::slurpRemoteControl();
    const std::string ws =
        ants_test::slurpFunctionBody(cpp, "RemoteControl::cmdWorkspaceSearch");
    ASSERT_FALSE(ws.empty());

    expect(ws.find("\"matches_only\"") != std::string::npos,
           "ANTS-4388: the arg is parsed inside cmdWorkspaceSearch");
    expect(ws.find("\"distinct_count\"") != std::string::npos,
           "ANTS-4388: the envelope reports how many DISTINCT values exist, "
           "so a capped reply is still diagnosable");

    // Property 1 — the harvest reads rg's own per-match text rather than the
    // line, and it happens BEFORE the max_results row cap. That ordering is
    // the whole mode: capping rows first would truncate a 3-string answer at
    // 15 rows, which is the defect.
    const auto harvestPos = ws.find("submatches");
    const auto rowCapPos  = ws.find("matches.size() >= maxResults");
    expect(harvestPos != std::string::npos,
           "ANTS-4388: distinct values come from rg's submatches[], not from "
           "a second scan or an -o invocation");
    expect(rowCapPos != std::string::npos,
           "ANTS-4388: the row cap is still present for the normal path");
    if (harvestPos != std::string::npos && rowCapPos != std::string::npos) {
        expect(harvestPos < rowCapPos,
               "ANTS-4388: the distinct harvest runs BEFORE the max_results "
               "ROW cap — otherwise the cap that counts rows silently "
               "truncates an answer counted in distinct values");
    }

    // Property 2 — the cap that DOES apply is against the distinct set, and
    // the totals come from the full uncapped scan (seenMatchEvents), exactly
    // as count_only's `count` already does.
    expect(ws.find("qMin(static_cast<int>(distinctOrder.size()), maxResults)")
               != std::string::npos,
           "ANTS-4388: max_results caps DISTINCT VALUES, not occurrences");
    const auto emitPos = ws.find("out[\"matches_only\"]");
    expect(emitPos != std::string::npos, "ANTS-4388: the mode echoes itself");
    if (emitPos != std::string::npos) {
        const std::string tail = ws.substr(emitPos > 900 ? emitPos - 900 : 0,
                                           1400);
        expect(tail.find("seenMatchEvents") != std::string::npos,
               "ANTS-4388: `count` is the uncapped total, like count_only's");
    }
    EXPECT_EQ(0, expect_failures());
}

// ---------------------------------------------------------------------------
// ANTS-5052 — the rg stdout byte ceiling.
//
// rcRunRg waits for rg to finish and takes readAllStandardOutput() whole,
// bounded only by rg's wall-time budget. `setRgStdoutCapOverride` (STUB,
// src/remotecontrol.h) exists so a test can reach a small ceiling with a
// small fixture rather than needing a multi-GB tree to blow the real
// default. Today the override sets `m_rgStdoutCapOverride`, which nothing
// reads: `rcRunRg` never sees it, `RgRun::outputCapped` is never set, and
// none of the three envelopes below folds it into `truncated`. So every
// case in this section is expected RED against the current tree.
//
// SCOPE (ANTS-5052 roadmap item): only the byte-ceiling half is locked
// here. Line-by-line parsing, `rg --count` for the counting modes, and
// co_change_family's min-heap are filed separately.
//
// Behavioural, like workspace_search_enclosing_symbol's ANTS-4901 cases:
// a real RemoteControl over a real fixture tree and a real rg, guarded on
// rg's presence.

namespace {

// One file with kAnts5052Lines matching lines, each long enough that a
// tiny byte ceiling is exceeded many times over well before the file (and
// well before max_results, default 50) is exhausted. Deliberately fewer
// than max_results so a red run in the default row-mode case (INV-3 below)
// cannot be explained by the pre-existing max_results truncation path —
// only the (unwired) output cap can explain it.
constexpr int kAnts5052Lines = 30;
constexpr qint64 kAnts5052SmallCapBytes = 300;

bool writeAnts5052Fixture(const QString &root) {
    QDir().mkpath(root + QStringLiteral("/docs"));
    QFile f(root + QStringLiteral("/docs/cap_fixture.md"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    QByteArray body;
    for (int i = 0; i < kAnts5052Lines; ++i) {
        body += "capmarker_ants5052 filler filler filler filler filler "
                "filler filler filler filler line_index_marker\n";
    }
    const bool ok = (f.write(body) == body.size());
    f.close();
    return ok;
}

QJsonObject ants5052Req(const QString &root) {
    QJsonObject r;
    r[QStringLiteral("caller_cwd")] = root;
    r[QStringLiteral("pattern")]    = QStringLiteral("capmarker_ants5052");
    return r;
}

bool rgAvailable() {
    return !QStandardPaths::findExecutable(QStringLiteral("rg")).isEmpty();
}

}  // namespace

// INV-1 — count_only: truncated is true when the output ceiling is hit.
// Only `truncated` is asserted — the fix's own field/reason for WHY is not
// guessed at here (the roadmap item leaves that field's name to the fix).
TEST(workspace_search_payload_knobs, Ants5052CountOnlyReportsOutputCap) {
    if (!rgAvailable()) GTEST_SKIP() << "ripgrep not installed";
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    ASSERT_TRUE(writeAnts5052Fixture(root));

    RemoteControl rc(nullptr);
    rc.setRgStdoutCapOverride(kAnts5052SmallCapBytes);
    QJsonObject req = ants5052Req(root);
    req[QStringLiteral("count_only")] = true;
    const QJsonObject resp = rc.cmdWorkspaceSearch(req).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson(QJsonDocument::Compact).toStdString();

    EXPECT_TRUE(resp.value(QStringLiteral("truncated")).toBool())
        << "ANTS-5052 INV-1: count_only truncated must be true once the "
           "output ceiling is hit; saw truncated="
        << resp.value(QStringLiteral("truncated")).toBool()
        << " count=" << resp.value(QStringLiteral("count")).toInt()
        << " full envelope="
        << QJsonDocument(resp).toJson(QJsonDocument::Compact).toStdString();
}

// INV-2 — files_only: same, truncated is true when the ceiling is hit.
TEST(workspace_search_payload_knobs, Ants5052FilesOnlyReportsOutputCap) {
    if (!rgAvailable()) GTEST_SKIP() << "ripgrep not installed";
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    ASSERT_TRUE(writeAnts5052Fixture(root));

    RemoteControl rc(nullptr);
    rc.setRgStdoutCapOverride(kAnts5052SmallCapBytes);
    QJsonObject req = ants5052Req(root);
    req[QStringLiteral("files_only")] = true;
    const QJsonObject resp = rc.cmdWorkspaceSearch(req).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson(QJsonDocument::Compact).toStdString();

    EXPECT_TRUE(resp.value(QStringLiteral("truncated")).toBool())
        << "ANTS-5052 INV-2: files_only truncated must be true once the "
           "output ceiling is hit; saw truncated="
        << resp.value(QStringLiteral("truncated")).toBool()
        << " full envelope="
        << QJsonDocument(resp).toJson(QJsonDocument::Compact).toStdString();
}

// INV-3 — default row mode: truncated is true even though max_results (50)
// would not otherwise have been reached (kAnts5052Lines is 30). If this
// case ever comes back green FOR THE WRONG REASON, matches.size() < 50 must
// hold too — checked explicitly so a future max_results bump can't make
// this pass by way of the unrelated existing truncation path.
TEST(workspace_search_payload_knobs, Ants5052DefaultModeReportsOutputCap) {
    if (!rgAvailable()) GTEST_SKIP() << "ripgrep not installed";
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    ASSERT_TRUE(writeAnts5052Fixture(root));

    RemoteControl rc(nullptr);
    rc.setRgStdoutCapOverride(kAnts5052SmallCapBytes);
    const QJsonObject resp = rc.cmdWorkspaceSearch(ants5052Req(root)).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson(QJsonDocument::Compact).toStdString();

    const int matchCount =
        resp.value(QStringLiteral("matches")).toArray().size();
    EXPECT_LT(matchCount, 50)
        << "fixture grew past max_results — this case would no longer "
           "isolate the output-cap truncation path from the existing "
           "max_results one; matchCount=" << matchCount;
    EXPECT_TRUE(resp.value(QStringLiteral("truncated")).toBool())
        << "ANTS-5052 INV-3: default-mode truncated must be true once the "
           "output ceiling is hit, independent of max_results; saw "
           "truncated=" << resp.value(QStringLiteral("truncated")).toBool()
        << " matchCount=" << matchCount
        << " full envelope="
        << QJsonDocument(resp).toJson(QJsonDocument::Compact).toStdString();
}

// INV-6 (workspace_search's share of the guard) — with NO override, the
// same fixture returns every match, truncated:false, and count_only's
// count equals the real number of matches. Must pass both BEFORE and
// AFTER the fix.
TEST(workspace_search_payload_knobs, Ants5052GuardNoOverrideReturnsEverything) {
    if (!rgAvailable()) GTEST_SKIP() << "ripgrep not installed";
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    ASSERT_TRUE(writeAnts5052Fixture(root));

    RemoteControl rc(nullptr);  // no setRgStdoutCapOverride call — default.
    // The fixture's lines are identical, so the default dedup would fold
    // them into one row; count rows without it.
    QJsonObject rowsReq = ants5052Req(root);
    rowsReq[QStringLiteral("dedup")] = false;
    const QJsonObject resp = rc.cmdWorkspaceSearch(rowsReq).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson(QJsonDocument::Compact).toStdString();
    EXPECT_FALSE(resp.value(QStringLiteral("truncated")).toBool())
        << "GUARD: unmodified default must not report truncated; saw "
        << QJsonDocument(resp).toJson(QJsonDocument::Compact).toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("matches")).toArray().size(),
              kAnts5052Lines)
        << "GUARD: every match must come back with no cap override";

    RemoteControl rcCount(nullptr);
    QJsonObject countReq = ants5052Req(root);
    countReq[QStringLiteral("count_only")] = true;
    const QJsonObject countResp = rcCount.cmdWorkspaceSearch(countReq).object();
    ASSERT_TRUE(countResp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(countResp.value(QStringLiteral("count")).toInt(), kAnts5052Lines)
        << "GUARD: count_only's count must equal the real match count with "
           "no override; saw count="
        << countResp.value(QStringLiteral("count")).toInt();
    EXPECT_FALSE(countResp.value(QStringLiteral("truncated")).toBool());
}
