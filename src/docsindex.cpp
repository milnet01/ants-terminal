// ANTS-2139 — docs_index pure helper. Qt6::Core-only, FS-reading, no widgets /
// no subprocess (mirrors codebaseindex.cpp). See docs/specs/ANTS-2139.md.

#include "docsindex.h"

#include "sessionmemoryengine.h"
#include "projectsettings.h"   // ANTS-2160 — docs_dir override
#include "markdownscan.h"      // ANTS-3604 — shared fence-aware scanner
#include "specparse.h"         // ANTS-3786 — shared header-field extent rule

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <algorithm>

namespace DocsIndex {

namespace {

// Parent directory of a project-relative doc path, project-relative; a
// root-level doc buckets under ".". QFileInfo("README.md").path() == ".".
QString relDirOf(const QString &rel) {
    return QFileInfo(rel).path();
}

// Filename stem (basename minus ".md"); never empty for a *.md candidate.
QString idFor(const QString &rel) {
    return QFileInfo(rel).completeBaseName();
}

// Resolve a markdown link target against the doc's own directory. Returns a
// normalised project-relative .md path, or "" to drop (external / mailto /
// pure-anchor / non-.md / escapes the root). The #anchor is stripped first.
QString resolveLink(const QString &relDir, const QString &rawTarget) {
    QString t = rawTarget;
    const int hash = t.indexOf(QLatin1Char('#'));
    if (hash >= 0) t = t.left(hash);
    if (t.isEmpty()) return QString();                       // pure anchor
    if (t.contains(QStringLiteral("://"))
        || t.startsWith(QStringLiteral("mailto:")))          // external
        return QString();
    if (!t.endsWith(QLatin1String(".md")))                   // non-markdown
        return QString();
    const QString combined =
        (relDir.isEmpty() || relDir == QLatin1String("."))
            ? t
            : relDir + QLatin1Char('/') + t;
    const QString clean = QDir::cleanPath(combined);
    if (clean.startsWith(QLatin1String("../")) || clean == QLatin1String(".."))
        return QString();                                    // escapes root
    return clean;
}

struct ScanResult {
    QString          title;
    QString          status;
    QVector<Heading> headings;
    QStringList      links;
    int              lines = 0;
};

// Single-pass markdown scanner (§ 2.2). Skips any line longer than
// kMaxLineBytes (the FileOutline regex-skip guard — a continue, not a partial
// read) and stops once the cumulative bytes read exceed opts.maxDocBytes (the
// doc still gets an entry, with whatever was collected before the cutoff).
ScanResult scanDoc(const QString &absPath, const QString &relDir,
                   const Options &opts) {
    ScanResult r;
    QFile f(absPath);
    if (!f.open(QIODevice::ReadOnly)) return r;

    static const QRegularExpression headingRx(
        QStringLiteral("^(#{1,6})\\s+(.+)$"));
    static const QRegularExpression linkRx(
        QStringLiteral("\\[[^\\]]*\\]\\(([^)\\s]+)\\)"));
    static const QRegularExpression trailFenceRx(
        QStringLiteral("\\s+#+\\s*$"));

    QSet<QString> linkSet;
    qint64 budget = 0;
    int lineNo = 0;
    QChar openFence;  // ANTS-3604 — null when not inside a fenced code block
    int   openFenceRun = 0;  // ANTS-4820 — that fence's character-run length

    // ANTS-3786 — the header block, buffered so SpecParse::headerField reads it
    // once per document instead of a per-line regex that could only ever see one
    // physical line. Bounded by maxHeaderBlockLines and released at whichever of
    // the three exits fires (first `^## `, the cap, or EOF), never held across
    // documents.
    QStringList headerLines;
    bool        headerDone = false;
    bool        budgetHit  = false;   // the maxDocBytes break, distinguished from EOF

    while (!f.atEnd()) {
        const QByteArray raw = f.readLine();
        budget += raw.size();
        if (budget > opts.maxDocBytes) { budgetHit = true; break; }  // INV-19
        ++lineNo;
        if (raw.size() > kMaxLineBytes) continue;  // over-long line skip (INV-3)

        QString line = QString::fromUtf8(raw);
        while (line.endsWith(QLatin1Char('\n')) || line.endsWith(QLatin1Char('\r')))
            line.chop(1);

        // ANTS-3604 — a heading / link inside a ```/~~~ fenced code block is
        // an illustration, not real index content. Track the open fence and
        // skip its opener, body, and closer lines (line/byte counting above
        // still runs so line numbers stay accurate).
        // ANTS-4820 — close on the run LENGTH too (CommonMark § 4.5), not on
        // the fence character alone: a ``` line was ending a ```` block, so a
        // heading quoted as sample text was indexed as real content.
        int openerRun = 0;
        const QChar opener = MarkdownScan::fenceOpenerChar(line, 3, &openerRun);
        if (!openFence.isNull()) {
            if (MarkdownScan::fenceCloses(line, openFence, openFenceRun)) {
                openFence    = QChar();
                openFenceRun = 0;
            }
            continue;
        }
        if (!opener.isNull()) {
            openFence    = opener;
            openFenceRun = openerRun;
            continue;
        }

        const auto hm = headingRx.match(line);
        if (hm.hasMatch()) {
            if (r.headings.size() < opts.maxHeadingsPerDoc) {
                Heading h;
                h.level = hm.captured(1).size();
                QString text = hm.captured(2);
                text.remove(trailFenceRx);  // ATX closing fence
                h.text = text.trimmed();
                h.line = lineNo;
                r.headings << h;
                if (h.level == 1 && r.title.isEmpty()) r.title = h.text;
            }
            // headings beyond the cap are silently dropped (INV-19)
        }

        // ANTS-3786 — accumulate the header block, then parse it once with the
        // shared rule. This sits AFTER the over-long-line (INV-3) and fenced
        // block (ANTS-3604) `continue`s, so those lines never enter the buffer:
        // every existing scanDoc invariant stays intact, at the cost of the
        // buffer being a filtered view of the file rather than its raw lines.
        if (!headerDone) {
            if (SpecParse::isHeaderBlockEnd(line) ||
                headerLines.size() >= opts.maxHeaderBlockLines) {
                headerDone = true;                       // block end / silent cap
                r.status = SpecParse::headerField(headerLines,
                                                  QStringLiteral("Status")).value;
                headerLines.clear();                     // released immediately
            } else {
                headerLines << line;
            }
        }

        auto lit = linkRx.globalMatch(line);
        while (lit.hasNext()) {
            const QString resolved = resolveLink(relDir, lit.next().captured(1));
            if (!resolved.isEmpty() && !linkSet.contains(resolved)) {
                linkSet.insert(resolved);
                r.links << resolved;
            }
        }
    }

    // ANTS-3786 INV-8 — the EOF exit. A document whose header block is never
    // closed by a `^## ` (and never hits the cap) reaches here with lines still
    // buffered; without this flush its status would be silently lost. It does
    // NOT run when the loop ended on the byte budget (INV-5): that buffer is a
    // truncated PREFIX of the header block, not a complete one, so emitting
    // from it would report a partial field value as a whole one. Both paths
    // leave headerDone == false and only one of them means "the block ended".
    if (!headerDone && !budgetHit) {
        r.status = SpecParse::headerField(headerLines,
                                          QStringLiteral("Status")).value;
    }
    headerLines.clear();

    r.links.sort();
    if (r.links.size() > opts.maxLinksPerDoc)
        r.links = r.links.mid(0, opts.maxLinksPerDoc);  // post-sort cap (INV-19)
    r.lines = lineNo;
    return r;
}

DocEntry scanToEntry(const QString &rootCanonical, const QString &rel,
                     const Options &opts) {
    DocEntry de;
    de.path = rel;
    de.id   = idFor(rel);
    const QString abs = rootCanonical + QLatin1Char('/') + rel;
    de.mtimeMs = QFileInfo(abs).lastModified().toMSecsSinceEpoch();
    const ScanResult sr = scanDoc(abs, relDirOf(rel), opts);
    de.title    = sr.title;
    de.status   = sr.status;
    de.lines    = sr.lines;
    de.headings = sr.headings;
    de.links    = sr.links;
    return de;
}

// Deterministic candidate list: <root>/*.md (sorted) then <root>/docs/**/*.md
// (sorted), project-relative paths, .md only, no symlinks.
QStringList walkDocs(const QString &rootCanonical) {
    QStringList rootMd, docsMd;
    const int prefix = rootCanonical.size() + 1;  // strip "<root>/"

    {
        QDirIterator it(rootCanonical, QDir::Files | QDir::NoSymLinks);
        while (it.hasNext()) {
            it.next();
            if (it.fileInfo().suffix().toLower() == QLatin1String("md"))
                rootMd << it.filePath().mid(prefix);
        }
    }
    rootMd.sort();

    // ANTS-2160 — .ants/project.json docs_dir override; else the default
    // <root>/docs subtree. An absent / non-dir override falls back, so the
    // no-settings walk is unchanged (INV-1/INV-3).
    QString docsRel = QStringLiteral("docs");
    {
        const ProjectSettings::Settings s = ProjectSettings::load(rootCanonical);
        if (s.docsDir &&
            QDir(rootCanonical + QLatin1Char('/') + *s.docsDir).exists())
            docsRel = *s.docsDir;
    }
    const QString docsBase = rootCanonical + QLatin1Char('/') + docsRel;
    if (QDir(docsBase).exists()) {
        QDirIterator it(docsBase, QDir::Files | QDir::NoSymLinks,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            if (it.fileInfo().suffix().toLower() == QLatin1String("md"))
                docsMd << it.filePath().mid(prefix);
        }
    }
    docsMd.sort();

    return rootMd + docsMd;
}

// Rough serialised-byte estimate for the incremental cache ceiling (INV-15).
qint64 estimateEntryBytes(const DocEntry &de) {
    qint64 n = de.path.size() + de.id.size() + de.title.size()
             + de.status.size() + 80;
    for (const Heading &h : de.headings) n += h.text.size() + 40;
    for (const QString &l : de.links)    n += l.size() + 8;
    return n;
}

const DocEntry *findDoc(const Index &idx, const QString &rel) {
    for (const DocEntry &de : idx.docs)
        if (de.path == rel) return &de;
    return nullptr;
}

QJsonArray headingsJson(const QVector<Heading> &hs) {
    QJsonArray a;
    for (const Heading &h : hs) {
        QJsonObject o;
        o[QStringLiteral("level")] = h.level;
        o[QStringLiteral("text")]  = h.text;
        o[QStringLiteral("line")]  = h.line;
        a.append(o);
    }
    return a;
}

}  // namespace

Index build(const QString &rootCanonical, qint64 generatedAtMs,
            const Options &opts) {
    Index idx;
    idx.version       = kIndexVersion;
    idx.rootCanonical = rootCanonical;
    idx.generatedAtMs = generatedAtMs;

    qint64 budget = 0;
    for (const QString &rel : walkDocs(rootCanonical)) {
        if (idx.docs.size() >= opts.maxIndexDocs) { idx.docsTruncated = true; break; }
        DocEntry de = scanToEntry(rootCanonical, rel, opts);
        const qint64 est = estimateEntryBytes(de);
        if (!idx.docs.isEmpty() && budget + est > opts.maxCacheBytes) {
            idx.docsTruncated = true; break;
        }
        budget += est;
        idx.docs << de;
    }
    return idx;
}

// ANTS-2198 — internal worker accepting a precomputed doc list so the hot
// serve()->refresh() path walks walkDocs() (which loads .ants/project.json)
// once instead of 2-3x. The public staleDocs/refresh below pass a fresh
// walkDocs() for standalone (test) callers.
static StaleSet staleDocsWith(const Index &prev, const QString &rootCanonical,
                              const QStringList &docs) {
    StaleSet ss;
    QSet<QString> prevPaths;
    QMap<QString, qint64> prevMtime;
    for (const DocEntry &de : prev.docs) {
        prevPaths.insert(de.path);
        prevMtime.insert(de.path, de.mtimeMs);
    }

    QSet<QString> curSet;
    for (const QString &rel : docs) {
        curSet.insert(rel);
        const qint64 m = QFileInfo(rootCanonical + QLatin1Char('/') + rel)
                             .lastModified().toMSecsSinceEpoch();
        if (!prevPaths.contains(rel)) ss.added << rel;
        else if (prevMtime.value(rel) != m) ss.changed << rel;
    }
    for (const QString &p : prevPaths)
        if (!curSet.contains(p)) ss.removed << p;
    return ss;
}

StaleSet staleDocs(const Index &prev, const QString &rootCanonical) {
    return staleDocsWith(prev, rootCanonical, walkDocs(rootCanonical));
}

// ANTS-2198 — internal worker taking the precomputed StaleSet + doc list so
// the serve() hot path does not recompute either.
static Index refreshWith(const Index &prev, const QString &rootCanonical,
                         qint64 generatedAtMs, const Options &opts,
                         int *refreshedOut, const StaleSet &ss,
                         const QStringList &docs) {
    QSet<QString> rescan;
    for (const QString &p : ss.changed) rescan.insert(p);
    for (const QString &p : ss.added)   rescan.insert(p);

    QMap<QString, DocEntry> byPath;
    for (const DocEntry &de : prev.docs) byPath.insert(de.path, de);

    Index idx;
    idx.version       = kIndexVersion;
    idx.rootCanonical = rootCanonical;
    idx.generatedAtMs = generatedAtMs;

    qint64 budget = 0;
    for (const QString &rel : docs) {
        if (idx.docs.size() >= opts.maxIndexDocs) { idx.docsTruncated = true; break; }
        DocEntry de = (rescan.contains(rel) || !byPath.contains(rel))
                          ? scanToEntry(rootCanonical, rel, opts)
                          : byPath.value(rel);
        const qint64 est = estimateEntryBytes(de);
        if (!idx.docs.isEmpty() && budget + est > opts.maxCacheBytes) {
            idx.docsTruncated = true; break;
        }
        budget += est;
        idx.docs << de;
    }
    if (refreshedOut) *refreshedOut = ss.changed.size() + ss.added.size();
    return idx;
}

Index refresh(const Index &prev, const QString &rootCanonical,
              qint64 generatedAtMs, const Options &opts, int *refreshedOut) {
    // Standalone (test) entry: compute the doc list + stale set once, then
    // delegate. The hot serve() path computes these once itself and calls
    // refreshWith directly, so walkDocs() runs once per serve (ANTS-2198).
    const QStringList docs = walkDocs(rootCanonical);
    const StaleSet ss = staleDocsWith(prev, rootCanonical, docs);
    return refreshWith(prev, rootCanonical, generatedAtMs, opts,
                       refreshedOut, ss, docs);
}

QJsonObject query(const Index &idx, const QueryParams &params,
                  int refreshedDocs, const QString &cachePath,
                  const Options &opts) {
    const int n = (!params.topic.isEmpty()   ? 1 : 0)
                + (!params.docPath.isEmpty()  ? 1 : 0)
                + (!params.id.isEmpty()       ? 1 : 0);
    if (n >= 2) {
        QJsonObject e;
        e[QStringLiteral("ok")]    = false;
        e[QStringLiteral("code")]  = QStringLiteral("bad_args");
        e[QStringLiteral("error")] = QStringLiteral(
            "docs_index: at most one of {topic, doc_path, id}");
        return e;
    }

    QJsonObject env;
    env[QStringLiteral("ok")]              = true;
    env[QStringLiteral("generated_at_ms")] = idx.generatedAtMs;
    env[QStringLiteral("doc_count")]       = idx.docs.size();
    env[QStringLiteral("refreshed_docs")]  = refreshedDocs;
    env[QStringLiteral("cache_path")]      = cachePath;
    env[QStringLiteral("docs_truncated")]  = idx.docsTruncated;
    bool topicTruncated = false;
    bool linkedFromTruncated = false;

    if (n == 0) {
        // Summary: light per-doc list + dir rollup, computed from the
        // post-truncation in-memory docs[] (INV-16).
        QJsonArray docsArr;
        QMap<QString, int> dirCounts;
        for (const DocEntry &de : idx.docs) {
            QJsonObject d;
            d[QStringLiteral("path")]          = de.path;
            d[QStringLiteral("id")]            = de.id;
            d[QStringLiteral("title")]         = de.title;
            d[QStringLiteral("status")]        = de.status;
            d[QStringLiteral("heading_count")] = de.headings.size();
            docsArr.append(d);
            dirCounts[relDirOf(de.path)]++;
        }
        QJsonArray dirsArr;
        for (auto it = dirCounts.cbegin(); it != dirCounts.cend(); ++it) {
            QJsonObject d;
            d[QStringLiteral("dir")]       = it.key();
            d[QStringLiteral("doc_count")] = it.value();
            dirsArr.append(d);
        }
        env[QStringLiteral("docs")] = docsArr;
        env[QStringLiteral("dirs")] = dirsArr;
    } else if (!params.topic.isEmpty()) {
        // ANTS-4784 — compiled once, not per query.
        static const QRegularExpression kTopicWords(QStringLiteral("\\s+"));
        const QStringList tokens =
            params.topic.toLower().split(kTopicWords, Qt::SkipEmptyParts);
        struct Hit { int score{}; QString path, id, title; QStringList evidence; };
        QVector<Hit> hits;
        for (const DocEntry &de : idx.docs) {
            const QString titleLower = de.title.toLower();
            const QString pathLower  = de.path.toLower();
            int score = 0;
            QStringList evidence;
            for (const QString &tok : tokens) {
                if (titleLower.contains(tok)) {
                    score += 3;
                    evidence << QStringLiteral("title: ") + de.title;
                }
                if (pathLower.contains(tok)) {
                    score += 2;
                    evidence << QStringLiteral("path: ") + de.path;
                }
                for (const Heading &h : de.headings) {
                    if (h.text.toLower().contains(tok)) {
                        score += 1;
                        evidence << QStringLiteral("heading: ") + h.text;
                        break;  // heading weight counts at most once per token
                    }
                }
            }
            if (score > 0) {
                if (evidence.size() > 3) evidence = evidence.mid(0, 3);
                hits.push_back({score, de.path, de.id, de.title, evidence});
            }
        }
        std::sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) {
            if (a.score != b.score) return a.score > b.score;  // desc
            return a.path < b.path;                            // tie: path asc
        });
        if (hits.size() > opts.maxTopicHits) {
            hits.resize(opts.maxTopicHits);
            topicTruncated = true;
        }
        QJsonArray matches;
        for (const Hit &h : hits) {
            QJsonObject m;
            m[QStringLiteral("path")]  = h.path;
            m[QStringLiteral("id")]    = h.id;
            m[QStringLiteral("title")] = h.title;
            m[QStringLiteral("score")] = h.score;
            QJsonArray ev;
            for (const QString &e : h.evidence) ev.append(e);
            m[QStringLiteral("evidence")] = ev;
            matches.append(m);
        }
        env[QStringLiteral("found")]   = !matches.isEmpty();
        env[QStringLiteral("matches")] = matches;
    } else if (!params.docPath.isEmpty()) {
        const DocEntry *de = findDoc(idx, params.docPath);
        env[QStringLiteral("found")] = de != nullptr;
        if (de) {
            QJsonObject entry;
            entry[QStringLiteral("path")]     = de->path;
            entry[QStringLiteral("id")]       = de->id;
            entry[QStringLiteral("title")]    = de->title;
            entry[QStringLiteral("status")]   = de->status;
            entry[QStringLiteral("lines")]    = de->lines;
            entry[QStringLiteral("headings")] = headingsJson(de->headings);
            QJsonArray links;
            for (const QString &l : de->links) links.append(l);
            entry[QStringLiteral("links")] = links;

            QStringList linkers;
            for (const DocEntry &other : idx.docs)
                if (other.path != de->path && other.links.contains(de->path))
                    linkers << other.path;
            linkers.sort();
            if (linkers.size() > opts.maxLinkedFrom) {
                linkers = linkers.mid(0, opts.maxLinkedFrom);
                linkedFromTruncated = true;
            }
            QJsonArray lf;
            for (const QString &l : linkers) lf.append(l);
            entry[QStringLiteral("linked_from")] = lf;
            env[QStringLiteral("entry")] = entry;
        }
    } else {  // id=
        QStringList matchPaths;
        QMap<QString, const DocEntry *> byPath;
        for (const DocEntry &de : idx.docs)
            if (de.id == params.id) {           // exact, case-sensitive
                matchPaths << de.path;
                byPath.insert(de.path, &de);
            }
        matchPaths.sort();
        QJsonArray matches;
        for (const QString &p : matchPaths) {
            const DocEntry *de = byPath.value(p);
            QJsonObject m;
            m[QStringLiteral("path")]   = de->path;
            m[QStringLiteral("title")]  = de->title;
            m[QStringLiteral("status")] = de->status;   // id omitted (== query)
            matches.append(m);
        }
        env[QStringLiteral("found")]   = !matches.isEmpty();
        env[QStringLiteral("matches")] = matches;
    }

    env[QStringLiteral("topic_truncated")]       = topicTruncated;
    env[QStringLiteral("linked_from_truncated")] = linkedFromTruncated;
    return env;
}

