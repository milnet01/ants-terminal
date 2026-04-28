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

    // Guard must precede the unconditional write+save. The exact text
    // doesn't matter, but the early-return-on-equality must be present
    // BEFORE the m_data["theme"] = name; save(); pair.
    const auto returnPos = body.find("return");
    const auto writePos  = body.find("m_data[\"theme\"] = name");
    expect(returnPos != std::string::npos,
           "INV-1: Config::setTheme contains an early return");
    expect(writePos != std::string::npos,
           "INV-1: Config::setTheme retains the value-set + save() write");
    expect(returnPos != std::string::npos && writePos != std::string::npos
               && returnPos < writePos,
           "INV-1: early return precedes the m_data[\"theme\"] = name write");
    expect(contains(body, "m_data.value(\"theme\").toString() == name"),
           "INV-1: guard compares incoming name against current theme value");
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

    // Most filesystems track mtime in millisecond granularity; sleep just
    // past that so a rewrite would produce a strictly later mtime if it
    // happened.
    QThread::msleep(50);

    cfg.setTheme(QStringLiteral("Dark"));
    const qint64 mtimeAfterSecond =
        QFileInfo(cfgPath).lastModified().toMSecsSinceEpoch();

    expect(mtimeAfterSecond == mtimeAfterFirst,
           "INV-1 (functional): second setTheme(\"Dark\") does not rewrite "
           "the file (mtime unchanged)");

    // Sanity: a setTheme to a different value DOES rewrite.
    QThread::msleep(50);
    cfg.setTheme(QStringLiteral("Solarized"));
    const qint64 mtimeAfterChange =
        QFileInfo(cfgPath).lastModified().toMSecsSinceEpoch();
    expect(mtimeAfterChange > mtimeAfterFirst,
           "INV-1 (functional): setTheme to a NEW value rewrites the file");
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
    testInv2_onConfigFileChanged_skipsNoOpApplyTheme();
    testInv3_reentrancyGuard();
    testInv4_failedBlockSignalsRemoved();
    return g_failures == 0 ? 0 : 1;
}
