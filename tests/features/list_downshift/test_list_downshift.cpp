// ANTS-3543 — auto-downshift a would-be-truncated list to its lean projection.
// Pure-function tests on PaginationEngine::pageBullets (engine INV-1..4) and
// RemoteControl::downshiftMatches (workspace_search INV-8/9/10), plus
// source-scrape on the cmdRoadmapQuery + cmdWorkspaceSearch wiring
// (INV-5/6/7/11). See docs/specs/ANTS-3543.md.

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"
#include "paginationengine.h"
#include "remotecontrol.h"

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <string>

ANTS_TEST_SCOPE();

namespace {

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

size_t countOccurrences(const std::string &hay, const std::string &needle) {
    size_t n = 0, pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

// Fat roadmap bullets: a heavy `body` field is what blows the byte budget;
// the lean projection drops it. Sized so the fat array overflows the ~20 KB
// soft cap while individual rows fit.
QJsonArray makeFatBullets(int n, int bodyBytes = 200) {
    QJsonArray arr;
    const QString body(bodyBytes, QLatin1Char('y'));
    for (int i = 0; i < n; ++i) {
        QJsonObject o;
        o["id"]               = QStringLiteral("ANTS-%1").arg(i, 4, 10,
                                                              QLatin1Char('0'));
        o["status"]           = QStringLiteral("planned");
        o["headline_oneline"] = QStringLiteral("headline number %1").arg(i);
        o["section_slug"]     = QStringLiteral("sec");
        o["body"]             = body;   // heavy field the downshift removes
        arr.append(o);
    }
    return arr;
}

// Mirrors rcProjectHeadlineOnly: keep the four identity keys, drop body.
void headlineProj(QJsonArray &arr) {
    for (int i = 0; i < arr.size(); ++i) {
        const QJsonObject src = arr.at(i).toObject();
        QJsonObject p;
        p["id"]               = src.value("id");
        p["status"]           = src.value("status");
        p["headline_oneline"] = src.value("headline_oneline");
        p["section_slug"]     = src.value("section_slug");
        arr.replace(i, p);
    }
}

// Fat workspace_search matches: big context_before/after arrays are the
// weight; the lean projection drops them (rename text→headline, keep file/
// line + also_at). Sized to overflow a small maxBytes.
QJsonObject makeMatchEnv(int n, bool scanTruncated, int contextLines = 8) {
    QJsonObject out;
    out["ok"]        = true;
    out["pattern"]   = QStringLiteral("foo");
    out["truncated"] = scanTruncated;   // pre-cap scan-cutoff meaning
    QJsonArray matches;
    const QString ctxPad(50, QLatin1Char('c'));
    for (int i = 0; i < n; ++i) {
        QJsonObject m;
        m["file"] = QStringLiteral("src/file%1.cpp").arg(i);
        m["line"] = i;
        m["text"] = QStringLiteral("a matching line %1").arg(i);
        QJsonArray ctxB, ctxA;
        for (int k = 0; k < contextLines; ++k) {
            QJsonObject b; b["line"] = i - k - 1; b["text"] = ctxPad; ctxB.append(b);
            QJsonObject a; a["line"] = i + k + 1; a["text"] = ctxPad; ctxA.append(a);
        }
        m["context_before"] = ctxB;
        m["context_after"]  = ctxA;
        matches.append(m);
    }
    out["matches"] = matches;
    return out;
}

// Mirrors rcApplyHeadlineOnly: text→headline, drop context, keep also_at.
void matchProj(QJsonArray &m) {
    for (int i = 0; i < m.size(); ++i) {
        const QJsonObject o = m.at(i).toObject();
        QJsonObject p;
        p["file"] = o.value("file");
        p["line"] = o.value("line");
        if (o.contains("text"))    p["headline"] = o.value("text");
        if (o.contains("also_at")) p["also_at"]  = o.value("also_at");
        m.replace(i, p);
    }
}

}  // namespace

// ─── Engine INV-1: default (absent) projector never downshifts ───────────────
TEST(list_downshift, Inv1DefaultProjectorNoDownshift) {
    const auto arr = makeFatBullets(500);            // overflows the soft cap
    const auto r = PaginationEngine::pageBullets(arr, 0, -1);  // no projector
    EXPECT_TRUE(r.truncated);       // the fat set still truncates as before
    EXPECT_FALSE(r.downshifted);    // but no projector → no downshift
    // A small set is likewise never downshifted (and never truncates).
    const auto s = PaginationEngine::pageBullets(makeFatBullets(3), 0, -1);
    EXPECT_FALSE(s.truncated);
    EXPECT_FALSE(s.downshifted);
}

// ─── Engine INV-2: fires iff projector set AND limit<=0 AND fat truncated ────
TEST(list_downshift, Inv2GateConditions) {
    // auto path (limit -1) + projector + fat truncates → downshift.
    const auto a = PaginationEngine::pageBullets(makeFatBullets(500), 0, -1,
                                                 headlineProj);
    EXPECT_TRUE(a.downshifted);
    // explicit positive limit → never downshift (fat paging respected).
    const auto b = PaginationEngine::pageBullets(makeFatBullets(500), 0, 50,
                                                 headlineProj);
    EXPECT_FALSE(b.downshifted);
    EXPECT_EQ(b.slice.size(), 50);
    // projector set but the set doesn't truncate → no downshift.
    const auto c = PaginationEngine::pageBullets(makeFatBullets(3), 0, -1,
                                                 headlineProj);
    EXPECT_FALSE(c.truncated);
    EXPECT_FALSE(c.downshifted);
}

// ─── Engine INV-3: projects a copy of the FULL set, re-pages from offset ─────
TEST(list_downshift, Inv3FitsWhenLean) {
    const int N = 120;
    const auto full = makeFatBullets(N, 200);        // fat overflows, lean fits
    const auto r = PaginationEngine::pageBullets(full, 0, -1, headlineProj);
    EXPECT_TRUE(r.downshifted);
    EXPECT_FALSE(r.truncated);          // whole lean set fits
    EXPECT_EQ(r.nextOffset, -1);
    EXPECT_EQ(r.slice.size(), N);       // every item present
    EXPECT_FALSE(r.slice.at(0).toObject().contains("body"));  // rows are lean
}

TEST(list_downshift, Inv3OverflowsEvenWhenLean) {
    const auto full = makeFatBullets(500, 200);
    const auto fat  = PaginationEngine::pageBullets(full, 0, -1);  // no proj
    const auto lean = PaginationEngine::pageBullets(full, 0, -1, headlineProj);
    EXPECT_TRUE(lean.downshifted);
    EXPECT_TRUE(lean.truncated);        // even lean overflows
    EXPECT_GT(lean.nextOffset, 0);
    // Body-bearing rows → the lean page holds strictly MORE rows than fat.
    EXPECT_GT(lean.slice.size(), fat.slice.size());
}

TEST(list_downshift, Inv3RepagesFromOffset) {
    const int N = 120;
    const auto full = makeFatBullets(N, 200);
    const auto r = PaginationEngine::pageBullets(full, 10, -1, headlineProj);
    EXPECT_TRUE(r.downshifted);
    EXPECT_EQ(r.offset, 10);
    EXPECT_FALSE(r.truncated);
    EXPECT_EQ(r.slice.size(), N - 10);  // every item from offset present
}

// ─── Engine INV-4: input never mutated; downshifted defaults false ───────────
TEST(list_downshift, Inv4NoInputMutationAndDefault) {
    auto full = makeFatBullets(500, 200);
    const QJsonArray before = full;
    const auto r = PaginationEngine::pageBullets(full, 0, -1, headlineProj);
    EXPECT_TRUE(r.downshifted);
    EXPECT_TRUE(full == before);        // caller's array untouched (copy proj)
    PaginationEngine::PageResult def;
    EXPECT_FALSE(def.downshifted);      // struct default
}

// ─── workspace_search INV-8: honest `truncated` across the 4-case truth table ─
TEST(list_downshift, Inv8DownshiftMatchesTruthTable) {
    const int maxBytes = 6000;

    // (a) no scan cutoff + lean fits → all matches lean, downshifted, no
    //     results_dropped, truncated falsy (the fat-cap's stale true cleared).
    {
        QJsonObject out = makeMatchEnv(40, /*scan*/false);
        RemoteControl::downshiftMatches(out, /*alreadyLean*/false,
                                        /*scanTruncated*/false, maxBytes, matchProj);
        EXPECT_TRUE(out.value("downshifted").toBool());
        EXPECT_TRUE(out.value("headline_only").toBool());
        EXPECT_FALSE(out.contains("results_dropped"));
        EXPECT_FALSE(out.value("truncated").toBool());
        EXPECT_EQ(out.value("matches").toArray().size(), 40);  // whole set
    }
    // (b) scan cutoff + lean fits → downshifted AND truncated:true preserved.
    {
        QJsonObject out = makeMatchEnv(40, /*scan*/true);
        RemoteControl::downshiftMatches(out, false, /*scanTruncated*/true,
                                        maxBytes, matchProj);
        EXPECT_TRUE(out.value("downshifted").toBool());
        EXPECT_TRUE(out.value("truncated").toBool());          // scan cutoff kept
        EXPECT_FALSE(out.contains("results_dropped"));
    }
    // (c) scan cutoff + lean drops → results_dropped + truncated:true.
    {
        QJsonObject out = makeMatchEnv(500, /*scan*/true);
        RemoteControl::downshiftMatches(out, false, /*scanTruncated*/true,
                                        maxBytes, matchProj);
        EXPECT_TRUE(out.value("downshifted").toBool());
        EXPECT_TRUE(out.contains("results_dropped"));
        EXPECT_TRUE(out.value("truncated").toBool());
    }
    // (d) no scan cutoff + lean drops → results_dropped + truncated:true
    //     (recomputed from the lean re-cap drop, not the scan).
    {
        QJsonObject out = makeMatchEnv(500, /*scan*/false);
        RemoteControl::downshiftMatches(out, false, /*scanTruncated*/false,
                                        maxBytes, matchProj);
        EXPECT_TRUE(out.value("downshifted").toBool());
        EXPECT_TRUE(out.contains("results_dropped"));
        EXPECT_TRUE(out.value("truncated").toBool());
    }
}

// ─── workspace_search INV-9: already-lean caller never downshifts ────────────
TEST(list_downshift, Inv9AlreadyLeanNoDownshift) {
    QJsonObject out = makeMatchEnv(500, /*scan*/false);
    RemoteControl::downshiftMatches(out, /*alreadyLean*/true,
                                    /*scanTruncated*/false, 6000, matchProj);
    EXPECT_FALSE(out.contains("downshifted"));   // gated off
    EXPECT_TRUE(out.contains("results_dropped")); // capped exactly as pre-3543
}

// ─── workspace_search INV-10: also_at preserved, fat tail never resurrected ──
TEST(list_downshift, Inv10AlsoAtPreservedLeanOnly) {
    QJsonObject out = makeMatchEnv(40, /*scan*/false);
    // Inject an also_at fan-out on match 0.
    {
        QJsonArray matches = out.value("matches").toArray();
        QJsonObject m0 = matches.at(0).toObject();
        QJsonArray alsoAt;
        QJsonObject dup; dup["file"] = QStringLiteral("src/dup.cpp"); dup["line"] = 7;
        alsoAt.append(dup);
        m0["also_at"] = alsoAt;
        matches.replace(0, m0);
        out["matches"] = matches;
    }
    RemoteControl::downshiftMatches(out, false, false, 6000, matchProj);
    ASSERT_TRUE(out.value("downshifted").toBool());
    const QJsonObject first = out.value("matches").toArray().at(0).toObject();
    EXPECT_TRUE(first.contains("also_at"));            // dedup fan-out survives
    EXPECT_FALSE(first.contains("context_before"));    // fat context dropped
    EXPECT_FALSE(first.contains("context_after"));
    EXPECT_TRUE(first.contains("headline"));           // text→headline
}

// bytes_cap_clamped echo survives the helper (non-downshift clamp path).
TEST(list_downshift, BytesCapClampedPreserved) {
    QJsonObject out = makeMatchEnv(3, /*scan*/false);  // tiny, fits under ceiling
    RemoteControl::downshiftMatches(out, /*alreadyLean*/true, false,
                                    RemoteControl::kReadToolMaxBytesCeiling + 1,
                                    matchProj);
    EXPECT_TRUE(out.value("bytes_cap_clamped").toBool());
}

// ─── Source-scrape INV-5/6/7/11 — cmdRoadmapQuery wiring ─────────────────────
TEST(list_downshift, Inv5And6And11RoadmapWiring) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    // INV-5 — gated projector at BOTH bullet sites (section + full-file).
    expect(countOccurrences(cpp, "RowProjector(&rcProjectHeadlineOnly)") == 2,
           "INV-5: rcProjectHeadlineOnly wired as the projector at exactly the "
           "two bullet-emitting pageBullets sites");
    expect(countOccurrences(
               cpp, "(mode != QLatin1String(\"headline_only\")) && !includeBody")
               == 2,
           "INV-5: the downshift gate (not-lean AND not-include_body) guards "
           "both bullet sites");
    // INV-6 / INV-11 — truthy-only downshifted emit at both bullet sites.
    expect(countOccurrences(cpp, "if (page.downshifted) out[\"downshifted\"] = true;")
               == 2,
           "INV-6/INV-11: downshifted emitted only when true, at both sites");
    // INV-11 — the ANTS-1436 exactly-5-pageBullets-calls count survives the
    // new 4th arg (no call site added or removed).
    expect(countOccurrences(cpp, "PaginationEngine::pageBullets(") == 5,
           "INV-11: pageBullets still called exactly 5 times (2 bullet + 1 "
           "section_index + 2 changelog); the 4th arg added no call site");
    EXPECT_EQ(0, expect_failures());
}

