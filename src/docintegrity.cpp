// ANTS-3601 — see docintegrity.h for the contract.

#include "docintegrity.h"

#include "markdownscan.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QVector>

#include <algorithm>

namespace DocIntegrity {

namespace {

// A markdown inline link `[text](target)`. Targets containing `)` are not
// supported (vanishingly rare in doc links, and matching them needs a full
// parser) — the `[^)]*` body matches to the first `)`.
const QRegularExpression &linkRe() {
    static const QRegularExpression re(QStringLiteral(R"(\[[^\]]*\]\(([^)]*)\))"));
    return re;
}

// ATX heading: 1-6 leading `#` then at least one space then the text.
const QRegularExpression &atxRe() {
    static const QRegularExpression re(QStringLiteral(R"(^(#{1,6})[ \t]+(.*)$)"));
    return re;
}

// Trailing ATX closing-hash sequence (` ##` at end of a heading) — stripped
// before slugging, matching docsindex's scanDoc.
const QRegularExpression &trailHashRe() {
    static const QRegularExpression re(QStringLiteral(R"([ \t]+#+[ \t]*$)"));
    return re;
}

// A markdown list item: bullet (`-`/`*`/`+`) or ordered (`N.`/`N)`) marker,
// at any indent, followed by whitespace. (A `---` rule has no space, so it is
// not a list item.)
bool isListItem(const QString &line) {
    static const QRegularExpression re(QStringLiteral(R"(^\s*([-*+]|\d+[.)])\s)"));
    return re.match(line).hasMatch();
}

struct Heading {
    QString slug;
    int     level;
    int     line;   // 1-based
    QString text;
};

struct Link {
    QString target;  // raw target inside the parens, incl. any #anchor
    int     line;    // 1-based
};

// Everything parsed from one doc, bounded by the Options caps.
struct DocData {
    QString        rel;
    QString        absDir;
    QSet<QString>  slugs;      // fence-aware heading slugs
    QList<Heading> headings;   // in document order (for TOC coverage)
    QList<Link>    links;
    // TOC region (check 3); tocEndLine < 0 → no recognisable TOC.
    QList<QPair<QString, int>> tocEntries;  // (slug, line)
    int            tocEndLine = -1;         // 1-based last line of the TOC run
};

QList<Heading> extractHeadings(const QStringList &lines,
                               const QVector<bool> &fence, int cap) {
    QList<Heading> out;
    QHash<QString, int> seen;
    for (int i = 0; i < lines.size() && out.size() < cap; ++i) {
        if (fence.value(i)) continue;
        const auto m = atxRe().match(lines[i]);
        if (!m.hasMatch()) continue;
        const int level = m.captured(1).size();
        QString text = m.captured(2);
        text.remove(trailHashRe());
        text = text.trimmed();
        const QString slug = gfmSlug(text, seen);
        out.append({slug, level, i + 1, text});
    }
    return out;
}

// ANTS-3623 — blank out inline-code spans so link harvesting ignores them.
// Fenced blocks were already skipped via fenceMask, but INLINE code was not,
// and that was the dominant source of false positives: 21 of the 22
// broken_link findings over this project's own doc tree were back-ticked
// prose, 12 of them in doc_integrity's OWN spec, which necessarily contains
// `[text](target)` examples to describe what the checker does. A checker
// that flags the document explaining it is a checker nobody can gate on.
//
// Replaces the span (backticks included) with spaces rather than deleting
// it, so a real link whose TEXT is code — [`a/b.md`](a/b.md) — still parses:
// the brackets and the target sit outside the span and survive. Follows the
// CommonMark rule that a span closes on a backtick run of equal length.
QString maskInlineCode(const QString &line) {
    QString out = line;
    const int n = out.size();
    int i = 0;
    while (i < n) {
        if (out.at(i) != QLatin1Char('`')) { ++i; continue; }
        const int openStart = i;
        int openLen = 0;
        while (i < n && out.at(i) == QLatin1Char('`')) { ++i; ++openLen; }
        int j = i;
        bool closed = false;
        while (j < n) {
            if (out.at(j) != QLatin1Char('`')) { ++j; continue; }
            int closeLen = 0;
            while (j < n && out.at(j) == QLatin1Char('`')) { ++j; ++closeLen; }
            if (closeLen == openLen) { closed = true; break; }
        }
        if (!closed) break;   // unterminated run — leave the remainder alone
        for (int k = openStart; k < j; ++k) out[k] = QLatin1Char(' ');
        i = j;
    }
    return out;
}

QList<Link> extractLinks(const QStringList &lines,
                         const QVector<bool> &fence, int cap) {
    QList<Link> out;
    for (int i = 0; i < lines.size() && out.size() < cap; ++i) {
        if (fence.value(i)) continue;
        const QString scanLine = maskInlineCode(lines[i]);
        auto it = linkRe().globalMatch(scanLine);
        while (it.hasNext() && out.size() < cap) {
            const auto m = it.next();
            out.append({m.captured(1).trimmed(), i + 1});
        }
    }
    return out;
}

// Locate a TOC region (§ 2.5) and populate d.tocEntries + d.tocEndLine.
void detectToc(const QStringList &lines, DocData &d) {
    int contentsLine = -1;  // 1-based line of the `## Contents` heading
    for (const Heading &h : d.headings) {
        if (h.slug == QLatin1String("contents") ||
            h.slug == QLatin1String("table-of-contents")) {
            contentsLine = h.line;
            break;
        }
    }
    if (contentsLine < 0) return;  // no TOC heading → no region

    const int n = lines.size();
    int idx = contentsLine;  // 0-based index of the line AFTER the heading
    while (idx < n && lines[idx].trimmed().isEmpty()) ++idx;  // skip blanks
    if (idx >= n || !isListItem(lines[idx])) return;  // no list → no region

    int lastItem = -1;
    for (int j = idx; j < n; ++j) {
        if (lines[j].trimmed().isEmpty()) continue;  // blank within a loose list
        if (!isListItem(lines[j])) break;            // run ends
        lastItem = j + 1;                            // 1-based
        // Collect the item's in-doc anchor entries (a linkless item is skipped
        // but does not end the run).
        auto it = linkRe().globalMatch(lines[j]);
        while (it.hasNext()) {
            const QString target = it.next().captured(1).trimmed();
            if (target.startsWith('#'))
                d.tocEntries.append({target.mid(1), j + 1});
        }
    }
    d.tocEndLine = lastItem;
}

// Resolve a relative link target (the file part, `#anchor` already stripped by
// the caller) to an absolute path under root. Returns {skip=true} for external
// / absolute / root-escaping / pure-anchor targets (no finding, no probe).
struct Resolved {
    bool    skip;
    QString absPath;
};
Resolved resolveRelative(const QString &rootCanonical, const QString &docAbsDir,
                         const QString &filePart) {
    if (filePart.isEmpty()) return {true, {}};                 // pure anchor
    if (filePart.contains(QLatin1String("://"))) return {true, {}};  // external
    if (filePart.startsWith(QLatin1String("mailto:"))) return {true, {}};
    if (filePart.startsWith('/')) return {true, {}};           // absolute → skip
    const QString joined = QDir(docAbsDir).filePath(filePart);
    const QString clean = QDir::cleanPath(joined);
    const QString rel = QDir(rootCanonical).relativeFilePath(clean);
    if (rel.startsWith(QLatin1String("../"))) return {true, {}};  // escapes root
    return {false, clean};
}

}  // namespace

QString gfmSlug(const QString &headingText, QHash<QString, int> &seen) {
    QString s = headingText;
    s.remove('`');          // 1. drop backtick code-span markers, keep text
    s = s.toLower();        // 2. lowercase
    QString base;           // 3. keep only letter/digit/space/'-'/'_'
    base.reserve(s.size());
    for (const QChar c : s) {
        if (c.isLetterOrNumber() || c == ' ' || c == '-' || c == '_')
            base += c;
    }
    base.replace(' ', '-');  // 4. spaces→'-', no collapse, no trim
    const int n = seen.value(base, 0);  // 5. duplicate suffix (2nd → -1)
    seen.insert(base, n + 1);
    if (n == 0) return base;
    return base + '-' + QString::number(n);
}

QString gfmSlug(const QString &headingText) {
    QHash<QString, int> seen;
    return gfmSlug(headingText, seen);
}

QList<Finding> check(const QString &rootCanonical, const QStringList &relDocs,
                     const Options &opts, QStringList *checkedDocs) {
    const QDir rootDir(rootCanonical);

    // ---- Phase A — parse every openable in-scope doc (capped) ---------------
    QHash<QString, DocData> byRel;   // normalized rel → parsed data
    QStringList order;               // parse/emit order
    QSet<QString> inScope;           // normalized rel set (cross-doc anchor gate)
    for (const QString &raw : relDocs) {
        if (order.size() >= opts.maxDocsPerRun) break;
        const QString abs = QDir::isAbsolutePath(raw)
                                ? QDir::cleanPath(raw)
                                : QDir::cleanPath(rootDir.filePath(raw));
        const QString rel = rootDir.relativeFilePath(abs);
        if (byRel.contains(rel)) continue;  // dedup
        QFile f(abs);
        if (!f.open(QIODevice::ReadOnly)) continue;  // INV-15 — silently skip
        const QByteArray bytes = f.read(opts.maxDocBytes);
        f.close();
        const QStringList lines = QString::fromUtf8(bytes).split('\n');
        const QVector<bool> fence = MarkdownScan::fenceMask(lines);

        DocData d;
        d.rel = rel;
        d.absDir = QFileInfo(abs).absolutePath();
        d.headings = extractHeadings(lines, fence, opts.maxHeadingsPerDoc);
        for (const Heading &h : d.headings) d.slugs.insert(h.slug);
        d.links = extractLinks(lines, fence, opts.maxLinksPerDoc);
        detectToc(lines, d);

        byRel.insert(rel, d);
        inScope.insert(rel);
        order.append(rel);
        if (checkedDocs) checkedDocs->append(rel);
    }

    // ---- Phase B — run the three checks over each parsed doc ----------------
    QList<Finding> findings;
    for (const QString &rel : order) {
        const DocData &d = byRel.value(rel);

        // Checks 1 (dead anchors) + 2 (broken links) over the doc's links.
        for (const Link &link : d.links) {
            const QString target = link.target;
            const int hashIdx = target.indexOf('#');
            const QString filePart = hashIdx >= 0 ? target.left(hashIdx) : target;
            const QString anchor = hashIdx >= 0 ? target.mid(hashIdx + 1) : QString();

            if (filePart.isEmpty()) {
                // Pure in-doc anchor.
                if (!anchor.isEmpty() && !d.slugs.contains(anchor))
                    findings.append({Kind::DeadAnchor, rel, link.line,
                                     QStringLiteral("anchor #%1 resolves to no "
                                                    "heading in this doc").arg(anchor)});
                continue;
            }

            const Resolved r = resolveRelative(rootCanonical, d.absDir, filePart);
            if (r.skip) continue;  // external / absolute / escape
            if (!QFileInfo::exists(r.absPath)) {
                findings.append({Kind::BrokenLink, rel, link.line,
                                 QStringLiteral("link target `%1` does not exist")
                                     .arg(filePart)});
                continue;  // missing file — no anchor check
            }
            // File exists — cross-doc anchor only checked if the target is in scope.
            if (!anchor.isEmpty()) {
                const QString relTarget = rootDir.relativeFilePath(r.absPath);
                if (inScope.contains(relTarget) &&
                    !byRel.value(relTarget).slugs.contains(anchor)) {
                    findings.append({Kind::DeadAnchor, rel, link.line,
                                     QStringLiteral("anchor #%1 resolves to no "
                                                    "heading in %2").arg(anchor, relTarget)});
                }
            }
        }

        // Check 3 — TOC coverage (only when a TOC region with at least one
        // `#anchor` entry was found). Coverage is matched by slug, so a TOC
        // whose items are all plain text (e.g. `- §1 Problem`) offers nothing
        // to match and would report every section missing (ANTS-3634).
        if (d.tocEndLine >= 0 && !d.tocEntries.isEmpty()) {
            QHash<QString, int> firstSeen;
            for (const auto &e : d.tocEntries) {
                if (firstSeen.contains(e.first)) {
                    findings.append({Kind::TocGap, rel, e.second,
                                     QStringLiteral("duplicate TOC entry '%1'")
                                         .arg(e.first)});
                } else {
                    firstSeen.insert(e.first, e.second);
                }
            }
            QSet<QString> entrySlugs;
            for (const auto &e : d.tocEntries) entrySlugs.insert(e.first);
            for (const Heading &h : d.headings) {
                if (h.level != 2) continue;
                if (h.line <= d.tocEndLine) continue;  // before/within TOC region
                if (!entrySlugs.contains(h.slug))
                    findings.append({Kind::TocGap, rel, h.line,
                                     QStringLiteral("section '%1' missing from TOC")
                                         .arg(h.text)});
            }
        }
    }

    std::stable_sort(findings.begin(), findings.end(),
                     [](const Finding &a, const Finding &b) {
                         if (a.file != b.file) return a.file < b.file;
                         return a.line < b.line;
                     });
    return findings;
}

}  // namespace DocIntegrity
