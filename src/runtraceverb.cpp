// ANTS-5299 — the run_trace seam. Contract: tests/features/run_trace/spec.md.
//
// Three ops. `start` mints the id and parks a pending record OUTSIDE the
// project, so a run in flight dirties nothing git can see. `finish` writes the
// detail file, the optional cost row and the index row, in that order: the row
// is what the draft gate-record hook would check, so it lands last and a
// failure before it leaves no row claiming a run that recorded nothing. `get`
// reads either state.
//
// `gate-record` and `draft/v2` below name the claude-config v2 workflow draft,
// deleted from ~/.claude on 2026-09-25 and still readable at its commit 1b5847a
// (`git -C ~/.claude show 1b5847a:draft/v2/hooks/gate-record`). The hook was
// never installed to ~/.claude/hooks, and this repo's tools/hooks run no
// commit-msg hook, so nothing checks the index today (CFG-0596). Each mention
// records a rule this verb shares with that draft, not a check that runs.
//
// Refusal codes: docs/standards/mcp-error-codes.md (`already_recorded` and
// `coverage_required` were minted here).

#include "runtraceverb.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

#include <optional>

namespace RunTraceVerb {
namespace {

const char kDefaultIndex[] = "docs/reviews/gate-log.md";
const char kCostTsv[] = "docs/reviews/run-costs.tsv";
// The v2 workflow's design.md § Names this workflow fixes.
const char kKinds[] = "GCVEFRAL";

// The run-costs.tsv columns, identical to draft/v2/tools/run-cost.py's
// COLUMNS (~/.claude commit 1b5847a) so a table either tool wrote is readable
// by the other.
const QStringList &costColumns() {
    static const QStringList c{
        QStringLiteral("date"), QStringLiteral("arm"), QStringLiteral("subject"),
        QStringLiteral("kind"), QStringLiteral("loop"), QStringLiteral("agents"),
        QStringLiteral("new_tokens"), QStringLiteral("cache_reads"),
        QStringLiteral("verified"), QStringLiteral("dismissed"),
        QStringLiteral("executed"), QStringLiteral("outcome")};
    return c;
}

QJsonObject refuse(const char *code, const QString &msg) {
    return QJsonObject{{QStringLiteral("ok"), false},
                       {QStringLiteral("code"), QString::fromLatin1(code)},
                       {QStringLiteral("error"), QStringLiteral("run_trace: ") + msg}};
}

QString headerRow() {
    return QStringLiteral("| Run | Date | Subject | Lanes | Outcome |");
}

QString squash(const QString &line) {
    QString s = line;
    s.remove(QLatin1Char(' '));
    s.remove(QLatin1Char('\t'));
    return s;
}

bool isHeader(const QString &line) {
    return squash(line) == squash(headerRow());
}

bool isTableLine(const QString &line) {
    return line.trimmed().startsWith(QLatin1Char('|'));
}

// A row's cells, split on unescaped pipes, trimmed, escapes kept.
QStringList cells(const QString &line) {
    QStringList out;
    QString cur;
    const QString t = line.trimmed();
    for (int i = 0; i < t.size(); ++i) {
        const QChar c = t.at(i);
        if (c == QLatin1Char('\\') && i + 1 < t.size() && t.at(i + 1) == QLatin1Char('|')) {
            cur += QStringLiteral("\\|");
            ++i;
        } else if (c == QLatin1Char('|')) {
            out << cur.trimmed();
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!out.isEmpty())
        out.removeFirst();          // before the leading pipe
    return out;
}

QString unescape(const QString &cell) {
    QString s = cell;
    return s.replace(QStringLiteral("\\|"), QStringLiteral("|"));
}

QString escapeCell(const QString &s) {
    QString out = s.trimmed();
    return out.replace(QLatin1Char('|'), QStringLiteral("\\|"));
}

bool hasNewline(const QString &s) {
    return s.contains(QLatin1Char('\n')) || s.contains(QLatin1Char('\r'));
}

QString readText(const QString &path, bool *ok = nullptr) {
    QFile f(path);
    const bool opened = f.open(QIODevice::ReadOnly);
    if (ok)
        *ok = opened;
    return opened ? QString::fromUtf8(f.readAll()) : QString();
}

bool writeText(const QString &path, const QString &text, QString *err) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        *err = QStringLiteral("cannot create %1").arg(QFileInfo(path).absolutePath());
        return false;
    }
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        *err = QStringLiteral("cannot open %1: %2").arg(path, f.errorString());
        return false;
    }
    f.write(text.toUtf8());
    if (!f.commit()) {
        *err = QStringLiteral("cannot write %1: %2").arg(path, f.errorString());
        return false;
    }
    return true;
}