// INV-7 — the verb forwards an explicit limit verbatim (both bullet sites pass
// limitArg through unchanged; the engine, not the verb, gates the downshift).
TEST(list_downshift, Inv7ForwardsExplicitLimit) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    expect(countOccurrences(cpp, "filtered, offsetArg, limitArg,") == 2,
           "INV-7: both bullet sites pass offsetArg/limitArg verbatim into "
           "pageBullets (explicit limit respected; downshift is the 4th arg)");
    EXPECT_EQ(0, expect_failures());
}

// workspace_search routing + projector IDENTITY — cmdWorkspaceSearch routes its
// cap through downshiftMatches passing rcApplyHeadlineOnly (a wrong/empty
// projector would blank every row and no other test would catch it).
TEST(list_downshift, WorkspaceSearchRoutingAndProjectorIdentity) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    const std::string body = ants_test::slurpFunctionBody(
        cpp, "RemoteControl::cmdWorkspaceSearch");
    expect(!body.empty(), "cmdWorkspaceSearch body located");
    expect(contains(body, "RemoteControl::downshiftMatches("),
           "routing: cmdWorkspaceSearch caps via RemoteControl::downshiftMatches");
    expect(contains(body, "rcApplyHeadlineOnly"),
           "projector identity: rcApplyHeadlineOnly passed as the lean "
           "projector (not an empty/wrong callback)");
    // The byte-cap itself now lives in the header helper.
    const std::string hdr = ants_test::slurpFile(SRC_RC_HEADER);
    // Anchor on the definition signature, not the bare name — the bare name
    // also appears in an #include comment above, and slurpFunctionBody takes
    // the FIRST match.
    const std::string helper =
        ants_test::slurpFunctionBody(hdr, "void downshiftMatches");
    expect(contains(helper, "capJsonArrayToBytes") &&
               contains(helper, "results_dropped"),
           "downshiftMatches byte-caps matches[] (ANTS-1293 preserved)");
    EXPECT_EQ(0, expect_failures());
}

