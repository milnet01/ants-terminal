// git_state op:"hooks" (ANTS-5505). See githooksreport.h.

#include "githooksreport.h"

#include "gitwrap.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>

namespace GitHooksReport {

namespace {

// The hook's --gate-candidates answers before it touches the repository, so
// anything slower than this is a hook that is not answering.
constexpr int kCandidatesTimeoutMs = 2000;

QJsonObject refuse(const char *code, const QString &message,
                   const QByteArray &stderrTail = {}) {
    QJsonObject o;
    o[QStringLiteral("ok")]    = false;
    o[QStringLiteral("code")]  = QString::fromLatin1(code);
    o[QStringLiteral("error")] = message;
    if (!stderrTail.isEmpty())
        o[QStringLiteral("stderr")] = QString::fromUtf8(stderrTail);
    return o;
}

// git's trimmed stdout, or a null QString when git exited non-zero — the
// answer `git config --get` gives for an unset key.
QString gitOut(const QString &root, const QStringList &argv) {
    const GitWrap::Result g = GitWrap::run(root, argv);
    if (!g.started || g.hardKilled || g.crashed || g.exitCode != 0) return {};
    return QString::fromUtf8(g.stdoutBytes).trimmed();
}

QString absUnder(const QString &root, const QString &path) {
    return QDir::cleanPath(QDir(root).absoluteFilePath(path));
}

QJsonObject fileFacts(const QString &path) {
    const QFileInfo fi(path);
    QJsonObject o;
    o[QStringLiteral("path")]       = path;
    o[QStringLiteral("exists")]     = fi.isFile();
    o[QStringLiteral("executable")] = fi.isFile() && fi.isExecutable();
    return o;
}

// The same test ci-gate's owns_hook applies: a hook that only delegates names
// the shared hook AND execs a variable with every argument. Naming it alone
// is not enough — a hook of its own may pipe to `--secrets-only` on it and
// still pick its own gate.
QString prePushKind(const QString &prePush, const QString &machinePrePush) {
    const QFileInfo fi(prePush);
    if (!fi.isFile()) return QStringLiteral("none");
    const QString mine = QFileInfo(machinePrePush).canonicalFilePath();
    if (!mine.isEmpty() && fi.canonicalFilePath() == mine)
        return QStringLiteral("machine_wide");
    QFile f(prePush);
    if (!f.open(QIODevice::ReadOnly)) return QStringLiteral("own");
    const QString text = QString::fromUtf8(f.readAll());
    static const QRegularExpression execAll(
        QStringLiteral(R"(^[ \t]*exec[ \t]+"?\$[A-Za-z_{].*"\$@")"),
        QRegularExpression::MultilineOption);
    const bool namesShared = text.contains(QStringLiteral("claude/githooks"))
                          || text.contains(QStringLiteral("ANTS_GLOBAL_HOOKS"));
    return namesShared && execAll.match(text).hasMatch()
               ? QStringLiteral("hands_off") : QStringLiteral("own");
}

// `git config --get-regexp` prints one "key value" line per entry, lowest
// scope first, so a later line for the same key is the one git answers with.
void readKeys(const QString &root, const QStringList &argv,
              const QString &source, QJsonObject &keys) {
    const QString out = gitOut(root, argv);
    for (const QString &line : out.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const int sp = line.indexOf(QLatin1Char(' '));
        const QString key = sp < 0 ? line : line.left(sp);
        QJsonObject v;
        v[QStringLiteral("value")]  = sp < 0 ? QString() : line.mid(sp + 1);
        v[QStringLiteral("source")] = source;
        keys[key] = v;
    }
}

// The machine-wide hook's own discovery list, or an empty list when it
// cannot be asked.
QStringList askCandidates(const QString &machinePrePush) {
    if (!QFileInfo(machinePrePush).isExecutable()) return {};
    QProcess p;
    p.start(machinePrePush, {QStringLiteral("--gate-candidates")});
    if (!p.waitForFinished(kCandidatesTimeoutMs)) {
        p.kill();
        p.waitForFinished(200);
        return {};
    }
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) return {};
    return QString::fromUtf8(p.readAllStandardOutput())
        .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

// Resolved the way the hook resolves it: ants.gate.command first, re-anchored
// inside the repository when absolute; otherwise the first executable
// candidate on the hook's list.
QJsonObject resolveGate(const QString &root, const QJsonObject &keys,
                        const QString &machinePrePush) {
    QJsonObject gate;
    QString rel;
    const QString configured = keys.value(QStringLiteral("ants.gate.command"))
                                   .toObject().value(QStringLiteral("value")).toString();
    if (!configured.isEmpty()) {
        gate[QStringLiteral("source")] = QStringLiteral("config");
        rel = configured;
        if (QDir::isAbsolutePath(configured)) {
            const QString clean = QDir::cleanPath(configured);
            if (clean.startsWith(root + QLatin1Char('/'))) {
                rel = clean.mid(root.size() + 1);
            } else {
                gate[QStringLiteral("outside_repo")] = true;
                rel.clear();
            }
        }
        if (rel.startsWith(QStringLiteral("./"))) rel = rel.mid(2);
        if (rel.isEmpty()) {
            gate[QStringLiteral("path")] = configured;
            gate[QStringLiteral("exists")] = QFileInfo(configured).isFile();
            gate[QStringLiteral("executable")] = false;
            return gate;
        }
    } else {
        const QStringList candidates = askCandidates(machinePrePush);
        gate[QStringLiteral("candidates_source")] = candidates.isEmpty()
            ? QStringLiteral("unavailable") : QStringLiteral("machine_hook");
        for (const QString &c : candidates) {
            if (QFileInfo(absUnder(root, c)).isExecutable()) { rel = c; break; }
        }
        if (rel.isEmpty()) {
            gate[QStringLiteral("source")] = QJsonValue::Null;
            gate[QStringLiteral("path")]   = QJsonValue::Null;
            return gate;
        }
        gate[QStringLiteral("source")] = QStringLiteral("discovered");
    }
    const QJsonObject facts = fileFacts(absUnder(root, rel));
    gate[QStringLiteral("path")]       = rel;
    gate[QStringLiteral("exists")]     = facts.value(QStringLiteral("exists"));
    gate[QStringLiteral("executable")] = facts.value(QStringLiteral("executable"));
    return gate;
}

} // namespace

QString defaultMachineHooksDir() {
    const QString env = QProcessEnvironment::systemEnvironment()
                            .value(QStringLiteral("ANTS_GLOBAL_HOOKS"));
    return env.isEmpty() ? QDir::homePath() + QStringLiteral("/.claude/githooks") : env;
}

QJsonObject run(const QString &root, const QString &machineHooksDir) {
    const GitWrap::Result probe =
        GitWrap::run(root, {QStringLiteral("rev-parse"), QStringLiteral("--git-path"),
                            QStringLiteral("hooks")});
    if (!probe.started)
        return refuse("git_missing", QStringLiteral("git_state: git binary not found on PATH"));
    if (probe.exitCode != 0 || probe.hardKilled || probe.crashed) {
        if (QString::fromUtf8(probe.stderrTail)
                .contains(QStringLiteral("not a git repository"), Qt::CaseInsensitive))
            return refuse("not_git_repo", QStringLiteral("git_state: not a git repository"));
        return refuse("git_failed", QStringLiteral("git_state: git rev-parse failed"),
                      probe.stderrTail);
    }
    const QString hooksDir = absUnder(root, QString::fromUtf8(probe.stdoutBytes).trimmed());
    const QString machinePrePush = machineHooksDir + QStringLiteral("/pre-push");

    QJsonObject out;
    out[QStringLiteral("ok")] = true;
    out[QStringLiteral("op")] = QStringLiteral("hooks");

    // "<scope>\t<origin>\t<value>", or nothing when core.hooksPath is unset.
    QJsonObject hp;
    const QStringList scoped = gitOut(root, {QStringLiteral("config"),
        QStringLiteral("--show-scope"), QStringLiteral("--show-origin"),
        QStringLiteral("--get"), QStringLiteral("core.hooksPath")}).split(QLatin1Char('\t'));
    const bool set = scoped.size() >= 3;
    hp[QStringLiteral("value")]  = set ? QJsonValue(scoped.mid(2).join(QLatin1Char('\t'))) : QJsonValue::Null;
    hp[QStringLiteral("scope")]  = set ? QJsonValue(scoped.at(0)) : QJsonValue::Null;
    hp[QStringLiteral("origin")] = set ? QJsonValue(scoped.at(1)) : QJsonValue::Null;
    hp[QStringLiteral("dir")]    = hooksDir;
    out[QStringLiteral("hooks_path")] = hp;

    const QString prePush = hooksDir + QStringLiteral("/pre-push");
    QJsonObject pp = fileFacts(prePush);
    pp[QStringLiteral("kind")] = prePushKind(prePush, machinePrePush);
    out[QStringLiteral("pre_push")]     = pp;
    out[QStringLiteral("machine_hook")] = fileFacts(machinePrePush);

    // The hook's gate_cfg: the clone's git config, then .ants/gate.conf. So
    // read the file first and let git config overwrite it.
    QJsonObject keys;
    const QString keyRe = QStringLiteral(R"(^ants\.gate\.)");
    if (QFileInfo(absUnder(root, QStringLiteral(".ants/gate.conf"))).isFile())
        readKeys(root, {QStringLiteral("config"), QStringLiteral("-f"),
                        QStringLiteral(".ants/gate.conf"), QStringLiteral("--get-regexp"), keyRe},
                 QStringLiteral(".ants/gate.conf"), keys);
    readKeys(root, {QStringLiteral("config"), QStringLiteral("--get-regexp"), keyRe},
             QStringLiteral("git_config"), keys);
    out[QStringLiteral("gate_keys")] = keys;

    QJsonObject gate = resolveGate(root, keys, machinePrePush);
    const QDir wfDir(absUnder(root, QStringLiteral(".github/workflows")));
    const QStringList workflows = wfDir.entryList(
        {QStringLiteral("*.yml"), QStringLiteral("*.yaml")}, QDir::Files, QDir::Name);
    out[QStringLiteral("workflows")] = QJsonArray::fromStringList(workflows);
    // ci-gate's workflow_calls: a workflow naming the script's basename.
    const QString gatePath = gate.value(QStringLiteral("path")).toString();
    if (!gatePath.isEmpty()) {
        const QString base = QFileInfo(gatePath).fileName();
        QJsonArray calling;
        for (const QString &w : workflows) {
            QFile f(wfDir.filePath(w));
            if (f.open(QIODevice::ReadOnly) && QString::fromUtf8(f.readAll()).contains(base))
                calling.append(w);
        }
        gate[QStringLiteral("workflows_calling")] = calling;
        gate[QStringLiteral("called_by_ci")] = !calling.isEmpty();
    }
    out[QStringLiteral("gate")] = gate;

    // The hook keys its passed record on the pushed tip's tree; HEAD's tree is
    // the record a push of HEAD would find.
    QJsonObject passed;
    const QString tree = gitOut(root, {QStringLiteral("rev-parse"), QStringLiteral("HEAD^{tree}")});
    passed[QStringLiteral("tree")]   = tree.isEmpty() ? QJsonValue::Null : QJsonValue(tree);
    passed[QStringLiteral("passed")] = false;
    passed[QStringLiteral("record")] = QJsonValue::Null;
    const QString common = gitOut(root, {QStringLiteral("rev-parse"), QStringLiteral("--git-common-dir")});
    if (!tree.isEmpty() && !common.isEmpty()) {
        QFile rec(absUnder(root, common) + QStringLiteral("/ants-gate-passed/") + tree);
        if (rec.open(QIODevice::ReadOnly)) {
            passed[QStringLiteral("passed")] = true;
            passed[QStringLiteral("record")] = QString::fromUtf8(rec.readLine()).trimmed();
        }
    }
    out[QStringLiteral("gate_passed")] = passed;
    return out;
}

} // namespace GitHooksReport
