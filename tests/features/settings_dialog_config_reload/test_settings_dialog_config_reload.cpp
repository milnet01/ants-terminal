// Feature-conformance test for spec.md —
//
// Invariant 1 — onConfigFileChanged invalidates m_settingsDialog.
// Invariant 2 — an open/visible dialog is closed on reload.
// Invariant 3 — destruction uses deleteLater, not raw delete.
// Invariant 4 — invalidation lives only in onConfigFileChanged.
//
// Source-grep only. MainWindow is far too heavy to instantiate here —
// it wires a PTY, builds the tab bar, wires a dozen dialogs; the
// feature under test is a five-line invalidation block that is
// structurally obvious from the source. A grep that fires on the
// absence of the right tokens (or the presence of the wrong ones)
// is both sufficient and much cheaper than a MainWindow harness.

#include <QCoreApplication>
#include <QFile>
#include <QString>

#include <cstdio>

namespace {

int g_failures = 0;

void expect(bool cond, const char *label, const QString &detail = {}) {
    if (cond) {
        std::fprintf(stderr, "[PASS] %s\n", label);
    } else {
        std::fprintf(stderr, "[FAIL] %s  %s\n", label,
                     qUtf8Printable(detail));
        ++g_failures;
    }
}

// Return the substring of `src` between the first line starting with
// `signature` and the matching `^}` line below it. Used to scope a
// grep to the body of a specific function.
QString extractFunctionBody(const QString &src, const QString &signature) {
    const int start = src.indexOf(signature);
    if (start < 0) return {};
    // Scan forward for a line starting with "}" at column 0.
    int pos = src.indexOf(QStringLiteral("\n}"), start);
    if (pos < 0) return {};
    return src.mid(start, pos - start + 2);
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    const QString path = QStringLiteral(SRC_MAINWINDOW_PATH);
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::fprintf(stderr,
                     "[FAIL] source-open: cannot read %s\n",
                     qUtf8Printable(path));
        return 1;
    }
    const QString src = QString::fromUtf8(f.readAll());
    f.close();

    const QString onCfgBody = extractFunctionBody(src,
        QStringLiteral("void MainWindow::onConfigFileChanged"));
    expect(!onCfgBody.isEmpty(),
           "precondition/onConfigFileChanged-located",
           QStringLiteral("could not locate onConfigFileChanged in %1")
               .arg(path));

    // Invariant 1 — cache is nulled.
    expect(onCfgBody.contains(
               QStringLiteral("m_settingsDialog = nullptr")),
           "I1/cache-nulled-in-onConfigFileChanged",
           QStringLiteral("onConfigFileChanged must set "
                          "m_settingsDialog = nullptr after reload"));

    // Invariant 2 — if visible, close first.
    expect(onCfgBody.contains(QStringLiteral("isVisible()")) &&
               onCfgBody.contains(QStringLiteral("->close()")),
           "I2/visible-dialog-closed-before-delete",
           QStringLiteral("onConfigFileChanged must call isVisible() + "
                          "->close() before scheduling deletion"));

    // Invariant 3 — deleteLater, not raw delete.
    expect(onCfgBody.contains(
               QStringLiteral("m_settingsDialog->deleteLater()")),
           "I3/uses-deleteLater");
    expect(!onCfgBody.contains(
               QStringLiteral("delete m_settingsDialog")),
           "I3/no-raw-delete",
           QStringLiteral("onConfigFileChanged must not use raw "
                          "`delete m_settingsDialog` — deleteLater() "
                          "only"));

    // Invariant 4 — no other function in this file clears the cache.
    // The dialog's own `finished` signal does *not* clear the pointer
    // (see mainwindow.cpp:1578-1580) — it only refocuses the terminal.
    // So a second `m_settingsDialog = nullptr` anywhere else would be
    // unexpected, likely a mistaken paste, and we call it out.
    int count = 0;
    int pos = 0;
    const QString needle = QStringLiteral("m_settingsDialog = nullptr");
    while ((pos = src.indexOf(needle, pos)) >= 0) {
        ++count;
        pos += needle.size();
    }
    expect(count == 1,
           "I4/invalidation-scoped-to-onConfigFileChanged",
           QStringLiteral("expected exactly 1 site nulling "
                          "m_settingsDialog (onConfigFileChanged), "
                          "found %1").arg(count));

    // Sanity: the cached-dialog construction site is still at its
    // expected shape — if this shifts, the invariants above are
    // checking an obsolete pattern.
    expect(src.contains(
               QStringLiteral("new SettingsDialog(&m_config, this)")),
           "sanity/dialog-still-caches-m_config-pointer",
           QStringLiteral("the fix assumes SettingsDialog is cached "
                          "with `&m_config`; if that changed, this "
                          "test needs rewriting"));

    if (g_failures) {
        std::fprintf(stderr, "\n%d invariant(s) failed\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nall invariants hold\n");
    return 0;
}