QJsonObject toJson(const Index &idx) {
    QJsonObject o;
    o[QStringLiteral("version")]         = idx.version;
    o[QStringLiteral("root_canonical")]  = idx.rootCanonical;
    o[QStringLiteral("generated_at_ms")] = idx.generatedAtMs;
    o[QStringLiteral("docs_truncated")]  = idx.docsTruncated;
    QJsonArray docs;
    for (const DocEntry &de : idx.docs) {
        QJsonObject d;
        d[QStringLiteral("path")]     = de.path;
        d[QStringLiteral("id")]       = de.id;
        d[QStringLiteral("title")]    = de.title;
        d[QStringLiteral("status")]   = de.status;
        d[QStringLiteral("lines")]    = de.lines;
        d[QStringLiteral("mtime_ms")] = de.mtimeMs;
        d[QStringLiteral("headings")] = headingsJson(de.headings);
        QJsonArray links;
        for (const QString &l : de.links) links.append(l);
        d[QStringLiteral("links")] = links;
        docs.append(d);
    }
    o[QStringLiteral("docs")] = docs;
    return o;
}

Index fromJson(const QJsonObject &obj) {
    Index idx;
    idx.version       = obj.value(QStringLiteral("version")).toInt();
    idx.rootCanonical = obj.value(QStringLiteral("root_canonical")).toString();
    idx.generatedAtMs = static_cast<qint64>(
        obj.value(QStringLiteral("generated_at_ms")).toDouble());
    idx.docsTruncated = obj.value(QStringLiteral("docs_truncated")).toBool();
    for (const QJsonValue &v : obj.value(QStringLiteral("docs")).toArray()) {
        const QJsonObject d = v.toObject();
        DocEntry de;
        de.path    = d.value(QStringLiteral("path")).toString();
        de.id      = d.value(QStringLiteral("id")).toString();
        de.title   = d.value(QStringLiteral("title")).toString();
        de.status  = d.value(QStringLiteral("status")).toString();
        de.lines   = d.value(QStringLiteral("lines")).toInt();
        de.mtimeMs = static_cast<qint64>(
            d.value(QStringLiteral("mtime_ms")).toDouble());
        for (const QJsonValue &hv : d.value(QStringLiteral("headings")).toArray()) {
            const QJsonObject ho = hv.toObject();
            Heading h;
            h.level = ho.value(QStringLiteral("level")).toInt();
            h.text  = ho.value(QStringLiteral("text")).toString();
            h.line  = ho.value(QStringLiteral("line")).toInt();
            de.headings << h;
        }
        for (const QJsonValue &lv : d.value(QStringLiteral("links")).toArray())
            de.links << lv.toString();
        idx.docs << de;
    }
    return idx;
}

