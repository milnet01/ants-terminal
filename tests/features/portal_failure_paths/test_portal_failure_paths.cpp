// The global-shortcuts portal reports every failure — see spec.md.
// ANTS-5081. Source-scrape of src/globalshortcutsportal.cpp.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>

#include <gtest/gtest.h>


namespace {

QString readSrc(const QString &name) {
    const QDir src = QDir(QStringLiteral(SRC_DIR));
    QFile f(src.filePath(name));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(f.readAll());
}

QString portalSource() {
    return readSrc(QStringLiteral("globalshortcutsportal.cpp"));
}

QString functionBody(const QString &src, const QString &signature) {
    const int start = src.indexOf(signature);
    if (start < 0) return QString();
    const int brace = src.indexOf(QChar('{'), start);
    if (brace < 0) return QString();
    int depth = 1;
    int i = brace + 1;
    while (i < src.size() && depth > 0) {
        if (src.at(i) == QChar('{')) ++depth;
        else if (src.at(i) == QChar('}')) --depth;
        ++i;
    }
    return src.mid(brace, i - brace);
}

}  // namespace

// INV-1
TEST(PortalFailurePaths, BindShortcutsReplyIsWatched) {
    const QString src = portalSource();
    ASSERT_FALSE(src.isEmpty()) << "globalshortcutsportal.cpp not readable";
    const int call = src.indexOf(QStringLiteral("QStringLiteral(\"BindShortcuts\")"));
    ASSERT_GE(call, 0) << "BindShortcuts call not found";
    const QString after = src.mid(call, 1600);
    EXPECT_TRUE(after.contains(QStringLiteral("QDBusPendingCallWatcher")))
        << "the BindShortcuts reply is discarded, so an error never fails the session";
    EXPECT_TRUE(after.contains(QStringLiteral("emit sessionFailed(")));
    // Scoped to flushPending: the destructor's Session.Close is a deliberate
    // fire-and-forget asyncCall (portal_session_close).
    const QString flush = functionBody(src,
        QStringLiteral("void GlobalShortcutsPortal::flushPending()"));
    ASSERT_FALSE(flush.isEmpty()) << "flushPending not found";
    EXPECT_FALSE(flush.contains(QStringLiteral("    m_bus.asyncCall(msg);\n")))
        << "BindShortcuts is still a bare asyncCall whose reply nobody reads";
}

// INV-2
TEST(PortalFailurePaths, OnDemandPortalIsAvailable) {
    const QString body = functionBody(portalSource(),
        QStringLiteral("bool GlobalShortcutsPortal::isAvailable()"));
    ASSERT_FALSE(body.isEmpty()) << "isAvailable not found";
    EXPECT_TRUE(body.contains(QStringLiteral("activatableServiceNames()")))
        << "an on-demand portal that has not started yet is reported unavailable";
}

// INV-3
TEST(PortalFailurePaths, BindsAreSentBeforeSessionReady) {
    const QString body = functionBody(portalSource(),
        QStringLiteral("void GlobalShortcutsPortal::onCreateSessionResponse("));
    ASSERT_FALSE(body.isEmpty()) << "onCreateSessionResponse not found";
    const int flush = body.indexOf(QStringLiteral("flushPending();"));
    const int ready = body.indexOf(QStringLiteral("emit sessionReady();"));
    ASSERT_GE(flush, 0);
    ASSERT_GE(ready, 0);
    EXPECT_LT(flush, ready) << "sessionReady fires before BindShortcuts is sent";
}

// INV-4
TEST(PortalFailurePaths, CreateSessionTimesOut) {
    const QString src = portalSource();
    const QString create = functionBody(src,
        QStringLiteral("void GlobalShortcutsPortal::createSession()"));
    ASSERT_FALSE(create.isEmpty()) << "createSession not found";
    EXPECT_TRUE(create.contains(QStringLiteral("m_createSessionTimer.start()")))
        << "CreateSession has no timeout, so a missing Response queues every bind";

    const QString ctor = functionBody(src,
        QStringLiteral("GlobalShortcutsPortal::GlobalShortcutsPortal(QObject *parent)"));
    ASSERT_FALSE(ctor.isEmpty()) << "constructor not found";
    const int timeout = ctor.indexOf(QStringLiteral("&QTimer::timeout"));
    ASSERT_GE(timeout, 0) << "nothing handles the CreateSession timeout";
    const QString handler = ctor.mid(timeout);
    EXPECT_TRUE(handler.contains(QStringLiteral("m_permanentlyFailed = true;")));
    EXPECT_TRUE(handler.contains(QStringLiteral("emit sessionFailed(")));

    const QString response = functionBody(src,
        QStringLiteral("void GlobalShortcutsPortal::onCreateSessionResponse("));
    EXPECT_TRUE(response.contains(QStringLiteral("m_createSessionTimer.stop();")))
        << "a Response that arrives does not cancel the timeout";
}

// INV-5
TEST(PortalFailurePaths, EachBindResponseDetachesItsOwnPath) {
    const QString response = functionBody(portalSource(),
        QStringLiteral("void GlobalShortcutsPortal::onBindShortcutsResponse("));
    ASSERT_FALSE(response.isEmpty()) << "onBindShortcutsResponse not found";
    EXPECT_TRUE(response.contains(QStringLiteral("detachResponseSlots(message.path());")))
        << "the Response detaches a remembered path, not its own";
    EXPECT_FALSE(readSrc(QStringLiteral("globalshortcutsportal.h"))
                     .contains(QStringLiteral("m_bindShortcutsReqPath")))
        << "one member still holds the BindShortcuts request path, so a second flush overwrites it";
}
