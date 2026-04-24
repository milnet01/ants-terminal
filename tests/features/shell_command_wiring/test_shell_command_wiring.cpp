// Feature-conformance test for spec.md —
//
// I1: TerminalWidget::startShell signature carries a `shell` param.
// I2: TerminalWidget::startShell forwards `shell` to VtStream::start,
//     not the hardcoded QString() that dropped the value pre-0.7.18.
// I3: All production call sites in MainWindow pass
//     m_config.shellCommand() as the shell argument; no orphan
//     startShell calls that forget the arg.
// I4: Config::shellCommand round-trip persists across reload.
//
// Exit 0 = invariants hold. Non-zero = regression.
//
// No widget / PTY harness — source-grep + Config round-trip only.

#include "config.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QUuid>

#include <cstdio>
#include <cstdlib>

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

QString slurp(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::fprintf(stderr, "cannot open %s\n", qUtf8Printable(path));
        std::exit(2);
    }
    return QString::fromUtf8(f.readAll());
}

// Strip // line comments so the I3 grep doesn't false-positive on
// narrative documentation that talks about startShell / shellCommand.
QString stripLineComments(const QString &src) {
    QStringList out;
    out.reserve(src.count(QChar('\n')) + 1);
    for (const QString &line : src.split(QChar('\n'))) {
        const int commentStart = line.indexOf(QStringLiteral("//"));
        if (commentStart < 0) {
            out.append(line);
        } else {
            out.append(line.left(commentStart));
        }
    }
    return out.join(QChar('\n'));
}

void checkI1() {
    const QString hdr = slurp(QStringLiteral(SRC_TERMINALWIDGET_H_PATH));
    const QString code = stripLineComments(hdr);
    // Canonical form: two parameters, both `const QString &`, in this order.
    const bool ok = code.contains(
        QStringLiteral("startShell(const QString &workDir"))
        && code.contains(QStringLiteral("const QString &shell"));
    expect(ok, "I1/startShell-signature-has-shell-param",
           QStringLiteral("terminalwidget.h startShell must declare "
                          "(const QString &workDir, const QString &shell)"));
}

void checkI2() {
    const QString src = slurp(QStringLiteral(SRC_TERMINALWIDGET_CPP_PATH));
    // Find the invokeMethod call that launches VtStream::start. The
    // canonical shape is multi-line; concatenate neighbouring lines
    // into one string for regex-free substring matching.
    const int idx = src.indexOf(
        QStringLiteral("invokeMethod(m_vtStream, \"start\""));
    expect(idx > 0, "I2/invokeMethod-start-present",
           QStringLiteral("terminalwidget.cpp no longer has "
                          "invokeMethod(m_vtStream, \"start\" …) — "
                          "signature changed?"));
    if (idx < 0) return;

    // Look at the next ~500 chars for the Q_ARG(QString, shell) form.
    // Pre-fix was Q_ARG(QString, QString()); that's what we're guarding
    // against.
    const QString window = src.mid(idx, 500);
    const bool passesShell =
        window.contains(QStringLiteral("Q_ARG(QString, shell)"));
    const bool passesEmptyString =
        window.contains(QStringLiteral("Q_ARG(QString, QString())"));

    expect(passesShell, "I2/invokeMethod-forwards-shell-param",
           QStringLiteral("expected Q_ARG(QString, shell) — found\n%1")
               .arg(window));
    expect(!passesEmptyString,
           "I2/invokeMethod-does-not-pass-empty-string",
           QStringLiteral("regression: Q_ARG(QString, QString()) is back "
                          "in the VtStream start invocation"));
}

void checkI3() {
    const QString src = slurp(QStringLiteral(SRC_MAINWINDOW_CPP_PATH));
    const QString code = stripLineComments(src);

    // Count occurrences of `startShell(` that include `shellCommand()` on
    // the same logical call. Sample the 120 chars starting at each
    // `startShell(` occurrence; if `shellCommand()` appears in that
    // window, count as wired.
    int wired = 0;
    int orphan = 0;
    int from = 0;
    while (true) {
        const int at = code.indexOf(QStringLiteral("startShell("), from);
        if (at < 0) break;
        // Skip the declaration at terminalwidget.h-mirror site (nothing
        // in mainwindow.cpp should declare startShell).
        const QString window = code.mid(at, 120);
        if (window.contains(QStringLiteral("shellCommand()"))) {
            ++wired;
        } else {
            ++orphan;
            std::fprintf(stderr,
                         "    orphan startShell call: %s\n",
                         qUtf8Printable(window.left(80)));
        }
        from = at + 11;  // len("startShell(")
    }

    expect(wired >= 4,
           "I3/at-least-4-call-sites-pass-shellCommand",
           QStringLiteral("expected >= 4 wired call sites, got %1 "
                          "(newTab + newTabForRemote + splitCurrentPane + "
                          "session-restore)")
               .arg(wired));
    expect(orphan == 0,
           "I3/no-orphan-startShell-calls",
           QStringLiteral("%1 startShell call(s) in mainwindow.cpp do "
                          "not thread m_config.shellCommand() — the "
                          "Settings shell picker will be ignored on "
                          "those code paths")
               .arg(orphan));
}

void checkI4() {
    // Sandbox via XDG_CONFIG_HOME so the test doesn't touch the real
    // user's config. QUuid keeps parallel test runs isolated.
    const QString isolateDir =
        QStandardPaths::writableLocation(QStandardPaths::TempLocation)
        + QStringLiteral("/ants-shellcmd-wiring-")
        + QUuid::createUuid().toString(QUuid::Id128);
    QDir().mkpath(isolateDir);
    qputenv("XDG_CONFIG_HOME", isolateDir.toLocal8Bit());

    {
        Config c;
        c.setShellCommand(QStringLiteral("zsh"));
        c.save();
    }
    {
        Config c;  // fresh load
        const QString got = c.shellCommand();
        expect(got == QStringLiteral("zsh"),
               "I4/shellCommand-round-trip",
               QStringLiteral("setShellCommand(\"zsh\") + save + reload "
                              "returned \"%1\", expected \"zsh\"")
                   .arg(got));
    }

    // Courtesy cleanup.
    QDir(isolateDir).removeRecursively();
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    checkI1();
    checkI2();
    checkI3();
    checkI4();

    if (g_failures) {
        std::fprintf(stderr, "\n%d invariant(s) failed\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nall invariants hold\n");
    return 0;
}
