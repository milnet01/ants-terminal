// ANTS-2022 — apply_edits pure in-memory edit helper. Qt6::Core-only.
// A whole-content substring replace per edit; the wrapper threads one
// edit's result into the next so two edits to one file compose, then
// writes atomically. See docs/specs/ANTS-2022-apply-edits-mcp-tool-n.md.

#include "applyedits.h"

#include "wrapmatch.h"   // ANTS-4723 — the wrapped-match rule, one owner

#include <QStringList>

#include <utility>

namespace ApplyEdits {

namespace {

// ANTS-4418 — collapse runs of horizontal whitespace to one space and drop
// leading and trailing whitespace, so two spellings that differ only in spacing compare
// equal. Deliberately NOT a full normalisation: newlines are left alone, since
// they are what the line split below is keyed on.
QString wsNormalised(const QString &s) {
    QString out;
    out.reserve(s.size());
    bool pendingSpace = false;
    for (const QChar c : s) {
        if (c == QLatin1Char(' ') || c == QLatin1Char('\t')) {
            pendingSpace = !out.isEmpty();
            continue;
        }
        if (pendingSpace) { out.append(QLatin1Char(' ')); pendingSpace = false; }
        out.append(c);
    }
    return out;
}

// ANTS-4857 — fold the punctuation prose is most often re-typed in ASCII:
// any run of hyphens, en and em dashes becomes one '-' (so "--", "-" and "—"
// agree), curly quotes become straight, and an ellipsis becomes "...".
QString punctFolded(const QString &s) {
    QString out;
    out.reserve(s.size());
    for (const QChar c : s) {
        const char16_t u = c.unicode();
        if (u == u'-' || u == u'–' || u == u'—') {
            if (!out.endsWith(QLatin1Char('-'))) out.append(QLatin1Char('-'));
            continue;
        }
        if (u == u'‘' || u == u'’') out.append(QLatin1Char('\''));
        else if (u == u'“' || u == u'”') out.append(QLatin1Char('"'));
        else if (u == u'…') out.append(QStringLiteral("..."));
        else out.append(c);
    }
    return out;
}

// The single line of `lines` whose `norm` form equals `needle`, 1-based, or
// -1 when there is none or more than one: two candidates cannot tell the
// caller which to retry, and naming one arbitrarily is worse than naming none.
template <typename Norm>
int uniqueLineMatching(const QStringList &lines, const QString &needle, Norm norm) {
    int hit = -1;
    for (int i = 0; i < lines.size(); ++i) {
        if (norm(lines.at(i)) != needle) continue;
        if (hit >= 0) return -1;           // ambiguous → report nothing
        hit = i + 1;                       // 1-based, matching read_region
    }
    return hit;
}

// Find where a missed `oldStr` most likely is, so the caller can retry
// without another read. Sets the near-miss fields only on a UNIQUE hit: two
// candidates cannot tell the caller which to retry, and naming one
// arbitrarily is worse than naming none. Two shapes:
//   - a multi-line `oldStr` whose only disagreement with the file is where a
//     hard wrap falls ("§ Run\nhousekeeping" against "§ Run housekeeping\nas"),
//     found by WrapMatch::find (ANTS-4723), kind "wrapped";
//   - a single-line `oldStr` that differs in spacing (ANTS-4418), kind
//     "whitespace", or in spacing and punctuation (ANTS-4857), kind
//     "punctuation".
// Reported on every miss, whether or not `match_wrapped` was asked for: the
// round-trip this saves is the one where the caller does not yet know the
// flag would help.
void findNearMiss(const QString &contents, const QString &oldStr,
                  EditOutcome &r) {
    if (oldStr.contains(QLatin1Char('\n'))) {
        const QVector<WrapMatch::Span> hits = WrapMatch::find(contents, oldStr, 2);
        if (hits.size() != 1) return;
        int line = 1;
        for (int i = 0; i < hits.at(0).start && i < contents.size(); ++i)
            if (contents.at(i) == QLatin1Char('\n')) ++line;
        int ls = hits.at(0).start;
        while (ls > 0 && contents.at(ls - 1) != QLatin1Char('\n')) --ls;
        int le = ls;
        while (le < contents.size() && contents.at(le) != QLatin1Char('\n')) ++le;
        r.nearMissLine = line;
        r.nearMissText = contents.mid(ls, le - ls);
        r.nearMissKind = QStringLiteral("wrapped");
        return;
    }
    const QString needle = wsNormalised(oldStr);
    if (needle.isEmpty()) return;
    const QStringList lines = contents.split(QLatin1Char('\n'));

    // Spacing first, so a spacing-only miss keeps its own kind. ANTS-4857 —
    // then spacing and punctuation together. The fold is coarser, so two
    // spacing matches stay two under it and the guard holds on both passes.
    int hitLine = uniqueLineMatching(lines, needle, wsNormalised);
    const char *kind = "whitespace";
    if (hitLine < 0) {
        const auto fold = [](const QString &s) { return wsNormalised(punctFolded(s)); };
        hitLine = uniqueLineMatching(lines, fold(oldStr), fold);
        kind = "punctuation";
    }
    if (hitLine < 0) return;
    r.nearMissLine = hitLine;
    r.nearMissText = lines.at(hitLine - 1);
    r.nearMissKind = QString::fromLatin1(kind);
}

// ANTS-4473 — record WHERE the N occurrences behind an `ambiguous` are.
// `total` is passed in rather than recounted: it is already known at the call
// site, and two counting methods that can disagree is how a reported total
// stops matching the list beside it.
//
// The walk advances by ONE character, which is what QString::count does, so
// overlapping occurrences (`aa` in `aaa`) are counted the same way the caller's
// uniqueness test counted them. Line numbers are 1-based and name the line the
// match STARTS on — a multi-line `oldStr` has no single line, and its first is
// the one a caller retries from.
void collectOccurrenceLines(const QString &contents, const QString &oldStr,
                            int total, EditOutcome &r) {
    // Enough to disambiguate by eye; a pathological `old` must not turn one
    // skip row into hundreds. matchCount carries the truth either way.
    constexpr int kMaxReported = 10;
    r.matchCount = total;

    int from = 0;
    int line = 1;
    int scanned = 0;   // how far the line counter has consumed
    while (r.matchLines.size() < kMaxReported) {
        const int idx = contents.indexOf(oldStr, from);
        if (idx < 0) break;
        for (int i = scanned; i < idx; ++i)
            if (contents.at(i) == QLatin1Char('\n')) ++line;
        scanned = idx;

        int ls = idx;
        while (ls > 0 && contents.at(ls - 1) != QLatin1Char('\n')) --ls;
        int le = idx;
        while (le < contents.size() && contents.at(le) != QLatin1Char('\n')) ++le;

        r.matchLines.append(line);
        r.matchTexts.append(contents.mid(ls, le - ls));
        from = idx + 1;
    }
}

}  // namespace

EditOutcome applyToContent(const QString &contents, const QString &oldStr,
                           const QString &newStr, bool replaceAll,
                           bool matchWrapped) {
    EditOutcome r;
    // An empty `old` is rejected upstream (bad_args); guard defensively so
    // QString::count("") (which returns size()+1) can't be misread as a hit.
    if (oldStr.isEmpty()) {
        r.skipReason = QStringLiteral("not_found");
        return r;
    }
    const int count = contents.count(oldStr);
    if (count == 0) {
        // ANTS-4723 — the verbatim search found nothing; fall back to the
        // wrapped rule when the caller asked for it. Ordered this way on
        // purpose: the fallback is unreachable while an exact match exists,
        // so no call that succeeds today can change behaviour under the flag.
        if (matchWrapped && !replaceAll) {
            const WrapMatch::Patch p =
                WrapMatch::patchOnce(contents, oldStr, newStr);
            if (p.hits > 1) {
                // A phrase occurring twice under the looser rule is ambiguous,
                // not a licence to take the first — the same guard the
                // verbatim path applies.
                r.skipReason = QStringLiteral("ambiguous");
                r.matchCount = p.hits;
                return r;
            }
            if (p.hits == 1 && !p.structuredBlock) {
                r.applied      = true;
                r.newContents  = p.text;
                r.replacements = 1;
                r.wrappedMatch = true;
                return r;
            }
            // p.structuredBlock — ANTS-4612: the span was found but a
            // continuation line of it carries alignment or opens a list item,
            // which the re-flow would destroy. Declining is the contract, and
            // it falls through to the near-miss report below so the caller is
            // told WHERE rather than merely refused.
        }
        r.skipReason = QStringLiteral("not_found");
        findNearMiss(contents, oldStr, r);
        return r;
    }
    if (count > 1 && !replaceAll) {
        r.skipReason = QStringLiteral("ambiguous");
        collectOccurrenceLines(contents, oldStr, count, r);   // ANTS-4473
        return r;
    }

    QString out = contents;
    if (replaceAll) {
        // ANTS-5103 — replace() makes non-overlapping replacements, while
        // `count` above counts overlapping occurrences (`aa` in `aaa` is 2,
        // replaced once). Report what was replaced; `count` stays the
        // stricter test for the ambiguity check.
        int made = 0;
        for (int from = 0; (from = contents.indexOf(oldStr, from)) >= 0;
             from += oldStr.size())
            ++made;
        out.replace(oldStr, newStr);
        r.replacements = made;
    } else {
        const int idx = out.indexOf(oldStr);
        out.replace(idx, oldStr.size(), newStr);
        r.replacements = 1;
    }
    r.applied = true;
    r.newContents = std::move(out);
    return r;
}

namespace {
// The text's line count. A final '\n' ends the last line; the empty element
// split() returns after it is not a line a range can name.
int lineCount(const QString &contents, const QStringList &lines) {
    return int(lines.size()) - (contents.endsWith(QLatin1Char('\n')) ? 1 : 0);
}
}  // namespace

EditOutcome applyRangeToContent(const QString &contents, int startLine,
                                int endLine, const QString &expectFirst,
                                const QString &expectLast,
                                const QString &newStr) {
    EditOutcome r;
    QStringList lines = contents.split(QLatin1Char('\n'));
    if (startLine < 1 || endLine < startLine || endLine > lineCount(contents, lines)) {
        r.skipReason = QStringLiteral("range_out_of_bounds");
        return r;
    }
    // Verbatim, both ends. Checking only one end would let a range that has
    // grown or shrunk at the other end through — the commonest drift there is.
    if (lines.at(startLine - 1) != expectFirst ||
        lines.at(endLine - 1) != expectLast) {
        r.skipReason = QStringLiteral("range_mismatch");
        return r;
    }

    const int count = endLine - startLine + 1;
    // An empty replacement removes the lines outright; splicing `{""}` would
    // leave a blank line where the caller asked for nothing.
    const QStringList repl = newStr.isEmpty()
                                 ? QStringList()
                                 : newStr.split(QLatin1Char('\n'));
    auto first = lines.begin() + (startLine - 1);
    lines.erase(first, first + count);
    for (int i = 0; i < repl.size(); ++i)
        lines.insert(startLine - 1 + i, repl.at(i));

    r.applied      = true;
    r.replacements = 1;
    r.newContents  = lines.join(QLatin1Char('\n'));
    return r;
}

bool rangeMatchesAt(const QString &contents, int startLine, int endLine,
                    const QString &expectFirst, const QString &expectLast) {
    const QStringList lines = contents.split(QLatin1Char('\n'));
    if (startLine < 1 || endLine < startLine || endLine > lineCount(contents, lines))
        return false;
    return lines.at(startLine - 1) == expectFirst
        && lines.at(endLine - 1) == expectLast;
}

LineShift lineShiftBetween(const QString &before, const QString &after) {
    LineShift s;
    const qsizetype n = qMin(before.size(), after.size());
    qsizetype i = 0;
    while (i < n && before.at(i) == after.at(i)) ++i;
    if (i == n && before.size() == after.size()) return s;   // identical
    s.firstLine = int(QStringView(before).left(i).count(QLatin1Char('\n'))) + 1;
    s.lineDelta = int(after.count(QLatin1Char('\n'))
                      - before.count(QLatin1Char('\n')));
    return s;
}

namespace {
std::function<void(const QString &)> g_beforeWriteHook;   // ANTS-6467
}  // namespace

void setBeforeWriteHookForTest(std::function<void(const QString &)> hook) {
    g_beforeWriteHook = std::move(hook);
}

void beforeWriteHookForTest(const QString &path) {
    if (const auto hook = g_beforeWriteHook)   // a copy: the hook may clear itself
        hook(path);
}

}  // namespace ApplyEdits