QString indexPath(const Paths &p) {
    return QDir(p.root).filePath(p.indexRel);
}

// A run's detail goes BESIDE the index (documents.md § The header).
QString detailPath(const Paths &p, const QString &id) {
    return QFileInfo(indexPath(p)).absoluteDir().filePath(id + QStringLiteral(".md"));
}

QString pendingPath(const Paths &p, const QString &id) {
    return QDir(p.stateDir).filePath(id + QStringLiteral(".json"));
}

bool validId(const QString &id) {
    static const QRegularExpression re(QStringLiteral("^[GCVEFRAL]-\\d{8}-[0-9a-f]{4}$"));
    return re.match(id).hasMatch();
}

// The index as lines, and where its fixed header sits. headerAt is -1 when the
// file is absent (`exists` false) or carries no such header.
struct Index {
    bool exists = false;
    QStringList lines;
    int headerAt = -1;
    int lastRow = -1;               // the last table line after the header
};

Index readIndex(const Paths &p) {
    Index ix;
    const QString path = indexPath(p);
    if (!QFileInfo::exists(path))
        return ix;
    ix.exists = true;
    ix.lines = readText(path).split(QLatin1Char('\n'));
    for (int i = 0; i < ix.lines.size(); ++i) {
        if (isHeader(ix.lines.at(i))) {
            ix.headerAt = i;
            break;
        }
    }
    if (ix.headerAt >= 0) {
        ix.lastRow = ix.headerAt;
        for (int i = ix.headerAt + 1; i < ix.lines.size() && isTableLine(ix.lines.at(i)); ++i)
            ix.lastRow = i;
    }
    return ix;
}

// The row whose FIRST field is `id` — the same test gate-record's
// run_recorded() applies, so the two agree on what "recorded" means.
int rowFor(const Index &ix, const QString &id) {
    if (ix.headerAt < 0)
        return -1;
    for (int i = ix.headerAt + 2; i <= ix.lastRow; ++i) {
        const QStringList c = cells(ix.lines.at(i));
        if (!c.isEmpty() && QString(c.first()).remove(QLatin1Char('`')) == id)
            return i;
    }
    return -1;
}

QString firstLines(const QString &text, int n) {
    return text.section(QLatin1Char('\n'), 0, n - 1);
}

bool declaresGenre(const QString &text) {
    static const QRegularExpression re(
        QStringLiteral("^[ \\t]*(?:[-*][ \\t]+)?\\**Genre:?\\**[ \\t]*`?[A-Za-z]+`?[ \\t]*$"),
        QRegularExpression::MultilineOption);
    return re.match(firstLines(text, 20)).hasMatch();
}

// An integer JSON value, or nullopt. QJsonValue carries every number as a
// double, so "is it an integer" is a question about the double.
std::optional<qint64> asInt(const QJsonValue &v) {
    if (!v.isDouble())
        return std::nullopt;
    const double d = v.toDouble();
    if (d != static_cast<double>(static_cast<qint64>(d)))
        return std::nullopt;
    return static_cast<qint64>(d);
}

struct Usage {
    qint64 newTokens = 0;
    qint64 cacheReads = 0;
    int messages = 0;
    int duplicates = 0;
};

