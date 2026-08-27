// ANTS-3519 — feedback_query resolves foreign-prefix mapped ids (ANTS-*)
// against the sibling project roadmap that owns the prefix, instead of only
// flagging them "foreign_repo" (ANTS-3518). Topology mirrors the real shared
// root: the feedback file sits at the shared root, the caller project and the
// owning project are sibling subdirs, each with its own ROADMAP.md.
//
// Drives cmdFeedbackQuery behaviourally (m_main-independent).

#include "remotecontrol.h"
#include "build_info.h"    // ANTS-4741

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>

namespace {

const char *kClip  = "\xF0\x9F\x93\x8B";  // 📋
const char *kCheck = "\xE2\x9C\x85";      // ✅

bool writeFile(const QString &path, const QByteArray &body) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool ok = (f.write(body) == body.size());
    f.close();
    return ok;
}

// Consumer feedback file (v2) citing two ANTS-* mapped ids.
QByteArray consumerFeedback() {
    return "<!-- ants-mcp-feedback: 2 -->\n"
           "# Ants MCP Feedback — Consumer\n\n"
           "## 2026-07-14 — s\n\n"
           "### Finding A\n\n- **What:** a.\n- **Proposed ID:** ANTS-3517\n\n"
           "### Finding B\n\n- **What:** b.\n- **Proposed ID:** ANTS-3599\n";
}

QHash<QString, QJsonObject> statusMap(const QJsonObject &env) {
    QHash<QString, QJsonObject> got;
    for (const auto &v : env.value("mapped_id_status").toArray()) {
        const QJsonObject o = v.toObject();
        got.insert(o.value("id").toString(), o);
    }
    return got;
}

}  // namespace

// INV-1 / INV-2 — foreign ANTS-* ids resolve against the sibling owner roadmap
// (real status + resolved_from), and a ✅ id carries its cross-repo ship date.
TEST(feedback_query_foreign_resolve, Inv1ResolvesFromSiblingRoadmap) {
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString sharedRoot = root.path();

    // Feedback file at the shared root.
    const QString fb = sharedRoot + "/Consumer_Ants_MCP_Feedback.md";
    ASSERT_TRUE(writeFile(fb, consumerFeedback()));

    // Caller project (a subdir) with its OWN prefix — ANTS ids are foreign here.
    ASSERT_TRUE(writeFile(sharedRoot + "/Consumer/ROADMAP.md",
        QString::fromUtf8(
            "# Consumer ROADMAP\n\n"
            "- \xF0\x9F\x93\x8B [CONS-0100] **A local planned item.**\n")
            .toUtf8()));

    // Owning sibling project (another subdir) whose roadmap owns ANTS-*.
    ASSERT_TRUE(writeFile(sharedRoot + "/OwnerProj/ROADMAP.md",
        QString::fromUtf8(
            "# Owner ROADMAP\n\n"
            "- \xE2\x9C\x85 [ANTS-3517] **A shipped cross-repo item.**\n"
            "  Resolved (2026-07-12): done.\n"
            "- \xF0\x9F\x93\x8B [ANTS-3599] **A planned cross-repo item.**\n")
            .toUtf8()));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req["path"]       = fb;
    req["caller_cwd"] = sharedRoot + "/Consumer";
    const QJsonObject env = rc.cmdFeedbackQuery(req).object();
    ASSERT_TRUE(env.value("ok").toBool());
    ASSERT_TRUE(env.contains("mapped_id_status"));

    const auto got = statusMap(env);
    // INV-1 — resolved to the sibling's live status, not foreign_repo.
    EXPECT_EQ(got.value("ANTS-3517").value("status").toString(),
              QString::fromUtf8(kCheck));
    EXPECT_EQ(got.value("ANTS-3599").value("status").toString(),
              QString::fromUtf8(kClip));
    EXPECT_EQ(got.value("ANTS-3517").value("resolved_from").toString(),
              QStringLiteral("OwnerProj"));
    // INV-2 — cross-repo ship date carried on the ✅ id.
    EXPECT_EQ(got.value("ANTS-3517").value("shipped_date").toString(),
              QStringLiteral("2026-07-12"));
    // All foreign ids resolved → no unresolved-foreign note.
    EXPECT_FALSE(env.contains("mapped_id_status_note"));
}

