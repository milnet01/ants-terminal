// Feature-conformance test for tests/features/kwin_position_tracker/spec.md.
//
// Locks ANTS-1045 — XcbPositionTracker → KWinPositionTracker rename,
// KWin-presence env-var guard, and QScopeGuard cleanup of the temp
// script file. INV-1 through INV-6 (and the b/c sub-INVs).
//
// Exit 0 = all invariants hold.

#include "kwinpositiontracker.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

namespace {

std::string slurp(const char *path) {
    std::ifstream in(path);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

int fail(const char *label, const char *why) {
    std::fprintf(stderr, "[%s] FAIL: %s\n", label, why);
    return 1;
}

// INV-1 carve-out: lines starting with `//` or ` *` may mention the
// old name (rename-history comment). Returns true if the line is a
// comment line.
bool isCommentLine(const std::string &line) {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i + 1 >= line.size()) return false;
    if (line[i] == '/' && line[i + 1] == '/') return true;
    if (line[i] == '*') return true;
    return false;
}

QStringList tempEntriesByGlob(const QString &glob) {
    return QDir(QDir::tempPath()).entryList(QStringList{glob},
                                            QDir::Files | QDir::NoDotAndDotDot);
}

}  // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);

    const std::string headerPath = KWINPOS_H;
    const std::string sourcePath = KWINPOS_CPP;
    const std::string mainwinH   = MAINWINDOW_H;
    const std::string mainwinCpp = MAINWINDOW_CPP;
    const std::string cmakeLists = CMAKELISTS_TXT;

    const std::string header = slurp(headerPath.c_str());
    const std::string source = slurp(sourcePath.c_str());
    const std::string mwH    = slurp(mainwinH.c_str());
    const std::string mwCpp  = slurp(mainwinCpp.c_str());
    const std::string cmake  = slurp(cmakeLists.c_str());
    if (header.empty() || source.empty() || mwH.empty() ||
        mwCpp.empty() || cmake.empty())
        return fail("INV-1", "one of the source files not readable");

    // INV-1: class renamed.
    if (!contains(header, "class KWinPositionTracker"))
        return fail("INV-1", "KWinPositionTracker class declaration missing");

    // INV-2: file renamed (header path itself proves the new name lives;
    // the old paths must NOT exist).
    {
        const std::string oldHdr = std::string(SRC_DIR) + "/xcbpositiontracker.h";
        const std::string oldCpp = std::string(SRC_DIR) + "/xcbpositiontracker.cpp";
        if (QFileInfo::exists(QString::fromStdString(oldHdr)))
            return fail("INV-2", "old src/xcbpositiontracker.h still exists");
        if (QFileInfo::exists(QString::fromStdString(oldCpp)))
            return fail("INV-2", "old src/xcbpositiontracker.cpp still exists");
        if (!QFileInfo::exists(QString::fromStdString(headerPath)) ||
            !QFileInfo::exists(QString::fromStdString(sourcePath)))
            return fail("INV-2", "new kwinpositiontracker files missing");
    }

    // INV-3: no `XcbPositionTracker` references outside comments
    // anywhere in src/, CMakeLists, packaging.
    auto scanForOldName = [&](const std::string &where,
                              const std::string &body) -> int {
        std::stringstream ss(body);
        std::string line;
        int hits = 0;
        while (std::getline(ss, line)) {
            if (!contains(line, "XcbPositionTracker")) continue;
            if (isCommentLine(line)) continue;
            std::fprintf(stderr,
                "[INV-3] FAIL: non-comment XcbPositionTracker reference in %s: %s\n",
                where.c_str(), line.c_str());
            ++hits;
        }
        return hits;
    };
    int leakHits = 0;
    leakHits += scanForOldName("kwinpositiontracker.h", header);
    leakHits += scanForOldName("kwinpositiontracker.cpp", source);
    leakHits += scanForOldName("mainwindow.h", mwH);
    leakHits += scanForOldName("mainwindow.cpp", mwCpp);
    leakHits += scanForOldName("CMakeLists.txt", cmake);
    if (leakHits > 0) return 1;

    // INV-4: KWin-presence guard. ANTS-1142 (0.7.69) lifted the
    // KDE_FULL_SESSION / XDG_CURRENT_DESKTOP env-var checks out
    // of setPosition() and into the `kwinPresent()` free function
    // in kwinpositiontracker.h, so MainWindow::moveViaKWin /
    // centerWindow could share the same guard. The test now
    // verifies (a) the env-var literals live in the header, and
    // (b) setPosition() calls kwinPresent() before any
    // QTemporaryFile is constructed.
    if (!contains(header, "KDE_FULL_SESSION"))
        return fail("INV-4",
            "KDE_FULL_SESSION env-var check missing in kwinpositiontracker.h");
    if (!contains(header, "XDG_CURRENT_DESKTOP"))
        return fail("INV-4",
            "XDG_CURRENT_DESKTOP env-var check missing in kwinpositiontracker.h");
    if (!contains(header, "inline bool kwinPresent"))
        return fail("INV-4",
            "kwinPresent() helper missing in kwinpositiontracker.h "
            "(ANTS-1142 lift)");
    // Scope ordering checks to setPosition's body.
    const size_t fnStart = source.find("setPosition(int x, int y)");
    if (fnStart == std::string::npos)
        return fail("INV-4", "setPosition() definition not found");
    const std::string fnBody = source.substr(fnStart);
    {
        const size_t firstGuard = fnBody.find("kwinPresent(");
        const std::regex tempCtor(R"(QTemporaryFile\s+\w+\()");
        std::smatch tempMatch;
        if (firstGuard == std::string::npos)
            return fail("INV-4",
                "kwinPresent() not invoked in setPosition() — guard missing");
        if (!std::regex_search(fnBody, tempMatch, tempCtor))
            return fail("INV-4",
                "no QTemporaryFile constructor call in setPosition()");
        const size_t firstTempCtor = static_cast<size_t>(tempMatch.position(0));
        if (firstGuard >= firstTempCtor)
            return fail("INV-4",
                "kwinPresent() must appear before QTemporaryFile ctor (guard placement)");
    }

    // INV-4b: behavioural drive of the bail. With both env vars
    // unset, setPosition() must NOT write a kwin_pos_ants_* file.
    {
        unsetenv("KDE_FULL_SESSION");
        unsetenv("XDG_CURRENT_DESKTOP");
        const QStringList before = tempEntriesByGlob(
            QStringLiteral("kwin_pos_ants_*"));
        QWidget dummy;
        KWinPositionTracker tracker(&dummy);
        tracker.setPosition(0, 0);
        const QStringList after = tempEntriesByGlob(
            QStringLiteral("kwin_pos_ants_*"));
        if (after != before)
            return fail("INV-4b",
                "setPosition() wrote a temp file despite both env vars unset");
    }

    // INV-5: qScopeGuard reference + dismiss() in same function.
    if (!contains(source, "qScopeGuard") &&
        !contains(source, "QScopeGuard"))
        return fail("INV-5", "qScopeGuard not used");
    if (!contains(source, "dismiss()"))
        return fail("INV-5", "dismiss() not called");

    // INV-5b: qScopeGuard *call* line < first QTemporaryFile
    // *constructor* line (both scoped to the function body and
    // matched as expressions so comments don't false-match).
    {
        const std::regex guardCall(R"(qScopeGuard\s*\()");
        const std::regex tempCtor(R"(QTemporaryFile\s+\w+\()");
        std::smatch g, t;
        if (!std::regex_search(fnBody, g, guardCall))
            return fail("INV-5b", "qScopeGuard call not found in setPosition()");
        if (!std::regex_search(fnBody, t, tempCtor))
            return fail("INV-5b",
                "QTemporaryFile constructor not found in setPosition()");
        if (static_cast<size_t>(g.position(0)) >=
            static_cast<size_t>(t.position(0)))
            return fail("INV-5b",
                "qScopeGuard must precede QTemporaryFile constructor (ordering)");
    }

    // INV-6: TOCTOU fix preserved + no hardcoded path regression.
    if (!contains(source, "QTemporaryFile"))
        return fail("INV-6", "QTemporaryFile call removed (TOCTOU regression)");
    if (!contains(source, "setAutoRemove(false)"))
        return fail("INV-6",
            "setAutoRemove(false) call removed (cleanup contract regression)");
    // Negative literal-path check across src/.
    {
        QDir srcDir(SRC_DIR);
        const QStringList entries =
            srcDir.entryList(QStringList{QStringLiteral("*.cpp"),
                                         QStringLiteral("*.h")},
                             QDir::Files, QDir::NoSort);
        for (const QString &name : entries) {
            const std::string body = slurp(
                (std::string(SRC_DIR) + "/" + name.toStdString()).c_str());
            if (contains(body, "/tmp/kwin_pos_ants")) {
                std::fprintf(stderr,
                    "[INV-6] FAIL: hardcoded /tmp/kwin_pos_ants path in %s\n",
                    name.toUtf8().constData());
                return 1;
            }
        }
    }

    // INV-5c (behavioural cleanup on failure): set KWin gate to
    // "true" but break dbus-send by overriding PATH to an empty
    // dir. The temp file should be cleaned up after the QProcess
    // settles.
    {
        setenv("KDE_FULL_SESSION", "true", 1);
        const QByteArray savedPath = qgetenv("PATH");
        setenv("PATH", "", 1);
        const QStringList before = tempEntriesByGlob(
            QStringLiteral("kwin_pos_ants_*"));
        QWidget dummy;
        KWinPositionTracker tracker(&dummy);
        tracker.setPosition(7, 7);
        // Pump events so the QProcess errorOccurred / finished
        // lambdas fire. Bounded loop, ~500 ms cap.
        for (int i = 0; i < 50; ++i) {
            QCoreApplication::processEvents(
                QEventLoop::AllEvents, 10);
        }
        const QStringList after = tempEntriesByGlob(
            QStringLiteral("kwin_pos_ants_*"));
        // Restore PATH for any post-test work.
        setenv("PATH", savedPath.constData(), 1);
        if (after.size() > before.size()) {
            std::fprintf(stderr,
                "[INV-5c] FAIL: %lld new temp files left behind on failure path\n",
                static_cast<long long>(after.size() - before.size()));
            return 1;
        }
    }

    std::fprintf(stderr, "OK — KWinPositionTracker INVs hold.\n");
    return 0;
}
