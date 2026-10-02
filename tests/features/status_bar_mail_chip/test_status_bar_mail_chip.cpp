// Feature-conformance test for ANTS-5620 — the status-bar unread-mail chip.
// Contract: tests/features/status_bar_mail_chip/spec.md
//
// INV-1 and INV-2 drive RoadmapStore against a store inside a QTemporaryDir.
// NEVER default-construct RoadmapStore here: its default path is the
// developer's REAL machine-global store. INV-3 to INV-5 scrape the wiring,
// as the tokens-saved pill's tests do, since the controller needs the whole
// Claude integration to construct.

#include <gtest/gtest.h>

#include "roadmapstore.h"
#include "../../_support/srcgrep.h"

#include <QDir>
#include <QString>
#include <QTemporaryDir>

#include <memory>
#include <string>

#ifndef SRC_CLAUDESTATUSWIDGETS_CPP_PATH
#error "SRC_CLAUDESTATUSWIDGETS_CPP_PATH required"
#endif
#ifndef ANTS_MAINWINDOW_SOURCES
#error "ANTS_MAINWINDOW_SOURCES required"
#endif

namespace {

struct Fixture {
    QTemporaryDir dir;
    std::unique_ptr<RoadmapStore> store;

    bool init() {
        if (!dir.isValid())
            return false;
        store = std::make_unique<RoadmapStore>(dir.filePath(QStringLiteral("mail.sqlite")));
        QString err;
        return store->open(&err);
    }

    qint64 addProject(const QString &slug) {
        const QString root = dir.filePath(slug);
        if (!QDir().mkpath(root))
            return 0;
        QString err;
        const auto pk = store->registerProject(root, slug, slug, &err);
        return pk ? *pk : 0;
    }
};

bool has(const std::string &h, const std::string &n) {
    return h.find(n) != std::string::npos;
}

size_t count(const std::string &h, const std::string &n) {
    size_t c = 0, p = 0;
    while ((p = h.find(n, p)) != std::string::npos) { ++c; p += n.size(); }
    return c;
}

}  // namespace

// INV-1 — the root and anything under it resolve to the project; a sibling
// and the filesystem root do not.
TEST(StatusBarMailChip, Inv1ProjectIdContaining) {
    Fixture f;
    ASSERT_TRUE(f.init());
    const qint64 a = f.addProject(QStringLiteral("alpha"));
    ASSERT_GT(a, 0);
    const QString sub = f.dir.filePath(QStringLiteral("alpha/src/deep"));
    ASSERT_TRUE(QDir().mkpath(sub));
    const QString sibling = f.dir.filePath(QStringLiteral("beta"));
    ASSERT_TRUE(QDir().mkpath(sibling));

    const auto atRoot = f.store->projectIdContaining(f.dir.filePath(QStringLiteral("alpha")));
    ASSERT_TRUE(atRoot.has_value());
    EXPECT_EQ(*atRoot, a);
    const auto below = f.store->projectIdContaining(sub);
    ASSERT_TRUE(below.has_value()) << "a subdirectory belongs to its project";
    EXPECT_EQ(*below, a);
    EXPECT_FALSE(f.store->projectIdContaining(sibling).has_value());
    EXPECT_FALSE(f.store->projectIdContaining(QStringLiteral("/")).has_value());
}

// INV-2 — acked mail and mail the project sent are not counted.
TEST(StatusBarMailChip, Inv2CountsOnlyUnreadInbox) {
    Fixture f;
    ASSERT_TRUE(f.init());
    const qint64 a = f.addProject(QStringLiteral("alpha"));
    const qint64 b = f.addProject(QStringLiteral("bravo"));
    ASSERT_GT(a, 0);
    ASSERT_GT(b, 0);

    const QString t = QStringLiteral("2026-10-02T00:00:00Z");
    QString code, err;
    qint64 first = 0, second = 0, out = 0;
    ASSERT_TRUE(f.store->sendMessage(b, QStringLiteral("alpha"), QStringLiteral("one"),
                                     QString(), t, &first, &code, &err)) << err.toStdString();
    ASSERT_TRUE(f.store->sendMessage(b, QStringLiteral("alpha"), QStringLiteral("two"),
                                     QString(), t, &second, &code, &err)) << err.toStdString();
    ASSERT_TRUE(f.store->sendMessage(a, QStringLiteral("bravo"), QStringLiteral("sent"),
                                     QString(), t, &out, &code, &err)) << err.toStdString();
    bool already = false;
    ASSERT_TRUE(f.store->ackMessage(a, first, t, &already, &code, &err)) << err.toStdString();

    const auto pid = f.store->projectIdContaining(f.dir.filePath(QStringLiteral("alpha")));
    ASSERT_TRUE(pid.has_value());
    int unread = -1;
    QStringList senders;
    ASSERT_TRUE(f.store->mailSummaryFor(*pid, &unread, &senders, &err)) << err.toStdString();
    EXPECT_EQ(unread, 1);
    EXPECT_EQ(senders, QStringList{QStringLiteral("bravo")});
}

// INV-3 to INV-5 — the chip's face, refresh points and cost guard.
TEST(StatusBarMailChip, Inv3To5Wiring) {
    const std::string cpp = ants_test::slurpFile(SRC_CLAUDESTATUSWIDGETS_CPP_PATH);
    const std::string body =
        ants_test::slurpFunctionBody(cpp, "ClaudeStatusBarController::refreshMailChip");
    ASSERT_FALSE(body.empty()) << "refreshMailChip body not found";
    EXPECT_TRUE(has(cpp, "\"claudeMailChip\"")) << "INV-3: objectName";
    EXPECT_TRUE(has(body, "unread == 0")) << "INV-3: hidden at zero";
    EXPECT_TRUE(has(body, "unread")) << "INV-3: face names the count";
    EXPECT_TRUE(has(body, "setAccessibleName")) << "INV-3: accessible name";
    EXPECT_TRUE(has(body, "projectIdContaining")) << "INV-1 is how the project is found";
    EXPECT_TRUE(has(body, "mailSummaryFor")) << "INV-2 is the count";
    EXPECT_TRUE(has(body, "m_mailSig")) << "INV-5: re-query only on a changed signature";
    EXPECT_TRUE(has(body, "-wal")) << "INV-5: the WAL file is in the signature";
    EXPECT_TRUE(has(body, "RoadmapSource::storeFor")) << "INV-5: never creates a store";

    const std::string mw = ants_test::slurpMainWindow();
    EXPECT_TRUE(has(mw, "&ClaudeStatusBarController::refreshMailChip"))
        << "INV-4: connected to the status timer";
    const std::string tab =
        ants_test::slurpFunctionBody(mw, "void MainWindow::refreshStatusBarForActiveTab");
    ASSERT_FALSE(tab.empty()) << "refreshStatusBarForActiveTab body not found";
    EXPECT_GE(count(tab, "refreshMailChip()"), 2u)
        << "INV-4: both arms of the tab-switch refresh update the chip";
}
