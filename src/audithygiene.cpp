// Implementation: see audithygiene.h for the contract.

#include "audithygiene.h"

#include <QRegularExpression>

namespace AuditHygiene {

QStringList parseSemgrepExcludeRules(const QString &text) {
    // Locate the excluded-rules block. Start marker: a comment line whose
    // body contains "Excluded upstream rules". End marker: the first non-
    // comment line (rule IDs are always comments; non-comment content means
    // we've left the header and entered YAML body).
    //
    // Within the block we pick out only lines that match the rule-ID shape
    // (`#   dotted.identifier`); prose and separator lines (`# ---`) are
    // ignored, so fuzzy "next section" detection isn't needed.
    const QStringList lines = text.split('\n');
    int i = 0;
    for (; i < lines.size(); ++i) {
        const QString s = lines[i].trimmed();
        if (s.startsWith('#') && s.contains("Excluded upstream rules",
                                             Qt::CaseInsensitive))
            break;
    }
    if (i >= lines.size()) return {};

    static const QRegularExpression ruleIdLine(
        R"(^#\s+([a-z][a-z0-9._-]+\.[a-z0-9._-]+(?:\.[a-z0-9._-]+)*)\s*$)",
        QRegularExpression::CaseInsensitiveOption);

    QStringList rules;
    for (int j = i + 1; j < lines.size(); ++j) {
        const QString &raw = lines[j];
        const QString s = raw.trimmed();
        if (s.isEmpty()) continue;
        if (!s.startsWith('#')) break;
        const QRegularExpressionMatch m = ruleIdLine.match(raw);
        if (m.hasMatch()) rules << m.captured(1);
    }
    rules.removeDuplicates();
    return rules;
}

QStringList parseBanditSkipCodes(const QString &text) {
    // Locate `[tool.ruff.lint]` (or `[tool.ruff]` as a fallback) and then
    // the `ignore = [ ... ]` array within it. Ruff also accepts `extend-
    // ignore` — both count.
    static const QRegularExpression sectionRe(
        R"(^\s*\[tool\.ruff(?:\.lint)?\]\s*$)",
        QRegularExpression::MultilineOption);
    QRegularExpressionMatchIterator sit = sectionRe.globalMatch(text);
    int sectionStart = -1;
    int sectionBodyStart = -1;
    while (sit.hasNext()) {
        const auto m = sit.next();
        // Prefer the later/more-specific `[tool.ruff.lint]` if present.
        sectionStart = m.capturedStart();
        sectionBodyStart = m.capturedEnd();
    }
    if (sectionStart < 0) return {};

    // Section body ends at the next `[...]` header or EOF. We start the next-
    // header search AFTER the current header line so the same header isn't
    // rematched (MultilineOption means `^` anchors to every line start, and
    // `capturedStart()` of a `^`-anchored match points at the newline before
    // the line — so `sectionStart + 1` would still lie at column 0 of the very
    // same header line).
    static const QRegularExpression nextHeader(
        R"(^\s*\[[^\]]+\]\s*$)",
        QRegularExpression::MultilineOption);
    int sectionEnd = text.size();
    QRegularExpressionMatchIterator nit = nextHeader.globalMatch(text, sectionBodyStart);
    if (nit.hasNext()) sectionEnd = nit.next().capturedStart();
    const QString body = text.mid(sectionStart, sectionEnd - sectionStart);

    // Find `ignore = [ ... ]` (or `extend-ignore`). The array is multi-line in
    // practice; match up to the closing bracket.
    static const QRegularExpression ignoreRe(
        R"((?:^|\n)\s*(?:extend-)?ignore\s*=\s*\[([^\]]*)\])",
        QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch im = ignoreRe.match(body);
    if (!im.hasMatch()) return {};

    const QString arrayBody = im.captured(1);
    // Extract S-codes (quoted string literals starting with S followed by
    // digits). Bandit uses B-codes; the mapping is S<nnn> ↔ B<nnn>.
    static const QRegularExpression sCodeRe(
        R"(["']S(\d{3})["'])");
    QRegularExpressionMatchIterator sc = sCodeRe.globalMatch(arrayBody);
    QStringList bCodes;
    while (sc.hasNext()) {
        const auto m = sc.next();
        bCodes << "B" + m.captured(1);
    }
    bCodes.removeDuplicates();
    return bCodes;
}

} // namespace AuditHygiene
