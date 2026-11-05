// ci_report (ANTS-6399) — the workflow reader of spec § 4.4.

#include "ciworkflows.h"

#include "gitwrap.h"    // readOnlyEnvironment
#include "hostexec.h"   // git runs on the host inside a Flatpak

#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QVariant>
#include <QVector>

#include <algorithm>
#include <climits>
#include <utility>

// A `|` or `>` scalar, kept apart from a plain one: a shell guard is read
// from a single-line plain `run:` only (spec § 4.4).
struct BlockText { QString text; };
Q_DECLARE_METATYPE(BlockText)

namespace CiWorkflows {

namespace {

// The YAML subset a workflow file is written in: block maps, block lists,
// plain and quoted scalars, `|`/`>` block scalars, and flow maps and lists. A
// map value's plain scalar or flow collection may continue on deeper lines;
// a quoted scalar or a list item's scalar may not. An alias or a merge key, which would lose keys unnamed, and anything
// else marks the file unparseable.
class Parser {
public:
    explicit Parser(const QString &text) {
        for (QString raw : text.split(QLatin1Char('\n'))) {
            if (raw.endsWith(QLatin1Char('\r'))) raw.chop(1);
            Line l;
            l.raw = raw;
            while (l.indent < raw.size() && raw.at(l.indent) == QLatin1Char(' ')) ++l.indent;
            l.text = raw.mid(l.indent).trimmed();
            // A tab is bad indentation only on a line the structure reads;
            // inside a block scalar it is text.
            l.tabIndent = l.indent < raw.size() && raw.at(l.indent) == QLatin1Char('\t');
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
    struct Line { QString raw; QString text; int indent = 0; bool skip = false; bool tabIndent = false; };

    int next(int from) const {
        while (from < m_l.size() && m_l[from].skip) ++from;
        return from;
    }
    void fail() { m_ok = false; m_i = int(m_l.size()); }
    static bool isDash(const QString &t) {
        return t == QLatin1String("-") || t.startsWith(QLatin1String("- "));
    }
    static bool splitKey(const QString &t, QString *key, QString *rest) {
        static const QRegularExpression re(QStringLiteral(
            R"re(^("([^"]*)"|'([^']*)'|[^\s:#"'{\[][^:#]*?)\s*:(?:\s+(.*))?$)re"));
        const auto m = re.match(t);
        if (!m.hasMatch()) return false;
        *key = !m.captured(2).isEmpty() ? m.captured(2)
             : !m.captured(3).isEmpty() ? m.captured(3) : m.captured(1);
        *rest = m.captured(4).trimmed();
        return true;
    }
    static QString quoted(const QString &rest) {
        const QChar q = rest.at(0);
        QString out;
        for (int i = 1; i < rest.size(); ++i) {
            const QChar c = rest.at(i);
            if (q == QLatin1Char('"') && c == QLatin1Char('\\') && i + 1 < rest.size()) {
                out += rest.at(++i);
            } else if (c == q) {
                if (q == QLatin1Char('\'') && i + 1 < rest.size() && rest.at(i + 1) == q) {
                    out += q;  // '' is one quote
                    ++i;
                } else {
                    break;
                }
            } else {
                out += c;
            }
        }
        return out;
    }
    // Walks flow-collection text: quoted spans (with \" escapes inside double
    // quotes) are passed over, and brackets move the depth. Calls
    // visit(i, c, depth) for each character outside quotes, with the depth
    // after that character; visit returns false to stop.
    template <typename Visit>
    static void scanFlow(const QString &s, Visit visit) {
        int depth = 0;
        QChar inQuote;
        bool escaped = false;
        for (int i = 0; i < s.size(); ++i) {
            const QChar c = s.at(i);
            if (!inQuote.isNull()) {
                if (escaped) escaped = false;
                else if (c == QLatin1Char('\\') && inQuote == QLatin1Char('"')) escaped = true;
                else if (c == inQuote) inQuote = QChar();
                continue;
            }
            if (c == QLatin1Char('"') || c == QLatin1Char('\'')) { inQuote = c; continue; }
            if (c == QLatin1Char('{') || c == QLatin1Char('[')) ++depth;
            else if (c == QLatin1Char('}') || c == QLatin1Char(']')) --depth;
            if (!visit(i, c, depth)) return;
        }
    }
    // A flow collection's top-level items: commas inside ${{ }}, nested
    // brackets or quotes do not split.
    static QStringList flowItems(const QString &inner) {
        QStringList items;
        int start = 0;
        scanFlow(inner, [&](int i, QChar c, int depth) {
            if (c == QLatin1Char(',') && depth == 0) {
                items.append(inner.mid(start, i - start).trimmed());
                start = i + 1;
            }
            return true;
        });
        const QString last = inner.mid(start).trimmed();
        if (!last.isEmpty()) items.append(last);
        return items;
    }
    // A flow collection followed by ` # comment`: the collection alone.
    // Anything else comes back unchanged.
    static QString flowText(const QString &rest) {
        if (!rest.startsWith(QLatin1Char('{')) && !rest.startsWith(QLatin1Char('['))) return rest;
        int close = -1;
        scanFlow(rest, [&](int i, QChar c, int depth) {
            if ((c == QLatin1Char('}') || c == QLatin1Char(']')) && depth == 0) close = i;
            return close < 0;
        });
        if (close < 0) return rest;
        const QString after = rest.mid(close + 1).trimmed();
        return after.isEmpty() || after.startsWith(QLatin1Char('#')) ? rest.left(close + 1) : rest;
    }
    QVariant value(const QString &rest) {
        if (rest.startsWith(QLatin1Char('*'))) { fail(); return {}; }  // an alias
        if (rest.startsWith(QLatin1Char('"')) || rest.startsWith(QLatin1Char('\'')))
            return quoted(rest);
        const QString flow = flowText(rest);
        if (flow != rest) return value(flow);  // a flow collection with a comment after it
        if (rest.startsWith(QLatin1Char('{')) && rest.endsWith(QLatin1Char('}'))) {
            QVariantMap m;
            for (const QString &item : flowItems(rest.mid(1, rest.size() - 2))) {
                QString k, r;
                if (!splitKey(item, &k, &r)) { fail(); return {}; }
                m.insert(k, value(r));
            }
            return m;
        }
        if (rest.startsWith(QLatin1Char('[')) && rest.endsWith(QLatin1Char(']'))) {
            QVariantList l;
            for (const QString &item : flowItems(rest.mid(1, rest.size() - 2))) l.append(value(item));
            return l;
        }
        const int hash = rest.indexOf(QLatin1String(" #"));
        return (hash >= 0 ? rest.left(hash) : rest).trimmed();
    }
    // A value may run on below its key, on lines indented deeper than the
    // key. Returns its whole text and moves past the lines it took: a plain
    // scalar's lines folded with a space (an empty line between them is a
    // newline, as YAML folds them), a flow collection's lines joined until its
    // brackets close. A comment line, or a line reading as a key, ends a plain
    // scalar; YAML rejects what follows, so map() fails the file there.
    QString continued(int indent, const QString &rest) {
        if (rest.startsWith(QLatin1Char('"')) || rest.startsWith(QLatin1Char('\''))) return rest;
        const bool flow = rest.startsWith(QLatin1Char('{')) || rest.startsWith(QLatin1Char('['));
        const auto open = [](const QString &s) {
            int d = 0;
            scanFlow(s, [&](int, QChar, int depth) { d = depth; return true; });
            return d > 0;
        };
        const auto ends = [&](const QString &s) {
            return flow ? !open(s) : s.contains(QLatin1String(" #"));
        };
        QString out = rest;
        if (ends(out)) return out;
        QString gap = QStringLiteral(" ");
        for (int j = m_i; j < m_l.size(); ++j) {
            const Line &l = m_l[j];
            if (l.text.isEmpty()) {
                if (!flow) gap = gap == QLatin1String(" ") ? QStringLiteral("\n") : gap + QLatin1Char('\n');
                continue;
            }
            QString k, r;
            if (l.skip || l.tabIndent || l.indent <= indent || (!flow && splitKey(l.text, &k, &r))) break;
            out += gap + l.text;
            gap = QStringLiteral(" ");
            m_i = j + 1;
            if (ends(out)) break;
        }
        return out;
    }
    // An anchor (`&name`) names a value and changes nothing this reader reads.
    static QString dropAnchor(const QString &rest) {
        if (!rest.startsWith(QLatin1Char('&'))) return rest;
        const int sp = rest.indexOf(QLatin1Char(' '));
        return sp < 0 ? QString() : rest.mid(sp + 1).trimmed();
    }

    QVariant node(int indent) {
        const int i = next(m_i);
        if (i >= m_l.size()) return QString();
        if (m_l[i].tabIndent) { fail(); return {}; }
        QString k, r;
        if (isDash(m_l[i].text)) return list(indent);
        if (splitKey(m_l[i].text, &k, &r)) return map(indent);
        m_i = i + 1;
        return value(m_l[i].text);
    }

    QVariant map(int indent) {
        QVariantMap m;
        for (;;) {
            const int i = next(m_i);
            if (i >= m_l.size() || m_l[i].indent < indent) break;
            if (m_l[i].indent > indent || m_l[i].tabIndent) { fail(); break; }
            if (isDash(m_l[i].text)) break;
            QString key, rest;
            if (!splitKey(m_l[i].text, &key, &rest)) { fail(); break; }
            if (key == QLatin1String("<<")) { fail(); break; }  // a merge key
            rest = dropAnchor(rest);
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
                m.insert(key, QVariant::fromValue(BlockText{block(indent)}));
            } else {
                m.insert(key, value(continued(indent, rest)));
            }
        }
        return m;
    }

    QVariant list(int indent) {
        QVariantList out;
        for (;;) {
            const int i = next(m_i);
            if (i >= m_l.size() || m_l[i].indent != indent || !isDash(m_l[i].text)) break;
            if (m_l[i].tabIndent) { fail(); break; }
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
            // This rewrites the line in place, so node() reads the item as a
            // map starting there.
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
            if (!blank && !l.tabIndent && l.indent <= parentIndent) break;
            if (!blank && !l.tabIndent) minIndent = std::min(minIndent, l.indent);
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

bool isBlock(const QVariant &v) { return v.userType() == qMetaTypeId<BlockText>(); }

QString text(const QVariant &v) {
    if (isBlock(v)) return v.value<BlockText>().text;
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
                              {QStringLiteral("job"), orNull(job)},
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

// The values `runs-on: ${{ matrix.<key> }}` takes: <key>'s axis list and each
// `include` entry's <key>. A matrix held in an expression gives none.
QStringList matrixValues(const QString &runsOn, const QVariant &strategy) {
    static const QRegularExpression ref(QStringLiteral(R"(^\$\{\{\s*matrix\.([\w-]+)\s*\}\}$)"));
    const auto m = ref.match(runsOn.trimmed());
    if (!m.hasMatch()) return {};
    const QString key = m.captured(1);
    const QVariantMap matrix = strategy.toMap().value(QStringLiteral("matrix")).toMap();
    QStringList out;
    auto add = [&](const QVariant &v) {
        const QString t = text(v).trimmed();
        if (!t.isEmpty() && !out.contains(t)) out.append(t);
    };
    for (const QVariant &v : matrix.value(key).toList()) add(v);
    for (const QVariant &e : matrix.value(QStringLiteral("include")).toList())
        if (e.toMap().contains(key)) add(e.toMap().value(key));
    return out;
}

}  // namespace

QString Step::displayName() const {
    if (!name.isEmpty()) return name;
    if (!uses.isEmpty()) return QStringLiteral("Run ") + uses;
    return QStringLiteral("Run ") + run.section(QLatin1Char('\n'), 0, 0).trimmed();
}

bool Step::isNamed(QStringView apiName) const {
    if (!name.isEmpty()) return apiName == name;
    static const QLatin1String kRun("Run ");
    if (!apiName.startsWith(kRun)) return false;
    const QStringView rest = apiName.mid(kRun.size());
    if (!uses.isEmpty()) return rest == uses;
    const qsizetype nl = run.indexOf(QLatin1Char('\n'));
    return rest == QStringView(run).left(nl < 0 ? run.size() : nl).trimmed();
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
        // A block-map runs-on (group, labels) names its runner in `labels`.
        const QVariant runsOn = jm.value(QStringLiteral("runs-on"));
        j.runsOn = text(runsOn.typeId() == QMetaType::QVariantMap
                            ? runsOn.toMap().value(QStringLiteral("labels")) : runsOn);
        j.matrixRunsOn = matrixValues(j.runsOn, jm.value(QStringLiteral("strategy")));
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
            const double guard = isBlock(sm.value(QStringLiteral("run"))) ? -1 : shellGuardMinutes(s.run);
            s.budgetMin = own < 0 ? guard : guard < 0 ? own : std::min(own, guard);
            j.steps.append(s);
        }
        w.jobs.append(j);
    }
    out.workflows.append(w);
}

QStringList files(const QString &root) {
    return QDir(root + QStringLiteral("/.github/workflows"))
        .entryList({QStringLiteral("*.yml"), QStringLiteral("*.yaml")}, QDir::Files, QDir::Name);
}

Result read(const QString &root) {
    Result out;
    const QDir dir(root + QStringLiteral("/.github/workflows"));
    for (const QString &f : files(root)) {
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

BySha readAtCommits(const QString &root, const QStringList &shas) {
    static const QRegularExpression hex(QStringLiteral("^[0-9a-f]{7,64}$"));
    const QStringList names = files(root);
    QList<std::pair<QString, QString>> asked;   // {sha, file} per request line, in order
    QByteArray input;
    QSet<QString> seen;
    for (const QString &sha : shas) {
        if (!hex.match(sha).hasMatch() || seen.contains(sha)) continue;
        seen.insert(sha);
        for (const QString &f : names) {
            asked.append({sha, f});
            // `./` resolves against the working directory, so a project
            // below its clone's top reads its own .github.
            input += (sha + QStringLiteral(":./.github/workflows/") + f + QLatin1Char('\n')).toUtf8();
        }
    }
    BySha out;
    if (asked.isEmpty()) return out;
    QProcess p;
    p.setWorkingDirectory(root);
    p.setProcessEnvironment(GitWrap::readOnlyEnvironment());
    HostExec::start(p, QStringLiteral("git"), {QStringLiteral("cat-file"), QStringLiteral("--batch")});
    if (!p.waitForStarted(5000)) return out;
    p.write(input);
    p.closeWriteChannel();
    if (!p.waitForFinished(20000)) { p.kill(); p.waitForFinished(500); return out; }
    // Each answer is "<oid> <type> <size>\n<bytes>\n", or "<name> missing\n"
    // (or "ambiguous") with no bytes.
    const QByteArray reply = p.readAllStandardOutput();
    qsizetype at = 0;
    for (const auto &[sha, file] : asked) {
        const qsizetype nl = reply.indexOf('\n', at);
        if (nl < 0) break;
        const QList<QByteArray> head = reply.mid(at, nl - at).split(' ');
        at = nl + 1;
        if (head.size() != 3) continue;
        const qsizetype size = head[2].toLongLong();
        if (head[1] == "blob")
            parseInto(out[sha], QStringLiteral(".github/workflows/") + file,
                      QString::fromUtf8(reply.mid(at, size)));
        at += size + 1;
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
    // A matrix leg's API name is "<name> (<values>)". Fall back to that only
    // when no job's name holds an expression, which GitHub would expand into
    // the same shape, and exactly one job fits.
    const Job *only = nullptr;
    for (const Job &j : w.jobs) {
        if (j.name.contains(QLatin1String("${{"))) return nullptr;
        if (!apiJobName.startsWith(j.name + QStringLiteral(" ("))) continue;
        if (only) return nullptr;
        only = &j;
    }
    return only;
}

const Step *matchStep(const Job &j, const QString &apiStepName) {
    for (const Step &s : j.steps)
        if (s.isNamed(apiStepName)) return &s;
    return nullptr;
}

}  // namespace CiWorkflows
