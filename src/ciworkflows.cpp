// ci_report (ANTS-6399) — the workflow reader of spec § 4.4.

#include "ciworkflows.h"

#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QRegularExpression>
#include <QVariant>
#include <QVector>

#include <algorithm>
#include <climits>

namespace CiWorkflows {

namespace {

// The YAML subset a workflow file is written in: block maps, block lists,
// plain and quoted scalars, `|`/`>` block scalars. A flow collection
// (`[a, b]`) stays one string. Anything else marks the file unparseable.
class Parser {
public:
    explicit Parser(const QString &text) {
        for (QString raw : text.split(QLatin1Char('\n'))) {
            if (raw.endsWith(QLatin1Char('\r'))) raw.chop(1);
            Line l;
            l.raw = raw;
            while (l.indent < raw.size() && raw.at(l.indent) == QLatin1Char(' ')) ++l.indent;
            l.text = raw.mid(l.indent).trimmed();
            if (l.indent < raw.size() && raw.at(l.indent) == QLatin1Char('\t')) m_ok = false;
            l.skip = l.text.isEmpty() || l.text.startsWith(QLatin1Char('#'))
                  || l.text == QLatin1String("---") || l.text == QLatin1String("...");
            m_l.append(l);
        }
    }

    QVariant parse() {
        const int i = next(0);
        if (i >= m_l.size()) return QVariantMap();
        QVariant v = node(m_l[i].indent);
        if (next(m_i) < m_l.size()) m_ok = false;  // a line no rule consumed
        return v;
    }
    bool ok() const { return m_ok; }

private:
    struct Line { QString raw; QString text; int indent = 0; bool skip = false; };

