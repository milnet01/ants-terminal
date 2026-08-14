// ANTS-3601 — see docintegrity.h for the contract.

#include "docintegrity.h"

#include "markdownscan.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMap>
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

// ANTS-3700 — the leading section number of a heading, as its components:
// "5.7 Emission model" → [5, 7]; "5. Problem" → [5]; "Overview" → {}. A single
// trailing dot is consumed ("## 5. Problem" is the dominant form in this
// corpus, and the shape a strict `\d+(\.\d+)*\s` would miss). Anything that
// is not digits-and-dots up to the first space simply has no number, which is
// how prose headings opt out.
QList<int> headingNumber(const QString &text) {
    static const QRegularExpression re(
        QStringLiteral(R"(^(\d+(?:\.\d+)*)\.?(?:\s|$))"));
    const auto m = re.match(text);
    if (!m.hasMatch()) return {};
    QList<int> out;
    bool ok = true;
    for (const QString &part : m.captured(1).split(QLatin1Char('.'))) {
        const int v = part.toInt(&ok);
        if (!ok) return {};   // overflow — treat as unnumbered, not as 0
        out.append(v);
    }
    return out;
}

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
    // ANTS-3719 — (1-based line, verb) per ungranted MCP verb, first mention
    // only. Empty for every doc that is not a skill file with an
    // `allowed-tools:` frontmatter key, which is almost all of them.
    QList<QPair<int, QString>> ungranted;
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
//
// ANTS-3635(a) — the mask runs over the whole document, not line by line,
// because a code span may cross a newline (CommonMark § 6.1). Masking each
// line in isolation left the tail of a multi-line span exposed:
// docs/specs/ANTS-1150.md wraps a C++ lambda in a span that opens on :197
// and closes on :198, so `[this](int idx)` was harvested as a link.
//
// ANTS-3649 — the span-finding half now lives in MarkdownScan::codeSpans
// (whole-document scan, forward closing-run search bounded by blank and fenced
// lines, unmatched run = literal text). What is left here is the *policy*: this
// verb masks spans out, where doc_citations reads them.
QStringList maskInlineCode(const QStringList &lines,
                           const QVector<bool> &fence) {
    QStringList out = lines;
    for (const MarkdownScan::CodeSpan &s : MarkdownScan::codeSpans(lines, fence)) {
        const int from = s.startCol - s.delimLen;  // first char of the open run
        const int to   = s.endCol + s.delimLen;    // one past the close run
        if (s.startLine == s.endLine) {
            for (int k = from; k < to; ++k) out[s.startLine][k] = QLatin1Char(' ');
            continue;
        }
        // Multi-line span: this line's tail, each whole line between, and the
        // closing line's head.
        for (int k = from; k < out.at(s.startLine).size(); ++k)
            out[s.startLine][k] = QLatin1Char(' ');
        for (int m = s.startLine + 1; m < s.endLine; ++m)
            out[m].fill(QLatin1Char(' '));
        for (int k = 0; k < to; ++k) out[s.endLine][k] = QLatin1Char(' ');
    }
    return out;
}

