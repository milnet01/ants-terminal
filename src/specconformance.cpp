// ANTS-4108 — spec_conformance engine. See specconformance.h and
// docs/specs/ANTS-4108-spec-conformance-verb.md.
//
// Executes PATTERNS ONLY (spec § 2.2). No QProcess, no interpreter — INV-8
// scopes that claim to this TU, and it is the whole reason the verb is safe
// to point at a document.

#include "specconformance.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStringList>

namespace SpecConformance {
namespace {

// ---------------------------------------------------------------- helpers --

QJsonObject refusal(const char *code, const QString &message) {
    QJsonObject o;
    o[QStringLiteral("ok")]    = false;
    o[QStringLiteral("error")] = message;
    o[QStringLiteral("code")]  = QString::fromLatin1(code);
    return o;
}

void addRow(QJsonArray &a, const QJsonObject &o) { a.append(o); }

// A fence opens with >= 3 backticks at column 0. Returns the run length, or 0.
int fenceDelimLen(const QString &line) {
    int n = 0;
    while (n < line.size() && line.at(n) == QLatin1Char('`')) ++n;
    return (n >= 3) ? n : 0;
}

// Strip ONE layer of surrounding backticks. `` -> "" ; `a` -> "a".
QString stripTicks(const QString &raw) {
    QString s = raw.trimmed();
    if (s.size() >= 2 && s.startsWith(QLatin1Char('`')) &&
        s.endsWith(QLatin1Char('`'))) {
        return s.mid(1, s.size() - 2);
    }
    return s;
}

// Split a GFM table row into trimmed cells, dropping the leading/trailing pipe.
QStringList cellsOf(const QString &line) {
    QString s = line.trimmed();
    if (s.startsWith(QLatin1Char('|'))) s.remove(0, 1);
    if (s.endsWith(QLatin1Char('|'))) s.chop(1);
    QStringList out;
    for (const QString &c : s.split(QLatin1Char('|'))) out << c.trimmed();
    return out;
}

bool isSeparatorRow(const QString &line) {
    const QString s = line.trimmed();
    if (!s.startsWith(QLatin1Char('|'))) return false;
    for (const QChar c : s) {
        if (c != QLatin1Char('|') && c != QLatin1Char('-') &&
            c != QLatin1Char(':') && !c.isSpace())
            return false;
    }
    return s.contains(QLatin1Char('-'));
}

// The outcome a run produces, in the § 2.4 encoding: "no match",
// "no capture", or the captured/matched text (possibly empty).
QString outcomeOf(const QRegularExpression &rx, const QString &input) {
    const QRegularExpressionMatch m = rx.match(input);
    if (!m.hasMatch()) return QStringLiteral("no match");
    if (rx.captureCount() >= 1) {
        // hasCaptured, never a string compare: Qt returns a null QString for a
        // group that did not participate and an empty one for a zero-length
        // capture, and QString() == QString("") is TRUE.
        if (!m.hasCaptured(1)) return QStringLiteral("no capture");
        return m.captured(1);
    }
    return m.captured(0);
}

}  // namespace

// -------------------------------------------------------------------- run --

QJsonObject run(const QString &absPath, const Options &opt) {
    if (opt.maxCases < 1 || opt.maxCases > kMaxCasesCeiling) {
        return refusal("bad_args",
                       QStringLiteral("max_cases must be in [1, %1]; got %2")
                           .arg(kMaxCasesCeiling)
                           .arg(opt.maxCases));
    }

    QFile f(absPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return refusal("not_found",
                       QStringLiteral("cannot read %1").arg(absPath));
    }
    const QStringList lines =
        QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
    f.close();

    QJsonArray findings, candidates, refusals, observations;
    int casesRun = 0;
    bool truncated = false;

    for (int i = 0; i < lines.size(); ++i) {
        const int delim = fenceDelimLen(lines.at(i));
        if (delim == 0) continue;

        // ---- top-level fence: find its close, at the same delimiter length --
        const QString info = lines.at(i).mid(delim).trimmed();
        int close = -1;
        for (int j = i + 1; j < lines.size(); ++j) {
            if (fenceDelimLen(lines.at(j)) >= delim &&
                lines.at(j).trimmed().count(QLatin1Char('`')) ==
                    lines.at(j).trimmed().size()) {
                close = j;
                break;
            }
        }
        if (close < 0) break;   // unterminated fence — nothing further is safe

        // Everything between i and close is INSIDE this fence, so a nested
        // ```regex fence is skipped: the loop resumes past the close. This is
        // what stops the verb extracting cases from a spec's own illustration
        // of a case (INV-1).
        const int fenceLine = i + 1;   // 1-based
        const int resumeAt  = close;
        i = resumeAt;                  // for-loop's ++i moves past the close

        const QStringList infoWords =
            info.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (infoWords.isEmpty() ||
            infoWords.first() != QLatin1String("regex")) {
            continue;   // not ours — invisible, not a candidate (§ 2.6)
        }

        // ---- precedence: engine -> pattern cap -> table -> input cap -------
        if (infoWords.size() < 2) {
            QJsonObject c;
            c[QStringLiteral("kind")] = QStringLiteral("engine_not_declared");
            c[QStringLiteral("line")] = fenceLine;
            addRow(candidates, c);
            continue;
        }
        if (infoWords.at(1) != QLatin1String("pcre2")) {
            QJsonObject r;
            r[QStringLiteral("line")] = fenceLine;
            r[QStringLiteral("code")] = QStringLiteral("unsupported_engine");
            r[QStringLiteral("engine")] = infoWords.at(1);
            addRow(refusals, r);
            continue;
        }

        QStringList body;
        for (int j = fenceLine; j < close; ++j) body << lines.at(j);
        const QString pattern = body.join(QLatin1Char('\n')).trimmed();

        if (pattern.toUtf8().size() > kMaxPatternBytes) {
            QJsonObject r;
            r[QStringLiteral("line")] = fenceLine;
            r[QStringLiteral("code")] = QStringLiteral("too_large");
            addRow(refusals, r);
            continue;
        }

        // ---- the first input/expected table before the next fence/heading --
        int hdr = -1;
        for (int j = close + 1; j < lines.size(); ++j) {
            const QString t = lines.at(j).trimmed();
            if (t.isEmpty()) continue;
            if (fenceDelimLen(lines.at(j)) > 0 || t.startsWith(QLatin1Char('#')))
                break;
            if (t.startsWith(QLatin1Char('|'))) {
                const QStringList c = cellsOf(t);
                if (c.size() >= 2 && c.at(0).compare(QLatin1String("input"),
                                                     Qt::CaseInsensitive) == 0 &&
                    c.at(1).compare(QLatin1String("expected"),
                                    Qt::CaseInsensitive) == 0) {
                    hdr = j;
                }
                break;
            }
            break;   // any other prose ends the search
        }
        if (hdr < 0) {
            QJsonObject c;
            c[QStringLiteral("kind")] =
                QStringLiteral("pattern_without_expectation");
            c[QStringLiteral("line")]    = fenceLine;
            c[QStringLiteral("pattern")] = pattern;
            addRow(candidates, c);
            continue;
        }

        const QRegularExpression rx(pattern);
        int row = hdr + 1;
        if (row < lines.size() && isSeparatorRow(lines.at(row))) ++row;

        for (; row < lines.size(); ++row) {
            const QString t = lines.at(row).trimmed();
            if (!t.startsWith(QLatin1Char('|'))) break;
            const QStringList c = cellsOf(t);
            if (c.size() < 2) continue;
            const int rowLine = row + 1;   // 1-based

            if (casesRun >= opt.maxCases) { truncated = true; break; }

            const QString rawInput    = c.at(0);
            const QString rawExpected = c.at(1);
            if (rawExpected.isEmpty()) {
                QJsonObject cd;
                cd[QStringLiteral("kind")] =
                    QStringLiteral("malformed_expectation_cell");
                cd[QStringLiteral("line")] = rowLine;
                addRow(candidates, cd);
                continue;
            }

            const QString input = stripTicks(rawInput);
            if (input.toUtf8().size() > kMaxInputBytes) {
                QJsonObject r;
                r[QStringLiteral("line")] = rowLine;
                r[QStringLiteral("code")] = QStringLiteral("too_large");
                addRow(refusals, r);
                continue;
            }

            // Sentinels are recognised BEFORE backtick stripping, so
            // `no match` is the literal and no match is the sentinel.
            const QString expected =
                (rawExpected == QLatin1String("no match") ||
                 rawExpected == QLatin1String("no capture"))
                    ? rawExpected
                    : stripTicks(rawExpected);

            QElapsedTimer timer;
            timer.start();
            const QString actual = outcomeOf(rx, input);
            const qint64 micros = timer.nsecsElapsed() / 1000;
            ++casesRun;

            QJsonObject obs;
            obs[QStringLiteral("kind")]   = QStringLiteral("timing");
            obs[QStringLiteral("line")]   = rowLine;
            obs[QStringLiteral("micros")] = static_cast<double>(micros);
            addRow(observations, obs);

            if (actual != expected) {
                QJsonObject fd;
                fd[QStringLiteral("kind")]     = QStringLiteral("mismatch");
                fd[QStringLiteral("line")]     = rowLine;
                fd[QStringLiteral("pattern")]  = pattern;
                fd[QStringLiteral("engine")]   = QStringLiteral("pcre2");
                fd[QStringLiteral("input")]    = input;
                fd[QStringLiteral("expected")] = expected;
                fd[QStringLiteral("actual")]   = actual;
                addRow(findings, fd);
            }
        }
    }

    QJsonObject out;
    out[QStringLiteral("ok")]         = true;
    out[QStringLiteral("path")]       = absPath;
    out[QStringLiteral("cases_run")]  = casesRun;
    out[QStringLiteral("findings")]   = findings;
    out[QStringLiteral("candidates")] = candidates;
    out[QStringLiteral("refusals")]   = refusals;
    out[QStringLiteral("truncated")]  = truncated;

    // The etag is computed over the envelope MINUS observations: measured µs
    // differ per run, so hashing them would make every call a cache miss
    // (INV-9).
    const QByteArray canon = QJsonDocument(out).toJson(QJsonDocument::Compact);
    out[QStringLiteral("etag")] = QString::fromLatin1(
        QCryptographicHash::hash(canon, QCryptographicHash::Sha256)
            .toHex()
            .left(16));
    out[QStringLiteral("observations")] = observations;
    return out;
}

}  // namespace SpecConformance
