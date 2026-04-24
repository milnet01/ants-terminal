// Feature-conformance test for spec.md —
//
// Invariant 1 — after saveSettings(), the final settings file is 0600.
// Invariant 2 — both pre- and post-commit setOwnerOnlyPerms calls exist
//               in src/claudeallowlist.cpp (source-grep).
// Invariant 3 — commit-failure path doesn't chmod a path that may not
//               have been created (exercised via source-grep, not a
//               runtime test — the "commit false" branch is hard to
//               fake without mocking QSaveFile internals).
// Invariant 4 — first-run creation lands 0600 even with a permissive
//               umask (0022).
//
// Links against src/claudeallowlist.cpp. Runs under
// QT_QPA_PLATFORM=offscreen because ClaudeAllowlistDialog is a QDialog.
// Exit 0 = all invariants hold. Non-zero = regression.

#include "claudeallowlist.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QString>
#include <QUuid>

#include <sys/stat.h>

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

// Returns the POSIX mode bits (0..07777) of `path`, or -1 if stat fails.
int statMode(const QString &path) {
    struct stat st{};
    if (::stat(path.toLocal8Bit().constData(), &st) != 0) return -1;
    return static_cast<int>(st.st_mode & 07777);
}

// Invariant 1 + Invariant 4: saveSettings lands a 0600 file under a
// permissive umask.
void checkPermsAfterSave() {
    // Umask 0022 — the POSIX default. If the code relied on umask to set
    // perms (instead of an explicit chmod), fresh files would land 0644
    // here and the invariant would fail.
    const mode_t oldUmask = ::umask(0022);

    // Isolated path under TempLocation so parallel test runs don't race.
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::TempLocation)
        + QStringLiteral("/ants-allowlist-perms-")
        + QUuid::createUuid().toString(QUuid::Id128);
    QDir().mkpath(dir);
    const QString path = dir + QStringLiteral("/settings.local.json");

    // Invariant 4: file does not exist yet — exercise the first-run path.
    expect(!QFileInfo::exists(path),
           "I4/precondition: settings file does not exist pre-save");

    ClaudeAllowlistDialog dlg;
    dlg.setSettingsPath(path);
    const bool saved = dlg.saveSettings();
    expect(saved, "I1/save-returns-true", path);

    expect(QFileInfo::exists(path),
           "I1/file-exists-after-save", path);

    const int mode = statMode(path);
    expect(mode == 0600,
           "I1+I4/first-run-perms-are-0600",
           QStringLiteral("expected 0600, got 0%1")
               .arg(mode, 4, 8, QLatin1Char('0')));

    // Invariant 1 repeat — run saveSettings again (file exists + parses
    // path). Permissions must stay 0600.
    const bool savedAgain = dlg.saveSettings();
    expect(savedAgain, "I1/second-save-returns-true");
    const int modeAgain = statMode(path);
    expect(modeAgain == 0600,
           "I1/repeat-save-perms-are-0600",
           QStringLiteral("expected 0600, got 0%1")
               .arg(modeAgain, 4, 8, QLatin1Char('0')));

    // Cleanup — test harness courtesy, not part of the invariant.
    QFile::remove(path);
    QDir().rmdir(dir);

    ::umask(oldUmask);
}

// Invariant 2: both perm-setting call sites remain in the source. A
// refactor that drops either one silently exposes the file to the
// race the spec describes.
void checkSourceBindings() {
    const QString path = QStringLiteral(SRC_CLAUDEALLOWLIST_PATH);
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::fprintf(stderr,
                     "[FAIL] I2/source-open: cannot read %s\n",
                     qUtf8Printable(path));
        ++g_failures;
        return;
    }
    const QString src = QString::fromUtf8(f.readAll());
    f.close();

    expect(src.contains(QStringLiteral("setOwnerOnlyPerms(file)")),
           "I2/pre-commit-fd-chmod-retained",
           QStringLiteral("src/claudeallowlist.cpp lost the pre-commit "
                          "setOwnerOnlyPerms(file) call"));

    expect(src.contains(QStringLiteral("setOwnerOnlyPerms(m_settingsPath)")),
           "I1+I2/post-commit-path-chmod-present",
           QStringLiteral("src/claudeallowlist.cpp lost the post-commit "
                          "setOwnerOnlyPerms(m_settingsPath) call"));

    // Invariant 3: the post-commit chmod must be gated on commit()
    // returning true — it appears AFTER the commit() call, not before
    // or alongside it. A regression that moves it up would chmod a
    // file that may not exist in the filesystem yet. We use
    // lastIndexOf on the chmod-call needle so block-comment
    // occurrences in the source don't shadow the actual call site;
    // the commit() sentinel only appears in code, so indexOf is fine.
    const int chmodIdx = src.lastIndexOf(
        QStringLiteral("setOwnerOnlyPerms(m_settingsPath)"));
    const int commitIdx = src.indexOf(
        QStringLiteral("if (!file.commit()) return false;"));
    expect(chmodIdx > 0 && commitIdx > 0 && commitIdx < chmodIdx,
           "I3/post-commit-chmod-gated-on-commit-success",
           QStringLiteral("expected `if (!file.commit()) return false;` "
                          "to precede the setOwnerOnlyPerms(m_settingsPath) "
                          "call; commitIdx=%1 chmodIdx=%2")
               .arg(commitIdx).arg(chmodIdx));
}

}  // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);

    checkPermsAfterSave();
    checkSourceBindings();

    if (g_failures) {
        std::fprintf(stderr, "\n%d invariant(s) failed\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nall invariants hold\n");
    return 0;
}