QString cachePathFor(const QString &rootCanonical) {
    const QString base = QStandardPaths::writableLocation(
        QStandardPaths::GenericCacheLocation);
    return base + QStringLiteral("/ants-terminal/docs-index/")
         + SessionMemoryEngine::cwdHash(rootCanonical) + QStringLiteral(".json");
}

QJsonObject serve(const QString &rootCanonical, qint64 nowMs,
                  const QueryParams &params, const Options &opts,
                  const QString &cachePathOverride) {
    const QString cachePath =
        cachePathOverride.isEmpty() ? cachePathFor(rootCanonical) : cachePathOverride;

    Index idx;
    int refreshed = 0;
    bool needWrite = false;

    // Load + validate the cache (cold-build triggers: absent / unparseable /
    // version mismatch / root mismatch — INV-14).
    bool haveValid = false;
    Index prev;
    {
        QFile f(cachePath);
        if (f.open(QIODevice::ReadOnly)) {
            QJsonParseError pe{};
            const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
            if (pe.error == QJsonParseError::NoError && doc.isObject()) {
                Index loaded = fromJson(doc.object());
                if (loaded.version == kIndexVersion
                    && loaded.rootCanonical == rootCanonical) {
                    prev = loaded;
                    haveValid = true;
                }
            }
        }
    }

    if (haveValid) {
        // ANTS-2198 — walkDocs() (loads .ants/project.json) + the stale set are
        // computed ONCE and threaded into refreshWith, instead of staleDocs +
        // refresh each re-walking the docs tree.
        const QStringList docs = walkDocs(rootCanonical);
        const StaleSet ss = staleDocsWith(prev, rootCanonical, docs);
        if (ss.any()) {
            idx = refreshWith(prev, rootCanonical, nowMs, opts, &refreshed, ss, docs);
            needWrite = true;
        } else {
            idx = prev;  // fully warm — no write, generated_at_ms preserved
        }
    } else {
        idx = build(rootCanonical, nowMs, opts);
        refreshed = idx.docs.size();
        needWrite = true;
    }

    if (needWrite) {
        QDir().mkpath(QFileInfo(cachePath).absolutePath());
        QSaveFile sf(cachePath);
        if (sf.open(QIODevice::WriteOnly)) {
            sf.write(QJsonDocument(toJson(idx)).toJson(QJsonDocument::Compact));
            sf.commit();
        }
    }

    return query(idx, params, refreshed, cachePath, opts);
}

}  // namespace DocsIndex
