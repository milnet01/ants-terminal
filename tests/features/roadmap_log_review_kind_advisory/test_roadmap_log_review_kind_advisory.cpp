// ANTS-4989 — roadmap_log warns when a review-shaped Source: is filed as a
// plain `fix`. See spec.md.
//
// BEHAVIOURAL on the decision, SCRAPE on the wiring, and BOTH halves of that
// split were forced rather than chosen. cmdRoadmapLog refuses `no_main`
// without a MainWindow, so a first draft driving the verb got a refusal for
// every input and its "no advisory here" cases passed vacuously. And the
// predicate first lived in remotecontrol_internal.h, which rc_tu_split INV-5
// forbids any test to include — so it moved to RoadmapParse, where the rule
// belongs anyway: it is a roadmap-convention rule, not RemoteControl
// plumbing. RoadmapParse returns the decision; RemoteControl builds the
// envelope.
#include "remotecontrol.h"
#include "roadmapparse.h"

#include <gtest/gtest.h>
#include "../../_support/roadmap_migrated_store.h"
#include "../../_support/srcgrep.h"
#include "../../_support/xdg_guard.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

#include <string>

using RoadmapParse::reviewKindMismatch;

namespace {

bool warns(const char *kind, const char *source) {
    return !reviewKindMismatch(QString::fromUtf8(kind),
                               QString::fromUtf8(source)).matchedPrefix.isEmpty();
}

QString suggestion(const char *kind, const char *source) {
    return reviewKindMismatch(QString::fromUtf8(kind),
                              QString::fromUtf8(source)).suggestedKind;
}

}  // namespace

// INV-1 — the pairing the rule is about.
TEST(RoadmapLogReviewKindAdvisory, Inv1FiresOnAReviewShapedSourceWithPlainFix) {
    EXPECT_TRUE(warns("fix", "indie-review-2026-05-13"));
    EXPECT_EQ(reviewKindMismatch(QStringLiteral("fix"),
                                 QStringLiteral("indie-review-2026-05-13")).matchedPrefix,
              QStringLiteral("indie-review"));
}

// INV-3 — silent on correctly-filed work and on work from no review. These
// cases are the ones that passed vacuously when the verb refused every call.
TEST(RoadmapLogReviewKindAdvisory, Inv3SilentWhenTheKindOrSourceIsRight) {
    EXPECT_FALSE(warns("review-fix", "indie-review-2026-05-13"))
        << "a check that fires on correctly-filed work trains its reader to "
           "ignore it";
    EXPECT_FALSE(warns("audit-fix", "audit-2026-05-13"));
    EXPECT_FALSE(warns("fix", "user-request-2026-05-13"))
        << "a user report is not a review origin";
    EXPECT_FALSE(warns("fix", "planned"));
    EXPECT_FALSE(warns("perf", "indie-review-2026-05-13"))
        << "the rule reaches only items that would otherwise be `fix` — a "
           "review legitimately produces work of any kind, which is the whole "
           "point of roadmap-format.md 3.5.3's orthogonality rule";
}

// INV-4 — every live spelling of one origin. The standard keeps several.
TEST(RoadmapLogReviewKindAdvisory, Inv4EveryLiveSpellingTriggersIt) {
    for (const char *src : {"indie-review-2026-05-13",
                            "code-quality-review-2026-08-20",
                            "review-code sweep 2026-08-31",
                            "cold-eyes-2026-05-21",
                            "check-code-sweep-2026-09-01",
                            "test-audit-2026-05-15",
                            "doc-review-2026-04-15"}) {
        EXPECT_TRUE(warns("fix", src))
            << src << " is a live review spelling; a check recognising only "
            << "the current skill name misses most of the corpus";
    }
    // Case-folded: the stored value's casing is the author's.
    EXPECT_TRUE(warns("FIX", "Indie-Review-2026-05-13"));
}

// INV-5 — the suggestion follows the origin.
TEST(RoadmapLogReviewKindAdvisory, Inv5SuggestionMatchesTheOrigin) {
    EXPECT_EQ(suggestion("fix", "audit-2026-05-13"), QStringLiteral("audit-fix"));
    EXPECT_EQ(suggestion("fix", "check-code-sweep-2026-09-01"),
              QStringLiteral("audit-fix"));
    EXPECT_EQ(suggestion("fix", "cold-eyes-2026-05-21"),
              QStringLiteral("review-fix"));
    EXPECT_EQ(suggestion("fix", "indie-review-2026-05-13"),
              QStringLiteral("review-fix"));
}

// `audit-` carries its hyphen so the prefix cannot swallow an unrelated
// source that merely starts with the word.
TEST(RoadmapLogReviewKindAdvisory, AuditPrefixDoesNotSwallowUnrelatedSources) {
    EXPECT_FALSE(warns("fix", "auditor-tooling-2026-05-13"));
}

