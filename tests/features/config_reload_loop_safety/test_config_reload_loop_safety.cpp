// Feature-conformance test for spec.md — locks down the 0.7.50 fix for
// the inotify-driven config-reload loop.
//
// Two layers of assertion:
//   1. Source-grep on src/config.cpp + src/mainwindow.cpp + src/mainwindow.h
//      to catch the call shape (idempotent setter + re-entrancy flag +
//      no-op skip in onConfigFileChanged + absence of the failed
//      blockSignals attempt).
//   2. Functional: Config::setTheme is genuinely idempotent — a second
//      call with the same value does not re-write the file (verified via
//      stable mtime in a sandboxed XDG_CONFIG_HOME).
//
// Exit 0 = all assertions hold. Non-zero = regression.

#include "config.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

int g_failures = 0;

void expect(bool ok, const char *label, const char *detail = "") {
    std::fprintf(stderr, "[%-72s] %s%s%s\n",
                 label, ok ? "PASS" : "FAIL",
                 (detail && *detail) ? " — " : "",
                 detail ? detail : "");
    if (!ok) ++g_failures;
}

std::string readFile(const char *path) {
    std::ifstream f(path);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool contains(const std::string &haystack, const std::string &needle) {
    return haystack.find(needle) != std::string::npos;
}

// Extract the body of a function whose signature line contains `signature`.
// Returns the slice from the signature line to the matching closing brace.
// Brace-counting is naive (string literals containing braces would confuse
// it) — fine for our use, where we only feed it real C++ functions.
std::string extractBody(const std::string &source, const std::string &signature) {
    auto sigPos = source.find(signature);
    if (sigPos == std::string::npos) return {};
    auto open = source.find('{', sigPos);
    if (open == std::string::npos) return {};
    int depth = 1;
    auto pos = open + 1;
    while (pos < source.size() && depth > 0) {
        if (source[pos] == '{') ++depth;
        else if (source[pos] == '}') --depth;
        ++pos;
    }
    return source.substr(sigPos, pos - sigPos);
}

void testInv1_setThemeIdempotent_callShape() {
    const std::string config = readFile("src/config.cpp");
    expect(!config.empty(), "INV-1: src/config.cpp readable");

    const std::string body = extractBody(config, "void Config::setTheme(");
    expect(!body.empty(), "INV-1: Config::setTheme body found");

    // Guard must precede save(). Two shapes are valid:
    //   (a) inline: `if (m_data.value("theme").toString() == name) return;`
    //   (b) helper: `if (!storeIfChanged("theme", ...)) return;`
    // Either is correct as long as the early-return precedes save().
    const auto returnPos = body.find("return");
    const auto savePos   = body.find("save()");
    expect(returnPos != std::string::npos,
           "INV-1: Config::setTheme contains an early return");
    expect(savePos != std::string::npos,
           "INV-1: Config::setTheme retains a save() call");
    expect(returnPos != std::string::npos && savePos != std::string::npos
               && returnPos < savePos,
           "INV-1: early return precedes save()");

    const bool inlineGuard =
        contains(body, "m_data.value(\"theme\").toString() == name");
    const bool helperGuard =
        contains(body, "storeIfChanged(\"theme\"");
    expect(inlineGuard || helperGuard,
           "INV-1: setTheme uses an idempotent guard "
           "(inline value-compare OR storeIfChanged helper)");
}

// Drive a setter twice with the same value in a fresh sandbox. Assert the
// second call does NOT rewrite the file (mtime unchanged), proving the
// setter is idempotent. The bounce sleep is past most filesystems' mtime
// granularity (msec on ext4/btrfs).
//
// Each call constructs its own Config inline AFTER setting XDG_CONFIG_HOME,
// not via a struct member — Config's constructor reads from disk via
// QStandardPaths, which honors XDG_CONFIG_HOME at call time, so the
// envvar must be set first or the Config loads the user's REAL config.
template <typename Setter>
void expectIdempotent(const char *name, Setter &&apply) {
    QTemporaryDir tmp;
    if (!tmp.isValid()) { expect(false, name, "QTemporaryDir setup"); return; }
    qputenv("XDG_CONFIG_HOME", tmp.path().toLocal8Bit());
    const QString antsDir = tmp.path() + "/ants-terminal";
    QDir().mkpath(antsDir);
    const QString cfgPath = antsDir + "/config.json";

    Config cfg;
    apply(cfg);
    if (!QFile::exists(cfgPath)) {
        // First call may not write if the value already matches the
        // default-constructed Config's view. That's still idempotent —
        // the second call also won't write — so call it a pass.
        std::fprintf(stderr, "[%-72s] PASS — first call no-op (default match)\n", name);
        return;
    }
    const qint64 t1 = QFileInfo(cfgPath).lastModified().toMSecsSinceEpoch();
    QThread::msleep(50);
    apply(cfg);
    const qint64 t2 = QFileInfo(cfgPath).lastModified().toMSecsSinceEpoch();
    expect(t1 == t2, name, "second call rewrote the file");
}

void testInv1_setThemeIdempotent_functional() {
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        expect(false, "INV-1 (functional): QTemporaryDir construction");
        return;
    }
    qputenv("XDG_CONFIG_HOME", tmp.path().toLocal8Bit());
    const QString antsDir = tmp.path() + "/ants-terminal";
    QDir().mkpath(antsDir);
    const QString cfgPath = antsDir + "/config.json";

    Config cfg;
    cfg.setTheme(QStringLiteral("Dark"));
    expect(QFile::exists(cfgPath),
           "INV-1 (functional): first setTheme writes config.json");

    const qint64 mtimeAfterFirst =
        QFileInfo(cfgPath).lastModified().toMSecsSinceEpoch();
    QThread::msleep(50);

    cfg.setTheme(QStringLiteral("Dark"));
    expect(QFileInfo(cfgPath).lastModified().toMSecsSinceEpoch() == mtimeAfterFirst,
           "INV-1 (functional): second setTheme(\"Dark\") does not rewrite "
           "the file (mtime unchanged)");

    // Sanity: a setTheme to a different value DOES rewrite.
    QThread::msleep(50);
    cfg.setTheme(QStringLiteral("Solarized"));
    expect(QFileInfo(cfgPath).lastModified().toMSecsSinceEpoch() > mtimeAfterFirst,
           "INV-1 (functional): setTheme to a NEW value rewrites the file");
}

