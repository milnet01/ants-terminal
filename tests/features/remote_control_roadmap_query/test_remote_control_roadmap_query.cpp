// Feature-conformance test for tests/features/remote_control_roadmap_query/spec.md.
//
// Locks the ANTS-1117 v1 contract for the `roadmap-query` IPC verb:
// - `RoadmapDialog::parseBullets` correctness (behavioural)
// - `RemoteControl::cmdRoadmapQuery` registration + error shape
//   (source-grep)
//
// Exit 0 = all 9 invariants hold.

#include "roadmapdialog.h"

#include <QCoreApplication>
#include <QStringList>

#include <string>


#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"
namespace {


bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

int fail(const char *label, const char *why) {
    ADD_FAILURE_AT(__FILE__, __LINE__) << "[" << label << "] " << why;
    return 1;
}

}  // namespace

static int runMain() {
    // QCoreApplication app(argc, argv);  // ANTS-1217: bundle_main creates the app

    const std::string rcSrc = ants_test::slurpFile(SRC_RC_CPP);
    const std::string rcHdr = ants_test::slurpFile(SRC_RC_HEADER);
    const std::string ciSrc =
        ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);   // ANTS-3698
    if (rcSrc.empty()) fail("INV-7", "remotecontrol.cpp not readable");
    if (rcHdr.empty()) fail("INV-7", "remotecontrol.h not readable");

    // Behavioural INVs 1-6: drive parseBullets directly.
    {
        const QString doc = QStringLiteral(
            "## Section A\n"
            "\n"
            "- ✅ [ANTS-1042] **Done thing.** body line 1\n"
            "  body line 2 (continuation)\n"
            "  Kind: implement.\n"
            "  Lanes: AuditDialog, MainWindow.\n"
            "\n"
            "- 📋 [ANTS-1043] **Planned thing.** body.\n"
            "  Kind: fix.\n"
            "\n"
            "- 🚧 **In-progress thing.** body.\n"
            "\n"
            "- 💭 [ANTS-1099] **Considered thing.** body.\n"
            "\n"
            "- Plain narration bullet without emoji.\n");

        const auto bullets = RoadmapDialog::parseBullets(doc);

        // INV-1: only status-emoji bullets — 4 entries.
        if (bullets.size() != 4)
            fail("INV-1",
                        "expected exactly 4 status-emoji bullets");

        // INV-3: emojis recognised in document order.
        const QString expectedStatuses[] = {
            QStringLiteral("✅"), QStringLiteral("📋"),
            QStringLiteral("🚧"), QStringLiteral("💭"),
        };
        for (int i = 0; i < 4; ++i) {
            if (bullets[i].status != expectedStatuses[i])
                fail("INV-3", "status emoji mismatch in document order");
        }

        // INV-2: id matches ^ANTS-\d+$ when token present, else empty.
        if (bullets[0].id != QStringLiteral("ANTS-1042"))
            fail("INV-2", "ANTS-1042 not extracted");
        if (bullets[1].id != QStringLiteral("ANTS-1043"))
            fail("INV-2", "ANTS-1043 not extracted");
        if (!bullets[2].id.isEmpty())
            fail("INV-2",
                        "no [ANTS-NNNN] token → id field must be empty");
        if (bullets[3].id != QStringLiteral("ANTS-1099"))
            fail("INV-2", "ANTS-1099 not extracted");

        // INV-3 negative: plain narration bullet must NOT appear.
        for (const auto &b : bullets) {
            if (b.headline.contains(QStringLiteral("Plain narration")))
                fail("INV-3",
                            "plain-narration bullet was incorrectly included");
        }

        // INV-5: multi-line body bullet keeps headline ("Done thing.").
        if (!bullets[0].headline.contains(QStringLiteral("Done thing")))
            fail("INV-5",
                        "multi-line bullet should still expose Done thing headline");

        // INV-6: Kind / Lanes extraction.
        if (bullets[0].kind != QStringLiteral("implement"))
            fail("INV-6",
                        "kind=implement not extracted from Kind: line");
        if (bullets[0].lanes.size() != 2 ||
            bullets[0].lanes[0] != QStringLiteral("AuditDialog") ||
            bullets[0].lanes[1] != QStringLiteral("MainWindow"))
            fail("INV-6",
                        "lanes list not parsed correctly");
        if (bullets[1].kind != QStringLiteral("fix"))
            fail("INV-6", "kind=fix not extracted");
        if (!bullets[1].lanes.isEmpty())
            fail("INV-6",
                        "absent Lanes: should yield empty list");

        // INV-4: idempotent — second call returns byte-identical output.
        const auto bullets2 = RoadmapDialog::parseBullets(doc);
        if (bullets.size() != bullets2.size())
            fail("INV-4", "idempotency size mismatch");
        for (int i = 0; i < bullets.size(); ++i) {
            if (bullets[i].id != bullets2[i].id ||
                bullets[i].status != bullets2[i].status ||
                bullets[i].headline != bullets2[i].headline ||
                bullets[i].kind != bullets2[i].kind ||
                bullets[i].lanes != bullets2[i].lanes)
                fail("INV-4", "idempotency record mismatch");
        }
    }

    // INV-10 (ANTS-2075): a headline longer than the 120-char display cap
    // is truncated in `headline` but retained verbatim in `headlineFull`,
    // so a roadmap_log headline locator (which hashes the FULL headline)
    // is recoverable. Short headlines leave headlineFull == headline.
    {
        const QString longText =
            QStringLiteral("This is a deliberately very long narrator-style "
                           "headline that comfortably exceeds the one hundred "
                           "and twenty character display cap so the parser "
                           "must truncate it.");
        const QString doc = QStringLiteral(
            "## Section L\n\n- 📋 [ANTS-9001] **%1** body.\n").arg(longText);
        const auto bullets = RoadmapDialog::parseBullets(doc);
        if (bullets.size() != 1)
            fail("INV-10", "expected exactly one long-headline bullet");
        const auto &b = bullets[0];
        if (b.headline.size() > 121)
            fail("INV-10", "headline not truncated to the 120-char cap");
        if (!b.headline.endsWith(QStringLiteral("…")))
            fail("INV-10", "truncated headline must end with an ellipsis");
        if (b.headlineFull != longText)
            fail("INV-10",
                        "headlineFull must retain the untruncated headline");
        if (b.headlineFull == b.headline)
            fail("INV-10",
                        "headlineFull must differ from the truncated headline");

        // Short headline: headlineFull mirrors headline (no spurious echo).
        const QString shortDoc = QStringLiteral(
            "## Section S\n\n- 📋 [ANTS-9002] **Short headline.** body.\n");
        const auto sb = RoadmapDialog::parseBullets(shortDoc);
        if (sb.size() != 1 || sb[0].headlineFull != sb[0].headline)
            fail("INV-10",
                        "short headline must leave headlineFull == headline");
    }

    // INV-7: dispatch registers `roadmap-query`.
    if (!contains(rcSrc, "QLatin1String(\"roadmap-query\")"))
        fail("INV-7", "dispatch missing roadmap-query branch");
    if (!contains(rcHdr, "cmdRoadmapQuery"))
        fail("INV-7", "cmdRoadmapQuery not declared in remotecontrol.h");
    if (!contains(rcSrc, "RemoteControl::cmdRoadmapQuery"))
        fail("INV-7", "cmdRoadmapQuery handler body missing");

    // INV-8: unified-shape error when no roadmap is loaded.
    if (!contains(rcSrc, "no_roadmap_loaded"))
        fail("INV-8",
                    "no_roadmap_loaded error code missing from cmdRoadmapQuery");
    if (!contains(rcSrc, "roadmapPathForRemote"))
        fail("INV-8",
                    "cmdRoadmapQuery must call MainWindow::roadmapPathForRemote()");

    // INV-9: cache fields wired up.
    const char *cacheFields[] = {
        "m_roadmapCachePath",
        "m_roadmapCacheMtimeMs",
        "m_roadmapCacheBullets",
    };
    for (const char *f : cacheFields) {
        if (!contains(rcHdr, f))
            fail("INV-9", f);
        if (!contains(rcSrc, f))
            fail("INV-9", f);
    }

    // INV-10 (ANTS-2075) source surface: roadmap_query emits headline_full
    // via the rcMaybeEmitHeadlineFull helper on the bullets[] path.
    if (!contains(rcSrc, "rcMaybeEmitHeadlineFull"))
        fail("INV-10",
                    "roadmap_query does not emit headline_full (ANTS-2075)");

    // INV-12 (ANTS-3722) — a bullet that QUOTES a trailer key in its prose
    // must not acquire that key as a field. Un-anchored `Lanes:` matching is
    // deliberate (ANTS-2058: bullets write `Kind: x. Lanes: y. Source: z.`
    // inline), so the guard is backticks, not an anchor.
    {
        const QString doc = QStringLiteral(
            "## Work\n\n"
            "- \U0001F4CB [ANTS-0001] **Quoting the trailer keys.**\n"
            "  The note landed above the `Layman:`/`Kind:`/`Lanes:`/`Source:` "
            "trailer.\n"
            "  Kind: fix.\n\n"
            "- \U0001F4CB [ANTS-0002] **Real inline trailer.**\n"
            "  Body. Kind: fix. Lanes: AuditDialog, MainWindow. Source: x.\n\n");
        const auto bs = RoadmapDialog::parseBullets(doc);
        if (bs.size() != 2)
            fail("INV-12", "fixture did not parse as two bullets");
        else {
            if (!bs[0].lanes.isEmpty())
                fail("INV-12",
                     "a backticked `Lanes:` in prose must not become a lane");
            if (bs[1].lanes.size() != 2)
                fail("INV-12",
                     "ANTS-2058's inline `Lanes:` trailer must still parse");
        }
    }

    // INV-11 (ANTS-3698) — `filter` is honoured as an alias for `status`.
    // The envelope has always echoed the applied lifecycle as `filter`, so
    // that is the name a caller writing the next call from a response sends;
    // it used to be an unrecognised arg, silently dropped, answered with the
    // FULL set under an echo that read as confirmation. The verb is not
    // behaviourally reachable from this bundle (no GUI link), so the wiring
    // is asserted at the source — both halves, since honouring the arg
    // without declaring it would leave it reported in ignored_args.
    if (!contains(rcSrc, "req.value(QStringLiteral(\"filter\"))"))
        fail("INV-11",
             "cmdRoadmapQuery does not fall back to the `filter` alias");
    if (!contains(ciSrc, "props[\"filter\"] = filterAliasProp"))
        fail("INV-11",
             "roadmap_query schema does not declare the `filter` alias");

    std::puts("OK remote_control_roadmap_query: 12/12 invariants");
    return 0;
}

TEST(RemoteControlRoadmapQuery, Main) {
    ASSERT_EQ(0, runMain());
}
