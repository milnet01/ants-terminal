// ANTS-1287: see roadmapindex.h.

#include "roadmapindex.h"

#include <QChar>
#include <QStringList>

#include <algorithm>

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

// ANTS-1442 — descendant-aware tally rollup. A child section's
// `[lineStart, lineEnd)` is fully nested inside its parent's by
// construction in buildIndex (lineEnd extends to the next heading
// with level <= self.level). So `child` is a descendant of `parent`
// iff child.lineStart > parent.lineStart && child.lineEnd <=
// parent.lineEnd. We treat each section as its own descendant so the
// emitted tally includes self.
QHash<QString, SectionCounts> rollupCounts(
    const QVector<Section> &index,
    const QHash<QString, SectionCounts> &direct) {
    QHash<QString, SectionCounts> out;
    out.reserve(index.size());

    // ANTS-1783 — linear stack pass instead of the O(N²) all-pairs
    // containment scan (~30k checks on the live ROADMAP per
    // roadmap_query rollup). Sections form a tree by [lineStart,
    // lineEnd] containment; walking them in lineStart order with a
    // stack of open ancestors lets each section's direct counts bubble
    // up to every ancestor in amortised O(1). A copy is sorted first so
    // we don't depend on buildIndex's emission order.
    QVector<const Section *> ordered;
    ordered.reserve(index.size());
    for (const auto &s : index) ordered.append(&s);
    std::sort(ordered.begin(), ordered.end(),
              [](const Section *a, const Section *b) {
                  if (a->lineStart != b->lineStart)
                      return a->lineStart < b->lineStart;
                  // Wider span first on a tie so a parent is pushed
                  // before a same-start child.
                  return a->lineEnd > b->lineEnd;
              });

    struct Frame { const Section *sec; SectionCounts agg; };
    QVector<Frame> stack;
    auto addInto = [](SectionCounts &dst, const SectionCounts &src) {
        dst.active        += src.active;
        dst.shipped       += src.shipped;
        dst.total         += src.total;
        dst.activeWithId  += src.activeWithId;   // ANTS-1622 parallels
        dst.shippedWithId += src.shippedWithId;
        dst.totalWithId   += src.totalWithId;
    };
    auto closeFrame = [&](const Frame &f) {
        out.insert(f.sec->slug, f.agg);
        if (!stack.isEmpty()) addInto(stack.last().agg, f.agg);
    };

    for (const Section *s : ordered) {
        // Pop ancestors that end at or before this section begins —
        // their subtree is complete. lineEnd is EXCLUSIVE, so a sibling
        // whose lineStart equals the previous section's lineEnd (e.g.
        // [10,50) then [50,100)) is NOT nested: pop on `<=`, not `<`.
        while (!stack.isEmpty() &&
               stack.last().sec->lineEnd <= s->lineStart) {
            closeFrame(stack.takeLast());
        }
        // Seed with the section's own direct counts (covers "self").
        SectionCounts seed;
        const auto it = direct.constFind(s->slug);
        if (it != direct.cend()) seed = it.value();
        stack.append(Frame{s, seed});
    }
    while (!stack.isEmpty()) closeFrame(stack.takeLast());
    return out;
}

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
