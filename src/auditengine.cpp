#include "auditengine.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>

#include <algorithm>

namespace AuditEngine {

// Catastrophic-regex shape detector (ANTS-1123 indie-review
// C1/C2/C3 unification: previously this lived in two places —
// engine's anonymous-namespace local + AuditDialog's static. The
// shapes detected drifted between them. Single definition now.)
bool isCatastrophicRegex(const QString &pattern) {
    // Matches the dialog's pre-unification shape — broader than
    // the engine-local version (catches `(a+b)*` plus `(a|b)+`).
    static const QRegularExpression nested(
        QStringLiteral(R"(\([^()]*[+*][^()]*\)[?*+])"));
    return nested.match(pattern).hasMatch();
}

// Wrap a user-supplied regex with PCRE2's inline LIMIT_MATCH so a
// slow-match input has bounded cost (ANTS-1123 unification:
// previously the engine used 200000 and the dialog used 100000;
// settled on 100000 — accommodates every sane pattern, aborts
// adversarial in milliseconds. Already-prefixed patterns pass
// through unchanged so users / config authors can specify their
// own budget.)
QString hardenUserRegex(const QString &pattern) {
    if (pattern.isEmpty()) return {};
    if (pattern.startsWith(QStringLiteral("(*LIMIT_"))) return pattern;
    return QStringLiteral("(*LIMIT_MATCH=100000)") + pattern;
}

namespace {

// Lifted from auditdialog.cpp: resolve `./relative.cpp` references
// (and bare relative paths) to absolute paths under projectPath.
QString resolveProjectPathLocal(const QString &maybeRelative,
                                const QString &projectPath) {
    if (maybeRelative.isEmpty()) return {};
    QString path = maybeRelative;
    if (path.startsWith(QStringLiteral("./"))) path.remove(0, 2);
    QFileInfo fi(path);
    if (fi.isAbsolute()) return fi.canonicalFilePath();
    if (projectPath.isEmpty()) return {};
    return QFileInfo(QDir(projectPath), path).canonicalFilePath();
}

}  // namespace (close anon for the public sourceForCheck/computeDedup)

// File-scope helpers carried over from auditdialog.cpp. Now public
// so the dialog can call them too — closes the silent-divergence
// vector ANTS-1123 indie-review H1 flagged.
QString sourceForCheck(const QString &checkId) {
    if (checkId.startsWith("cppcheck"))  return "cppcheck";
    if (checkId == "clang_tidy")         return "clang-tidy";
    if (checkId == "clazy")              return "clazy";
    if (checkId == "semgrep")            return "semgrep";
    if (checkId == "pylint")             return "pylint";
    if (checkId == "bandit")             return "bandit";
    if (checkId == "ruff")               return "ruff";
    if (checkId == "mypy")               return "mypy";
    if (checkId == "shellcheck")         return "shellcheck";
    if (checkId == "luacheck")           return "luacheck";
    if (checkId == "cargo_clippy")       return "cargo-clippy";
    if (checkId == "cargo_audit")        return "cargo-audit";
    if (checkId == "go_vet")             return "go vet";
    if (checkId == "govulncheck")        return "govulncheck";
    if (checkId == "golangci_lint")      return "golangci-lint";
    if (checkId == "eslint")             return "eslint";
    if (checkId == "npm_audit")          return "npm audit";
    if (checkId == "osv_scanner")        return "osv-scanner";
    if (checkId == "trufflehog")         return "trufflehog";
    if (checkId == "hadolint")           return "hadolint";
    if (checkId == "checkov")            return "checkov";
    if (checkId == "ast_grep")           return "ast-grep";
    if (checkId == "spec_code_drift" ||
        checkId == "changelog_test_coverage") return "feature-coverage";
    if (checkId.startsWith("git_"))      return "git";
    if (checkId == "compiler_warnings")  return "gcc";
    if (checkId == "large_files" || checkId == "dup_files" ||
        checkId == "dangling_symlinks" || checkId == "binary_in_repo" ||
        checkId == "env_files" || checkId == "temp_files" ||
        checkId == "file_perms" || checkId == "header_guards" ||
        checkId == "line_stats" || checkId == "long_files" ||
        checkId == "encoding_check")
        return "find";
    return "grep";
}

QString computeDedup(const QString &file, int line,
                     const QString &checkId, const QString &title) {
    const QString raw = QString("%1:%2:%3:%4")
                           .arg(file).arg(line).arg(checkId, title);
    return QString::fromLatin1(
        QCryptographicHash::hash(raw.toUtf8(), QCryptographicHash::Sha256)
            .toHex().left(24));
}

// (anon namespace already closed above, before sourceForCheck.)

FilterResult applyFilter(const QString &raw,
                         const OutputFilter &f,
                         const QString &projectPath) {
    if (raw.isEmpty()) return { QString(), 0 };

    QRegularExpression dropRe;
    bool hasDropRe = !f.dropIfMatches.isEmpty();
    if (hasDropRe) {
        if (isCatastrophicRegex(f.dropIfMatches)) {
            qWarning("audit: dropIfMatches pattern rejected for shape-DoS risk: %s",
                     qPrintable(f.dropIfMatches));
            hasDropRe = false;
        } else {
            dropRe.setPattern(hardenUserRegex(f.dropIfMatches));
            dropRe.setPatternOptions(QRegularExpression::CaseInsensitiveOption);
            if (!dropRe.isValid()) {
                qWarning("audit: dropIfMatches invalid after hardening: %s",
                         qPrintable(dropRe.errorString()));
                hasDropRe = false;
            }
        }
    }

    const bool hasContextFilter = !f.dropIfContextContains.isEmpty();
    QHash<QString, QStringList> fileCache;
    static const QRegularExpression fileLineRe(
        QStringLiteral(R"(^\./([^:]+\.(?:cpp|cc|cxx|c|h|hpp|hxx|py|sh|js|ts|go|rs|lua|java)):(\d+):)"));

    QStringList out;
    const QStringList lines = raw.split('\n', Qt::KeepEmptyParts);
    int keptCount = 0;
    for (const QString &line : lines) {
        if (line.isEmpty()) continue;

        bool drop = false;
        for (const QString &needle : f.dropIfContains) {
            if (line.contains(needle, Qt::CaseInsensitive)) { drop = true; break; }
        }
        if (drop) continue;

        if (hasDropRe && dropRe.match(line).hasMatch()) continue;

        if (!f.keepOnlyIfContains.isEmpty()) {
            bool allHit = true;
            for (const QString &needle : f.keepOnlyIfContains) {
                if (!line.contains(needle, Qt::CaseInsensitive)) { allHit = false; break; }
            }
            if (!allHit) continue;
        }

        if (hasContextFilter) {
            const QRegularExpressionMatch m = fileLineRe.match(line);
            if (m.hasMatch()) {
                const QString relPath = m.captured(1);
                const int lineNo = m.captured(2).toInt();
                // const* — only read through fileLines below (size/at/
                // isEmpty); the write into the slot goes via the
                // reference, not via this pointer. ANTS-1122 audit-
                // fold-in (2026-04-30).
                const QStringList *fileLines = fileCache.contains(relPath)
                    ? &fileCache[relPath]
                    : nullptr;
                if (!fileLines) {
                    QStringList &slot = fileCache[relPath];
                    const QString abs = resolveProjectPathLocal(relPath, projectPath);
                    if (!abs.isEmpty()) {
                        QFile src(abs);
                        if (src.open(QIODevice::ReadOnly | QIODevice::Text)) {
                            slot = QString::fromUtf8(src.readAll())
                                       .split('\n', Qt::KeepEmptyParts);
                        }
                    }
                    fileLines = &slot;
                }
                if (!fileLines->isEmpty() && lineNo > 0) {
                    const int total = static_cast<int>(fileLines->size());
                    const int lo = std::max(1, lineNo - f.contextWindow);
                    const int hi = std::min(total, lineNo + f.contextWindow);
                    bool ctxHit = false;
                    for (int i = lo; i <= hi && !ctxHit; ++i) {
                        const QString &ctxLine = fileLines->at(i - 1);
                        for (const QString &needle : f.dropIfContextContains) {
                            if (ctxLine.contains(needle, Qt::CaseInsensitive)) {
                                ctxHit = true;
                                break;
                            }
                        }
                    }
                    if (ctxHit) continue;
                }
            }
        }

        out << line;
        ++keptCount;
        if (f.maxLines > 0 && keptCount >= f.maxLines) break;
    }
    return { out.join('\n'), keptCount };
}

QList<Finding> parseFindings(const QString &body, const AuditCheck &check) {
    QList<Finding> out;
    if (body.isEmpty()) return out;

    static const QRegularExpression reFileLineCol(
        R"(^([^\s:]+):(\d+):(?:\d+:)?\s*(.*)$)");
    static const QRegularExpression reFileLine(
        R"(^([^\s:]+):(\d+):\s*(.*)$)");
    static const QRegularExpression reJustFile(
        R"(^([^\s:]+\.[A-Za-z0-9_]+)$|^([^\s:]+/[^\s:]+)$)");

    const QString source = sourceForCheck(check.id);
    const QStringList lines = body.split('\n', Qt::SkipEmptyParts);
    for (const QString &rawLine : lines) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty()) continue;

        Finding f;
        f.checkId   = check.id;
        f.checkName = check.name;
        f.category  = check.category;
        f.type      = check.type;
        f.severity  = check.severity;
        f.source    = source;
        f.message   = line;

        auto m1 = reFileLineCol.match(line);
        auto m2 = reFileLine.match(line);
        auto m3 = reJustFile.match(line);
        if (m1.hasMatch()) {
            f.file = m1.captured(1);
            f.line = m1.captured(2).toInt();
        } else if (m2.hasMatch()) {
            f.file = m2.captured(1);
            f.line = m2.captured(2).toInt();
        } else if (m3.hasMatch()) {
            f.file = m3.captured(1).isEmpty() ? m3.captured(2) : m3.captured(1);
        }

        const QString title = line.left(80);
        f.dedupKey = computeDedup(f.file, f.line, check.id, title);
        out.append(f);
    }
    return out;
}

void capFindings(CheckResult &r, int cap) {
    if (cap <= 0 || r.findings.size() <= cap) return;
    r.omittedCount = r.findings.size() - cap;
    r.findings.erase(r.findings.begin() + cap, r.findings.end());
}

}  // namespace AuditEngine
