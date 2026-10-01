// ANTS-5527 — run a tool on the host when ants-mcpd is inside a Flatpak.
//
// The KDE runtime carries no rg, git or ctest, so a verb that runs one
// inside the sandbox fails. flatpak-spawn --host runs it on the host
// instead, the same route ptyhandler.cpp takes for the user's shell.
// Outside a Flatpak the launch is unchanged.
//
// Contract: tests/features/flatpak_host_tools/spec.md.

#ifndef ANTS_HOSTEXEC_H
#define ANTS_HOSTEXEC_H

#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

namespace HostExec {

struct Launch {
    QString     program;
    QStringList args;
};

// Pure: the launch for `program args` run in `dir` with `env`. `env` empty
// means the caller set no environment. `base` is the environment the
// variables are compared against (the process's own).
inline Launch wrap(const QString &program, const QStringList &args,
                   const QString &dir, const QProcessEnvironment &env,
                   bool sandboxed,
                   const QProcessEnvironment &base =
                       QProcessEnvironment::systemEnvironment()) {
    if (!sandboxed) return {program, args};
    // --watch-bus: the host process dies with us, even on SIGKILL, which
    // flatpak-spawn cannot forward.
    QStringList out{QStringLiteral("--host"), QStringLiteral("--watch-bus")};
    if (!dir.isEmpty()) out << QStringLiteral("--directory=") + dir;
    // flatpak-spawn --host does not pass our environment, so forward what
    // the caller changed (GIT_OPTIONAL_LOCKS, say). The rest is the host's.
    for (const QString &key : env.keys()) {
        const QString value = env.value(key);
        if (!base.contains(key) || base.value(key) != value)
            out << QStringLiteral("--env=%1=%2").arg(key, value);
    }
    out << QStringLiteral("--") << program << args;
    return {QStringLiteral("flatpak-spawn"), out};
}

// The same two signals ptyhandler.cpp tests (flatpak_host_shell INV-1).
inline bool inFlatpak() {
    static const bool sandboxed =
        qEnvironmentVariableIsSet("FLATPAK_ID") ||
        QFile::exists(QStringLiteral("/.flatpak-info"));
    return sandboxed;
}

// QProcess::start(program, args), on the host when inside a Flatpak. Set
// the working directory and environment on `p` first.
inline void start(QProcess &p, const QString &program,
                  const QStringList &args) {
    const Launch l = wrap(program, args, p.workingDirectory(),
                          p.processEnvironment(), inFlatpak());
    p.start(l.program, l.args);
}

}  // namespace HostExec

#endif