    int next(int from) const {
        while (from < m_l.size() && m_l[from].skip) ++from;
        return from;
    }
    static bool isDash(const QString &t) {
        return t == QLatin1String("-") || t.startsWith(QLatin1String("- "));
    }
    static bool splitKey(const QString &t, QString *key, QString *rest) {
        static const QRegularExpression re(QStringLiteral(
            R"re(^("([^"]*)"|'([^']*)'|[^\s:#"'][^:#]*?)\s*:(?:\s+(.*))?$)re"));
        const auto m = re.match(t);
        if (!m.hasMatch()) return false;
        *key = !m.captured(2).isEmpty() ? m.captured(2)
             : !m.captured(3).isEmpty() ? m.captured(3) : m.captured(1);
        *rest = m.captured(4).trimmed();
        return true;
    }
    static QString scalar(const QString &rest) {
        if (rest.startsWith(QLatin1Char('"')) || rest.startsWith(QLatin1Char('\''))) {
            const QChar q = rest.at(0);
            const int end = rest.indexOf(q, 1);
            return end > 0 ? rest.mid(1, end - 1) : rest.mid(1);
        }
        const int hash = rest.indexOf(QLatin1String(" #"));
        return (hash >= 0 ? rest.left(hash) : rest).trimmed();
    }

    QVariant node(int indent) {
        const int i = next(m_i);
        if (i >= m_l.size()) return QString();
        QString k, r;
        if (isDash(m_l[i].text)) return list(indent);
        if (splitKey(m_l[i].text, &k, &r)) return map(indent);
        m_i = i + 1;
        return scalar(m_l[i].text);
    }

    QVariant map(int indent) {
        QVariantMap m;
        for (;;) {
            const int i = next(m_i);
            if (i >= m_l.size() || m_l[i].indent < indent) break;
            if (m_l[i].indent > indent) { m_ok = false; m_i = m_l.size(); break; }
            if (isDash(m_l[i].text)) break;
            QString key, rest;
            if (!splitKey(m_l[i].text, &key, &rest)) { m_ok = false; m_i = m_l.size(); break; }
            m_i = i + 1;
            if (rest.isEmpty() || rest.startsWith(QLatin1Char('#'))) {
                const int j = next(m_i);
                if (j < m_l.size() && m_l[j].indent > indent)
                    m.insert(key, node(m_l[j].indent));
                else if (j < m_l.size() && m_l[j].indent == indent && isDash(m_l[j].text))
                    m.insert(key, list(indent));
                else
                    m.insert(key, QString());
            } else if (rest.startsWith(QLatin1Char('|')) || rest.startsWith(QLatin1Char('>'))) {
                m.insert(key, block(indent));
            } else {
                m.insert(key, scalar(rest));
            }
        }
        return m;
    }

    QVariant list(int indent) {
        QVariantList out;
        for (;;) {
            const int i = next(m_i);
            if (i >= m_l.size() || m_l[i].indent != indent || !isDash(m_l[i].text)) break;
            const QString after = m_l[i].text.mid(1);
            const QString item = after.trimmed();
            if (item.isEmpty()) {
                m_i = i + 1;
                const int j = next(m_i);
                out.append(j < m_l.size() && m_l[j].indent > indent ? node(m_l[j].indent)
                                                                    : QVariant(QString()));
                continue;
            }
            int spaces = 0;
            while (spaces < after.size() && after.at(spaces) == QLatin1Char(' ')) ++spaces;
            // The item's own keys sit at the column its first key starts in.
            m_l[i].indent = indent + 1 + spaces;
            m_l[i].text = item;
            m_i = i;
            out.append(node(m_l[i].indent));
        }
        return out;
    }

    QString block(int parentIndent) {
        QStringList lines;
        int minIndent = INT_MAX;
        while (m_i < m_l.size()) {
            const Line &l = m_l[m_i];
            const bool blank = l.raw.trimmed().isEmpty();
            if (!blank && l.indent <= parentIndent) break;
            if (!blank) minIndent = std::min(minIndent, l.indent);
            lines.append(l.raw);
            ++m_i;
        }
        while (!lines.isEmpty() && lines.last().trimmed().isEmpty()) lines.removeLast();
        for (QString &s : lines) s = s.mid(std::min<int>(minIndent, s.size()));
        return lines.join(QLatin1Char('\n'));
    }

    QVector<Line> m_l;
    int m_i = 0;
    bool m_ok = true;
};

QString text(const QVariant &v) {
    if (v.typeId() == QMetaType::QVariantList) {
        QStringList parts;
        for (const QVariant &e : v.toList()) parts.append(e.toString());
        return parts.join(QStringLiteral(", "));
    }
    return v.typeId() == QMetaType::QString ? v.toString() : QString();
}

QStringList keysOf(const QVariant &v) {
    return v.typeId() == QMetaType::QVariantMap ? v.toMap().keys() : QStringList();
}

void addUnread(QJsonArray &unread, const QString &file, const QString &job, const QString &field) {
    unread.append(QJsonObject{{QStringLiteral("file"), file},
                              {QStringLiteral("job"), job.isEmpty() ? QJsonValue() : QJsonValue(job)},
                              {QStringLiteral("field"), field}});
}

// timeout-minutes: a positive number, else unknown (and named in `unread`
// when the key is present but holds something else, an expression say).
double budgetOf(const QVariantMap &m, Result &out, const QString &file,
                const QString &job, const QString &field) {
    if (!m.contains(QStringLiteral("timeout-minutes"))) return -1;
    bool ok = false;
    const double d = text(m.value(QStringLiteral("timeout-minutes"))).trimmed().toDouble(&ok);
    if (ok && d > 0) return d;
    addUnread(out.unread, file, job, field);
    return -1;
}

}  // namespace

QString Step::displayName() const {
    if (!name.isEmpty()) return name;
    if (!uses.isEmpty()) return QStringLiteral("Run ") + uses;
    return QStringLiteral("Run ") + run.section(QLatin1Char('\n'), 0, 0).trimmed();
}

double shellGuardMinutes(const QString &run) {
    if (run.contains(QLatin1Char('\n'))) return -1;
    static const QRegularExpression re(QStringLiteral(R"(^timeout (\d+(?:\.\d+)?)([smh]) )"));
    const auto m = re.match(run.trimmed() + QLatin1Char(' '));
    if (!m.hasMatch()) return -1;
    const double n = m.captured(1).toDouble();
    const QChar unit = m.captured(2).at(0);
    return unit == QLatin1Char('s') ? n / 60.0 : unit == QLatin1Char('h') ? n * 60.0 : n;
}

void parseInto(Result &out, const QString &rel, const QString &body) {
    Parser p(body);
    const QVariant root = p.parse();
    if (!p.ok() || root.typeId() != QMetaType::QVariantMap) {
        addUnread(out.unread, rel, QString(), QStringLiteral("file"));
        return;
    }
    const QVariantMap top = root.toMap();
    Workflow w;
    w.file = rel;
    w.name = text(top.value(QStringLiteral("name")));
    if (w.name.isEmpty()) w.name = rel;
    w.envKeys = keysOf(top.value(QStringLiteral("env")));
    const QVariantMap jobs = top.value(QStringLiteral("jobs")).toMap();
    for (auto it = jobs.cbegin(); it != jobs.cend(); ++it) {
        const QVariantMap jm = it.value().toMap();
        Job j;
        j.id = it.key();
        j.name = text(jm.value(QStringLiteral("name")));
        if (j.name.isEmpty()) j.name = j.id;
        j.runsOn = text(jm.value(QStringLiteral("runs-on")));
        j.budgetMin = budgetOf(jm, out, rel, j.id, QStringLiteral("timeout-minutes"));
        j.envKeys = keysOf(jm.value(QStringLiteral("env")));
        for (const QVariant &sv : jm.value(QStringLiteral("steps")).toList()) {
            const QVariantMap sm = sv.toMap();
            Step s;
            s.name = text(sm.value(QStringLiteral("name")));
            s.uses = text(sm.value(QStringLiteral("uses")));
            s.run = text(sm.value(QStringLiteral("run")));
            s.ifExpr = text(sm.value(QStringLiteral("if")));
            s.cacheKey = text(sm.value(QStringLiteral("with")).toMap().value(QStringLiteral("key")));
            s.envKeys = keysOf(sm.value(QStringLiteral("env")));
            const double own = budgetOf(sm, out, rel, j.id,
                QStringLiteral("steps[%1].timeout-minutes").arg(j.steps.size()));
            const double guard = shellGuardMinutes(s.run);
            s.budgetMin = own < 0 ? guard : guard < 0 ? own : std::min(own, guard);
            j.steps.append(s);
        }
        w.jobs.append(j);
    }
    out.workflows.append(w);
}

Result read(const QString &root) {
    Result out;
    const QDir dir(root + QStringLiteral("/.github/workflows"));
    const QStringList files = dir.entryList({QStringLiteral("*.yml"), QStringLiteral("*.yaml")},
                                            QDir::Files, QDir::Name);
    for (const QString &f : files) {
        const QString rel = QStringLiteral(".github/workflows/") + f;
        QFile file(dir.filePath(f));
        if (!file.open(QIODevice::ReadOnly)) {
            addUnread(out.unread, rel, QString(), QStringLiteral("file"));
            continue;
        }
        parseInto(out, rel, QString::fromUtf8(file.readAll()));
    }
    return out;
}

const Workflow *matchWorkflow(const Result &r, const QString &workflowName) {
    for (const Workflow &w : r.workflows)
        if (w.name == workflowName) return &w;
    for (const Workflow &w : r.workflows)
        if (w.file == workflowName) return &w;
    return nullptr;
}

const Job *matchJob(const Workflow &w, const QString &apiJobName) {
    for (const Job &j : w.jobs)
        if (j.name == apiJobName) return &j;
    for (const Job &j : w.jobs)
        if (apiJobName.startsWith(j.name + QStringLiteral(" ("))) return &j;
    return nullptr;
}

const Step *matchStep(const Job &j, const QString &apiStepName) {
    for (const Step &s : j.steps)
        if (s.displayName() == apiStepName) return &s;
    return nullptr;
}

}  // namespace CiWorkflows
