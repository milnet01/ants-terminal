// Plugins hear output, lines, prompts and keys — see spec.md (ANTS-4272).
// Drives a real PluginManager, TerminalGrid, TerminalWidget and pty, plus a
// source-scrape of the emit sites in terminalwidget.cpp.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include "luaengine.h"
#ifdef ANTS_LUA_PLUGINS
#include "pluginmanager.h"
#endif
#include "pluginoutputcoalescer.h"
#include "ptyhandler.h"
#include "terminalgrid.h"
#include "terminalwidget.h"
#include "vtparser.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMessageAuthenticationCode>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <pty.h>
#include <termios.h>
#include <unistd.h>

#include <string>
#include <utility>
#include <vector>

#ifndef SRC_TERMINALWIDGET_PATH
#  error "SRC_TERMINALWIDGET_PATH compile definition required"
#endif

namespace {

const std::string &widgetSrc() {
    static const std::string src =
        ants_test::stripComments(ants_test::slurpFile(SRC_TERMINALWIDGET_PATH));
    return src;
}

std::string body(const char *signature) {
    return ants_test::slurpFunctionBody(widgetSrc(), signature);
}

// True when `first` occurs in `text`, `second` too, and `first` comes first.
bool before(const std::string &text, const char *first, const char *second) {
    const std::size_t a = text.find(first);
    const std::size_t b = text.find(second);
    return a != std::string::npos && b != std::string::npos && a < b;
}

[[maybe_unused]] void pump(int ms) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

[[maybe_unused]] void writeFile(const QString &path, const QByteArray &bytes) {
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly)) << path.toStdString();
    f.write(bytes);
}

[[maybe_unused]] void addPlugin(const QString &pluginDir, const QString &name) {
    const QString dir = pluginDir + QLatin1Char('/') + name;
    QDir().mkpath(dir);
    writeFile(dir + QStringLiteral("/manifest.json"),
              R"({"permissions":["terminal.read","input.keys"]})");
    const QByteArray n = name.toUtf8();
    QByteArray init;
    for (const char *ev : {"output", "line", "prompt", "keypress"})
        init += "ants.on('" + QByteArray(ev) + "', function(d) ants.log('"
              + n + ":" + ev + "') end)\n";
    writeFile(dir + QStringLiteral("/init.lua"), init);
}

void feed(TerminalGrid &grid, const QByteArray &bytes) {
    VtParser parser([&grid](const VtAction &a) { grid.processAction(a); });
    parser.feed(bytes.constData(), static_cast<int>(bytes.size()));
}

QByteArray osc133(char marker, const QByteArray &exitCode,
                  const QByteArray &key, bool forge = false) {
    const QByteArray aid("p1");
    QByteArray msg;
    msg.append(marker).append('|').append(aid);
    if (marker == 'D' && !exitCode.isEmpty()) msg.append('|').append(exitCode);
    QByteArray wire = "\033]133;";
    wire.append(marker);
    if (marker == 'D' && !exitCode.isEmpty()) wire.append(';').append(exitCode);
    if (!key.isEmpty()) {
        QMessageAuthenticationCode mac(QCryptographicHash::Sha256, key);
        mac.addData(msg);
        wire += ";aid=" + aid + ";ahmac="
              + (forge ? QByteArray("ffffffff") : mac.result().toHex());
    }
    wire.append('\007');
    return wire;
}

}  // namespace