// Summed over assistant lines stamped at or after `since`. The harness writes
// one line per content block, each carrying the WHOLE message's usage, so a
// message is counted once by its id — summing lines counted it two to four
// times (measured 2026-09-24 on a live transcript: 53 lines, 27 messages).
bool sumUsage(const QString &path, const QDateTime &since, Usage *u, QString *err) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        *err = QStringLiteral("cannot read transcript %1").arg(path);
        return false;
    }
    QSet<QString> seen;
    while (!f.atEnd()) {
        const QByteArray line = f.readLine();
        const QJsonObject e = QJsonDocument::fromJson(line).object();
        if (e.value(QStringLiteral("type")).toString() != QStringLiteral("assistant"))
            continue;
        const QDateTime ts = QDateTime::fromString(
            e.value(QStringLiteral("timestamp")).toString(), Qt::ISODateWithMs);
        if (!ts.isValid() || ts < since)
            continue;
        const QJsonObject m = e.value(QStringLiteral("message")).toObject();
        const QString mid = m.value(QStringLiteral("id")).toString();
        if (!mid.isEmpty()) {
            if (seen.contains(mid)) {
                ++u->duplicates;
                continue;
            }
            seen.insert(mid);
        }
        const QJsonObject us = m.value(QStringLiteral("usage")).toObject();
        const auto n = [&](const char *k) {
            return static_cast<qint64>(us.value(QLatin1String(k)).toDouble());
        };
        u->newTokens += n("input_tokens") + n("cache_creation_input_tokens")
                        + n("output_tokens");
        u->cacheReads += n("cache_read_input_tokens");
        ++u->messages;
    }
    return true;
}

}  // namespace

QString projectRootFor(const QString &cwd) {
    QDir d(cwd);
    do {
        if (QFileInfo::exists(d.filePath(QStringLiteral(".git"))))
            return d.absolutePath();
    } while (d.cdUp());
    return cwd;
}

QString resolveIndex(const QString &root, QString *warning, QString *error) {
    QString fallback = QString::fromLatin1(kDefaultIndex);
    const QString cfgPath = QDir(root).filePath(QStringLiteral(".claude/workflow.json"));
    if (!QFileInfo::exists(cfgPath))
        return fallback;
    const QJsonDocument doc = QJsonDocument::fromJson(readText(cfgPath).toUtf8());
    if (!doc.isObject()) {
        // gate-record reads the default on the same failure, so the verb and
        // the hook still look in one place.
        if (warning)
            *warning = QStringLiteral(".claude/workflow.json is not a JSON object; "
                                      "using %1").arg(fallback);
        return fallback;
    }
    const QString rel = doc.object().value(QStringLiteral("gate_log")).toString();
    if (rel.isEmpty())
        return fallback;
    QString clean = QDir::cleanPath(rel);
    if (QDir::isAbsolutePath(clean) || clean == QStringLiteral("..")
        || clean.startsWith(QStringLiteral("../"))) {
        if (error)
            *error = QStringLiteral("gate_log \"%1\" escapes the project root").arg(rel);
        return {};
    }
    return clean;
}

QString stateDirFor(const QString &cacheBase, const QString &root) {
    const QByteArray h = QCryptographicHash::hash(root.toUtf8(),
                                                  QCryptographicHash::Sha256).toHex();
    return QDir(cacheBase).filePath(QString::fromLatin1(h.left(16)));
}