// INV-3 (regression) — no sibling owns the prefix → the ids stay foreign_repo
// and the note fires (ANTS-3518 behaviour preserved).
TEST(feedback_query_foreign_resolve, Inv3NoOwnerStaysForeign) {
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString sharedRoot = root.path();

    const QString fb = sharedRoot + "/Consumer_Ants_MCP_Feedback.md";
    ASSERT_TRUE(writeFile(fb, consumerFeedback()));
    // Caller has its own prefix; NO sibling owns ANTS.
    ASSERT_TRUE(writeFile(sharedRoot + "/Consumer/ROADMAP.md",
        QString::fromUtf8(
            "# Consumer ROADMAP\n\n"
            "- \xF0\x9F\x93\x8B [CONS-0100] **A local planned item.**\n")
            .toUtf8()));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req["path"]       = fb;
    req["caller_cwd"] = sharedRoot + "/Consumer";
    const QJsonObject env = rc.cmdFeedbackQuery(req).object();
    ASSERT_TRUE(env.value("ok").toBool());

    const auto got = statusMap(env);
    EXPECT_EQ(got.value("ANTS-3517").value("status").toString(),
              QStringLiteral("foreign_repo"));
    EXPECT_EQ(got.value("ANTS-3599").value("status").toString(),
              QStringLiteral("foreign_repo"));
    EXPECT_TRUE(env.contains("mapped_id_status_note"));
}

// INV-4 (regression) — mapped ids sharing the caller roadmap's own prefix do
// NOT trigger a sibling scan; caller-roadmap resolution (ANTS-3478) is intact.
TEST(feedback_query_foreign_resolve, Inv4SamePrefixUnaffected) {
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString sharedRoot = root.path();

    // Feedback file cites ANTS ids; caller roadmap IS an ANTS roadmap.
    const QString fb = sharedRoot + "/Self_Ants_MCP_Feedback.md";
    ASSERT_TRUE(writeFile(fb, consumerFeedback()));
    ASSERT_TRUE(writeFile(sharedRoot + "/Self/ROADMAP.md",
        QString::fromUtf8(
            "# Self ROADMAP\n\n"
            "- \xE2\x9C\x85 [ANTS-3517] **Shipped locally.**\n")
            .toUtf8()));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req["path"]       = fb;
    req["caller_cwd"] = sharedRoot + "/Self";
    const QJsonObject env = rc.cmdFeedbackQuery(req).object();
    ASSERT_TRUE(env.value("ok").toBool());

    const auto got = statusMap(env);
    // Resolved from the caller roadmap directly (no resolved_from stamp).
    EXPECT_EQ(got.value("ANTS-3517").value("status").toString(),
              QString::fromUtf8(kCheck));
    EXPECT_FALSE(got.value("ANTS-3517").contains("resolved_from"));
    // Same-prefix id absent from the caller roadmap stays "unknown", not foreign.
    EXPECT_EQ(got.value("ANTS-3599").value("status").toString(),
              QStringLiteral("unknown"));
}

