// 0.7.41 — Accessibility for chrome buttons.
//
// Companion to tests/features/a11y_chrome_names/spec.md. Verifies the
// six glyph-only chrome controls (TitleBar's center/minimize/maximize/
// close + CommandPalette's search input + result list) carry explicit
// setAccessibleName / setAccessibleDescription, and acts as the AT-SPI
// introspection lane (T7) by walking the chrome widget tree and
// asserting every QAbstractButton + QLineEdit / QListWidget reachable
// from those parents has either a non-empty accessibleName() or a
// non-empty text(). Future contributors who add a glyph-only chrome
// button without a name break this test.

#include <QApplication>
#include <QAbstractButton>
#include <QLineEdit>
#include <QListWidget>
#include <QToolButton>
#include <QPointer>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "titlebar.h"
#include "commandpalette.h"

namespace {

#ifndef TITLEBAR_CPP_PATH
#error "TITLEBAR_CPP_PATH must be defined by CMake"
#endif
#ifndef COMMANDPALETTE_CPP_PATH
#error "COMMANDPALETTE_CPP_PATH must be defined by CMake"
#endif

int g_fails = 0;

void fail(const std::string &where, const std::string &why) {
    std::fprintf(stderr, "FAIL %s: %s\n", where.c_str(), why.c_str());
    ++g_fails;
}

std::string slurp(const char *path) {
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int countSubstr(const std::string &hay, const char *needle) {
    int c = 0;
    size_t pos = 0;
    const size_t nlen = std::strlen(needle);
    while ((pos = hay.find(needle, pos)) != std::string::npos) {
        ++c;
        pos += nlen;
    }
    return c;
}

void requireAccessibleName(QWidget *root, const char *objectName,
                           const char *expectedName,
                           bool expectDescription = true) {
    if (!root) {
        fail(objectName, "root widget is null");
        return;
    }
    QWidget *w = root->findChild<QWidget *>(QString::fromUtf8(objectName));
    if (!w) {
        fail(objectName, "widget not found in chrome tree");
        return;
    }
    const QString got = w->accessibleName();
    const QString want = QString::fromUtf8(expectedName);
    if (got != want) {
        fail(objectName,
             "accessibleName mismatch: got='" + got.toStdString() +
                 "' want='" + want.toStdString() + "'");
    }
    if (expectDescription && w->accessibleDescription().isEmpty()) {
        fail(objectName, "accessibleDescription is empty");
    }
}

// T7 — coverage sweep. For every QAbstractButton or QLineEdit reachable
// from the given root, accessibleName() or text() (Qt promotes text() to
// the AT-SPI name) must be non-empty.
void coverageSweep(QWidget *root, const char *rootLabel) {
    if (!root) return;
    const QList<QWidget *> all = root->findChildren<QWidget *>();
    for (QWidget *w : all) {
        if (auto *b = qobject_cast<QAbstractButton *>(w)) {
            const QString name = b->accessibleName();
            const QString text = b->text();
            if (name.isEmpty() && text.isEmpty()) {
                fail(std::string(rootLabel) + " coverage",
                     "QAbstractButton (objectName='" +
                         b->objectName().toStdString() +
                         "') has empty accessibleName AND empty text");
            }
        } else if (auto *e = qobject_cast<QLineEdit *>(w)) {
            // QLineEdit doesn't have text() as a label — it's the user's
            // input. accessibleName must be set explicitly.
            if (e->accessibleName().isEmpty()) {
                fail(std::string(rootLabel) + " coverage",
                     "QLineEdit (objectName='" +
                         e->objectName().toStdString() +
                         "') has empty accessibleName");
            }
        }
    }
}

}  // namespace

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    // T1-T4 — TitleBar chrome buttons.
    auto *bar = new TitleBar(nullptr);
    requireAccessibleName(bar, "centerBtn", "Center window");
    requireAccessibleName(bar, "minimizeBtn", "Minimize window");
    requireAccessibleName(bar, "maximizeBtn", "Maximize window");
    requireAccessibleName(bar, "closeBtn", "Close window");

    // Each TitleBar button must be a QToolButton (sanity — protects against
    // a later refactor that swaps to QPushButton without preserving names).
    for (const char *name : {"centerBtn", "minimizeBtn", "maximizeBtn", "closeBtn"}) {
        auto *tb = bar->findChild<QToolButton *>(QString::fromUtf8(name));
        if (!tb) {
            fail(name, "expected QToolButton, got something else (or missing)");
        }
    }

    // T5 — CommandPalette search input. The palette doesn't need to be
    // shown for the input — it's constructed eagerly. m_list (T6) is
    // lazy on first show().
    auto *palette = new CommandPalette(nullptr);
    requireAccessibleName(palette, "commandPaletteInput",
                          "Command palette search");

    // Trigger lazy list construction. show() positions relative to
    // parentWidget() and returns early when there is none — but
    // ensureListReady() runs unconditionally before that early return.
    palette->show();
    requireAccessibleName(palette, "commandPaletteList",
                          "Command palette results",
                          /*expectDescription=*/false);
    // Description is required by the spec table for both palette
    // controls — assert it explicitly here too.
    {
        auto *list = palette->findChild<QListWidget *>("commandPaletteList");
        if (list && list->accessibleDescription().isEmpty()) {
            fail("commandPaletteList", "accessibleDescription is empty");
        }
    }

    // T7 — coverage sweep over both chrome roots.
    coverageSweep(bar, "TitleBar");
    coverageSweep(palette, "CommandPalette");

    // T8 — source-grep on titlebar.cpp.
    {
        const std::string src = slurp(TITLEBAR_CPP_PATH);
        if (src.empty()) {
            fail("TITLEBAR_CPP_PATH", "could not read source");
        } else {
            const int n = countSubstr(src, "setAccessibleName(");
            if (n < 4) {
                fail("titlebar.cpp",
                     "expected ≥4 setAccessibleName() calls, found " +
                         std::to_string(n));
            }
            const int d = countSubstr(src, "setAccessibleDescription(");
            if (d < 4) {
                fail("titlebar.cpp",
                     "expected ≥4 setAccessibleDescription() calls, found " +
                         std::to_string(d));
            }
            // Stable objectNames so Orca + the introspection sweep above
            // can always find them.
            for (const char *needle : {"\"centerBtn\"", "\"minimizeBtn\"",
                                       "\"maximizeBtn\"", "\"closeBtn\""}) {
                if (src.find(needle) == std::string::npos) {
                    fail("titlebar.cpp",
                         std::string("missing objectName literal: ") + needle);
                }
            }
        }
    }

    // T9 — source-grep on commandpalette.cpp.
    {
        const std::string src = slurp(COMMANDPALETTE_CPP_PATH);
        if (src.empty()) {
            fail("COMMANDPALETTE_CPP_PATH", "could not read source");
        } else {
            const int n = countSubstr(src, "setAccessibleName(");
            if (n < 2) {
                fail("commandpalette.cpp",
                     "expected ≥2 setAccessibleName() calls, found " +
                         std::to_string(n));
            }
        }
    }

    delete palette;
    delete bar;

    if (g_fails == 0) {
        std::printf("a11y_chrome_names: OK\n");
        return 0;
    }
    std::fprintf(stderr, "a11y_chrome_names: %d failure(s)\n", g_fails);
    return 1;
}