QList<Link> extractLinks(const QStringList &lines,
                         const QVector<bool> &fence, int cap) {
    const QStringList scan = maskInlineCode(lines, fence);
    QList<Link> out;
    for (int i = 0; i < scan.size() && out.size() < cap; ++i) {
        if (fence.value(i)) continue;
        auto it = linkRe().globalMatch(scan.at(i));
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
        // ANTS-3836 — an INDENTED line that is not itself a list item is a
        // CONTINUATION of the item above (CommonMark: a wrapped item's later
        // lines sit at the item's content column), not the end of the run.
        // Breaking here truncated the TOC at the first wrapped entry, and every
        // section below it was then reported as missing from a Contents list
        // that named it — six such findings on ANTS-1870, whose entry 2 wraps
        // onto lines beginning "2.4 delta ·". Those continuation lines match no
        // ordered-list pattern (`2.4` is not `2.` + space), so the run ended at
        // entry 2. Still scanned for anchors below: a wrapped entry can carry
        // its link on the second line.
        const bool continuation =
            lastItem >= 0 && !lines[j].isEmpty() && lines[j].at(0).isSpace();
        if (!isListItem(lines[j]) && !continuation) break;   // run ends
        if (!continuation) lastItem = j + 1;                 // 1-based
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

// ANTS-3700 — check 4. Siblings are headings sharing a numeric parent prefix
// ([5] and [6] are siblings under ""; [5,1] and [5,2] under "5"), which is the
// grouping the numbering itself asserts — stricter and cleaner than matching
// on `#` depth, since a doc may write `## 5.` and `### 5.1` at different
// levels while meaning one hierarchy.
//
// Two deliberate limits, both to keep the check quiet enough to gate on:
//
//   * A group's FIRST heading is never flagged. A doc whose sections start at
//     2 (or 0) may be an excerpt or use appendix numbering; "should have begun
//     at 1" is a guess, and a guess repeated on every doc is what makes a
//     checker something people filter out.
//   * A skipped number that turns up LATER under the same parent is not a
//     gap — it is the out-of-order heading, already reported where it sits.
//     Without this, the commonest shape of the defect (two siblings swapped)
//     reports twice: once as a hole at 5.8, once as a reversal at 5.7.
void checkHeadingSequence(const QString &rel, const QList<Heading> &headings,
                          QList<Finding> &out) {
    struct Group {
        QSet<int>  present;   // every number seen under this parent, any order
        QList<int> order;     // document order of those numbers
        QList<int> lines;
    };
    QMap<QString, Group> groups;   // parent prefix ("", "5", "5.1") → group
    QHash<QString, QString> label; // parent prefix → its heading text, for msgs

    for (const Heading &h : headings) {
        if (h.level < 2) continue;   // H1 is the doc title, not a section
        const QList<int> num = headingNumber(h.text);
        if (num.isEmpty()) continue;
        QStringList parentParts;
        for (int i = 0; i < num.size() - 1; ++i)
            parentParts << QString::number(num[i]);
        const QString parent = parentParts.join(QLatin1Char('.'));
        Group &g = groups[parent];
        g.present.insert(num.last());
        g.order.append(num.last());
        g.lines.append(h.line);
        label.insert(parent, h.text);
    }

    // Emit in document order across all groups, so findings interleave the way
    // the sort at the end of check() expects.
    QList<Finding> local;
    for (auto it = groups.constBegin(); it != groups.constEnd(); ++it) {
        const QString &parent = it.key();
        const Group   &g      = it.value();
        const QString  prefix = parent.isEmpty()
                                    ? QString()
                                    : parent + QLatin1Char('.');
        QSet<int> seen;
        int prev = 0, high = 0;
        for (int i = 0; i < g.order.size(); ++i) {
            const int  n    = g.order[i];
            const int  line = g.lines[i];
            if (i == 0) { seen.insert(n); prev = high = n; continue; }

            if (seen.contains(n)) {
                local.append({Kind::HeadingSequence, rel, line,
                              QStringLiteral("duplicate section number %1%2")
                                  .arg(prefix).arg(n)});
            } else if (n < prev) {
                local.append({Kind::HeadingSequence, rel, line,
                              QStringLiteral("section %1%2 is out of order — "
                                             "it follows %1%3")
                                  .arg(prefix).arg(n).arg(prev)});
            } else if (n > high + 1) {
                QStringList missing;
                for (int k = high + 1; k < n; ++k)
                    if (!g.present.contains(k))
                        missing << prefix + QString::number(k);
                if (!missing.isEmpty()) {
                    local.append({Kind::HeadingSequence, rel, line,
                                  QStringLiteral("section %1%2 skips %3")
                                      .arg(prefix).arg(n)
                                      .arg(missing.join(QStringLiteral(", ")))});
                }
            }
            seen.insert(n);
            prev = n;
            high = std::max(high, n);
        }
    }
    out.append(local);
}

// ANTS-3719 — a Claude Code skill declares the tools it may use in an
// `allowed-tools:` YAML frontmatter key, and its body names the MCP verbs its
// procedure requires. When the two drift the skill is unexecutable exactly as
// written. Single-file, self-consistency, fully deterministic — the shape this
// engine exists for.
//
// Three deliberate narrowings, each costing recall to buy precision, because a
// false positive on a checker nobody can gate on is worse than a missed one:
//
//   * Gated on the frontmatter actually carrying `allowed-tools:`. That is
//     self-scoping — only a skill file has one — so no other markdown in a
//     docs tree is touched, and a skill that grants nothing declares nothing
//     to contradict.
//   * Matches the fully-qualified `mcp__ants__<verb>` spelling ONLY. A bare
//     backticked verb name needs a known-verb list to tell a mandate from
//     prose that merely mentions one, and this engine has no such list.
//   * Reads the granted set from `allowed-tools:`' own value, not the whole
//     frontmatter, so a `description:` naming a verb cannot silently grant it.
//
// Fence-aware like every other check here (INV-3). Measured over the live
// ~/.claude corpus when this shipped: of the 13 skills naming a verb, all 99
// mentions sit outside fences, so the mask changes no current result and is
// consistency rather than a judgement call.
QList<QPair<int, QString>> detectUngrantedTools(const QStringList &lines,
                                                const QVector<bool> &fence) {
    static const QRegularExpression verbRe(QStringLiteral("mcp__ants__[a-z0-9_]+"));
    static const QRegularExpression topKeyRe(QStringLiteral("^[A-Za-z][A-Za-z0-9_-]*:"));
    QList<QPair<int, QString>> out;
    if (lines.isEmpty() || lines.first().trimmed() != QLatin1String("---"))
        return out;  // no frontmatter → not a skill file
    int fmEnd = -1;
    for (int i = 1; i < lines.size(); ++i)
        if (lines.at(i).trimmed() == QLatin1String("---")) { fmEnd = i; break; }
    if (fmEnd < 0) return out;  // unterminated frontmatter — parse nothing

    // The `allowed-tools:` value: its own line, plus any continuation lines
    // before the next top-level key (a YAML block list). Both the inline
    // `[A, B]` and the `- A` block forms fall out of collecting every verb
    // token in that span.
    bool inValue = false, hasAllowed = false;
    QSet<QString> granted;
    for (int i = 1; i < fmEnd; ++i) {
        const QString &l = lines.at(i);
        if (l.startsWith(QLatin1String("allowed-tools:"))) {
            hasAllowed = true;
            inValue = true;
        } else if (inValue && topKeyRe.match(l).hasMatch()) {
            inValue = false;  // next top-level key ends the value
        }
        if (!inValue) continue;
        auto it = verbRe.globalMatch(l);
        while (it.hasNext()) granted.insert(it.next().captured(0));
    }
    if (!hasAllowed) return out;

    QSet<QString> reported;  // one finding per verb, at its first mention
    for (int i = fmEnd + 1; i < lines.size(); ++i) {
        if (i < fence.size() && fence.at(i)) continue;
        auto it = verbRe.globalMatch(lines.at(i));
        while (it.hasNext()) {
            const QString v = it.next().captured(0);
            if (granted.contains(v) || reported.contains(v)) continue;
            reported.insert(v);
            out.append({i + 1, v});
        }
    }
    return out;
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
        d.ungranted = detectUngrantedTools(lines, fence);  // ANTS-3719

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
            // ANTS-4102 — a target carrying an unescaped `<`/`>` is a template
            // placeholder by construction (`../specs/<ID>-<topic>.md`), so no
            // filesystem path will ever match it and this check can never come
            // back clean on a project that keeps a doc skeleton. Local Web
            // Server Manager had documented the permanent finding as a known
            // false positive in its own workflow doc — the shape of a check
            // that has stopped carrying information. Needs no project config.
            if (target.contains(QLatin1Char('<')) || target.contains(QLatin1Char('>')))
                continue;
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

        // Check 4 — numbered-heading sequence (ANTS-3700).
        checkHeadingSequence(rel, d.headings, findings);

        // Check 5 — a skill calling an MCP verb it never granted (ANTS-3719).
        for (const auto &u : d.ungranted)
            findings.append({Kind::UngrantedTool, rel, u.first,
                             QStringLiteral("`%1` is called here but absent from "
                                            "this skill's allowed-tools")
                                 .arg(u.second)});
    }

    std::stable_sort(findings.begin(), findings.end(),
                     [](const Finding &a, const Finding &b) {
                         if (a.file != b.file) return a.file < b.file;
                         return a.line < b.line;
                     });
    return findings;
}

}  // namespace DocIntegrity