// INV-2 + INV-6 — the wiring. A predicate nothing calls warns nobody, and
// both write paths must carry it; ANTS-4527's advisory had to be duplicated
// across exactly these two.
//
// Scraped through ants_test::slurpRemoteControl(), which concatenates the RC
// translation units, rather than by naming those files: rc_tu_split INV-11
// requires every RC TU literal to live in the ANTS_RC_SOURCES_REL block and
// nowhere else, so a test naming one breaks that contract. It caught an
// earlier draft of this file doing exactly that.
TEST(RoadmapLogReviewKindAdvisory, Inv2AndInv6WiredIntoBothWritePaths) {
    const std::string rc = ants_test::slurpRemoteControl();
    ASSERT_FALSE(rc.empty());

    EXPECT_NE(rc.find("rlReviewKindAdvisory(kind, source)"), std::string::npos)
        << "ANTS-4989 INV-2: op:append must call the advisory";
    // No variable name in the needle: ANTS-5114 renamed the list this
    // assigns, and the wiring, not the local's name, is the contract.
    EXPECT_EQ(rc.find("b.value(QStringLiteral(\"id_hint\")).toString()"), std::string::npos)
        << "ANTS-5095: the batch advisory reads a numeric id_hint as a string "
           "and names headlines instead of the allocated ids";
    EXPECT_NE(rc.find("first[QStringLiteral(\"bullets\")] = "),
              std::string::npos)
        << "ANTS-4989 INV-6: op:append_batch must roll the advisory up over "
           "its bullets — a batch filing one review's findings is where the "
           "pairing recurs, and N identical advisories would bury the reply";
    EXPECT_EQ(rc.find("return rlErr(QStringLiteral(\"kind_ignores_review"),
              std::string::npos)
        << "ANTS-4989 INV-2: it is an ADVISORY — it must never become a refusal";
}

// INV-7 — the store path warns too, once, naming the ids it allocated. The
// batch op's store write built its own reply and left this advisory out
// (ANTS-6176), so every migrated project filed review fixes as `fix` unwarned.
TEST(RoadmapLogReviewKindAdvisory, Inv7StoreBatchWarnsOnceNamingIds) {
    QTemporaryDir xdg;
    QTemporaryDir proj;
    ASSERT_TRUE(xdg.isValid() && proj.isValid());
    ants_test::XdgGuard env;
    env.setEnv("XDG_DATA_HOME", xdg.path().toLocal8Bit());
    {
        QFile f(proj.path() + QStringLiteral("/ROADMAP.md"));
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        // ants-v1 with a Layman line on the open item: the store serves only
        // that dialect, and the render refuses an open item without one.
        f.write("<!-- ants-roadmap-format: 1 -->\n\n# Demo — Roadmap\n\n## Work\n\n"
                "- \xF0\x9F\x93\x8B [DEMO-0007] **An open item.**\n"
                "  Layman: A thing.\n  Kind: implement.\n  Source: fixture.\n");
    }
    ASSERT_TRUE(ants_test::migrateDefaultStore(proj.path()));

    QJsonArray bullets;
    for (const char *h : {"First finding.", "Second finding."}) {
        QJsonObject b;
        b[QStringLiteral("headline")] = QString::fromUtf8(h);
        b[QStringLiteral("layman")]   = QStringLiteral("A plain sentence.");
        bullets.append(b);
    }
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = proj.path();
    req[QStringLiteral("op")]         = QStringLiteral("append_batch");
    req[QStringLiteral("section")]    = QStringLiteral("work");
    req[QStringLiteral("status")]     = QStringLiteral("planned");
    req[QStringLiteral("kind")]       = QStringLiteral("fix");
    req[QStringLiteral("source")]     = QStringLiteral("indie-review-2026-11-20");
    req[QStringLiteral("bullets")]    = bullets;

    RemoteControl rc(nullptr);
    const QJsonObject out = rc.cmdRoadmapLogAppendBatchForTest(req).object();
    const std::string dump = QJsonDocument(out).toJson().toStdString();
    ASSERT_TRUE(out.value(QStringLiteral("ok")).toBool()) << dump;
    ASSERT_TRUE(out.contains(QStringLiteral("items_rendered")))
        << "did not take the store path, so this case proves nothing\n" << dump;

    QJsonArray hits;
    for (const auto &w : out.value(QStringLiteral("warnings")).toArray())
        if (w.toObject().value(QStringLiteral("code")).toString()
                == QLatin1String("kind_ignores_review_provenance"))
            hits.append(w);
    ASSERT_EQ(hits.size(), 1)
        << "INV-7: one warning for the batch on the store path\n" << dump;
    EXPECT_EQ(hits.at(0).toObject().value(QStringLiteral("bullets")).toArray(),
              out.value(QStringLiteral("ids")).toArray())
        << "INV-7: the warning names the ids the batch allocated\n" << dump;
}
