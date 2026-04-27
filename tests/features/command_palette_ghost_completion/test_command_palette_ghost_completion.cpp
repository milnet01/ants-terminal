// Feature test: Command Palette ghost-text completion (0.7.42).
// See spec.md for the user-facing contract and 10 invariants pinned
// here. Builds GhostLineEdit + CommandPalette directly under
// QT_QPA_PLATFORM=offscreen — no MainWindow, no PTY.

#include <QApplication>
#include <QAction>
#include <QFile>
#include <QKeyEvent>
#include <QSignalSpy>
#include <QString>
#include <QTextStream>

#include <cstdio>
#include <cstdlib>

#include "commandpalette.h"

#ifndef COMMANDPALETTE_CPP_PATH
#error "COMMANDPALETTE_CPP_PATH must be defined by CMake (full path to commandpalette.cpp)"
#endif

namespace {

int g_failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); \
        ++g_failures; \
    } \
} while (0)

#define CHECK_EQ_STR(actual, expected, label) do { \
    QString _a = (actual); \
    QString _e = (expected); \
    if (_a != _e) { \
        std::fprintf(stderr, "FAIL [%s:%d] %s: got \"%s\", expected \"%s\"\n", \
            __FILE__, __LINE__, label, _a.toUtf8().constData(), _e.toUtf8().constData()); \
        ++g_failures; \
    } \
} while (0)

void sendKey(QWidget *target, int key) {
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QApplication::sendEvent(target, &press);
}

QString readFile(const char *path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    QTextStream in(&f);
    return in.readAll();
}

int countSubstr(const QString &haystack, const QString &needle) {
    if (needle.isEmpty()) return 0;
    int n = 0;
    int i = 0;
    while ((i = haystack.indexOf(needle, i)) != -1) {
        ++n;
        i += needle.size();
    }
    return n;
}

} // namespace

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    // Three actions designed to exercise prefix / non-prefix / no-match:
    //   "Index Review"  — prefix-matched by "ind", "INDEX"
    //   "Index Browse"  — second prefix-matched candidate (sanity)
    //   "Open File"     — `contains` "File" but does NOT start with it
    QAction a1("Index Review");
    QAction a2("Index Browse");
    QAction a3("Open File");

    auto *palette = new CommandPalette(nullptr);
    palette->setActions({&a1, &a2, &a3});
    palette->show();  // builds m_list lazily

    auto *input = palette->findChild<GhostLineEdit *>("commandPaletteInput");
    CHECK(input != nullptr, "I1: m_input is a GhostLineEdit findable by objectName");
    if (!input) {
        std::fprintf(stderr, "FATAL: cannot continue without m_input\n");
        return 1;
    }

    // I2: empty filter → empty ghost.
    input->setText("");
    QApplication::processEvents();
    CHECK_EQ_STR(input->ghostSuffix(), QString(), "I2: empty filter → empty ghost");

    // I3: prefix "ind" → ghost = "ex Review" (case preserved from the
    //     action name; "Index Review" is first in setActions order so
    //     it ranks ahead of "Index Browse").
    input->setText("ind");
    QApplication::processEvents();
    CHECK_EQ_STR(input->ghostSuffix(), QStringLiteral("ex Review"),
                 "I3: prefix 'ind' → ghost 'ex Review'");

    // I4: uppercase prefix "INDEX" → ghost preserves casing from action name.
    input->setText("INDEX");
    QApplication::processEvents();
    CHECK_EQ_STR(input->ghostSuffix(), QStringLiteral(" Review"),
                 "I4: prefix 'INDEX' (case-insensitive) → ghost ' Review'");

    // I5: "File" matches "Open File" via contains but not startsWith → empty ghost.
    input->setText("File");
    QApplication::processEvents();
    CHECK_EQ_STR(input->ghostSuffix(), QString(),
                 "I5: contains-only match → empty ghost");

    // I6: filter that matches nothing → empty ghost.
    input->setText("xyz");
    QApplication::processEvents();
    CHECK_EQ_STR(input->ghostSuffix(), QString(),
                 "I6: no match → empty ghost");

    // I7: Tab commits the ghost; the post-commit text equals the
    // visible composition (user-typed prefix + ghost suffix), which
    // here is "ind" + "ex Review" = "index Review". This preserves
    // the user's casing — same as shell-completion semantics — and
    // matches what was on screen pre-commit. Ghost clears in the
    // same dispatch cycle since the new filter equals the action
    // name's tail exactly.
    input->setText("ind");
    QApplication::processEvents();
    sendKey(input, Qt::Key_Tab);
    QApplication::processEvents();
    CHECK_EQ_STR(input->text(), QStringLiteral("index Review"),
                 "I7a: Tab commit → text = filter + ghost (user casing preserved)");
    CHECK_EQ_STR(input->ghostSuffix(), QString(),
                 "I7b: Tab commit → ghost cleared");

    // I8: Tab with empty ghost is consumed (text unchanged, focus retained).
    input->setText("xyz");
    QApplication::processEvents();
    QString before = input->text();
    input->setFocus();
    sendKey(input, Qt::Key_Tab);
    QApplication::processEvents();
    CHECK_EQ_STR(input->text(), before, "I8a: Tab with empty ghost → text unchanged");
    CHECK(input->hasFocus(), "I8b: Tab with empty ghost → focus retained on input");

    // I9: source-grep — exactly one setAlphaF(0.45 literal, ≥1 cursorRect( ref.
    QString src = readFile(COMMANDPALETTE_CPP_PATH);
    CHECK(!src.isEmpty(), "I9-pre: commandpalette.cpp is readable");
    int alphaCount = countSubstr(src, QStringLiteral("setAlphaF(0.45"));
    CHECK(alphaCount == 1, "I9a: exactly one 'setAlphaF(0.45' literal in commandpalette.cpp");
    if (alphaCount != 1)
        std::fprintf(stderr, "    (count=%d)\n", alphaCount);
    int cursorRectCount = countSubstr(src, QStringLiteral("cursorRect("));
    CHECK(cursorRectCount >= 1,
          "I9b: at least one cursorRect( reference in commandpalette.cpp");
    if (cursorRectCount < 1)
        std::fprintf(stderr, "    (count=%d)\n", cursorRectCount);

    // I10: Esc still hides the palette and emits closed() — regression guard.
    palette->show();
    QApplication::processEvents();
    QSignalSpy closedSpy(palette, &CommandPalette::closed);
    sendKey(input, Qt::Key_Escape);
    QApplication::processEvents();
    CHECK(palette->isHidden(), "I10a: Esc hides the palette");
    CHECK(closedSpy.count() == 1, "I10b: Esc emits closed() exactly once");

    delete palette;

    if (g_failures == 0) {
        std::fprintf(stderr, "OK: command_palette_ghost_completion (10 invariants)\n");
        return 0;
    }
    std::fprintf(stderr, "FAIL: %d invariant(s) failed\n", g_failures);
    return 1;
}
