// ANTS-1372 — caller-cwd gate implementation. Header docs at
// src/remotecontrolgate.h; design at docs/specs/ANTS-1372.md.

#include "remotecontrolgate.h"

#include <QFileInfo>
#include <QLoggingCategory>

namespace RcGate {

CallerCwdGate checkCallerCwd(const QString &focusedRaw,
                             const QJsonObject &req,
                             const QString &verb) {
    CallerCwdGate g;
    g.focused = focusedRaw;

    const QString rawCaller =
        req.value(QStringLiteral("caller_cwd")).toString();
    if (rawCaller.isEmpty()) {
        g.errorCode = QStringLiteral("cwd_missing");
        g.error = QStringLiteral("%1: caller_cwd argument required "
            "(pass your $PWD; refuses on mismatch with focused tab)")
            .arg(verb);
        return g;
    }
    g.caller = QFileInfo(rawCaller).canonicalFilePath();
    if (g.caller.isEmpty()) {
        g.errorCode = QStringLiteral("cwd_bad");
        g.error = QStringLiteral("%1: caller_cwd \"%2\" does not exist")
            .arg(verb, rawCaller);
        return g;
    }
    if (g.focused.isEmpty()) {
        g.errorCode = QStringLiteral("no_project");
        g.error = QStringLiteral("%1: no focused project").arg(verb);
        return g;
    }
    if (g.caller != g.focused) {
        g.errorCode = QStringLiteral("cwd_mismatch");
        g.error = QStringLiteral("%1: calling session cwd \"%2\" does "
            "not match focused tab cwd \"%3\"")
            .arg(verb, g.caller, g.focused);
        qWarning("[ANTS-1372] cwd_mismatch refused: verb=%s caller=%s focused=%s",
                 qUtf8Printable(verb),
                 qUtf8Printable(g.caller),
                 qUtf8Printable(g.focused));
        return g;
    }
    g.ok = true;
    return g;
}

QJsonObject gateErrorEnvelope(const CallerCwdGate &g) {
    QJsonObject env;
    env[QStringLiteral("ok")]    = false;
    env[QStringLiteral("error")] = g.error;
    env[QStringLiteral("code")]  = g.errorCode;
    // ANTS-1418 — only the cwd_missing branch benefits from the
    // diagnostic-verb hint. The other branches (cwd_bad, no_project,
    // cwd_mismatch) refuse for reasons the verb can't directly help
    // with (the caller has a caller_cwd, just one that doesn't
    // resolve / has no project / mismatches the focused tab).
    if (g.errorCode == QStringLiteral("cwd_missing")) {
        env[QStringLiteral("hint")] = QStringLiteral(
            "call mcp__ants__caller_cwd_info with your $PWD to "
            "confirm which tab Ants would route this call to");
        // ANTS-1543 — concrete JSON snippet so the caller can copy
        // the exact shape (mirrors session_memory's per-op example).
        QJsonObject ex;
        ex[QStringLiteral("caller_cwd")] = QStringLiteral("<your $PWD>");
        env[QStringLiteral("example")] = ex;
    }
    return env;
}

} // namespace RcGate