#ifdef ANTS_LUA_PLUGINS
// INV-1, INV-2
TEST(PluginIoEvents, PermissionDecidesDelivery) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString pdir = tmp.path() + QStringLiteral("/plugins");
    addPlugin(pdir, QStringLiteral("granted"));
    addPlugin(pdir, QStringLiteral("denied"));

    QStringList logs;
    {
        PluginManager pm;
        pm.setGrantStore(
            [](const QString &name) {
                return name == QLatin1String("granted")
                    ? QStringList{QStringLiteral("terminal.read"),
                                  QStringLiteral("input.keys")}
                    : QStringList{};
            },
            [](const QString &, const QStringList &) {});
        QObject::connect(&pm, &PluginManager::logMessage,
                         [&logs](const QString &m) { logs << m; });
        pm.setPluginDir(pdir);
        pm.scanAndLoad({QStringLiteral("granted"), QStringLiteral("denied")});
        pump(300);

        // INV-2 — the manifest parser kept both strings and the grant held.
        LuaEngine *granted = pm.engineFor(QStringLiteral("granted"));
        LuaEngine *denied = pm.engineFor(QStringLiteral("denied"));
        ASSERT_NE(granted, nullptr);
        ASSERT_NE(denied, nullptr);
        EXPECT_TRUE(granted->hasPermission(QStringLiteral("terminal.read")));
        EXPECT_TRUE(granted->hasPermission(QStringLiteral("input.keys")));
        EXPECT_FALSE(denied->hasPermission(QStringLiteral("terminal.read")));

        EXPECT_TRUE(pm.wants(PluginEvent::Line));
        pm.fireEvent(PluginEvent::Output, QStringLiteral("o"));
        pm.fireEvent(PluginEvent::Line, QStringLiteral("l"));
        pm.fireEvent(PluginEvent::KeyPress, QStringLiteral("k"));
        pm.fireEvent(PluginEvent::Prompt, QStringLiteral("marker=A"));
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 3000 && !(logs.contains(QStringLiteral("granted:prompt"))
                                       && logs.contains(QStringLiteral("denied:prompt"))))
            pump(20);
        pump(100);
    }
    for (const char *s : {"granted:output", "granted:line", "granted:keypress",
                          "granted:prompt", "denied:prompt"})
        EXPECT_TRUE(logs.contains(QString::fromLatin1(s))) << s;
    for (const char *s : {"denied:output", "denied:line", "denied:keypress"})
        EXPECT_FALSE(logs.contains(QString::fromLatin1(s))) << s;
}

// INV-1 — with no engine holding the permission, nobody wants the event.
TEST(PluginIoEvents, WantsNeedsThePermission) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString pdir = tmp.path() + QStringLiteral("/plugins");
    addPlugin(pdir, QStringLiteral("denied"));
    PluginManager pm;
    pm.setGrantStore([](const QString &) { return QStringList{}; },
                     [](const QString &, const QStringList &) {});
    pm.setPluginDir(pdir);
    pm.scanAndLoad({QStringLiteral("denied")});
    pump(300);
    EXPECT_FALSE(pm.wants(PluginEvent::Output));
    EXPECT_FALSE(pm.wants(PluginEvent::Line));
    EXPECT_FALSE(pm.wants(PluginEvent::KeyPress));
    EXPECT_TRUE(pm.wants(PluginEvent::Prompt));
}

#endif  // ANTS_LUA_PLUGINS

// INV-3
TEST(PluginIoEvents, EmitSitesAskTheFilterFirst) {
    const std::string vt = body("void TerminalWidget::onVtBatch(");
    EXPECT_TRUE(before(vt, "pluginWants(PluginEvent::Output)", "m_pluginOutput.append("));
    const std::string vtBlock = ants_test::regionBetween(
        vt, "pluginWants(PluginEvent::Output)", "clearSelectionHint");
    EXPECT_EQ(vtBlock.find("return"), std::string::npos);

    const std::string line = body("void TerminalWidget::onGridLineCompleted(");
    EXPECT_TRUE(before(line, "pluginWants(PluginEvent::Line)", "lineText("));
    const std::string lineBlock = ants_test::regionBetween(
        line, "pluginWants(PluginEvent::Line)", "if (m_triggerRules.empty()) return;");
    EXPECT_FALSE(lineBlock.empty());
    EXPECT_EQ(lineBlock.find("return"), std::string::npos);

    const std::string key = body("void TerminalWidget::sendKeyData(");
    EXPECT_TRUE(before(key, "pluginWants(PluginEvent::KeyPress)", "keyCombination("));

    const std::string prompt = body("void TerminalWidget::onGridPromptMarker(");
    EXPECT_TRUE(before(prompt, "pluginWants(PluginEvent::Prompt)", "\"marker=\""));

    const std::string wants = body("bool TerminalWidget::pluginWants(");
    EXPECT_NE(wants.find("m_pluginEventFilter && m_pluginEventFilter(e)"),
              std::string::npos);
}

