// ANTS-1287: see roadmapindex.h.

#include "roadmapindex.h"

#include <QChar>
#include <QStringList>

namespace RoadmapIndex {

// Moved from roadmapdialog.cpp (anonymous-namespace static) — see
// ANTS-1287 § 7. Body is byte-identical so existing parseBullets /
// extractToc / renderCardsHtml callers see no behaviour change.
int headingLevel(const QString &raw, QString *text) {
    if (raw.startsWith(QStringLiteral("#### "))) {
        if (text) *text = raw.mid(5);
        return 4;
    }
    if (raw.startsWith(QStringLiteral("### "))) {
        if (text) *text = raw.mid(4);
        return 3;
    }
    if (raw.startsWith(QStringLiteral("## "))) {
        if (text) *text = raw.mid(3);
        return 2;
    }
    if (raw.startsWith(QStringLiteral("# "))) {
        if (text) *text = raw.mid(2);
        return 1;
    }
    return 0;
}

// Moved from roadmapdialog.cpp:485. Lowercase + non-alnum→'-' + trim
// trailing '-'. Skips leading dashes via `prevDash = true` seed.
QString slugifyHeading(const QString &heading) {
    QString out;
    out.reserve(heading.size());
    bool prevDash = true;
    for (QChar c : heading) {
        const QChar n = c.toLower();
        if (n.isLetterOrNumber()) {
            out.append(n);
            prevDash = false;
        } else if (!prevDash) {
            out.append('-');
            prevDash = true;
        }
    }
    while (out.endsWith('-')) out.chop(1);
    return out;
}

// Moved from roadmapdialog.cpp:511. Walk-order rule for duplicate
// headings: the Nth occurrence of "Performance" gets "performance",
// "performance-2", "performance-3", ... — both consumers (parseBullets
// and buildIndex) call this in the same document order so slugs line up.
QString uniqueSlug(QSet<QString> &seen, const QString &heading) {
    const QString base = slugifyHeading(heading);
    if (base.isEmpty()) return base;
    if (!seen.contains(base)) {
        seen.insert(base);
        return base;
    }
    int n = 2;
    QString candidate;
    do {
        candidate = base + QStringLiteral("-") + QString::number(n++);
    } while (seen.contains(candidate));
    seen.insert(candidate);
    return candidate;
}

QVector<Section> buildIndex(const QString &markdown) {
    QVector<Section> out;
    const QStringList lines = markdown.split('\n');
    QSet<QString> seen;

    // First pass: emit one Section per H2/H3 with lineStart set;
    // lineEnd is fixed up in the second pass.
    for (int i = 0; i < lines.size(); ++i) {
        QString headingText;
        const int level = headingLevel(lines[i], &headingText);
        if (level != 2 && level != 3) continue;  // INV-3
        Section s;
        s.slug        = uniqueSlug(seen, headingText);
        s.headingText = headingText;
        s.level       = level;
        s.lineStart   = i;
        s.lineEnd     = -1;  // patched below
        out.append(s);
    }

    // Second pass: each section ends at the next heading with
    // level <= its own. Last section ends at total_lines.
    for (int i = 0; i < out.size(); ++i) {
        int end = lines.size();
        for (int j = i + 1; j < out.size(); ++j) {
            if (out[j].level <= out[i].level) {
                end = out[j].lineStart;
                break;
            }
        }
        out[i].lineEnd = end;
    }

    return out;
}

const Section *findBySlug(const QVector<Section> &index, const QString &slug) {
    if (slug.isEmpty()) return nullptr;
    for (const auto &s : index) {
        if (s.slug == slug) return &s;
    }
    return nullptr;
}

QString sliceSection(const QString &markdown, const Section &section) {
    if (section.lineStart < 0 || section.lineEnd <= section.lineStart) {
        return {};
    }
    const QStringList lines = markdown.split('\n');
    const int hi = qMin(section.lineEnd, lines.size());
    if (section.lineStart >= hi) return {};
    QStringList out;
    out.reserve(hi - section.lineStart);
    for (int i = section.lineStart; i < hi; ++i) {
        out.append(lines[i]);
    }
    return out.join('\n');
}

}  // namespace RoadmapIndex
