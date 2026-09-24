// ANTS-5299 — the testable seam behind the `run_trace` MCP verb.
// Contract: tests/features/run_trace/spec.md.
//
// A review run records itself in the project's trace index — one row
// `| Run | Date | Subject | Lanes | Outcome |`, a detail file beside it, and
// optionally a `run-costs.tsv` row (ANTS-5298's record half). The row format
// is the v2 workflow's standards/documents.md § The header.
//
// SEPARATE TRANSLATION UNIT from the handler (RemoteControl::cmdRunTrace in
// src/remotecontrol_run_trace.cpp), for roadmapmigrateverb.h's reason: a
// static archive links at object granularity, and test_core links
// ants_core_lib alone. The clock, the random source and the state directory
// are PARAMETERS so a test can pin all three.

#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <functional>

namespace RunTraceVerb {

// Where one call reads and writes. `root` is canonical. `indexRel` is the
// trace index relative to root, already resolved by resolveIndex().
// `stateDir` holds pending (started, not finished) runs, outside the
// project tree so a run in flight dirties nothing git can see.
struct Paths {
    QString root;
    QString indexRel;
    QString stateDir;
};

// The nearest ancestor of `cwd` holding a `.git` entry, else `cwd` itself —
// the root gate-record's `git rev-parse --show-toplevel` finds, so a session
// started in a subdirectory writes the index the hook reads.
QString projectRootFor(const QString &cwd);

// The `gate_log` key from <root>/.claude/workflow.json, else the default.
// Sets *warning when the file exists but cannot be read as a JSON object.
// Returns an empty string, with *error set, when the key escapes the root.
QString resolveIndex(const QString &root, QString *warning, QString *error);

// A pending run's directory for one project: <cacheBase>/<sha256(root)[:16]>.
QString stateDirFor(const QString &cacheBase, const QString &root);

QJsonObject start(const Paths &p, const QString &kind, const QString &subject,
                  const QDateTime &now, bool dryRun,
                  const std::function<quint32()> &rng);

struct FinishRequest {
    QString id;
    QJsonObject args;           // lanes, outcome, detail, cost — as sent
    QString transcriptPath;     // absolute; empty when no cost was asked for
    bool dryRun = false;
};
QJsonObject finish(const Paths &p, const FinishRequest &req);

QJsonObject get(const Paths &p, const QString &id);

// INV-13. The harness names a session's transcript after the session's cwd
// with every non-alphanumeric character replaced by '-'.
QString transcriptForSession(const QString &projectsDir, const QString &cwd,
                             const QString &sessionId);
// True when `path` is a .jsonl file under one of `projectsDirs`.
bool transcriptAllowed(const QString &path, const QStringList &projectsDirs);

}  // namespace RunTraceVerb