QJsonObject start(const Paths &p, const QString &kind, const QString &subject,
                  const QDateTime &now, bool dryRun,
                  const std::function<quint32()> &rng) {
    if (kind.size() != 1 || !QByteArray(kKinds).contains(kind.at(0).toLatin1()))
        return refuse("bad_args", QStringLiteral("kind \"%1\" is not one of G C V E F R "
                                                 "A L").arg(kind));
    if (subject.trimmed().isEmpty() || hasNewline(subject))
        return refuse("bad_args", QStringLiteral("subject must be one non-empty line"));

    const QString date = now.toLocalTime().date().toString(QStringLiteral("yyyyMMdd"));
    const Index ix = readIndex(p);
    QString id;
    // A collision is one in 65,536 per id per day; sixteen draws make a
    // second one vanishingly unlikely, and a refusal is honest if it happens.
    for (int attempt = 0; attempt < 16 && id.isEmpty(); ++attempt) {
        const QString cand = QStringLiteral("%1-%2-%3")
                                 .arg(kind, date)
                                 .arg(rng() & 0xffffu, 4, 16, QLatin1Char('0'));
        if (rowFor(ix, cand) < 0 && !QFileInfo::exists(pendingPath(p, cand))
            && !QFileInfo::exists(detailPath(p, cand)))
            id = cand;
    }
    if (id.isEmpty())
        return refuse("io_error", QStringLiteral("could not mint an unused id"));

    const QString startedAt = now.toUTC().toString(Qt::ISODateWithMs);
    const QJsonObject pending{{QStringLiteral("id"), id},
                              {QStringLiteral("kind"), kind},
                              {QStringLiteral("subject"), subject.trimmed()},
                              {QStringLiteral("started_at"), startedAt},
                              {QStringLiteral("root"), p.root}};
    if (!dryRun) {
        QString err;
        if (!writeText(pendingPath(p, id),
                       QString::fromUtf8(QJsonDocument(pending).toJson()), &err))
            return refuse("write_failed", err);
    }
    QJsonObject out{{QStringLiteral("ok"), true},
                    {QStringLiteral("id"), id},
                    {QStringLiteral("started_at"), startedAt},
                    {QStringLiteral("index_path"), p.indexRel},
                    {QStringLiteral("detail_path"),
                     QDir(p.root).relativeFilePath(detailPath(p, id))},
                    // The verb cannot set the caller's shell; the agent-cost
                    // hook keys lane rows on this variable.
                    {QStringLiteral("env"),
                     QJsonObject{{QStringLiteral("CLAUDE_RUN_ID"), id}}}};
    if (dryRun)
        out[QStringLiteral("dry_run")] = true;
    return out;
}

