// ANTS-5299 TU 19/23 — the `run_trace` handler: a review run records its trace.
// Contract: tests/features/run_trace/spec.md.
//
// Thin by design, like its roadmap_migrate and session_message siblings. The
// seam (src/runtraceverb.cpp, outside ANTS_RC_SOURCES_REL) does everything
// that touches a file; what lives here is what cannot be tested without a
// RemoteControl — resolving caller_cwd, validating paths against the root,
// choosing the state directory, reading the clock once and naming the
// transcript.
//
// Every argument this file reads via req.value() MUST be declared in the verb's
// inputSchema.properties in claudeintegration.cpp (ANTS-4621): the schema sets
// additionalProperties:false, so an undeclared argument is refused by a strict
// client before the handler runs.

#include "remotecontrol.h"

#include "pathvalidation.h"
#include "resolvedroot.h"
#include "runtraceverb.h"
#include "sessioncostverb.h"

#include <QDateTime>
#include <QDir>
#include <QRandomGenerator>
#include <QStandardPaths>

namespace {

QJsonDocument refuse(const char *code, const QString &message) {
    QJsonObject e;
    e[QStringLiteral("ok")]    = false;
    e[QStringLiteral("code")]  = QString::fromLatin1(code);
    e[QStringLiteral("error")] = message;
    return QJsonDocument(e);
}

}  // namespace

QJsonDocument RemoteControl::cmdRunTrace(const QJsonObject &req) {
    // caller_cwd absent is the dispatcher's refusal (CallerCwdContract::Required).
    QJsonObject refusal;
    const QString cwd = ants::requiredCallerCwd(m_roots, req, QStringLiteral("run_trace"), &refusal);
    if (cwd.isEmpty())
        return QJsonDocument(refusal);

    const QString op = req.value(QStringLiteral("op")).toString();
    if (op != QStringLiteral("start") && op != QStringLiteral("finish")
        && op != QStringLiteral("get"))
        return refuse("bad_args",
                      QStringLiteral("run_trace: op \"%1\" — expected start, finish or get")
                          .arg(op));

    RunTraceVerb::Paths paths;
    paths.root = RunTraceVerb::projectRootFor(cwd);
    QString warning, cfgError;
    paths.indexRel = RunTraceVerb::resolveIndex(paths.root, &warning, &cfgError);
    if (paths.indexRel.isEmpty())
        return refuse("bad_path", QStringLiteral("run_trace: %1").arg(cfgError));
    // The lexical check above cannot see a symlink out of the tree; this can.
    const PathValidation::Check chk = PathValidation::validatePath(
        paths.indexRel, paths.root, QStringLiteral("run_trace"), QStringLiteral("gate_log"));
    if (chk.bad)
        return QJsonDocument(chk.err);
    paths.stateDir = RunTraceVerb::stateDirFor(
        QDir(QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation))
            .filePath(QStringLiteral("ants-terminal/run-trace")),
        paths.root);

    const bool dryRun = req.value(QStringLiteral("dry_run")).toBool(false);
    QJsonObject out;
    if (op == QStringLiteral("start")) {
        out = RunTraceVerb::start(paths, req.value(QStringLiteral("kind")).toString(),
                                  req.value(QStringLiteral("subject")).toString(),
                                  QDateTime::currentDateTime(), dryRun,
                                  [] { return QRandomGenerator::global()->generate(); });
    } else if (op == QStringLiteral("get")) {
        out = RunTraceVerb::get(paths, req.value(QStringLiteral("id")).toString());
    } else {
        RunTraceVerb::FinishRequest fr;
        fr.id = req.value(QStringLiteral("id")).toString();
        fr.dryRun = dryRun;
        for (const char *k : {"lanes", "outcome", "detail", "cost"}) {
            const QString key = QLatin1String(k);
            if (req.contains(key))
                fr.args[key] = req.value(key);
        }
        // The transcript is NAMED by the caller, never inferred: two sessions
        // in one project otherwise read each other's usage.
        const QString transcript = req.value(QStringLiteral("transcript")).toString();
        const QString sessionId = req.value(QStringLiteral("session_id")).toString();
        if (!transcript.isEmpty() || !sessionId.isEmpty()) {
            // A transcript outside the harness's folders is refused.
            const QStringList dirs = SessionCost::projectsDirs();
            const QString path = !transcript.isEmpty()
                ? transcript
                : RunTraceVerb::transcriptForSession(dirs.constLast(), cwd, sessionId);
            if (path.isEmpty())
                return refuse("bad_args", QStringLiteral("run_trace: session_id \"%1\" is "
                                                         "not a session id").arg(sessionId));
            if (!RunTraceVerb::transcriptAllowed(path, dirs))
                return refuse("bad_path",
                              QStringLiteral("run_trace: transcript \"%1\" is not a .jsonl "
                                             "file under %2")
                                  .arg(path, dirs.join(QStringLiteral(" or "))));
            fr.transcriptPath = path;
        }
        out = RunTraceVerb::finish(paths, fr);
    }
    if (!warning.isEmpty() && out.value(QStringLiteral("ok")).toBool())
        out[QStringLiteral("config_warning")] = warning;
    return QJsonDocument(out);
}
