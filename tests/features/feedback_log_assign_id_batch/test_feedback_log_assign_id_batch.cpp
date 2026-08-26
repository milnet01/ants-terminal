// ANTS-4671 — feature-conformance test for feedback_log op:"assign_id_batch".
// Contract: tests/features/feedback_log_assign_id_batch/spec.md
//
// Drives the live RemoteControl::cmdFeedbackLog against a real file in a
// temp dir, then re-reads the file — the batch's whole point is that N
// decisions reach disk in ONE write, so asserting on the file rather than on
// the envelope is what makes the test about this op.

#include "remotecontrol.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>

namespace {

const char *kV2 =
    "<!-- ants-mcp-feedback: 2 -->\n"
    "# Feedback TEST\n"
    "\n"
    "### Issue #1 \xE2\x80\x94 first\n"
    "- **Proposed ID:** _(maintainer to assign)_\n"
    "- **What:** open.\n"
    "\n"
    "### Issue #2 \xE2\x80\x94 second\n"
    "- **Proposed ID:** _(maintainer to assign)_\n"
    "- **What:** open.\n"
    "\n"
    "### Issue #3 \xE2\x80\x94 third\n"
    "- **Proposed ID:** _(maintainer to assign)_\n"
    "- **What:** open.\n";

const QString kH1 = QString::fromUtf8("### Issue #1 \xE2\x80\x94 first");
const QString kH2 = QString::fromUtf8("### Issue #2 \xE2\x80\x94 second");
const QString kH3 = QString::fromUtf8("### Issue #3 \xE2\x80\x94 third");

bool writeStr(const QString &path, const QString &body) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const QByteArray u = body.toUtf8();
    const bool ok = (f.write(u) == u.size());
    f.close();
    return ok;
}

QString readStr(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

QJsonObject one(const QString &heading, const QString &id) {
    QJsonObject a;
    a[QStringLiteral("heading")] = heading;
    a[QStringLiteral("ids")]     = QJsonArray{id};
    return a;
}

struct Fx {
    QTemporaryDir dir;
    QString path;
    bool ok() {
        if (!dir.isValid()) return false;
        path = dir.path() + QStringLiteral("/TEST_Ants_MCP_Feedback.md");
        return writeStr(path, QString::fromUtf8(kV2));
    }
    QJsonObject req(const QJsonArray &assignments) const {
        QJsonObject r;
        r[QStringLiteral("op")]          = QStringLiteral("assign_id_batch");
        r[QStringLiteral("path")]        = path;
        r[QStringLiteral("caller_cwd")]  = dir.path();
        r[QStringLiteral("assignments")] = assignments;
        return r;
    }
};

}  // namespace

// ---------------------------------------------------------------- INV-1 -----

TEST(FeedbackAssignIdBatch, Inv1AppliesAll) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    const QJsonObject env = rc.cmdFeedbackLog(fx.req({
        one(kH1, QStringLiteral("ANTS-1001")),
        one(kH2, QStringLiteral("ANTS-1002")),
        one(kH3, QStringLiteral("ANTS-1003")),
    })).object();

    EXPECT_TRUE(env.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(env).toJson().toStdString();
    EXPECT_EQ(env.value(QStringLiteral("applied_count")).toInt(), 3);
    EXPECT_EQ(env.value(QStringLiteral("skipped_count")).toInt(), 0);

    const QString out = readStr(fx.path);
    EXPECT_TRUE(out.contains(QStringLiteral("- **Proposed ID:** ANTS-1001")));
    EXPECT_TRUE(out.contains(QStringLiteral("- **Proposed ID:** ANTS-1002")));
    EXPECT_TRUE(out.contains(QStringLiteral("- **Proposed ID:** ANTS-1003")));
    EXPECT_FALSE(out.contains(QStringLiteral("_(maintainer to assign)_")))
        << "every placeholder was filled by the one write";
}

// ---------------------------------------------------------------- INV-2 -----

