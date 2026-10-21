// ANTS-5734 — see trace.h. One pass: walk the project (§ 2.5), read each
// file once, find the name as a whole word, parse only the files that hold
// it (their rows come from the cache when unchanged), then join the
// documents, spec invariants and roadmap items (§ 2.7) and page the rows
// under the byte cap (§ 2.2).

#include "trace/trace.h"

#include "focusedtest.h"
#include "pathvalidation.h"
#include "projectsettings.h"
#include "specparse.h"
#include "trace/extract.h"
#include "trace/languages.h"
#include "trace/queries.h"
#include "walkrules.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cstring>
#include <optional>
#include <tuple>

namespace trace {

namespace {

constexpr int kMaxWalkFiles = 5000;                 // § 2.5, SymbolQuery's cap
constexpr qint64 kMaxFileBytes = 2LL * 1024 * 1024;  // § 2.5
constexpr int kDefaultMaxBytes = 512 * 1024;         // § 2.1
constexpr int kTextMax = 200;                        // a row's quoted line
constexpr int kGitListMs = 10000;                    // git ls-files' time budget

// ANTS-5748 / § 2.5 — every path git would track under `root`, ignored ones
// left out, sorted by path. Nothing where `root` is not in a git checkout, or
// git is absent, fails or overruns: the caller then walks the directory.
std::optional<QStringList> gitListedPaths(const QString &root) {
    QProcess git;
    git.start(QStringLiteral("git"),
              {QStringLiteral("-C"), root, QStringLiteral("ls-files"),
               QStringLiteral("--cached"), QStringLiteral("--others"),
               QStringLiteral("--exclude-standard"), QStringLiteral("--deduplicate"),
               QStringLiteral("-z")});
    if (!git.waitForFinished(kGitListMs)) {
        git.kill();
        git.waitForFinished();
        return std::nullopt;
    }
    if (git.exitStatus() != QProcess::NormalExit || git.exitCode() != 0)
        return std::nullopt;
    QStringList paths;
    for (const QByteArray &p : git.readAllStandardOutput().split('\0'))
        if (!p.isEmpty()) paths << QString::fromUtf8(p);
    paths.sort();
    return paths;
}

const QStringList &allSections() {
    static const QStringList s = {
        QStringLiteral("definitions"), QStringLiteral("callers"),
        QStringLiteral("callees"),     QStringLiteral("references"),
        QStringLiteral("tests"),       QStringLiteral("docs"),
        QStringLiteral("spec_invariants"), QStringLiteral("roadmap_items")};
    return s;
}

QJsonObject refuse(const char *code, const QString &message) {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("error"), QStringLiteral("trace: ") + message},
            {QStringLiteral("code"), QString::fromLatin1(code)}};
}