QJsonObject finish(const Paths &p, const FinishRequest &req) {
    const QJsonObject &a = req.args;
    if (!validId(req.id))
        return refuse("bad_args", QStringLiteral("id \"%1\" is not <K>-<YYYYMMDD>-<4 hex>")
                                      .arg(req.id));

    Index ix = readIndex(p);
    if (ix.exists && ix.headerAt < 0) {
        QJsonObject r = refuse("format_mismatch",
            QStringLiteral("%1 carries no \"%2\" header; nothing was written")
                .arg(p.indexRel, headerRow()));
        r[QStringLiteral("format")] = QStringLiteral("unrecognised trace index");
        r[QStringLiteral("path")] = p.indexRel;
        return r;
    }
    if (rowFor(ix, req.id) >= 0)
        return refuse("already_recorded",
                      QStringLiteral("%1 already has a row in %2").arg(req.id, p.indexRel));

    bool ok = false;
    const QJsonObject pending =
        QJsonDocument::fromJson(readText(pendingPath(p, req.id), &ok).toUtf8()).object();
    if (!ok || pending.value(QStringLiteral("id")).toString() != req.id)
        return refuse("not_found",
                      QStringLiteral("no started run %1; call op:\"start\" first").arg(req.id));

    // `Lanes` is required, never defaulted: zero is what tells a real gate
    // from a self-read, so a blank must not be able to stand in for it.
    if (!a.contains(QStringLiteral("lanes")))
        return refuse("missing_field", QStringLiteral("lanes is required; write 0 for a run "
                                                      "that dispatched nobody"));
    const auto lanes = asInt(a.value(QStringLiteral("lanes")));
    if (!lanes || *lanes < 0)
        return refuse("bad_args", QStringLiteral("lanes must be a non-negative integer"));

    if (!a.contains(QStringLiteral("outcome")))
        return refuse("missing_field", QStringLiteral("outcome is required"));
    const QString outcome = a.value(QStringLiteral("outcome")).toString();
    if (outcome.trimmed().isEmpty() || hasNewline(outcome))
        return refuse("bad_args", QStringLiteral("outcome must be one non-empty line"));

    const QString subject = pending.value(QStringLiteral("subject")).toString();
    const QDateTime startedAt = QDateTime::fromString(
        pending.value(QStringLiteral("started_at")).toString(), Qt::ISODateWithMs);
    const QString idDate = req.id.section(QLatin1Char('-'), 1, 1);
    const QString date = QDate::fromString(idDate, QStringLiteral("yyyyMMdd"))
                             .toString(Qt::ISODate);

    // --- detail
    const QString dPath = detailPath(p, req.id);
    QString detailText;
    bool detailHeaderAdded = false;
    const bool haveDetail = a.contains(QStringLiteral("detail"));
    if (haveDetail) {
        detailText = a.value(QStringLiteral("detail")).toString();
        if (!declaresGenre(detailText)) {
            detailText = QStringLiteral("Genre: record\nStatus: active\n\n") + detailText;
            detailHeaderAdded = true;
        }
        if (!detailText.endsWith(QLatin1Char('\n')))
            detailText += QLatin1Char('\n');
    } else if (!QFileInfo::exists(dPath)) {
        return refuse("missing_field",
                      QStringLiteral("no detail given and %1 does not exist; every row has "
                                     "its detail file beside it")
                          .arg(QDir(p.root).relativeFilePath(dPath)));
    }

    // --- cost (ANTS-5298's record half)
    QString costLine, costPath;
    QString costFileText;
    QJsonObject costOut;
    if (a.contains(QStringLiteral("cost"))) {
        const QJsonObject c = a.value(QStringLiteral("cost")).toObject();
        if (!c.contains(QStringLiteral("verified")))
            return refuse("coverage_required",
                          QStringLiteral("cost.verified is required: a cost with no coverage "
                                         "figure cannot tell a cheaper review from a "
                                         "weaker one"));
        const auto verified = asInt(c.value(QStringLiteral("verified")));
        const auto dismissed = c.contains(QStringLiteral("dismissed"))
                                   ? asInt(c.value(QStringLiteral("dismissed")))
                                   : std::optional<qint64>(0);
        const auto loop = asInt(c.value(QStringLiteral("loop")));
        if (!verified || *verified < 0 || !dismissed || *dismissed < 0 || !loop || *loop < 1)
            return refuse("bad_args", QStringLiteral("cost.verified and cost.dismissed must be "
                                                     "integers >= 0, cost.loop >= 1"));
        if (*lanes < 1)
            return refuse("bad_args", QStringLiteral("a cost row needs lanes >= 1; a run that "
                                                     "dispatched nobody records no cost"));
        const QString executed = c.value(QStringLiteral("executed")).toString();
        const QString costOutcome = c.value(QStringLiteral("outcome")).toString(
            QStringLiteral("open"));
        const QString arm = c.value(QStringLiteral("arm")).toString();
        const QString costKind = c.value(QStringLiteral("kind")).toString();
        static const QStringList kExecuted{QStringLiteral("yes"), QStringLiteral("no"),
                                           QStringLiteral("partial")};
        static const QStringList kOutcomes{QStringLiteral("complete"),
                                           QStringLiteral("stopped-early"),
                                           QStringLiteral("open")};
        static const QStringList kArms{QStringLiteral("live"), QStringLiteral("draft")};
        if (!kExecuted.contains(executed) || !kOutcomes.contains(costOutcome)
            || !kArms.contains(arm) || costKind.isEmpty() || hasNewline(costKind)
            || costKind.contains(QLatin1Char('\t')))
            return refuse("bad_args",
                          QStringLiteral("cost needs kind, executed (yes|no|partial), arm "
                                         "(live|draft); outcome is complete|stopped-early|"
                                         "open"));
        if (req.transcriptPath.isEmpty())
            return refuse("missing_field", QStringLiteral("a cost row needs `transcript` or "
                                                          "`session_id`"));
        if (!startedAt.isValid())
            return refuse("bad_args", QStringLiteral("the pending record has no valid "
                                                     "started_at"));

        costPath = QDir(p.root).filePath(QString::fromLatin1(kCostTsv));
        const QString header = costColumns().join(QLatin1Char('\t'));
        if (QFileInfo::exists(costPath)) {
            costFileText = readText(costPath);
            const QString first = costFileText.section(QLatin1Char('\n'), 0, 0);
            if (!first.isEmpty() && first != header) {
                QJsonObject r = refuse("format_mismatch",
                    QStringLiteral("%1 has columns \"%2\"; this writes \"%3\". Migrate the "
                                   "rows first; nothing was written")
                        .arg(QString::fromLatin1(kCostTsv), first, header));
                r[QStringLiteral("format")] = QStringLiteral("run-costs.tsv");
                r[QStringLiteral("path")] = QString::fromLatin1(kCostTsv);
                return r;
            }
        }
        Usage u;
        QString err;
        if (!sumUsage(req.transcriptPath, startedAt, &u, &err))
            return refuse("read_failed", err);
        if (u.newTokens == 0)
            return refuse("bad_args",
                          QStringLiteral("the transcript shows no usage since %1; wrong "
                                         "transcript?").arg(startedAt.toString(Qt::ISODate)));
        const QStringList row{date, arm, subject, costKind, QString::number(*loop),
                              QString::number(*lanes), QString::number(u.newTokens),
                              QString::number(u.cacheReads), QString::number(*verified),
                              QString::number(*dismissed), executed, costOutcome};
        costLine = row.join(QLatin1Char('\t'));
        if (costFileText.isEmpty())
            costFileText = header + QLatin1Char('\n');
        else if (!costFileText.endsWith(QLatin1Char('\n')))
            costFileText += QLatin1Char('\n');
        costFileText += costLine + QLatin1Char('\n');
        costOut = QJsonObject{{QStringLiteral("path"), QString::fromLatin1(kCostTsv)},
                              {QStringLiteral("row"), costLine},
                              {QStringLiteral("new_tokens"), double(u.newTokens)},
                              {QStringLiteral("cache_reads"), double(u.cacheReads)},
                              {QStringLiteral("messages_counted"), u.messages},
                              {QStringLiteral("duplicate_lines_skipped"), u.duplicates}};
    }

    // --- index row
    const QString row = QStringLiteral("| %1 | %2 | %3 | %4 | %5 |")
                            .arg(req.id, date, escapeCell(subject),
                                 QString::number(*lanes), escapeCell(outcome));
    int rowLine;
    if (!ix.exists) {
        ix.lines = QStringList{QStringLiteral("Genre: record"), QStringLiteral("Status: active"),
                               QString(), QStringLiteral("# Trace index"), QString(),
                               headerRow(), QStringLiteral("|---|---|---|---|---|"), row,
                               QString()};
        rowLine = 7;
    } else {
        rowLine = ix.lastRow + 1;
        ix.lines.insert(rowLine, row);
    }
    QString indexText = ix.lines.join(QLatin1Char('\n'));
    if (!indexText.endsWith(QLatin1Char('\n')))
        indexText += QLatin1Char('\n');

    QJsonArray written;
    if (!req.dryRun) {
        QString err;
        if (haveDetail) {
            if (!writeText(dPath, detailText, &err))
                return refuse("write_failed", err);
            written.append(QDir(p.root).relativeFilePath(dPath));
        }
        if (!costLine.isEmpty()) {
            if (!writeText(costPath, costFileText, &err))
                return refuse("write_failed", err);
            written.append(QString::fromLatin1(kCostTsv));
        }
        if (!writeText(indexPath(p), indexText, &err))
            return refuse("write_failed", err);
        written.append(p.indexRel);
        QFile::remove(pendingPath(p, req.id));
    }

    QJsonObject out{{QStringLiteral("ok"), true},
                    {QStringLiteral("id"), req.id},
                    {QStringLiteral("row"), row},
                    {QStringLiteral("index_path"), p.indexRel},
                    {QStringLiteral("index_line"), rowLine + 1},
                    {QStringLiteral("detail_path"), QDir(p.root).relativeFilePath(dPath)},
                    {QStringLiteral("detail_written"), haveDetail && !req.dryRun},
                    {QStringLiteral("detail_header_added"), detailHeaderAdded},
                    {QStringLiteral("files_written"), written}};
    if (!costOut.isEmpty())
        out[QStringLiteral("cost_row")] = costOut;
    if (req.dryRun)
        out[QStringLiteral("dry_run")] = true;
    return out;
}