// ANTS-4741 — do the stale-binary comparison rather than describe it.
//
// This verb already carried `shipped_date`, and its own description told the
// caller to fetch session_orient's `server_build.build_date` and compare. Both
// operands are server-side at reply time, so the second call bought nothing —
// and the guidance lived on a different verb from the one surfacing the date.
//
// Dates here are relative to the REAL build date rather than hardcoded, so the
// test cannot go stale as the binary is rebuilt.
TEST(feedback_query_foreign_resolve, Ants4741FlagsAShipDateNotOlderThanTheBuild) {
    const QString buildDate = QString::fromLatin1(ANTS_BUILD_DATE);
    ASSERT_FALSE(buildDate.isEmpty()) << "the comparison needs a build date";

    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString sharedRoot = root.path();

    const QString fb = sharedRoot + "/Consumer_Ants_MCP_Feedback.md";
    ASSERT_TRUE(writeFile(fb, consumerFeedback()));
    ASSERT_TRUE(writeFile(sharedRoot + "/Consumer/ROADMAP.md",
        QString::fromUtf8("# Consumer ROADMAP\n\n"
            "- \xF0\x9F\x93\x8B [CONS-0100] **A local planned item.**\n").toUtf8()));

    // 3517 shipped long before this binary was built; 3599 shipped ON the day
    // it was built — the ambiguous case, and the one most likely to be misread,
    // because the dates match and only the time separates them.
    ASSERT_TRUE(writeFile(sharedRoot + "/OwnerProj/ROADMAP.md",
        (QString::fromUtf8(
            "# Owner ROADMAP\n\n"
            "- \xE2\x9C\x85 [ANTS-3517] **Shipped well before this build.**\n"
            "  Resolved (2020-01-01): done.\n"
            "- \xE2\x9C\x85 [ANTS-3599] **Shipped the day this build was made.**\n"
            "  Resolved (") + buildDate +
         QStringLiteral("): done.\n")).toUtf8()));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req["path"]       = fb;
    req["caller_cwd"] = sharedRoot + "/Consumer";
    const QJsonObject env = rc.cmdFeedbackQuery(req).object();
    ASSERT_TRUE(env.value("ok").toBool());

    const auto got = statusMap(env);
    ASSERT_EQ(got.value("ANTS-3517").value("shipped_date").toString(),
              QStringLiteral("2020-01-01"));
    EXPECT_FALSE(got.value("ANTS-3517").contains("possibly_stale_binary"))
        << "a fix that shipped before the build is IN this binary — flagging it "
           "would make the flag mean nothing";
    EXPECT_TRUE(got.value("ANTS-3599").value("possibly_stale_binary").toBool())
        << "same-day sets it: shipped_date has no time component, so it cannot "
           "be told from a fix that landed after the build was made";

    // Both operands must sit in the one reply — putting them in two verbs is
    // the defect, not the presentation.
    EXPECT_EQ(env.value("server_build_date").toString(), buildDate);
    EXPECT_TRUE(env.contains("server_build_commit"));
    EXPECT_TRUE(env.value("possibly_stale_binary_hint").toString()
                    .contains(QStringLiteral("relaunched")))
        << "the hint must name the action, not just the condition";
}

// It fires only when something IS suspect. A constant flag is noise, and the
// envelope keys must stay absent otherwise — without this arm every assertion
// above is satisfied by an implementation that flags every shipped id.
TEST(feedback_query_foreign_resolve, Ants4741QuietWhenEverythingPredatesTheBuild) {
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString sharedRoot = root.path();

    ASSERT_TRUE(writeFile(sharedRoot + "/Consumer_Ants_MCP_Feedback.md",
                          consumerFeedback()));
    ASSERT_TRUE(writeFile(sharedRoot + "/Consumer/ROADMAP.md",
        QString::fromUtf8("# Consumer ROADMAP\n\n"
            "- \xF0\x9F\x93\x8B [CONS-0100] **A local planned item.**\n").toUtf8()));
    ASSERT_TRUE(writeFile(sharedRoot + "/OwnerProj/ROADMAP.md",
        QString::fromUtf8(
            "# Owner ROADMAP\n\n"
            "- \xE2\x9C\x85 [ANTS-3517] **Shipped well before this build.**\n"
            "  Resolved (2020-01-01): done.\n"
            "- \xF0\x9F\x93\x8B [ANTS-3599] **Still planned.**\n").toUtf8()));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req["path"]       = sharedRoot + "/Consumer_Ants_MCP_Feedback.md";
    req["caller_cwd"] = sharedRoot + "/Consumer";
    const QJsonObject env = rc.cmdFeedbackQuery(req).object();
    ASSERT_TRUE(env.value("ok").toBool());

    const auto got = statusMap(env);
    EXPECT_FALSE(got.value("ANTS-3517").contains("possibly_stale_binary"));
    EXPECT_FALSE(got.value("ANTS-3599").contains("possibly_stale_binary"))
        << "a planned id has no ship date and cannot be stale";
    EXPECT_FALSE(env.contains("server_build_date"));
    EXPECT_FALSE(env.contains("possibly_stale_binary_hint"));
}
