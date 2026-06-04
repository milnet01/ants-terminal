// Implementation: see audithygiene.h for the contract.

#include "audithygiene.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
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
    // ANTS-2005 — prefer the most-specific `[tool.ruff.lint]` section
    // (ruff's modern home for `ignore`) REGARDLESS of file order. The old
    // "keep the last match" logic silently discarded the lint-specific
    // ignore list whenever `[tool.ruff.lint]` appeared *before* a later
    // `[tool.ruff]` (INV-8). TOML forbids duplicate tables, so the first
    // occurrence of each form is authoritative.
    int sectionStart = -1, sectionBodyStart = -1;  // [tool.ruff] fallback
    int lintStart = -1, lintBodyStart = -1;        // [tool.ruff.lint]
    while (sit.hasNext()) {
        const auto m = sit.next();
        if (m.captured(0).contains(QStringLiteral(".lint"))) {
            if (lintStart < 0) {
                lintStart = m.capturedStart();
                lintBodyStart = m.capturedEnd();
            }
        } else if (sectionStart < 0) {
            sectionStart = m.capturedStart();
            sectionBodyStart = m.capturedEnd();
        }
    }
    if (lintStart >= 0) {  // most-specific wins
        sectionStart = lintStart;
        sectionBodyStart = lintBodyStart;
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

// ----- ANTS-1111 framework auto-detect -----------------------------------

namespace {

// ANTS-2005 — project config files (requirements.txt, pyproject.toml,
// CMakeLists.txt, …) are small by nature; cap the read so a pathological
// multi-GB file dropped in the project root can't be slurped whole.
constexpr qint64 kMaxConfigBytes = 1 << 20;  // 1 MiB

QString slurpIfExists(const QString &absPath) {
    QFile f(absPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    return QString::fromUtf8(f.read(kMaxConfigBytes));
}

bool fileExists(const QString &absPath) {
    return QFileInfo::exists(absPath);
}

} // namespace

QStringList detectProjectFrameworks(const QString &projectPath) {
    QStringList out;
    const QDir root(projectPath);

    const QString reqs       = slurpIfExists(root.filePath("requirements.txt"));
    const QString pyproj     = slurpIfExists(root.filePath("pyproject.toml"));
    const QString pkgJson    = slurpIfExists(root.filePath("package.json"));
    const QString cmakeLists = slurpIfExists(root.filePath("CMakeLists.txt"));
    const QString cargoToml  = slurpIfExists(root.filePath("Cargo.toml"));
    const QString goMod      = slurpIfExists(root.filePath("go.mod"));
    const bool hasManagePy   = fileExists(root.filePath("manage.py"));

    auto pyHas = [&](const QString &needle) {
        // ANTS-2005 — the trailing class also accepts end-of-line so a bare
        // dependency name with no version specifier on the final line (no
        // trailing newline) still matches, e.g. a `requirements.txt` ending
        // in `flask` with no EOL.
        const QRegularExpression re(
            QStringLiteral("(?im)^[\\s'\"]*") + QRegularExpression::escape(needle)
                + QStringLiteral("(?:[\\s\\[<>=!~]|$)"));
        return re.match(reqs).hasMatch() || re.match(pyproj).hasMatch();
    };

    if (pyHas(QStringLiteral("flask"))) out << QStringLiteral("flask");
    if (hasManagePy || pyHas(QStringLiteral("django"))) {
        out << QStringLiteral("django");
    }

    if (!pkgJson.isEmpty()) {
        // Cheap dependency-name probe: look for `"react"` or `"vue"`
        // followed by `:` inside any dependencies-shaped block.
        // ANTS-1647 — literal patterns hoisted out of the per-call path.
        static const QRegularExpression reReact(
            QStringLiteral("\"react(?:-dom)?\"\\s*:"));
        static const QRegularExpression reVue(QStringLiteral("\"vue\"\\s*:"));
        if (reReact.match(pkgJson).hasMatch()) {
            out << QStringLiteral("react");
        }
        if (reVue.match(pkgJson).hasMatch()) {
            out << QStringLiteral("vue");
        }
    }

    if (!cmakeLists.isEmpty()
        && (cmakeLists.contains(QStringLiteral("Qt6::"))
            || cmakeLists.contains(QStringLiteral("find_package(Qt6")))) {
        out << QStringLiteral("qt6");
    }

    if (!cargoToml.isEmpty()) out << QStringLiteral("rust");
    if (!goMod.isEmpty())     out << QStringLiteral("go");

    out.removeDuplicates();
    return out;
}

QStringList semgrepRulePacks(const QStringList &frameworks) {
    QStringList args;
    for (const QString &fw : frameworks) {
        if (fw == QStringLiteral("flask")
            || fw == QStringLiteral("django")
            || fw == QStringLiteral("react")
            || fw == QStringLiteral("vue")) {
            args << QStringLiteral("--config")
                 << QStringLiteral("p/") + fw;
        }
    }
    return args;
}

} // namespace AuditHygiene