// INV-1b: every setter shape is idempotent — sample one of each
// representative shape (bool, int with qBound, double, QString,
// QStringList, QJsonArray, QJsonObject, compound sub-object). If a
// future setter regresses to unconditional save(), this catches the
// shape if it matches one of these. The 0.7.51 inotify-loop bug came
// from setTheme being non-idempotent; INV-3's runtime guard catches
// re-entry but only after the slot already woke up.
void testInv1b_setterShapesIdempotent() {
    expectIdempotent("INV-1b: setSessionLogging idempotent (bool)",
                     [](Config &c) { c.setSessionLogging(true); });

    expectIdempotent("INV-1b: setFontSize idempotent (int + qBound)",
                     [](Config &c) { c.setFontSize(14); });

    expectIdempotent("INV-1b: setOpacity idempotent (double + qBound)",
                     [](Config &c) { c.setOpacity(0.85); });

    expectIdempotent("INV-1b: setEditorCommand idempotent (QString)",
                     [](Config &c) {
                         c.setEditorCommand(QStringLiteral("/usr/bin/nvim"));
                     });

    expectIdempotent("INV-1b: setEnabledPlugins idempotent (QStringList)",
                     [](Config &c) {
                         c.setEnabledPlugins({QStringLiteral("foo"),
                                              QStringLiteral("bar")});
                     });

    {
        QJsonArray rules;
        QJsonObject r;
        r["pattern"] = QStringLiteral("ERROR.*");
        r["color"] = QStringLiteral("#ff0000");
        rules.append(r);
        expectIdempotent("INV-1b: setHighlightRules idempotent (QJsonArray)",
                         [rules](Config &c) { c.setHighlightRules(rules); });
    }
    {
        QJsonObject groups;
        groups["work"] = QJsonArray{QStringLiteral("default"),
                                    QStringLiteral("ssh")};
        expectIdempotent("INV-1b: setTabGroups idempotent (QJsonObject)",
                         [groups](Config &c) { c.setTabGroups(groups); });
    }

    expectIdempotent("INV-1b: setKeybinding idempotent (compound sub-object)",
                     [](Config &c) {
                         c.setKeybinding(QStringLiteral("new_tab"),
                                         QStringLiteral("Ctrl+T"));
                     });

    expectIdempotent("INV-1b: setPluginSetting idempotent (compound nested)",
                     [](Config &c) {
                         c.setPluginSetting(QStringLiteral("git_status"),
                                            QStringLiteral("interval_ms"),
                                            QStringLiteral("3000"));
                     });

    expectIdempotent("INV-1b: setWindowGeometry idempotent (4-tuple)",
                     [](Config &c) {
                         c.setWindowGeometry(100, 100, 1024, 768);
                     });
}

