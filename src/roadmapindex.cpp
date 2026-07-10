// ANTS-1287: see roadmapindex.h.

#include "roadmapindex.h"

#include <QChar>
#include <QRegularExpression>
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

// ANTS-1688 — see header. Anchored full match on the canonical
// allocated-ID shape; ANTS-1249 static-const lambda-init so the
// pattern compiles once.
bool isCanonicalId(const QString &id) {
    static const QRegularExpression rxCanonicalId = [] {
        // ANTS-3492 — prefix may be digit-led if it contains ≥1 letter
        // (3D_E-0042); a letter-free token (2026-07) is still rejected.
        QRegularExpression re(QStringLiteral(
            "^(?=[A-Za-z0-9_-]*[A-Za-z])[A-Za-z0-9][A-Za-z0-9_-]*-[0-9]+$"));
        re.optimize();
        return re;
    }();
    return rxCanonicalId.match(id).hasMatch();
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

// ANTS-1442 — descendant-aware tally rollup moved to a header-only
// template (ANTS-1693) so RoadmapDialog can reuse the identical
// tree-walk with its own count shape. See roadmapindex.h.

QString sliceSection(const QString &markdown, const Section &section) {
    if (section.lineStart < 0 || section.lineEnd <= section.lineStart) {
        return {};
    }
    // ANTS-1844 — walk newlines to the char span of [lineStart, lineEnd)
    // and return one mid(), instead of split('\n')-ing the whole ~19k-line
    // ROADMAP (allocating ~19k QStrings + a second QStringList) just to
    // extract one contiguous run. roadmap_query already re-entered this on
    // every section+body call. Byte-identical to the old
    // lines[lineStart..hi-1].join('\n'):
    //   • lo  = first char of `lineStart` (offset just past its leading \n)
    //   • end = the \n that begins `lineEnd` (excluded), or end-of-string
    //     when `lineEnd` is at/after the last line — which reproduces the
    //     trailing-newline part that split() would have emitted.
    const int n = markdown.size();
    int line = 0;
    int lo = (section.lineStart == 0) ? 0 : -1;
    int end = n;
    for (int i = 0; i < n; ++i) {
        if (markdown.at(i) != QLatin1Char('\n')) continue;
        ++line;
        if (line == section.lineStart) {
            lo = i + 1;
        } else if (line == section.lineEnd) {
            end = i;  // the separator before lineEnd — excluded from mid()
            break;
        }
    }
    if (lo < 0 || lo > end) return {};  // lineStart past end-of-document
    return markdown.mid(lo, end - lo);
}

}  // namespace RoadmapIndex
