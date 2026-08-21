// Feature-conformance test for spec.md — ANTS-4611.
//
//   INV-1 an explicit caller_cwd is never described as "the active tab"
//   INV-2 the refusal echoes the root that was actually searched
//   INV-3 the tab wording survives where the tab really was the source
//   INV-4 the verdict is unchanged — still ok:false / no_roadmap_loaded
//
// Behavioural: drives RemoteControl::cmdRoadmapQuery against a root with no
// ROADMAP.md. A source-scrape would prove a string exists; the defect being
// closed is what the caller reads and concludes from it.

#include "remotecontrol.h"

#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

#include <gtest/gtest.h>

namespace {

QJsonObject queryUnder(const QString &root) {
    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    return rc.cmdRoadmapQuery(req).object();
}

}  // namespace

// INV-4 first: the verdict was always right, and the fix must not move it.
// 9 of the 24 projects on this machine keep no ROADMAP.md, so this is the
// common path rather than an edge one.
TEST(roadmap_query_no_roadmap_refusal, Inv4VerdictUnchanged) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QJsonObject resp = queryUnder(tmp.path());
    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("no_roadmap_loaded"));
}

// INV-1 — the caller passed a root; blaming "the active tab" sends them to
// diagnose tab misrouting, and both wrong conclusions available to them
// (retry, switch tab, abandon) are reasonable given that wording.
TEST(roadmap_query_no_roadmap_refusal, Inv1ExplicitCwdIsNotBlamedOnTheTab) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QString err = queryUnder(tmp.path())
                            .value(QStringLiteral("error")).toString();
    EXPECT_FALSE(err.contains(QStringLiteral("active tab")))
        << "INV-1: an explicit caller_cwd was resolved, not the tab: " << err.toStdString();
}

// INV-2 — echo the root that was searched. Without it nothing in the envelope
// says where the verb looked, so the caller cannot tell a genuinely
// roadmap-less project from a misresolved root.
TEST(roadmap_query_no_roadmap_refusal, Inv2RefusalEchoesResolvedRoot) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QJsonObject resp = queryUnder(tmp.path());
    const QString root = resp.value(QStringLiteral("resolved_root")).toString();
    ASSERT_FALSE(root.isEmpty()) << "INV-2: no resolved_root in the refusal";
    EXPECT_TRUE(resp.value(QStringLiteral("error")).toString().contains(root))
        << "INV-2: the message must name the root it searched";
}