// ─── ANTS-3576 — changelog_query entries[] downshift wiring ──────────────────
// cmdChangelogQuery routes its fat entries[] page through the SAME engine
// downshift hook, passing a CHANGELOG-shaped projector (rcProjectChangelogHeadlineOnly,
// NOT the roadmap rcProjectHeadlineOnly — changelog rows differ), gated off in
// headline_only / include_body mode, and emits downshifted only when it fires.
TEST(list_downshift, ChangelogQueryDownshiftWiring) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    // The changelog-shaped projector must exist and differ from the roadmap
    // one (a copy-paste of rcProjectHeadlineOnly would blank version/ids).
    expect(contains(cpp, "void rcProjectChangelogHeadlineOnly(QJsonArray"),
           "ANTS-3576: rcProjectChangelogHeadlineOnly defined");
    const std::string projBody =
        ants_test::slurpFunctionBody(cpp, "void rcProjectChangelogHeadlineOnly");
    expect(contains(projBody, "text_oneline") &&
               contains(projBody, "simplified()"),
           "ANTS-3576: the changelog projector emits "
           "text_oneline = text.simplified() (lean shape, not the roadmap "
           "4-key shape)");
    const std::string body = ants_test::slurpFunctionBody(
        cpp, "RemoteControl::cmdChangelogQuery");
    expect(!body.empty(), "cmdChangelogQuery body located");
    expect(contains(body, "RowProjector(&rcProjectChangelogHeadlineOnly)"),
           "ANTS-3576: entries[] page wires the changelog-shaped projector as "
           "the downshift hook (projector identity — not the roadmap one)");
    expect(contains(body, "!headlineOnly && !includeBody"),
           "ANTS-3576: downshift gated off when the caller is already lean "
           "(headline_only) or wants bodies (include_body)");
    expect(contains(body, "if (page.downshifted) out[QStringLiteral(\"downshifted\")] = true;"),
           "ANTS-3576: downshifted emitted truthy-only on the entries branch");
    // The version_index page has no lean form and must stay 3-arg (no projector).
    expect(contains(body, "PaginationEngine::pageBullets(versions, offset,"),
           "ANTS-3576: version_index page stays projector-free (no lean form)");
    EXPECT_EQ(0, expect_failures());
}