TEST(FeedbackAssignIdBatch, Inv2OneBadHeadingDoesNotCostTheRest) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    const QJsonObject env = rc.cmdFeedbackLog(fx.req({
        one(kH1, QStringLiteral("ANTS-1001")),
        one(QStringLiteral("### No such finding"), QStringLiteral("ANTS-1002")),
        one(kH3, QStringLiteral("ANTS-1003")),
    })).object();

    EXPECT_TRUE(env.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(env).toJson().toStdString();
    EXPECT_EQ(env.value(QStringLiteral("applied_count")).toInt(), 2);
    ASSERT_EQ(env.value(QStringLiteral("skipped_count")).toInt(), 1);

    const QJsonObject bad =
        env.value(QStringLiteral("skipped")).toArray().at(0).toObject();
    EXPECT_EQ(bad.value(QStringLiteral("index")).toInt(), 1)
        << "the skipped row names WHICH assignment failed";
    EXPECT_EQ(bad.value(QStringLiteral("code")).toString(),
              QStringLiteral("target_not_found"));

    const QString out = readStr(fx.path);
    EXPECT_TRUE(out.contains(QStringLiteral("- **Proposed ID:** ANTS-1001")));
    EXPECT_TRUE(out.contains(QStringLiteral("- **Proposed ID:** ANTS-1003")));
    EXPECT_FALSE(out.contains(QStringLiteral("ANTS-1002")));
}

// ---------------------------------------------------------------- INV-3 -----

TEST(FeedbackAssignIdBatch, Inv3AllFailedRefuses) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    const QJsonObject env = rc.cmdFeedbackLog(fx.req({
        one(QStringLiteral("### Nope one"), QStringLiteral("ANTS-1001")),
        one(QStringLiteral("### Nope two"), QStringLiteral("ANTS-1002")),
    })).object();

    EXPECT_FALSE(env.value(QStringLiteral("ok")).toBool())
        << "an all-failed batch must not read as a completed triage: "
        << QJsonDocument(env).toJson().toStdString();
    EXPECT_EQ(env.value(QStringLiteral("skipped_count")).toInt(), 2);
    EXPECT_TRUE(readStr(fx.path).contains(QStringLiteral("_(maintainer to assign)_")));
}

// ---------------------------------------------------------------- INV-4 -----

TEST(FeedbackAssignIdBatch, Inv4DryRunWritesNothing) {
    Fx fx; ASSERT_TRUE(fx.ok());
    const QString before = readStr(fx.path);
    RemoteControl rc(nullptr);
    QJsonObject req = fx.req({one(kH1, QStringLiteral("ANTS-1001"))});
    req[QStringLiteral("dry_run")] = true;
    const QJsonObject env = rc.cmdFeedbackLog(req).object();

    EXPECT_TRUE(env.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(env).toJson().toStdString();
    EXPECT_TRUE(env.value(QStringLiteral("dry_run")).toBool());
    EXPECT_EQ(env.value(QStringLiteral("applied_count")).toInt(), 1)
        << "a preview still reports what it would do";
    EXPECT_EQ(readStr(fx.path), before) << "and writes none of it";
}

// ---------------------------------------------------------------- INV-5 -----

TEST(FeedbackAssignIdBatch, Inv5AssignmentsAreThreaded) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    const QJsonObject env = rc.cmdFeedbackLog(fx.req({
        one(kH1, QStringLiteral("ANTS-1001")),
        one(kH1, QStringLiteral("ANTS-2002")),
    })).object();

    EXPECT_TRUE(env.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(env).toJson().toStdString();
    EXPECT_EQ(env.value(QStringLiteral("applied_count")).toInt(), 2);

    const QString out = readStr(fx.path);
    EXPECT_TRUE(out.contains(QStringLiteral("- **Proposed ID:** ANTS-2002")))
        << "the second assignment wins, as two sequential calls would";
    EXPECT_FALSE(out.contains(QStringLiteral("ANTS-1001")));
}

// ---------------------------------------------------------------- INV-6 -----

TEST(FeedbackAssignIdBatch, Inv6EmptyAssignmentsRefused) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    const QJsonObject env = rc.cmdFeedbackLog(fx.req({})).object();
    EXPECT_FALSE(env.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(env.value(QStringLiteral("code")).toString(),
              QStringLiteral("bad_args"))
        << QJsonDocument(env).toJson().toStdString();
}