bool isWordByte(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

// Byte offsets of `needle` in `hay` as a whole word.
QVector<int> wholeWordHits(const QByteArray &hay, const QByteArray &needle) {
    QVector<int> out;
    if (needle.isEmpty()) return out;
    qsizetype i = 0;
    while ((i = hay.indexOf(needle, i)) >= 0) {
        const bool before = i == 0 || !isWordByte(hay.at(i - 1));
        const qsizetype e = i + needle.size();
        const bool after = e >= hay.size() || !isWordByte(hay.at(e));
        if (before && after) out.append(int(i));
        i += 1;
    }
    return out;
}

bool isPathByte(char c) { return isWordByte(c) || c == '-' || c == '.' || c == '/'; }

// Path mode (§ 2.7): the project-relative path, and the basename where it
// is unique, each bounded so "probe.service" does not match
// "probe.services". A basename hit inside a full-path hit is dropped.
QVector<int> pathHits(const QByteArray &hay, const QByteArray &rel, const QByteArray &base) {
    QVector<int> out;
    QVector<std::pair<int, int>> full;
    auto scan = [&](const QByteArray &needle, bool slashBefore) {
        QVector<int> hits;
        qsizetype i = 0;
        while (!needle.isEmpty() && (i = hay.indexOf(needle, i)) >= 0) {
            const char b = i > 0 ? hay.at(i - 1) : ' ';
            const bool beforeOk = i == 0 || (!isPathByte(b) || (slashBefore && b == '/'));
            const qsizetype e = i + needle.size();
            const char a = e < hay.size() ? hay.at(e) : ' ';
            const bool afterOk = !(isWordByte(a) || a == '-' || a == '/');
            if (beforeOk && afterOk) hits.append(int(i));
            i += 1;
        }
        return hits;
    };
    for (int h : scan(rel, false)) {
        out.append(h);
        full.append({h, h + int(rel.size())});
    }
    if (!base.isEmpty() && base != rel) {
        for (int h : scan(base, true)) {
            const bool inside = std::any_of(full.begin(), full.end(), [&](const auto &r) {
                return h >= r.first && h < r.second;
            });
            if (!inside) out.append(h);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Line starts, for byte offset -> (1-based line, 0-based column).
struct LineIndex {
    QVector<int> starts;
    explicit LineIndex(const QByteArray &text) {
        starts.append(0);
        for (int i = 0; i < text.size(); ++i)
            if (text.at(i) == '\n') starts.append(i + 1);
    }
    int lineOf(int byte) const {
        return int(std::upper_bound(starts.begin(), starts.end(), byte) - starts.begin());
    }
    int columnOf(int byte) const { return byte - starts.at(lineOf(byte) - 1); }
    QString textOf(const QByteArray &text, int line) const {
        const int s = starts.at(line - 1);
        const int e = line < starts.size() ? starts.at(line) - 1 : int(text.size());
        QByteArray t = text.mid(s, e - s).trimmed();
        if (t.size() > kTextMax) {
            t.truncate(kTextMax);
            while (!t.isEmpty() && (uchar(t.back()) & 0xC0) == 0x80) t.chop(1);
            if (!t.isEmpty() && (uchar(t.back()) & 0x80)) t.chop(1);
        }
        return QString::fromUtf8(t);
    }
};

bool inAny(const QVector<ByteRange> &ranges, uint32_t b) {
    return std::any_of(ranges.begin(), ranges.end(),
                       [b](const ByteRange &r) { return r.contains(b); });
}

// One row, with the key § 2.2's order sorts it by.
struct Row {
    QString file;
    int line = 0;
    int column = 0;
    QString tie;   // a last key, so two rows on one spot keep one order
    QJsonValue value;
};

void sortRows(QVector<Row> &rows) {
    std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
        return std::tie(a.file, a.line, a.column, a.tie) <
               std::tie(b.file, b.line, b.column, b.tie);
    });
}

bool isMarkdown(const QString &name) {
    const QString n = name.toLower();
    return n.endsWith(QLatin1String(".md")) || n.endsWith(QLatin1String(".markdown"));
}

// § 2.7 docs[]: per line, a match outside a fenced block (whole word, or a
// bounded path in path mode), or any match inside a code span. The row
// carries the nearest heading above.
void scanMarkdown(const QString &rel, const QByteArray &text, bool pathMode,
                  const QByteArray &needle, const QByteArray &rel2, const QByteArray &base,
                  QVector<Row> &docs) {
    static const QRegularExpression headingRe(QStringLiteral("^#{1,6}\\s+(.*?)\\s*#*\\s*$"));
    bool inFence = false;
    QByteArray fence;
    QString heading;
    int headingLine = 0;
    const QList<QByteArray> lines = text.split('\n');
    for (int i = 0; i < lines.size(); ++i) {
        const QByteArray &line = lines.at(i);
        const QByteArray t = line.trimmed();
        if (t.startsWith("```") || t.startsWith("~~~")) {
            const QByteArray marker = t.left(3);
            if (!inFence) {
                inFence = true;
                fence = marker;
            } else if (marker == fence) {
                inFence = false;
            }
            continue;
        }
        if (inFence) continue;
        const QString s = QString::fromUtf8(line);
        const auto hm = headingRe.match(s);
        if (hm.hasMatch()) {
            heading = hm.captured(1);
            headingLine = i + 1;
        }
        bool hit = pathMode ? !pathHits(line, rel2, base).isEmpty()
                            : !wholeWordHits(line, needle).isEmpty();
        if (!hit) {
            // Inside a code span any occurrence counts.
            const QList<QByteArray> parts = line.split('`');
            for (int p = 1; p < parts.size() && !hit; p += 2)
                hit = pathMode ? (parts.at(p).contains(rel2) ||
                                  (!base.isEmpty() && parts.at(p).contains(base)))
                               : parts.at(p).contains(needle);
        }
        if (!hit) continue;
        QJsonObject o{{QStringLiteral("file"), rel},
                      {QStringLiteral("line"), i + 1},
                      {QStringLiteral("heading"),
                       headingLine ? QJsonValue(heading) : QJsonValue()},
                      {QStringLiteral("heading_line"),
                       headingLine ? QJsonValue(headingLine) : QJsonValue()}};
        docs.append({rel, i + 1, 0, QString(), o});
    }
}

// "FIX-0001-probe.md" -> "FIX-0001"; a stem with no id prefix is its own id.
QString specIdOf(const QString &fileName) {
    static const QRegularExpression idRe(QStringLiteral("^([A-Z][A-Z0-9]*-[0-9]+)"));
    QString stem = fileName;
    if (stem.endsWith(QLatin1String(".md"))) stem.chop(3);
    const auto m = idRe.match(stem);
    return m.hasMatch() ? m.captured(1) : stem;
}

bool textNames(const QString &text, bool pathMode, const QByteArray &needle,
               const QByteArray &rel, const QByteArray &base) {
    const QByteArray b = text.toUtf8();
    return pathMode ? !pathHits(b, rel, base).isEmpty() : !wholeWordHits(b, needle).isEmpty();
}

}  // namespace

QJsonObject serve(const QJsonObject &args, const QString &rootCanonical, const Env &env) {
    // ---- arguments (§ 2.1) ----
    const bool hasSymbol = args.contains(QStringLiteral("symbol"));
    const bool hasPath = args.contains(QStringLiteral("path"));
    if (hasSymbol == hasPath)
        return refuse("bad_args", QStringLiteral("give exactly one of \"symbol\" and \"path\""));

    static const QRegularExpression symbolRe(QStringLiteral(
        "^[A-Za-z_][A-Za-z0-9_]{0,127}(?:(?:::|\\.)[A-Za-z_][A-Za-z0-9_]{0,127}){0,3}$"));
    QString symbolArg;
    QString name;
    QStringList scopes;
    if (hasSymbol) {
        symbolArg = args.value(QStringLiteral("symbol")).toString();
        if (!args.value(QStringLiteral("symbol")).isString() || !symbolRe.match(symbolArg).hasMatch())
            return refuse("bad_args",
                          QStringLiteral("\"symbol\" must be a name, or up to four "
                                         "names joined by :: or ."));
        static const QRegularExpression sep(QStringLiteral("::|\\."));
        scopes = symbolArg.split(sep);
        name = scopes.takeLast();
    }

    QSet<QString> include;
    if (args.contains(QStringLiteral("include"))) {
        const QJsonValue v = args.value(QStringLiteral("include"));
        if (!v.isArray())
            return refuse("bad_args", QStringLiteral("\"include\" must be an array"));
        for (const QJsonValue &e : v.toArray()) {
            const QString s = e.toString();
            if (!e.isString() || !allSections().contains(s))
                return refuse("bad_args", QStringLiteral("unknown \"include\" member \"%1\"")
                                              .arg(e.toVariant().toString()));
            if (s == QLatin1String("callees"))
                return refuse("bad_args", QStringLiteral("\"callees\" arrives with ship 2"));
            include.insert(s);
        }
    } else {
        for (const QString &s : allSections())
            if (s != QLatin1String("callees")) include.insert(s);
    }

    QString langFilter;
    if (args.contains(QStringLiteral("lang"))) {
        langFilter = args.value(QStringLiteral("lang")).toString();
        if (!isKnownLanguage(langFilter))
            return refuse("bad_args", QStringLiteral("unknown \"lang\" \"%1\"").arg(langFilter));
    }
    if (args.contains(QStringLiteral("depth"))) {
        const QJsonValue d = args.value(QStringLiteral("depth"));
        const double dv = d.toDouble(-1);
        if (!d.isDouble() || dv != int(dv) || dv < 1 || dv > 3)
            return refuse("bad_args", QStringLiteral("\"depth\" must be 1, 2 or 3"));
        if (dv > 1)
            return refuse("bad_args", QStringLiteral("\"depth\" above 1 arrives with ship 2"));
    }
    int maxBytes = kDefaultMaxBytes;
    if (args.contains(QStringLiteral("max_bytes"))) {
        maxBytes = args.value(QStringLiteral("max_bytes")).toInt(-1);
        if (maxBytes <= 0)
            return refuse("bad_args", QStringLiteral("\"max_bytes\" must be a positive integer"));
    }
    int offset = 0;
    if (args.contains(QStringLiteral("offset"))) {
        offset = args.value(QStringLiteral("offset")).toInt(-1);
        if (offset < 0)
            return refuse("bad_args", QStringLiteral("\"offset\" must be 0 or more"));
    }

    const QString root = QDir::cleanPath(rootCanonical);
    const int prefixLen = int(root.size()) + 1;
    QByteArray relNeedle, baseNeedle;
    QString pathRel;
    QJsonArray notCovered;
    if (hasPath) {
        const QString raw = args.value(QStringLiteral("path")).toString();
        const PathValidation::Check check = PathValidation::validatePath(
            raw, root, QStringLiteral("trace"), QStringLiteral("path"), false);
        if (check.bad) return check.err;
        const QString abs = QDir::cleanPath(QDir(root).absoluteFilePath(raw));
        pathRel = abs.mid(prefixLen);
        relNeedle = pathRel.toUtf8();
    }

    // ---- the walk (§ 2.5) ----
    QVector<QFileInfo> files;
    bool walkCapped = false;
    const auto addFile = [&](const QFileInfo &fi) {
        if (files.size() >= kMaxWalkFiles) {
            walkCapped = true;
            return;
        }
        files.append(fi);
    };
    std::function<void(const QString &)> walk = [&](const QString &dir) {
        for (const QFileInfo &fi : walkrules::entries(dir)) {
            if (walkCapped) return;
            if (!walkrules::followed(fi)) continue;
            if (fi.isDir()) {
                if (!walkrules::skippedDir(fi)) walk(fi.absoluteFilePath());
                continue;
            }
            if (fi.isFile()) addFile(fi);
        }
    };
    // ANTS-5748 — in a git checkout, git's list, so ignored trees never reach
    // the cap. The directory walk's rules still apply: entries() lists no
    // dot-files, and git's list holds them.
    if (const auto listed = gitListedPaths(root)) {
        for (QString rel : *listed) {
            if (walkCapped) break;
            if (rel.endsWith(QLatin1Char('/'))) rel.chop(1);   // a nested repository
            const QStringList parts = rel.split(QLatin1Char('/'));
            if (std::any_of(parts.cbegin(), parts.cend() - 1, walkrules::skippedDirName)
                || parts.last().startsWith(QLatin1Char('.')))
                continue;
            const QFileInfo fi(root + QLatin1Char('/') + rel);
            if (!walkrules::followed(fi)) continue;
            if (fi.isDir()) {                                   // a submodule
                if (!walkrules::skippedDir(fi)) walk(fi.absoluteFilePath());
                continue;
            }
            if (fi.isFile()) addFile(fi);
        }
    } else {
        walk(root);
    }

    if (hasPath) {
        const QString base = QFileInfo(pathRel).fileName();
        int same = 0;
        for (const QFileInfo &fi : std::as_const(files))
            if (fi.fileName() == base) ++same;
        if (same > 1)
            notCovered.append(QJsonObject{{QStringLiteral("reason"), QStringLiteral("ambiguous_basename")},
                                          {QStringLiteral("basename"), base},
                                          {QStringLiteral("files"), same}});
        else
            baseNeedle = base.toUtf8();
    }
    const QByteArray needle = name.toUtf8();

    const ProjectSettings::Settings settings = ProjectSettings::load(root);
    QStringList testRoots;
    if (settings.testRoots)
        for (const QString &t : *settings.testRoots)
            if (QFileInfo(root + QLatin1Char('/') + t).isDir()) testRoots << QDir::cleanPath(t);
    if (testRoots.isEmpty()) testRoots << QStringLiteral("tests");
    auto isTestFile = [&](const QString &rel) {
        return std::any_of(testRoots.begin(), testRoots.end(), [&](const QString &t) {
            return rel.startsWith(t + QLatin1Char('/'));
        });
    };

    // ---- per file ----
    QVector<Row> defRows, callerRows, refRows, docRows;
    QSet<QString> testFiles;
    QStringList defFiles;
    QMap<QString, int> noGrammarFiles;          // lang -> files present
    QMap<QString, QString> noGrammarDetail;     // lang -> why
    QMap<QString, QString> limitsByLang;        // vendored languages with files
    QStringList tooLarge, timedOut;
    int filesParsed = 0, filesWithErrors = 0;

    for (const QFileInfo &fi : std::as_const(files)) {
        const QString rel = fi.absoluteFilePath().mid(prefixLen);
        const bool md = isMarkdown(fi.fileName());
        if (md && !include.contains(QStringLiteral("docs"))) continue;
        QFile f(fi.absoluteFilePath());
        if (!f.open(QIODevice::ReadOnly)) continue;
        // ANTS-5748 — binary first, so a large binary is skipped, not reported.
        QByteArray text = f.read(8000);
        if (std::memchr(text.constData(), '\0', size_t(text.size())))
            continue;   // binary
        if (fi.size() > kMaxFileBytes) {
            tooLarge << rel;
            continue;
        }
        text += f.readAll();
        if (md) {
            scanMarkdown(rel, text, hasPath, needle, relNeedle, baseNeedle, docRows);
            continue;
        }

        const QString lang = languageForFile(fi.fileName(), text.left(256));
        const bool vendored = !lang.isEmpty() && languageFor(lang) != nullptr;
        if (!langFilter.isEmpty() && !lang.isEmpty() && lang != langFilter) continue;

        Query q;
        if (vendored) {
            q = queryFor(lang);
            if (q.query) {
                if (!q.limits.isEmpty()) limitsByLang.insert(lang, q.limits);
            } else {
                ++noGrammarFiles[lang];
                noGrammarDetail.insert(lang, q.error);
            }
        } else if (!lang.isEmpty()) {
            ++noGrammarFiles[lang];
        }

        const QVector<int> hits = hasPath ? pathHits(text, relNeedle, baseNeedle)
                                          : wholeWordHits(text, needle);
        if (hits.isEmpty()) continue;
        const bool test = isTestFile(rel);
        const LineIndex lines(text);

        std::shared_ptr<const FileRows> rows;
        if (vendored && q.query) {
            rows = rowsFor(fi.absoluteFilePath(), fi.lastModified().toMSecsSinceEpoch(),
                           fi.size(), lang, q.query, q.hash, text);
            if (rows->timedOut) {
                timedOut << rel;
                rows.reset();
            } else {
                ++filesParsed;
                if (rows->hasErrors) ++filesWithErrors;
            }
        }

        QVector<ByteRange> skip;   // definition and call names: not references
        if (rows && hasSymbol) {
            for (const DefRow &d : rows->defs) {
                if (d.name != name) continue;
                skip.append(d.nameRange);
                if (!scopes.isEmpty()) {
                    if (d.scope.size() < scopes.size()) continue;
                    if (d.scope.mid(d.scope.size() - scopes.size()) != scopes) continue;
                }
                QJsonArray sc;
                for (const QString &s : d.scope) sc.append(s);
                defRows.append({rel, d.line, d.column, d.isDeclaration ? QStringLiteral("d") : QString(),
                                QJsonObject{{QStringLiteral("file"), rel},
                                            {QStringLiteral("line"), d.line},
                                            {QStringLiteral("end_line"), d.endLine},
                                            {QStringLiteral("signature"), d.signature},
                                            {QStringLiteral("kind"), d.kind},
                                            {QStringLiteral("scope"), sc},
                                            {QStringLiteral("is_declaration"), d.isDeclaration},
                                            {QStringLiteral("lang"), lang}}});
                if (!defFiles.contains(rel)) defFiles << rel;
            }
            for (const CallRow &c : rows->calls) {
                if (c.name != name) continue;
                skip.append(c.nameRange);
                const DefRow *enclosing = nullptr;
                for (const DefRow &d : rows->defs) {
                    if (d.isDeclaration || !d.node.contains(c.nameRange.start)) continue;
                    if (!enclosing || d.node.end - d.node.start <
                                          enclosing->node.end - enclosing->node.start)
                        enclosing = &d;
                }
                const QJsonValue caller = enclosing
                    ? QJsonValue(QJsonObject{{QStringLiteral("name"), enclosing->name},
                                             {QStringLiteral("file"), rel},
                                             {QStringLiteral("line"), enclosing->line}})
                    : QJsonValue();
                callerRows.append({rel, c.line, c.column, QString(),
                                   QJsonObject{{QStringLiteral("caller"), caller},
                                               {QStringLiteral("file"), rel},
                                               {QStringLiteral("call_line"), c.line},
                                               {QStringLiteral("call_text"),
                                                lines.textOf(text, c.line)},
                                               {QStringLiteral("depth"), 1}}});
                if (test) testFiles.insert(rel);
            }
        }

        for (int h : hits) {
            const uint32_t b = uint32_t(h);
            if (inAny(skip, b)) continue;
            QString kind;
            if (test) kind = QStringLiteral("test");
            else if (rows) {
                if (inAny(rows->imports, b)) kind = QStringLiteral("import");
                else if (inAny(rows->comments, b)) kind = QStringLiteral("comment");
                else if (inAny(rows->strings, b)) kind = QStringLiteral("string_or_config");
                else kind = QStringLiteral("code");
            } else if (!lang.isEmpty()) {
                kind = QStringLiteral("unparsed");
            } else {
                kind = QStringLiteral("string_or_config");
            }
            const int line = lines.lineOf(h);
            refRows.append({rel, line, lines.columnOf(h), kind,
                            QJsonObject{{QStringLiteral("kind"), kind},
                                        {QStringLiteral("file"), rel},
                                        {QStringLiteral("line"), line},
                                        {QStringLiteral("text"), lines.textOf(text, line)}}});
            if (test) testFiles.insert(rel);
        }
    }

    // ---- spec invariants (§ 2.7) ----
    QVector<Row> specRows;
    if (include.contains(QStringLiteral("spec_invariants"))) {
        QString specsRel = QStringLiteral("docs/specs");
        if (settings.specsDir && QFileInfo(root + QLatin1Char('/') + *settings.specsDir).isDir())
            specsRel = *settings.specsDir;
        const QDir specsDir(root + QLatin1Char('/') + specsRel);
        const QStringList specFiles =
            specsDir.exists() ? specsDir.entryList({QStringLiteral("*.md")}, QDir::Files, QDir::Name)
                              : QStringList();
        for (const QString &fname : specFiles) {
            QFile f(specsDir.filePath(fname));
            if (!f.open(QIODevice::ReadOnly)) {
                notCovered.append(QJsonObject{{QStringLiteral("reason"), QStringLiteral("source_unreadable")},
                                              {QStringLiteral("source"), QStringLiteral("spec")},
                                              {QStringLiteral("file"), specsRel + QLatin1Char('/') + fname}});
                continue;
            }
            const QJsonObject parsed = SpecParse::parseSpecBody(QString::fromUtf8(f.readAll()));
            const QString sid = specIdOf(fname);
            int n = 0;
            for (const QJsonValue &inv : parsed.value(QStringLiteral("invariants")).toArray()) {
                const QJsonObject io = inv.toObject();
                ++n;
                if (!textNames(io.value(QStringLiteral("body")).toString(), hasPath, needle,
                               relNeedle, baseNeedle))
                    continue;
                specRows.append({specsRel + QLatin1Char('/') + fname, n, 0, QString(),
                                 QJsonObject{{QStringLiteral("spec"), sid},
                                             {QStringLiteral("inv"),
                                              io.value(QStringLiteral("id")).toString()}}});
            }
        }
    }

    // ---- roadmap items (§ 2.7) ----
    QVector<Row> roadmapRows;
    if (include.contains(QStringLiteral("roadmap_items"))) {
        std::optional<QVector<RoadmapParse::BulletRecord>> bullets;
        QString err;
        if (env.roadmapBullets) bullets = env.roadmapBullets(root, &err);
        if (!bullets && err.isEmpty()) {
            const QString rel = settings.roadmap ? *settings.roadmap : QStringLiteral("ROADMAP.md");
            QFile f(root + QLatin1Char('/') + rel);
            if (f.exists()) {
                if (f.open(QIODevice::ReadOnly))
                    bullets = RoadmapParse::parseBullets(QString::fromUtf8(f.readAll()),
                                                         ProjectSettings::idFormatFor(root));
                else
                    err = f.errorString();
            }
        }
        if (!err.isEmpty())
            notCovered.append(QJsonObject{{QStringLiteral("reason"), QStringLiteral("source_unreadable")},
                                          {QStringLiteral("source"), QStringLiteral("roadmap")},
                                          {QStringLiteral("detail"), err}});
        int n = 0;
        for (const RoadmapParse::BulletRecord &b : bullets.value_or(QVector<RoadmapParse::BulletRecord>{})) {
            ++n;
            if (b.id.isEmpty()) continue;
            if (!textNames(b.headlineFull.isEmpty() ? b.headline : b.headlineFull, hasPath,
                           needle, relNeedle, baseNeedle) &&
                !textNames(b.body, hasPath, needle, relNeedle, baseNeedle))
                continue;
            roadmapRows.append({QString(), n, 0, QString(),
                                QJsonObject{{QStringLiteral("id"), b.id},
                                            {QStringLiteral("headline"), b.headline}}});
        }
    }

    // ---- tests (§ 2.6) ----
    QVector<Row> testRows;
    QStringList sortedTests(testFiles.begin(), testFiles.end());
    sortedTests.sort();
    for (const QString &t : std::as_const(sortedTests)) testRows.append({t, 0, 0, QString(), t});
    QJsonArray ctestPatterns;
    QString selection;
    {
        const QStringList changed = hasPath ? QStringList{pathRel} : defFiles;
        if (!changed.isEmpty()) {
            const FocusedTest::Resolution res =
                FocusedTest::resolve(changed, FocusedTest::loadCoverageMap(root));
            if (res.selection != FocusedTest::Selection::Full) {
                for (const QString &p : res.patterns) ctestPatterns.append(p);
                selection = res.selection == FocusedTest::Selection::Map
                                ? QStringLiteral("map") : QStringLiteral("heuristic");
            }
        }
    }

    // ---- not_covered (§ 2.2), in a fixed order ----
    for (auto it = noGrammarFiles.constBegin(); it != noGrammarFiles.constEnd(); ++it) {
        QJsonObject o{{QStringLiteral("reason"), QStringLiteral("no_grammar")},
                      {QStringLiteral("lang"), it.key()},
                      {QStringLiteral("files"), it.value()}};
        if (noGrammarDetail.contains(it.key()))
            o.insert(QStringLiteral("detail"), noGrammarDetail.value(it.key()));
        notCovered.append(o);
    }
    tooLarge.sort();
    for (const QString &t : std::as_const(tooLarge))
        notCovered.append(QJsonObject{{QStringLiteral("reason"), QStringLiteral("file_too_large")},
                                      {QStringLiteral("file"), t}});
    timedOut.sort();
    for (const QString &t : std::as_const(timedOut))
        notCovered.append(QJsonObject{{QStringLiteral("reason"), QStringLiteral("parse_timeout")},
                                      {QStringLiteral("file"), t}});
    for (auto it = limitsByLang.constBegin(); it != limitsByLang.constEnd(); ++it)
        notCovered.append(QJsonObject{{QStringLiteral("reason"), QStringLiteral("limits")},
                                      {QStringLiteral("lang"), it.key()},
                                      {QStringLiteral("detail"), it.value()}});

    // ---- the reply, paged at row boundaries (§ 2.2) ----
    sortRows(defRows);
    sortRows(callerRows);
    sortRows(refRows);
    sortRows(docRows);

    struct Section {
        QString key;
        QVector<Row> *rows;
    };
    QVector<Section> sections;
    if (hasSymbol && include.contains(QStringLiteral("definitions")))
        sections.append({QStringLiteral("definitions"), &defRows});
    if (hasSymbol && include.contains(QStringLiteral("callers")))
        sections.append({QStringLiteral("callers"), &callerRows});
    if (include.contains(QStringLiteral("references")))
        sections.append({QStringLiteral("references"), &refRows});
    if (include.contains(QStringLiteral("tests")))
        sections.append({QStringLiteral("tests"), &testRows});
    if (include.contains(QStringLiteral("docs")))
        sections.append({QStringLiteral("docs"), &docRows});
    if (include.contains(QStringLiteral("spec_invariants")))
        sections.append({QStringLiteral("spec_invariants"), &specRows});
    if (include.contains(QStringLiteral("roadmap_items")))
        sections.append({QStringLiteral("roadmap_items"), &roadmapRows});

    QStringList scannedLangs = limitsByLang.keys();
    QJsonArray langs;
    for (const QString &l : scannedLangs) langs.append(l);

    QJsonObject reply{{QStringLiteral("ok"), true},
                      {QStringLiteral("mode"), hasSymbol ? QStringLiteral("symbol")
                                                         : QStringLiteral("path")},
                      {QStringLiteral("precision"), QStringLiteral("syntactic")},
                      {QStringLiteral("not_covered"), notCovered},
                      {QStringLiteral("scanned"),
                       QJsonObject{{QStringLiteral("files"), int(files.size())},
                                   {QStringLiteral("files_parsed"), filesParsed},
                                   {QStringLiteral("files_with_parse_errors"), filesWithErrors},
                                   {QStringLiteral("languages"), langs},
                                   {QStringLiteral("walk_capped"), walkCapped}}}};
    if (hasSymbol) reply.insert(QStringLiteral("symbol"), symbolArg);
    else reply.insert(QStringLiteral("path"), pathRel);

    // Every row in one sequence, so `offset` indexes across sections.
    int total = 0;
    for (const Section &s : sections) total += int(s.rows->size());
    auto sectionValue = [&](const Section &s, const QJsonArray &rows) -> QJsonValue {
        if (s.key != QLatin1String("tests")) return rows;
        QJsonObject t{{QStringLiteral("files"), rows}};
        if (!selection.isEmpty()) {
            t.insert(QStringLiteral("ctest_patterns"), ctestPatterns);
            t.insert(QStringLiteral("selection"), selection);
        }
        return t;
    };
    // The envelope with every section empty, to size the row budget.
    QJsonObject counts;
    for (const Section &s : sections)
        counts.insert(s.key, QJsonObject{{QStringLiteral("total"), int(s.rows->size())},
                                         {QStringLiteral("returned"), 0}});
    QJsonObject sizing = reply;
    for (const Section &s : sections) sizing.insert(s.key, sectionValue(s, {}));
    sizing.insert(QStringLiteral("counts"), counts);
    sizing.insert(QStringLiteral("truncated"), true);
    sizing.insert(QStringLiteral("next_offset"), total);
    const qint64 envelope = QJsonDocument(sizing).toJson(QJsonDocument::Compact).size();
    const qint64 budget = maxBytes - envelope;

    qint64 used = 0;
    int index = 0;
    int nextOffset = -1;
    for (const Section &s : sections) {
        QJsonArray page;
        for (const Row &r : *s.rows) {
            const int i = index++;
            if (i < offset || nextOffset >= 0) continue;
            const qint64 bytes =
                (r.value.isString()
                     ? r.value.toString().toUtf8().size() + 2
                     : QJsonDocument(r.value.toObject()).toJson(QJsonDocument::Compact).size()) + 1;
            const bool first = used == 0;
            if (!first && used + bytes > budget) {
                nextOffset = i;
                continue;
            }
            used += bytes;
            page.append(r.value);
        }
        reply.insert(s.key, sectionValue(s, page));
        QJsonObject c = counts.value(s.key).toObject();
        c.insert(QStringLiteral("returned"), int(page.size()));
        counts.insert(s.key, c);
    }
    reply.insert(QStringLiteral("counts"), counts);
    reply.insert(QStringLiteral("truncated"), nextOffset >= 0);
    reply.insert(QStringLiteral("next_offset"), nextOffset >= 0 ? QJsonValue(nextOffset) : QJsonValue());
    return reply;
}

}  // namespace trace