// INV-4
TEST(PluginIoEvents, CoalescerJoinsSplitCharsAndKeepsTheTail) {
    PluginOutputCoalescer c;
    EXPECT_FALSE(c.hasPending());
    c.append("ab");
    c.append("cd");
    EXPECT_EQ(c.take(), QStringLiteral("abcd"));
    EXPECT_FALSE(c.hasPending());

    c.append(QByteArray("\xC3"));
    c.append(QByteArray("\xA9"));
    EXPECT_EQ(c.take(), QString::fromUtf8("\xC3\xA9"));

    c.append(QByteArray(10, 'x'));
    c.append(QByteArray(PluginOutputCoalescer::kMaxChars, 'y'));
    EXPECT_EQ(c.take(), QString(PluginOutputCoalescer::kMaxChars, QLatin1Char('y')));
}

// INV-5
TEST(PluginIoEvents, OutputFlushIsOneSingleShotTimer) {
    const std::string &src = widgetSrc();
    EXPECT_NE(src.find("m_pluginOutputTimer->setSingleShot(true)"), std::string::npos);
    EXPECT_NE(src.find("m_pluginOutputTimer->setInterval(100)"), std::string::npos);
    const std::string vt = body("void TerminalWidget::onVtBatch(");
    EXPECT_NE(vt.find("if (!m_pluginOutputTimer->isActive()) m_pluginOutputTimer->start()"),
              std::string::npos);
    EXPECT_EQ(vt.find("emit pluginOutput"), std::string::npos);
}

// INV-6
TEST(PluginIoEvents, LineEmitsBeforeTriggersUnderItsOwnBudget) {
    const std::string line = body("void TerminalWidget::onGridLineCompleted(");
    EXPECT_TRUE(before(line, "emit pluginLine", "if (m_triggerRules.empty()) return;"));
    EXPECT_NE(line.find("takeEventBudget(m_pluginLineBudget, kPluginLinesPerSecond)"),
              std::string::npos);

    EXPECT_EQ(TerminalWidget::kPluginLinesPerSecond, 200);
    TerminalWidget::EventBudget budget;
    for (int i = 0; i < 200; ++i)
        EXPECT_TRUE(TerminalWidget::takeEventBudget(budget, 200)) << i;
    EXPECT_FALSE(TerminalWidget::takeEventBudget(budget, 200));

    PluginLineJoiner j;
    EXPECT_FALSE(j.addRow(QStringLiteral("ab"), true).has_value());
    const auto out = j.addRow(QStringLiteral("c"), false);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, QStringLiteral("abc"));
}

// INV-6 — a line wider than the terminal arrives as one event, unpadded.
TEST(PluginIoEvents, WrappedLineArrivesWhole) {
    TerminalWidget w;
    w.setPluginEventFilter([](PluginEvent) { return true; });
    QSignalSpy spy(&w, &TerminalWidget::pluginLine);
    const int cols = w.grid()->cols();
    const QByteArray longLine(cols + 7, 'a');
    VtParser parser([&w](const VtAction &a) { w.grid()->processAction(a); });
    const QByteArray bytes = longLine + "\r\nshort\r\n";
    parser.feed(bytes.constData(), static_cast<int>(bytes.size()));
    ASSERT_EQ(spy.count(), 2);
    EXPECT_EQ(spy.at(0).at(0).toString(), QString::fromLatin1(longLine));
    EXPECT_EQ(spy.at(1).at(0).toString(), QStringLiteral("short"));
}

