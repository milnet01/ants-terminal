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

namespace {

// ANTS-1759 — is the 'R' at rIdx a C++ raw-string prefix? The
// contiguous identifier run ending at R must be exactly one of the
// string-literal raw prefixes (R / LR / u8R / uR / UR); this rejects
// `fooR"..."` where R is just the tail of an identifier.
bool isRawStringPrefix(const QString &src, int rIdx) {
    int start = rIdx;
    while (start > 0) {
        const QChar p = src[start - 1];
        if (p.isLetterOrNumber() || p == QLatin1Char('_')) --start;
        else break;
    }
    const QString run = src.mid(start, rIdx - start + 1);
    return run == QLatin1String("R")  || run == QLatin1String("LR") ||
           run == QLatin1String("u8R")|| run == QLatin1String("uR") ||
           run == QLatin1String("UR");
}

}  // namespace

bool lineHasCode(const QString &source, const QString &path, int line) {
    // ANTS-2210 — moved verbatim from AuditDialog::lineIsCode (ANTS-1270 /
    // ANTS-1759) so the lexer is a pure, test-linkable surface. The GUI
    // method now reads + size-caps the file and forwards `source` here.
    if (line <= 0) return true;

    // State: 0=code, 1=line-comment (until \n), 2=block-comment,
    // 3=string, 4=C++ raw string (ANTS-1759).
    int state = 0;
    int curLine = 1;
    bool hasCodeOnLine = false;
    QChar stringQuote;
    QString rawTerminator;  // ")" + d-char-seq + "\"" for state 4

    // ANTS-1270 — comment/string syntax is language-specific. The default
    // C-style lexer (//, /* */, ", ', raw strings) misclassifies # comments
    // (Python / shell / Ruby / YAML / TOML / CMake / Makefile) and Lua
    // -- / [[ ]] forms, so e.g. `# TODO: "10.0.0.1"` was scanned as code and
    // its IP/secret/TODO finding survived. Pick the lexer by file extension.
    enum class Syntax { CStyle, Hash, Lua };
    Syntax syntax = Syntax::CStyle;
    bool pyTriple = false;
    {
        const QString p = path.toLower();
        auto ext = [&p](const char *e) { return p.endsWith(QLatin1String(e)); };
        if (ext(".py") || ext(".pyw")) { syntax = Syntax::Hash; pyTriple = true; }
        else if (ext(".sh") || ext(".bash") || ext(".zsh") || ext(".ksh") ||
                 ext(".rb") || ext(".pl") || ext(".pm") || ext(".yaml") ||
                 ext(".yml") || ext(".toml") || ext(".cmake") || ext(".mk") ||
                 p.endsWith(QLatin1String("cmakelists.txt")) ||
                 p.endsWith(QLatin1String("makefile")) ||
                 p.endsWith(QLatin1String("dockerfile")))
            syntax = Syntax::Hash;
        else if (ext(".lua"))
            syntax = Syntax::Lua;
    }

    for (int i = 0; i < source.size(); ++i) {
        const QChar c = source[i];
        const QChar n = (i + 1 < source.size()) ? source[i + 1] : QChar();

        if (curLine == line) {
            // A line counts as "code" only if there's a real identifier/
            // operator character outside strings and comments. Structural
            // punctuation alone (whitespace, argument-list glue like
            // `,;(){}[]`, and the string-delimiter characters themselves)
            // doesn't count — that catches continuation lines like
            //   "system(), popen()…", "Security",
            // which are pure string-literal data inside a multi-line call,
            // and shouldn't trigger security regex matches. Escape chars
            // (`\`) also don't count as code on their own.
            if (state == 0 && !c.isSpace() &&
                c != ',' && c != ';' &&
                c != '(' && c != ')' &&
                c != '{' && c != '}' &&
                c != '[' && c != ']' &&
                c != '"' && c != '\'' && c != '\\' &&
                // ANTS-1270 / ANTS-2230 — a bare comment-introducer must not
                // read as code, else an otherwise-comment-only line is kept as
                // code (a finding inside it survives dropFindingsInComments...).
                // '#' for Hash, '-' for Lua's '--', and the C-style '/' when it
                // opens '//' or '/*' (a division '/ ' keeps reading as code).
                !(syntax == Syntax::Hash && c == '#') &&
                !(syntax == Syntax::Lua && c == '-') &&
                !(syntax == Syntax::CStyle && c == '/' &&
                  (n == QLatin1Char('/') || n == QLatin1Char('*')))) {
                hasCodeOnLine = true;
            }
        } else if (curLine > line) {
            break;
        }

        if (c == '\n') {
            if (state == 1) state = 0;
            ++curLine;
            continue;
        }

        switch (state) {
        case 0:  // code
            if (syntax == Syntax::Hash) {
                if (c == '#') { state = 1; }
                else if (pyTriple && (c == '"' || c == '\'') &&
                         i + 2 < source.size() &&
                         source[i + 1] == c && source[i + 2] == c) {
                    stringQuote = c; state = 5; i += 2;  // consume the triple
                }
                else if (c == '"' || c == '\'') { state = 3; stringQuote = c; }
                break;
            }
            if (syntax == Syntax::Lua) {
                // -- line comment, --[[ ]] block comment, [[ ]] long string.
                // Level-0 brackets only; leveled [==[ ]==] forms are rare and
                // fall through as code (conservative — preserves findings).
                if (c == '-' && n == '-') {
                    if (i + 3 < source.size() && source[i + 2] == QLatin1Char('[') &&
                        source[i + 3] == QLatin1Char('[')) {
                        rawTerminator = QStringLiteral("]]"); state = 6; i += 3;
                    } else { state = 1; ++i; }
                }
                else if (c == '[' && n == '[') {
                    rawTerminator = QStringLiteral("]]"); state = 6; ++i;
                }
                else if (c == '"' || c == '\'') { state = 3; stringQuote = c; }
                break;
            }
            if (c == '/' && n == '/') { state = 1; ++i; }
            else if (c == '/' && n == '*') { state = 2; ++i; }
            else if (c == '"' && i > 0 && source[i - 1] == QLatin1Char('R') &&
                     isRawStringPrefix(source, i - 1)) {
                // ANTS-1759 — C++ raw string R"delim( ... )delim". Capture
                // the d-char-seq (up to '(', ≤16 chars, no ()\"/ws) so an
                // embedded //, " or \ inside the literal can't desync the
                // scanner for the rest of the file. Fall back to a normal
                // string if no valid delimiter terminates in '(' — covers
                // e.g. Python's R"foo" (no paren-delimited form).
                QString delim;
                int j = i + 1;
                bool isRaw = false;
                for (; j < source.size() && delim.size() <= 16; ++j) {
                    const QChar d = source[j];
                    if (d == QLatin1Char('(')) { isRaw = true; break; }
                    if (d == QLatin1Char(')') || d == QLatin1Char('\\') ||
                        d == QLatin1Char('"') || d.isSpace()) break;
                    delim += d;
                }
                if (isRaw) {
                    rawTerminator = QLatin1Char(')') + delim + QLatin1Char('"');
                    state = 4;
                    i = j;  // resume after '(' (loop does ++i)
                } else {
                    state = 3; stringQuote = c;
                }
            }
            else if (c == '"' || c == '\'') { state = 3; stringQuote = c; }
            break;
        case 1:  // line comment — terminated only by newline (handled above)
            break;
        case 2:  // block comment
            if (c == '*' && n == '/') { state = 0; ++i; }
            break;
        case 3:  // string
            if (c == '\\') { ++i; }  // skip escape
            else if (c == stringQuote) state = 0;
            break;
        case 4:  // C++ raw string — ends only at )delim" (no escapes)
            if (c == QLatin1Char(')') &&
                source.mid(i, rawTerminator.size()) == rawTerminator) {
                i += rawTerminator.size() - 1;  // loop does ++i
                state = 0;
            }
            break;
        case 5:  // ANTS-1270 — Python triple-quoted string (""" or ''')
            if (c == stringQuote && i + 2 < source.size() &&
                source[i + 1] == stringQuote && source[i + 2] == stringQuote) {
                i += 2;  // consume the closing triple (loop does ++i)
                state = 0;
            }
            break;
        case 6:  // ANTS-1270 — Lua long bracket (string / --[[ comment ]]), L0
            if (c == QLatin1Char(']') && n == QLatin1Char(']')) {
                ++i;  // consume the second ']' (loop does ++i)
                state = 0;
            }
            break;
        }
    }
    return hasCodeOnLine;
}

} // namespace AuditHygiene