QJsonObject get(const Paths &p, const QString &id) {
    if (!validId(id))
        return refuse("bad_args", QStringLiteral("id \"%1\" is not <K>-<YYYYMMDD>-<4 hex>")
                                      .arg(id));
    const Index ix = readIndex(p);
    const QString dPath = detailPath(p, id);
    const int at = rowFor(ix, id);
    if (at >= 0) {
        const QStringList c = cells(ix.lines.at(at));
        QJsonObject row;
        const char *names[] = {"run", "date", "subject", "lanes", "outcome"};
        for (int i = 0; i < 5; ++i)
            row[QLatin1String(names[i])] = unescape(c.value(i)).remove(QLatin1Char('`'));
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("found"), true},
                           {QStringLiteral("state"), QStringLiteral("recorded")},
                           {QStringLiteral("row"), row},
                           {QStringLiteral("index_line"), at + 1},
                           {QStringLiteral("detail_path"), QDir(p.root).relativeFilePath(dPath)},
                           {QStringLiteral("detail_exists"), QFileInfo::exists(dPath)}};
    }
    bool ok = false;
    const QJsonObject pending =
        QJsonDocument::fromJson(readText(pendingPath(p, id), &ok).toUtf8()).object();
    if (ok && pending.value(QStringLiteral("id")).toString() == id) {
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("found"), true},
                           {QStringLiteral("state"), QStringLiteral("pending")},
                           {QStringLiteral("subject"), pending.value(QStringLiteral("subject"))},
                           {QStringLiteral("started_at"),
                            pending.value(QStringLiteral("started_at"))}};
    }
    // Say what was looked at, so "not found" cannot be read as "not checked".
    return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("found"), false},
                       {QStringLiteral("index_path"), p.indexRel},
                       {QStringLiteral("index_exists"), ix.exists},
                       {QStringLiteral("rows_scanned"),
                        ix.headerAt < 0 ? 0 : qMax(0, ix.lastRow - ix.headerAt - 1)}};
}

QString transcriptForSession(const QString &projectsDir, const QString &cwd,
                             const QString &sessionId) {
    static const QRegularExpression sid(QStringLiteral("^[0-9A-Za-z-]{8,64}$"));
    if (!sid.match(sessionId).hasMatch() || cwd.isEmpty())
        return {};
    QString slug = cwd;
    for (QChar &c : slug)
        if (!c.isLetterOrNumber())
            c = QLatin1Char('-');
    return QDir(projectsDir).filePath(slug + QLatin1Char('/') + sessionId
                                      + QStringLiteral(".jsonl"));
}

bool transcriptAllowed(const QString &path, const QStringList &projectsDirs) {
    const QFileInfo fi(path);
    if (!fi.isFile() || fi.suffix() != QStringLiteral("jsonl"))
        return false;
    const QString canon = fi.canonicalFilePath();
    for (const QString &d : projectsDirs) {
        const QString base = QFileInfo(d).canonicalFilePath();
        if (!base.isEmpty() && canon.startsWith(base + QLatin1Char('/')))
            return true;
    }
    return false;
}

}  // namespace RunTraceVerb