// INV-7
TEST(PluginIoEvents, OnlyVerifiedPromptMarkersReachTheCallback) {
    std::vector<std::pair<char, int>> seen;
    TerminalGrid grid(24, 80);
    const QByteArray key("plugin-io-events-test-key");
    grid.setOsc133KeyForTest(key);
    grid.setPromptMarkerCallback([&seen](char m, int code) { seen.emplace_back(m, code); });
    feed(grid, osc133('A', "", key));
    feed(grid, osc133('B', "", key));
    feed(grid, osc133('C', "", key));
    feed(grid, osc133('D', "3", key));
    feed(grid, osc133('A', "", key, /*forge=*/true));
    ASSERT_EQ(seen.size(), 4u);
    EXPECT_EQ(seen[0].first, 'A');
    EXPECT_EQ(seen[1].first, 'B');
    EXPECT_EQ(seen[2].first, 'C');
    EXPECT_EQ(seen[3], std::make_pair('D', 3));

    // Key unset: every marker passes.
    TerminalGrid open(24, 80);
    int count = 0;
    open.setPromptMarkerCallback([&count](char, int) { ++count; });
    feed(open, osc133('A', "", QByteArray()));
    EXPECT_EQ(count, 1);
}

// INV-7 — the widget's payload.
TEST(PluginIoEvents, PromptPayload) {
    TerminalWidget w;
    w.setPluginEventFilter([](PluginEvent) { return true; });
    QSignalSpy spy(&w, &TerminalWidget::pluginPrompt);
    VtParser parser([&w](const VtAction &a) { w.grid()->processAction(a); });
    const QByteArray bytes = osc133('A', "", QByteArray()) + osc133('D', "3", QByteArray());
    parser.feed(bytes.constData(), static_cast<int>(bytes.size()));
    ASSERT_EQ(spy.count(), 2);
    EXPECT_EQ(spy.at(0).at(0).toString(), QStringLiteral("marker=A"));
    EXPECT_EQ(spy.at(1).at(0).toString(), QStringLiteral("marker=D&exit=3"));
}

// INV-8
TEST(PluginIoEvents, PasswordPromptWithholdsKeys) {
    int master = -1, slave = -1;
    ASSERT_EQ(::openpty(&master, &slave, nullptr, nullptr, nullptr), 0);
    struct termios t {};
    ASSERT_EQ(::tcgetattr(slave, &t), 0);

    t.c_lflag &= ~tcflag_t(ECHO);
    t.c_lflag |= ICANON;                       // a password prompt
    ASSERT_EQ(::tcsetattr(slave, TCSANOW, &t), 0);
    EXPECT_FALSE(Pty::keysShareable(master));

    t.c_lflag &= ~tcflag_t(ICANON);            // a shell at its prompt
    ASSERT_EQ(::tcsetattr(slave, TCSANOW, &t), 0);
    EXPECT_TRUE(Pty::keysShareable(master));

    ::close(slave);
    ::close(master);
    EXPECT_FALSE(Pty::keysShareable(-1));

    const std::string key = body("void TerminalWidget::sendKeyData(");
    EXPECT_TRUE(before(key, "m_vtStream->keysShareable()", "emit pluginKey("));
}

// INV-9
TEST(PluginIoEvents, KeysAreWatchOnly) {
    const std::string key = body("void TerminalWidget::sendKeyData(");
    EXPECT_TRUE(before(key, "ptyWrite(data)", "emit pluginKey("));
    for (const char *sig : {"void TerminalWidget::inputMethodEvent(",
                            "void TerminalWidget::performPaste("}) {
        const std::string b = body(sig);
        ASSERT_FALSE(b.empty()) << sig;
        EXPECT_EQ(b.find("pluginKey"), std::string::npos) << sig;
        EXPECT_EQ(b.find("sendKeyData("), std::string::npos) << sig;
    }
}