// INV-1c: storeIfChanged returns false for matching value, true for
// differing value, and leaves m_data correctly updated only in the
// differing-value path. Tested via observable side-effect — the file
// rewrite — since storeIfChanged is private.
void testInv1c_storeIfChangedSemantics() {
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        expect(false, "INV-1c: QTemporaryDir construction"); return;
    }
    qputenv("XDG_CONFIG_HOME", tmp.path().toLocal8Bit());
    const QString antsDir = tmp.path() + "/ants-terminal";
    QDir().mkpath(antsDir);
    const QString cfgPath = antsDir + "/config.json";
    Config cfg;

    // First write of a non-default — must write.
    cfg.setFontSize(14);
    expect(QFile::exists(cfgPath),
           "INV-1c: setFontSize(14) on default config writes the file");
    const qint64 t1 = QFileInfo(cfgPath).lastModified().toMSecsSinceEpoch();
    QThread::msleep(50);

    // Same value again — must not write.
    cfg.setFontSize(14);
    expect(QFileInfo(cfgPath).lastModified().toMSecsSinceEpoch() == t1,
           "INV-1c: setFontSize(14) when value already 14 does not write");
    QThread::msleep(50);

    // Different value — must write.
    cfg.setFontSize(16);
    expect(QFileInfo(cfgPath).lastModified().toMSecsSinceEpoch() > t1,
           "INV-1c: setFontSize(16) when value was 14 writes the file");
}

void testInv2_onConfigFileChanged_skipsNoOpApplyTheme() {
    const std::string source = readFile("src/mainwindow.cpp");
    expect(!source.empty(), "INV-2: src/mainwindow.cpp readable");

    const std::string body = extractBody(
        source, "void MainWindow::onConfigFileChanged(");
    expect(!body.empty(), "INV-2: onConfigFileChanged body found");

    expect(contains(body, "m_currentTheme"),
           "INV-2: onConfigFileChanged compares against m_currentTheme");
    expect(contains(body, "applyTheme("),
           "INV-2: onConfigFileChanged still calls applyTheme on real changes");
    // The skip must be a *conditional* call site — not an unconditional one.
    // Detect by requiring a preceding `if (` token within ~120 chars before
    // applyTheme(.
    const auto applyPos = body.find("applyTheme(");
    if (applyPos != std::string::npos) {
        const auto windowStart =
            applyPos > 200 ? applyPos - 200 : static_cast<size_t>(0);
        const std::string window = body.substr(windowStart, applyPos - windowStart);
        expect(window.find("if (") != std::string::npos
                   || window.find("if(") != std::string::npos,
               "INV-2: applyTheme call is gated by an `if` (no-op skip path)");
    }
}

void testInv3_reentrancyGuard() {
    const std::string header = readFile("src/mainwindow.h");
    expect(!header.empty(), "INV-3: src/mainwindow.h readable");
    expect(contains(header, "bool m_inConfigReload"),
           "INV-3: m_inConfigReload member declared in mainwindow.h");

    const std::string source = readFile("src/mainwindow.cpp");
    const std::string body = extractBody(
        source, "void MainWindow::onConfigFileChanged(");
    expect(!body.empty(), "INV-3: onConfigFileChanged body found");

    // Early return when the flag is set.
    expect(contains(body, "if (m_inConfigReload) return;"),
           "INV-3: onConfigFileChanged returns early when m_inConfigReload");
    // Flag is set on entry.
    expect(contains(body, "m_inConfigReload = true;"),
           "INV-3: onConfigFileChanged sets m_inConfigReload = true on entry");
    // Flag is cleared on the next event-loop tick.
    expect(contains(body, "QTimer::singleShot(0, this,")
               && contains(body, "m_inConfigReload = false;"),
           "INV-3: m_inConfigReload cleared via QTimer::singleShot(0)");
}

void testInv4_failedBlockSignalsRemoved() {
    const std::string source = readFile("src/mainwindow.cpp");
    const std::string body = extractBody(
        source, "void MainWindow::onConfigFileChanged(");
    expect(!body.empty(), "INV-4: onConfigFileChanged body found");

    expect(!contains(body, "m_configWatcher->blockSignals(true)"),
           "INV-4: failed 0.7.31 blockSignals(true) attempt is gone");
    expect(!contains(body, "m_configWatcher->blockSignals(false)"),
           "INV-4: failed 0.7.31 blockSignals(false) attempt is gone");
}

}  // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    testInv1_setThemeIdempotent_callShape();
    testInv1_setThemeIdempotent_functional();
    testInv1b_setterShapesIdempotent();
    testInv1c_storeIfChangedSemantics();
    testInv2_onConfigFileChanged_skipsNoOpApplyTheme();
    testInv3_reentrancyGuard();
    testInv4_failedBlockSignalsRemoved();
    return g_failures == 0 ? 0 : 1;
}
