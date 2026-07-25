#include "auditengine.h"

#include "auditfpledger.h"
#include "regexharden.h"  // ANTS-1665 — isCatastrophicRegex / hardenUserRegex

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QTextStream>
#include <QXmlStreamReader>

#include <algorithm>
#include <cstdlib>

namespace AuditEngine {

// ANTS-1665 — the catastrophic-regex detector + LIMIT_MATCH hardener moved to
// ants_core_lib (src/regexharden.cpp) so widget-side consumers can reach them
// without linking the audit lib. These remain the AuditEngine:: API (callers
// like settingsdialog / AuditDialog are unchanged); they forward to the single
// source of truth in ants::regex. (ANTS-1123 unification preserved.)
bool isCatastrophicRegex(const QString &pattern) {
    return ants::regex::isCatastrophicRegex(pattern);
}

QString hardenUserRegex(const QString &pattern) {
    return ants::regex::hardenUserRegex(pattern);
}

// ── ANTS-3615 — allowlist loader / matcher / glob compiler.
// Moved verbatim off AuditDialog (which now delegates) so the headless
// `audit_run` path can apply the same `.audit_allowlist.json` filter the
// GUI dialog has always applied. Behaviour is byte-identical to the
// pre-move dialog methods.

QRegularExpression globToRegex(const QString &glob) {
    // Convert a minimal subset of glob to regex:
    //   **   — any path (including slashes)
    //   *    — any segment (no slashes)
    //   ?    — one char (no slash)
    // Everything else is escaped.
    QString re;
    re.reserve(glob.size() * 2);
    re.append('^');
    for (int i = 0; i < glob.size(); ++i) {
        const QChar c = glob[i];
        if (c == '*' && i + 1 < glob.size() && glob[i + 1] == '*') {
            re.append(".*");
            ++i;
        } else if (c == '*') {
            re.append("[^/]*");
        } else if (c == '?') {
            re.append("[^/]");
        } else {
            re.append(QRegularExpression::escape(QString(c)));
        }
    }
    re.append('$');
    return QRegularExpression(re);
}

QList<AllowlistEntry> loadAllowlist(const QString &allowlistPath) {
    QList<AllowlistEntry> out;
    QFile f(allowlistPath);
    if (!f.open(QIODevice::ReadOnly)) return out;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    f.close();
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning(".audit_allowlist.json: %s", qPrintable(err.errorString()));
        return out;
    }
    const QJsonArray entries = doc.object().value("allowlist").toArray();
    for (const QJsonValue &v : entries) {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        const QString rule = o.value("rule").toString();
        const QString glob = o.value("path_glob").toString();
        const QString line = o.value("line_regex").toString();
        if (rule.isEmpty() || glob.isEmpty() || line.isEmpty()) continue;
        if (isCatastrophicRegex(line)) {
            qWarning(".audit_allowlist.json: line_regex rejected for "
                     "shape-DoS risk: %s", qPrintable(line));
            continue;
        }
        AllowlistEntry e;
        e.rule      = rule;
        e.pathRegex = globToRegex(glob);
        e.lineRegex = QRegularExpression(hardenUserRegex(line));
        if (!e.lineRegex.isValid()) {
            qWarning(".audit_allowlist.json: bad line_regex '%s': %s",
                     qPrintable(line),
                     qPrintable(e.lineRegex.errorString()));
            continue;
        }
        e.reason    = o.value("reason").toString();
        out.append(e);
    }
    return out;
}

bool allowlisted(const QList<AllowlistEntry> &allowlist,
                 const QString &checkId,
                 const QString &file,
                 const QString &message) {
    if (allowlist.isEmpty()) return false;
    for (const AllowlistEntry &e : allowlist) {
        if (e.rule != checkId) continue;
        if (!file.isEmpty() && !e.pathRegex.match(file).hasMatch()) continue;
        if (!e.lineRegex.match(message).hasMatch()) continue;
        return true;
    }
    return false;
}

// ANTS-1709 — centralised directory-exclusion set. See the header for
// the rationale (one source of truth, no per-tool drift). `build` is the
// only globbed family; each formatter emits it in its own syntax.
const QStringList &excludedDirNames() {
    static const QStringList kNames = {
        QStringLiteral(".git"),
        QStringLiteral("node_modules"),
        QStringLiteral(".cache"),
        QStringLiteral("dist"),
        QStringLiteral("vendor"),
        QStringLiteral(".audit_cache"),
        QStringLiteral("external"),
        QStringLiteral("third_party"),
        QStringLiteral("__pycache__"),
        QStringLiteral(".claude"),
        QStringLiteral("target"),
        QStringLiteral(".venv"),
        QStringLiteral("venv"),
        QStringLiteral(".tox"),
        QStringLiteral(".pytest_cache"),
        QStringLiteral(".mypy_cache"),
        QStringLiteral("Testing"),
    };
    return kNames;
}

QString findExcludeExpr() {
    QString s = QStringLiteral(" -not -path './build/*' -not -path './build-*/*'");
    for (const QString &d : excludedDirNames()) {
        // __pycache__ nests arbitrarily deep in Python trees, so match it
        // at any depth; the rest are conventionally top-level only. This
        // reproduces the pre-centralisation kFindExcl semantics exactly.
        s += (d == QLatin1String("__pycache__"))
                 ? QStringLiteral(" -not -path '*/") + d + QStringLiteral("/*'")
                 : QStringLiteral(" -not -path './") + d + QStringLiteral("/*'");
    }
    return s;
}

QString grepExcludeExpr() {
    QString s = QStringLiteral(" --exclude-dir=build --exclude-dir='build-*'");
    for (const QString &d : excludedDirNames())
        s += QStringLiteral(" --exclude-dir=") + d;
    return s;
}

QString trivySkipDirsCsv() {
    QStringList parts{QStringLiteral("build"), QStringLiteral("build-*")};
    parts += excludedDirNames();
    return parts.join(QLatin1Char(','));
}

QString cppcheckIgnoreShellExpr() {
    QString names = QStringLiteral("build build-*");
    for (const QString &d : excludedDirNames())
        names += QLatin1Char(' ') + d;
    return QStringLiteral(" $(for d in ") + names +
           QStringLiteral("; do [ -d \"$d\" ] && printf -- '-i %s ' \"$d\"; done)");
}

// ANTS-2182 — see the header. The probe order has `build/` first so a
// canonical build wins over an iteration tree (build-fast/build-asan).
// ANTS-3367 — the candidate list + probe live here ONCE. resolveBuildDir
// (returns the dir NAME, for the in-app dialog's clang-tidy/clazy `-p`
// shell strings) and resolveCompileCommands (returns the full PATH, for
// the MCP toolArgv) both consume it, so a new build-preset name only has
// to be added in one place (the ANTS-1707 miss class).
static const QStringList &buildDirCandidates() {
    static const QStringList kBuildDirs = {
        QStringLiteral("build"),         QStringLiteral("build-fast"),
        QStringLiteral("build-asan"),    QStringLiteral("build-workstation"),
        QStringLiteral("build-release"), QStringLiteral("build-debug"),
        QStringLiteral("build-test"),
    };
    return kBuildDirs;
}

QString resolveBuildDir(const QString &projectRoot) {
    for (const QString &d : buildDirCandidates()) {
        const QString db = projectRoot + QLatin1Char('/') + d +
                           QStringLiteral("/compile_commands.json");
        if (QFileInfo::exists(db)) return d;
    }
    return QString();
}

QString resolveCompileCommands(const QString &projectRoot) {
    const QString d = resolveBuildDir(projectRoot);
    if (d.isEmpty()) return QString();
    return projectRoot + QLatin1Char('/') + d +
           QStringLiteral("/compile_commands.json");
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
    // ANTS-2003 — the secret scanners are high-signal, not bare grep. Source
    // "secrets" lands in computeConfidence's external-tool set (+10) and
    // dodges the short-grep penalty.
    if (checkId == "secrets_scan" || checkId == "gitleaks") return "secrets";
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
    // Single-arg chain — the two-arg overload .arg(checkId, title)
    // substitutes %3 and %4 simultaneously, but if `file` ever contains
    // a literal "%3"/"%4" substring (legal in filesystems) those would
    // be matched as placeholders. Chained single-arg calls walk
    // left-to-right and skip already-substituted regions.
    const QString raw = QString("%1:%2:%3:%4")
                           .arg(file).arg(line).arg(checkId).arg(title);
    return QString::fromLatin1(
        QCryptographicHash::hash(raw.toUtf8(), QCryptographicHash::Sha256)
            .toHex().left(24));
}

// ANTS-1262 — confidence score, moved verbatim from AuditDialog. Pure
// data-transform over Finding; see the header for the weighting rationale.
//   - severity is the biggest single factor (~60% of the ceiling)
//   - multi-tool agreement is strong positive (CodeQL / Snyk model)
//   - external AST tools rank above raw regex grep (they understand AST)
//   - test paths get a penalty; AI triage can override into its own band
int computeConfidence(const Finding &f) {
    int score = 10;  // floor
    // Severity base: Info=0, Minor=15, Major=30, Critical=45, Blocker=60
    score += static_cast<int>(f.severity) * 15;

    if (f.highConfidence) score += 20;

    // External AST/semantic tools — hand-curated list that matches sources
    // produced by sourceForCheck().
    static const QSet<QString> kExternalTools = {
        "cppcheck", "clang-tidy", "clazy", "semgrep",
        "pylint", "bandit", "ruff", "mypy", "shellcheck", "luacheck",
        "cargo-clippy", "cargo-audit", "go vet", "govulncheck",
        "golangci-lint", "eslint", "npm audit", "gcc",
        "osv-scanner", "trufflehog", "hadolint", "checkov", "ast-grep",
        "secrets",  // ANTS-2003 — gitleaks/secrets_scan family
    };
    if (kExternalTools.contains(f.source)) score += 10;

    // Path-based penalties
    const QString lp = f.file.toLower();
    const bool inTests = lp.contains("/tests/") || lp.contains("/test/") ||
                         lp.contains("test_") || lp.endsWith("_test.py") ||
                         lp.endsWith("_test.go") || lp.endsWith(".test.js") ||
                         lp.endsWith(".spec.js");
    if (inTests) score -= 20;

    // Pure grep + message very short → likely noisy
    if (f.source == "grep" && f.message.size() < 30) score -= 5;

    // Explicit AI triage shifts confidence into its own band.
    if (f.aiVerdict == "FALSE_POSITIVE") score = std::min(score, 30);
    if (f.aiVerdict == "TRUE_POSITIVE")  score = std::max(score, 80);

    return std::clamp(score, 0, 100);
}

// (anon namespace already closed above, before sourceForCheck.)

QString mergeToolChannels(const QString &stdoutStr, const QString &stderrStr) {
    // See header for the parity rationale (ANTS-2118). The stdout-empty test
    // is trimmed-aware so a tool that flushes a trailing newline (or pure
    // whitespace) on stdout still falls back to the stderr findings stream —
    // blank lines never reach parseFindings either way, so this only sharpens
    // the empty/non-empty decision without changing any finding set.
    if (stdoutStr.trimmed().isEmpty())
        return stderrStr;
    if (!stderrStr.isEmpty())
        return stdoutStr + QStringLiteral("\n") + stderrStr;
    return stdoutStr;
}

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
    int keptCount = 0;
    // ANTS-1339 — stream over `raw` with indexOf('\n') + slice instead
    // of an up-front QStringList materialisation. The previous v1 form
    // built a full QStringList (one QString header per line) BEFORE the
    // f.maxLines bail at the loop tail, so a 64 MB tool output of
    // single-char lines allocated ~1.5–2 GB peak before being truncated
    // to the 100-line cap. The streaming walk allocates only kept lines.
    const int totalLen = raw.size();
    int pos = 0;
    while (pos <= totalLen) {
        int nl = raw.indexOf(QLatin1Char('\n'), pos);
        if (nl < 0) nl = totalLen;
        QString line = raw.mid(pos, nl - pos);
        pos = nl + 1;
        // ANTS-2118 — strip a trailing CR so CRLF output (a Windows-built
        // checker, or output piped through a CRLF-normalising layer) yields
        // the same kept lines as LF output. The downstream parseFindings
        // already per-line `.trimmed()`s, so the dedup key is CRLF-safe there;
        // this normalises applyFilter's OWN emitted body, which the dialog
        // shows verbatim and the SARIF exporter embeds as contextRegion. Cheap
        // and a no-op on LF input.
        if (line.endsWith(QLatin1Char('\r'))) line.chop(1);
        // ANTS-1123 indie-review M2: empty lines are dropped silently
        // — most checker outputs separate findings with blank lines and
        // those carry no signal for the dedup/SARIF pipeline downstream.
        if (line.isEmpty()) {
            if (nl == totalLen) break;
            continue;
        }

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
    // ANTS-1123 indie-review M1: previously the second alternation
    // `[^\s:]+/[^\s:]+` matched any path-shaped token including bare
    // version refs (`cargo/1.75`, `python/3.11.4`), producing bogus
    // SARIF physicalLocation.artifactLocation.uri entries. The
    // first-alternation shape `path.ext` already covers nested paths
    // (the leading `[^\s:]+` allows `/`), so the bare-path branch
    // added no real coverage and a lot of noise. Tightened to require
    // an extension whose first character is a letter — the `1.75`
    // shape's `.75` is rejected because `7` isn't `[A-Za-z]`.
    static const QRegularExpression reJustFile(
        R"(^([^\s:]+\.[A-Za-z][A-Za-z0-9_]{0,15})$)");

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

        // ANTS-1123 indie-review L1: short-circuit the regex chain.
        // The previous form ran all three patterns up front; only one
        // can match per line, so chaining the matches is a 2-3x cost
        // saving on the common file:line:col path.
        // ANTS-2003 — strip the file:line:col: prefix from the stored
        // message (group 3 is the message body). The raw prefix bloated the
        // SARIF message text and made the dedup title column-sensitive, so
        // two findings at the same line differing only by column were kept
        // as separate entries. Keying dedup on the clean message makes it
        // line-grained (computeFingerprint already stripLocation()s, so the
        // learned-FP ledger is unaffected). Lines that match no pattern keep
        // the full line as their message.
        if (auto m1 = reFileLineCol.match(line); m1.hasMatch()) {
            f.file = m1.captured(1);
            f.line = m1.captured(2).toInt();
            f.message = m1.captured(3);
        } else if (auto m2 = reFileLine.match(line); m2.hasMatch()) {
            f.file = m2.captured(1);
            f.line = m2.captured(2).toInt();
            f.message = m2.captured(3);
        } else if (auto m3 = reJustFile.match(line); m3.hasMatch()) {
            f.file = m3.captured(1);
        }

        const QString title = f.message.left(80);
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

int applyLearnedFpSuppressions(QList<Finding> &findings,
                               const QSet<QString> &learnedFingerprints) {
    if (learnedFingerprints.isEmpty()) return 0;
    int marked = 0;
    for (Finding &f : findings) {
        if (f.suppressed) continue;
        const QString fp = ants::auditfp::computeFingerprint(
            f.file, f.checkId, f.message);
        if (!learnedFingerprints.contains(fp)) continue;
        f.suppressed = true;
        if (f.aiReasoning.isEmpty())
            f.aiReasoning = QStringLiteral(
                "learned false positive (.audit_cache/learned-fp.jsonl)");
        ++marked;
    }
    return marked;
}

// ANTS-3506 — moved verbatim from AuditDialog::commentSuppresses (the
// pure predicate was trapped in the GUI TU). Body unchanged.
bool commentSuppresses(const QString &commentText, const QString &ruleId) {
    if (commentText.isEmpty()) return false;

    // Lowercase for case-insensitive token matching — rule ids are already
    // lowercase-alnum so this doesn't lose info.
    const QString body = commentText.toLower();
    const QString rule = ruleId.toLower();

    // The "bare" forms (no rule list) — any of these suppresses everything.
    // Matched conservatively: must be a whole word with `\b` semantics.
    static const QRegularExpression reBare(
        R"(\b(?:ants-audit:\s*disable(?:-next-line|-file)?|audit:\s*drop(?:-next-line|-file)?|nolint(?:nextline)?|cppcheck-suppress|noqa|nosec|nosemgrep|gitleaks:allow|eslint-disable(?:-line|-next-line)?|pylint:\s*disable)\b)",
        QRegularExpression::CaseInsensitiveOption);
    const auto bareMatch = reBare.match(body);
    if (!bareMatch.hasMatch()) return false;

    // Extract the rule-list payload, if any, following one of:
    //   `: rule1, rule2`    (noqa/nosec/nosemgrep/pylint:disable style)
    //   `=rule1,rule2`      (ants-audit, eslint, pylint variants)
    //   `(rule1, rule2)`    (clang-tidy, cppcheck block form)
    //   `[id1,id2]`         (cppcheck-suppress alt)
    //   ` rule1 rule2`      (eslint bare)
    // If none present → bare form → suppress everything.
    const int tokEnd = bareMatch.capturedEnd();
    const QString tail = body.mid(tokEnd).trimmed();

    // Nothing after the token = bare suppress.
    if (tail.isEmpty() || tail.startsWith("--")) return true;

    // Pull out anything that looks like a rule-id list.
    //
    // 0.7.55 (2026-04-27 indie-review) — terminator class previously
    // included `-`, which collides with the rule-id charset (rule IDs
    // commonly contain hyphens, e.g. `bash-c-non-literal`,
    // `google-cloud-credentials`). The non-greedy body match would
    // stop at the first `-` in the rule ID, and `// nosemgrep:
    // bash-c-non-literal` matched only `bash`, leaving the rest of
    // the suppression silently inactive. Terminator class is now
    // `[)\]]|$` — close-paren / close-bracket / end-of-string only.
    static const QRegularExpression reList(
        R"([:=(\[\s]\s*([a-z0-9_\-\*\s,\.]+?)(?:[)\]]|$))",
        QRegularExpression::CaseInsensitiveOption);
    const auto listMatch = reList.match(QString(" ") + tail);  // leading space ensures capture
    if (!listMatch.hasMatch()) return true;   // token present but shape unknown → conservative suppress

    const QString listStr = listMatch.captured(1);
    static const QRegularExpression reIdSep(R"([,\s]+)");  // ANTS-1647
    const QStringList ids = listStr.split(reIdSep, Qt::SkipEmptyParts);
    for (const QString &raw : ids) {
        const QString id = raw.trimmed();
        if (id.isEmpty()) continue;
        // Exact match or glob like `google-*`.
        if (id == rule) return true;
        if (id.contains('*')) {
            QString pat = QRegularExpression::escape(id);
            pat.replace("\\*", ".*");
            if (QRegularExpression("^" + pat + "$",
                    QRegularExpression::CaseInsensitiveOption)
                    .match(rule).hasMatch()) {
                return true;
            }
        }
    }
    return false;
}

// ANTS-1343 — see header comment. Pre-collapse `r.findings.size()` is
// captured before reassignment so the authored `findingCount` reflects
// the count the user actually saw on the wire from mypy, not the post-
// fold list size.
void consolidateMypyStubHints(CheckResult &r) {
    if (r.checkId != QStringLiteral("mypy")) return;
    if (r.findings.size() < 2) return;

    // Raw string uses `re` delimiter because the pattern itself
    // contains `)"`. Mirrors the prior AuditDialog::consolidate-
    // MypyStubHints body verbatim.
    static const QRegularExpression stubRe(
        QStringLiteral(R"re(Library stubs not installed for "([A-Za-z0-9_.\-]+)")re"));

    QStringList packages;
    QList<Finding> kept;
    kept.reserve(r.findings.size());
    for (const Finding &f : std::as_const(r.findings)) {
        const QRegularExpressionMatch m = stubRe.match(f.message);
        if (m.hasMatch()) {
            const QString pkg = m.captured(1);
            if (!packages.contains(pkg)) packages << pkg;
        } else {
            kept.append(f);
        }
    }
    if (packages.size() < 2) return;  // nothing to fold

    Finding hint;
    hint.checkId   = r.checkId;
    hint.checkName = r.checkName;
    hint.category  = r.category;
    hint.type      = CheckType::Info;
    hint.severity  = Severity::Info;
    hint.source    = QStringLiteral("mypy");
    QStringList pipTypes;
    for (const QString &p : std::as_const(packages))
        pipTypes << QStringLiteral("types-") + QString(p).replace('.', '-');
    hint.message = QStringLiteral("%1 missing stub package(s): pip install %2")
                       .arg(packages.size())
                       .arg(pipTypes.join(QChar(' ')));
    hint.dedupKey = computeDedup(
        QStringLiteral("mypy-stub-hint"), 0,
        QStringLiteral("mypy-stub-hint"),
        packages.join(QChar(',')));
    kept.prepend(hint);

    // ANTS-1343 — preserve the pre-collapse raw count. The post-cap
    // dispatcher at auditdialog.cpp's runCheck completion gates on
    // findingCountAuthored so the v1 `findings.size() + omittedCount`
    // overwrite no longer clobbers this value.
    const int preCollapse = r.findings.size();
    r.findings = kept;
    r.findingCount         = preCollapse + r.omittedCount;
    r.findingCountAuthored = true;
}

// ANTS-1254 — last_audit_summary helpers + parser.

namespace {

// SARIF level → ordinal (per spec § 2.0). Absent / unknown → 1
// (treated as "warning" — INV-3).
int levelOrdinal(const QString &level) {
    if (level == QStringLiteral("error")) return 2;
    if (level == QStringLiteral("note"))  return 0;
    return 1;  // warning + default
}

// Foreign-SARIF fallback when the rule index doesn't carry severity
// (per spec § 3.1 step 4). Maps level → 3 of the 5 internal labels;
// BLOCKER/MINOR are unreachable from foreign SARIF by design.
QString severityFromLevel(const QString &level) {
    if (level == QStringLiteral("error")) return QStringLiteral("CRITICAL");
    if (level == QStringLiteral("note"))  return QStringLiteral("INFO");
    return QStringLiteral("MAJOR");
}

// ANTS-3372 — parse-artifact guard. Scanner output occasionally leaks a
// progress-bar line ("Working... ━━ 100% 0") into the findings stream; it
// parses as a finding with no real location (line 0, no confidence) whose
// `file` field is a chunk of terminal chrome rather than a path, and it can
// out-rank real findings as top_findings[0]. Drop these before ranking.
// Conservative on purpose: requires BOTH the no-location signature (line 0
// AND the default confidence -1) AND a junk-`file` signal — box-drawing
// glyphs (U+2500..U+257F, progress-bar fill) or a bare percentage with no
// path separator — so a genuine project-level finding at line 0 is never
// dropped.
bool isLikelyParseArtifact(const AuditSummaryFinding &f) {
    if (f.line != 0 || f.confidence != -1) return false;
    for (const QChar c : f.file) {
        const char16_t u = c.unicode();
        if (u >= 0x2500 && u <= 0x257F) return true;  // box-drawing (progress bars)
    }
    return f.file.contains(QLatin1Char('%')) && !f.file.contains(QLatin1Char('/'));
}

// ANTS-3590 — a zero-content SARIF result: empty ruleId AND empty message AND
// empty artifact uri AND startLine 0. Trivy (and any adapter that renders an
// empty result set as one blank result) can emit one; it is "nothing found",
// not a finding, so it must not tally into counts or top_findings. A real
// finding always carries at least one of these fields. Defense-in-depth: the
// producer-side drop lives in auditrunner.cpp, this guards cached/foreign SARIFs.
bool isEmptyPlaceholderResult(const QJsonObject &res) {
    if (!res.value(QStringLiteral("ruleId")).toString().isEmpty()) return false;
    if (!res.value(QStringLiteral("message")).toObject()
             .value(QStringLiteral("text")).toString().isEmpty()) return false;
    const QJsonArray locs = res.value(QStringLiteral("locations")).toArray();
    if (!locs.isEmpty()) {
        const QJsonObject phys = locs.first().toObject()
            .value(QStringLiteral("physicalLocation")).toObject();
        if (!phys.value(QStringLiteral("artifactLocation")).toObject()
                 .value(QStringLiteral("uri")).toString().isEmpty())
            return false;
        if (phys.value(QStringLiteral("region")).toObject()
                .value(QStringLiteral("startLine")).toInt(0) != 0)
            return false;
    }
    return true;
}

}  // namespace

std::optional<AuditSummary> summariseSarif(
    const QString &sarifPath,
    int topN,
    const QString &levelFloor) {
    QFile f(sarifPath);
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    const QByteArray bytes = f.readAll();
    f.close();

    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        return std::nullopt;
    }
    const QJsonObject root = doc.object();
    const QJsonArray runs = root.value(QStringLiteral("runs")).toArray();
    if (runs.isEmpty()) return std::nullopt;  // INV-10 → not_audited

    AuditSummary s;
    s.sarifPath    = sarifPath;
    s.sourceFormat = QStringLiteral("sarif");  // ANTS-1459

    const int floorOrd = levelOrdinal(levelFloor);
    QList<AuditSummaryFinding> pool;

    // ANTS-1760 — fold ALL runs[]. The headless AuditRunner emits one
    // run per tool (idiomatic SARIF), so the pre-fix runs.first()-only
    // read summarised just the first tool in hash order and dropped the
    // rest. The dialog's single-run export still works (one run →
    // identical result). Per-run rule index resolves each result against
    // its own tool's rules; run-level metadata (run time, VCS) is
    // first-run-wins.
    for (const QJsonValue &runv : runs) {
        const QJsonObject run = runv.toObject();

        // invocations[0].startTimeUtc — first run that carries it.
        if (s.runAtIso.isEmpty()) {
            const QJsonArray invocations =
                run.value(QStringLiteral("invocations")).toArray();
            if (!invocations.isEmpty()) {
                s.runAtIso = invocations.first().toObject()
                    .value(QStringLiteral("startTimeUtc")).toString();
            }
        }

        // ANTS-1539 — SARIF run.versionControlProvenance § 3.14.18.
        // First run that carries the block wins (single-repo audits).
        if (s.branch.isEmpty() && s.commit.isEmpty() &&
            s.repositoryUri.isEmpty()) {
            const QJsonArray vcp = run.value(
                QStringLiteral("versionControlProvenance")).toArray();
            if (!vcp.isEmpty()) {
                const QJsonObject vcs = vcp.first().toObject();
                s.branch        = vcs.value(QStringLiteral("branch")).toString();
                s.commit        = vcs.value(QStringLiteral("revisionId")).toString();
                s.repositoryUri = vcs.value(QStringLiteral("repositoryUri")).toString();
            }
        }

        // Spec § 3.1 step 3 — build rule index keyed by id → severity.
        QHash<QString, QString> ruleSeverity;
        const QJsonObject tool = run.value(QStringLiteral("tool")).toObject();
        const QJsonObject driver = tool.value(QStringLiteral("driver")).toObject();
        const QJsonArray rules = driver.value(QStringLiteral("rules")).toArray();
        for (const QJsonValue &rv : rules) {
            const QJsonObject ro = rv.toObject();
            const QString id = ro.value(QStringLiteral("id")).toString();
            if (id.isEmpty()) continue;
            const QString sev = ro.value(QStringLiteral("properties")).toObject()
                                  .value(QStringLiteral("severity")).toString();
            if (!sev.isEmpty()) ruleSeverity.insert(id, sev);
        }

        // Spec § 3.1 step 4 — walk results, count, filter, build pool.
        const QJsonArray results = run.value(QStringLiteral("results")).toArray();
        for (const QJsonValue &rv : results) {
            const QJsonObject res = rv.toObject();
            // ANTS-3590 — skip a zero-content placeholder result before the
            // tally so a clean repo whose SARIF carries a blank trivy result
            // does not read as "1 actionable" with a blank MAJOR row.
            if (isEmptyPlaceholderResult(res)) continue;
            const QString level = res.value(QStringLiteral("level")).toString(
                QStringLiteral("warning"));

            const int ord = levelOrdinal(level);
            if (ord == 2) ++s.countError;
            else if (ord == 0) ++s.countNote;
            else ++s.countWarning;

            const QJsonArray suppressions = res.value(
                QStringLiteral("suppressions")).toArray();
            if (!suppressions.isEmpty()) ++s.countSuppressed;

            if (ord < floorOrd) continue;

            AuditSummaryFinding asf;
            asf.level   = level;
            asf.ruleId  = res.value(QStringLiteral("ruleId")).toString();
            asf.message = res.value(QStringLiteral("message")).toObject()
                              .value(QStringLiteral("text")).toString();

            const QJsonArray locs = res.value(QStringLiteral("locations")).toArray();
            if (!locs.isEmpty()) {
                const QJsonObject phys = locs.first().toObject()
                    .value(QStringLiteral("physicalLocation")).toObject();
                asf.file = phys.value(QStringLiteral("artifactLocation")).toObject()
                               .value(QStringLiteral("uri")).toString();
                asf.line = phys.value(QStringLiteral("region")).toObject()
                               .value(QStringLiteral("startLine")).toInt(0);
            }

            const QJsonObject props = res.value(QStringLiteral("properties")).toObject();
            if (props.contains(QStringLiteral("confidence"))) {
                asf.confidence = props.value(QStringLiteral("confidence")).toInt(-1);
            }
            asf.highConfidence = props.value(
                QStringLiteral("highConfidence")).toBool(false);

            // Resolve severity via rule index, fall back to level map.
            const auto it = ruleSeverity.constFind(asf.ruleId);
            asf.severity = (it != ruleSeverity.constEnd())
                               ? it.value() : severityFromLevel(level);

            pool.append(std::move(asf));
        }
    }

    // ANTS-3372 — strip scanner progress-bar artifacts before ranking.
    pool.removeIf(isLikelyParseArtifact);

    // Spec § 3.1 step 5 — sort: level desc → confidence desc →
    // file asc → line asc.
    std::sort(pool.begin(), pool.end(),
              [](const AuditSummaryFinding &a,
                 const AuditSummaryFinding &b) {
                  const int la = levelOrdinal(a.level);
                  const int lb = levelOrdinal(b.level);
                  if (la != lb) return la > lb;
                  if (a.confidence != b.confidence)
                      return a.confidence > b.confidence;
                  if (a.file != b.file) return a.file < b.file;
                  return a.line < b.line;
              });

    // Spec § 3.1 step 6 — clamp.
    if (topN < 0) topN = 0;
    if (pool.size() > topN) pool.erase(pool.begin() + topN, pool.end());
    s.topFindings = std::move(pool);

    // Spec § 3.1 step 8 — derive htmlPath.
    QFileInfo sarifInfo(sarifPath);
    QString htmlCandidate = sarifInfo.absoluteFilePath();
    htmlCandidate.chop(6);  // strip ".sarif"
    htmlCandidate.append(QStringLiteral(".html"));
    if (QFile::exists(htmlCandidate)) {
        s.htmlPath = htmlCandidate;
    } else {
        // Fallback (a) failed; lex-max audit-*.html within ±60 s.
        // Filename pattern: audit-YYYYMMDD-HHmmss.{sarif,html}.
        const QString sarifBase = sarifInfo.completeBaseName();  // "audit-YYYYMMDD-HHmmss"
        QDateTime sarifStamp;
        if (sarifBase.startsWith(QStringLiteral("audit-")) && sarifBase.size() == 21) {
            sarifStamp = QDateTime::fromString(
                sarifBase.mid(6), QStringLiteral("yyyyMMdd-HHmmss"));
        }
        if (sarifStamp.isValid()) {
            QDir cacheDir = sarifInfo.absoluteDir();
            const QStringList htmls = cacheDir.entryList(
                QStringList{QStringLiteral("audit-*.html")},
                QDir::Files, QDir::Name | QDir::Reversed);
            for (const QString &name : htmls) {
                if (name.size() != 26) continue;  // "audit-YYYYMMDD-HHmmss.html"
                const QDateTime stamp = QDateTime::fromString(
                    name.mid(6, 15), QStringLiteral("yyyyMMdd-HHmmss"));
                if (!stamp.isValid()) continue;
                if (std::abs(sarifStamp.secsTo(stamp)) <= 60) {
                    s.htmlPath = cacheDir.absoluteFilePath(name);
                    break;
                }
            }
        }
    }

    return s;
}

// ANTS-1459 — cppcheck native XML parser. Mirrors summariseSarif's
// AuditSummary output so last_audit_summary callers receive the same
// envelope shape regardless of which format the project ships.
//
// Format reference: cppcheck --xml --xml-version=2 emits a tree
//   <results version="2">
//     <cppcheck version="2.x"/>
//     <errors>
//       <error id="…" severity="…" msg="…" verbose="…">
//         <location file="…" line="…" column="…"/>
//       </error>
//     </errors>
//   </results>
// Only the <error> elements (and their first <location>) carry the
// finding payload we surface. CWE / inconclusive / hash attributes
// are ignored at v1 — they have no slot in AuditSummaryFinding.
std::optional<AuditSummary> summariseCppcheckXml(
    const QString &xmlPath,
    int topN,
    const QString &levelFloor) {
    QFile f(xmlPath);
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;

    AuditSummary s;
    s.sarifPath    = xmlPath;  // historical field name; XML path lives here
    s.sourceFormat = QStringLiteral("cppcheck-xml");
    // ANTS-2123 — countSuppressed stays 0 here by construction: cppcheck's XML
    // carries no per-finding suppression metadata (inconclusive/--suppress are
    // applied tool-side and the finding is simply absent from the output), so
    // there is nothing to tally. Documented gap vs the SARIF/semgrep paths.

    const int floorOrd = levelOrdinal(levelFloor);
    QList<AuditSummaryFinding> pool;

    // QXmlStreamReader does not expand DTD entities by default,
    // so XXE / billion-laughs payloads pasted into a malicious
    // cppcheck-*.xml are not reachable here. No extra hardening
    // needed.
    QXmlStreamReader xml(&f);
    bool sawResults = false;
    while (!xml.atEnd() && !xml.hasError()) {
        const auto tt = xml.readNext();
        if (tt != QXmlStreamReader::StartElement) continue;
        const QStringView name = xml.name();
        if (name == QStringLiteral("results")) {
            sawResults = true;
            continue;
        }
        if (name != QStringLiteral("error")) continue;

        const QXmlStreamAttributes a = xml.attributes();
        const QString cppSev =
            a.value(QStringLiteral("severity")).toString();
        const QString id =
            a.value(QStringLiteral("id")).toString();
        const QString msg =
            a.value(QStringLiteral("msg")).toString();

        // Map cppcheck severity → SARIF-level + severity-tier string.
        QString sarifLevel;
        QString sevTier;
        if (cppSev == QLatin1String("error")) {
            sarifLevel = QStringLiteral("error");
            sevTier    = QStringLiteral("MAJOR");
        } else if (cppSev == QLatin1String("warning")) {
            sarifLevel = QStringLiteral("warning");
            sevTier    = QStringLiteral("MAJOR");
        } else if (cppSev == QLatin1String("information")) {
            sarifLevel = QStringLiteral("note");
            sevTier    = QStringLiteral("INFO");
        } else {  // style / performance / portability / unknown
            sarifLevel = QStringLiteral("note");
            sevTier    = QStringLiteral("MINOR");
        }

        const int ord = levelOrdinal(sarifLevel);
        if (ord == 2)      ++s.countError;
        else if (ord == 0) ++s.countNote;
        else               ++s.countWarning;

        // First nested <location> only (cppcheck can emit several;
        // v1 mirrors SARIF's locations[0] convention).
        QString locFile;
        int     locLine = 0;
        while (!xml.atEnd()) {
            const auto next = xml.readNext();
            if (next == QXmlStreamReader::EndElement &&
                xml.name() == QStringLiteral("error")) {
                break;
            }
            if (next == QXmlStreamReader::StartElement &&
                xml.name() == QStringLiteral("location") &&
                locFile.isEmpty()) {
                const auto la = xml.attributes();
                locFile = la.value(QStringLiteral("file")).toString();
                locLine = la.value(QStringLiteral("line")).toInt();
            }
        }

        if (ord < floorOrd) continue;

        AuditSummaryFinding asf;
        asf.level    = sarifLevel;
        asf.severity = sevTier;
        asf.ruleId   = id;
        asf.message  = msg;
        asf.file     = locFile;
        asf.line     = locLine;
        // cppcheck XML has no confidence signal at v1; leave defaults.
        pool.append(std::move(asf));
    }

    if (xml.hasError() || !sawResults) return std::nullopt;

    // ANTS-3372 — strip scanner progress-bar artifacts before ranking.
    pool.removeIf(isLikelyParseArtifact);

    // Sort + clamp identically to summariseSarif (level desc →
    // confidence desc → file asc → line asc).
    std::sort(pool.begin(), pool.end(),
              [](const AuditSummaryFinding &a,
                 const AuditSummaryFinding &b) {
                  const int la = levelOrdinal(a.level);
                  const int lb = levelOrdinal(b.level);
                  if (la != lb) return la > lb;
                  if (a.confidence != b.confidence)
                      return a.confidence > b.confidence;
                  if (a.file != b.file) return a.file < b.file;
                  return a.line < b.line;
              });
    if (topN < 0) topN = 0;
    if (pool.size() > topN) {
        pool.erase(pool.begin() + topN, pool.end());
    }
    s.topFindings = std::move(pool);
    return s;
}

// ANTS-1494 — clang-tidy native text parser. Format:
//   path/to/file.cpp:42:5: warning: message text [check-name]
// Multi-line context (^^^ markers, fix-it hints) is ignored — only the
// finding header lines are surfaced.
std::optional<AuditSummary> summariseClangTidyText(
    const QString &textPath,
    int topN,
    const QString &levelFloor) {
    QFile f(textPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return std::nullopt;

    AuditSummary s;
    s.sarifPath    = textPath;
    s.sourceFormat = QStringLiteral("clang-tidy-text");
    // ANTS-2123 — countSuppressed stays 0 here by construction: clang-tidy's
    // text output carries no per-finding suppression metadata (NOLINT-ignored
    // diagnostics are dropped tool-side before printing), so there is nothing
    // to tally. Documented gap vs the SARIF/semgrep paths.

    const int floorOrd = levelOrdinal(levelFloor);
    QList<AuditSummaryFinding> pool;

    // Pattern: <file>:<line>:<col>: <severity>: <message> [<rule>]
    // Rule suffix is optional — clang-tidy occasionally omits it for
    // notes attached to a parent diagnostic.
    static const QRegularExpression rx(QStringLiteral(
        "^([^:]+):(\\d+):(\\d+):\\s*"
        "(warning|error|note):\\s*"
        "(.*?)(?:\\s*\\[([^\\]]+)\\])?$"));

    QTextStream ts(&f);
    while (!ts.atEnd()) {
        const QString line = ts.readLine();
        const auto m = rx.match(line);
        if (!m.hasMatch()) continue;
        const QString file    = m.captured(1);
        const int     lineNo  = m.captured(2).toInt();
        const QString sev     = m.captured(4);
        const QString msg     = m.captured(5);
        const QString ruleId  = m.captured(6);  // may be empty

        // Skip clang-tidy "<N> warnings/errors generated." trailers.
        if (file.contains(QLatin1Char(' '))) continue;

        QString sarifLevel = sev;          // warning/error/note → SARIF 1:1
        QString sevTier;
        if (sev == QLatin1String("error"))       sevTier = QStringLiteral("MAJOR");
        else if (sev == QLatin1String("warning")) sevTier = QStringLiteral("MAJOR");
        else                                       sevTier = QStringLiteral("INFO");

        const int ord = levelOrdinal(sarifLevel);
        if (ord == 2)      ++s.countError;
        else if (ord == 0) ++s.countNote;
        else               ++s.countWarning;

        if (ord < floorOrd) continue;

        AuditSummaryFinding asf;
        asf.level    = sarifLevel;
        asf.severity = sevTier;
        asf.ruleId   = ruleId;
        asf.message  = msg;
        asf.file     = file;
        asf.line     = lineNo;
        pool.append(std::move(asf));
    }

    if (pool.isEmpty() && s.countError + s.countWarning + s.countNote == 0) {
        return std::nullopt;  // no parseable lines — likely wrong format
    }

    // ANTS-3372 — strip scanner progress-bar artifacts before ranking.
    pool.removeIf(isLikelyParseArtifact);

    std::sort(pool.begin(), pool.end(),
              [](const AuditSummaryFinding &a,
                 const AuditSummaryFinding &b) {
                  const int la = levelOrdinal(a.level);
                  const int lb = levelOrdinal(b.level);
                  if (la != lb) return la > lb;
                  if (a.file != b.file) return a.file < b.file;
                  return a.line < b.line;
              });
    if (topN < 0) topN = 0;
    if (pool.size() > topN) pool.erase(pool.begin() + topN, pool.end());
    s.topFindings = std::move(pool);
    return s;
}

// ANTS-1494 — semgrep JSON output (`semgrep --json`).
std::optional<AuditSummary> summariseSemgrepJson(
    const QString &jsonPath,
    int topN,
    const QString &levelFloor) {
    QFile f(jsonPath);
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    const QByteArray raw = f.readAll();
    f.close();

    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject())
        return std::nullopt;

    const QJsonArray results =
        doc.object().value(QStringLiteral("results")).toArray();

    // ANTS-2003 — a semgrep run that errored (e.g. a bad --config, a parse
    // failure on every file) emits a populated errors[] with an empty
    // results[]. Reporting that as a clean zero-finding run is a false
    // all-clear. Treat "errors present, no results" as a failed parse so the
    // caller surfaces it instead of "0 findings". A run with BOTH errors and
    // results is a partial success — keep the findings it did produce.
    const QJsonArray errors =
        doc.object().value(QStringLiteral("errors")).toArray();
    if (results.isEmpty() && !errors.isEmpty()) return std::nullopt;

    AuditSummary s;
    s.sarifPath    = jsonPath;
    s.sourceFormat = QStringLiteral("semgrep-json");

    const int floorOrd = levelOrdinal(levelFloor);
    QList<AuditSummaryFinding> pool;

    for (const QJsonValue &v : results) {
        const QJsonObject ro = v.toObject();
        const QString file   = ro.value(QStringLiteral("path")).toString();
        const int     lineNo = ro.value(QStringLiteral("start"))
                                  .toObject()
                                  .value(QStringLiteral("line")).toInt();
        const QString ruleId = ro.value(QStringLiteral("check_id")).toString();
        const QJsonObject extra =
            ro.value(QStringLiteral("extra")).toObject();
        const QString msg    = extra.value(QStringLiteral("message")).toString();
        const QString sevRaw = extra.value(QStringLiteral("severity"))
                                    .toString().toUpper();

        QString sarifLevel;
        QString sevTier;
        if (sevRaw == QLatin1String("ERROR")) {
            sarifLevel = QStringLiteral("error");
            sevTier    = QStringLiteral("MAJOR");
        } else if (sevRaw == QLatin1String("WARNING")) {
            sarifLevel = QStringLiteral("warning");
            sevTier    = QStringLiteral("MAJOR");
        } else {
            sarifLevel = QStringLiteral("note");
            sevTier    = QStringLiteral("INFO");
        }

        const int ord = levelOrdinal(sarifLevel);
        if (ord == 2)      ++s.countError;
        else if (ord == 0) ++s.countNote;
        else               ++s.countWarning;

        // ANTS-2123 — SARIF parity. semgrep marks a `# nosemgrep`-ignored
        // finding with extra.is_ignored=true. summariseSarif tallies
        // suppressions[] into countSuppressed (ANTS-1254 INV-3) as a PARALLEL
        // count that does not exclude the finding; mirror that here so
        // last_audit_summary reports an honest non-zero `suppressed` for a
        // semgrep run instead of always 0 (which contradicted the SARIF path
        // for the same logical run).
        if (extra.value(QStringLiteral("is_ignored")).toBool())
            ++s.countSuppressed;

        if (ord < floorOrd) continue;

        AuditSummaryFinding asf;
        asf.level    = sarifLevel;
        asf.severity = sevTier;
        asf.ruleId   = ruleId;
        asf.message  = msg;
        asf.file     = file;
        asf.line     = lineNo;
        pool.append(std::move(asf));
    }

    // ANTS-3372 — strip scanner progress-bar artifacts before ranking.
    pool.removeIf(isLikelyParseArtifact);

    std::sort(pool.begin(), pool.end(),
              [](const AuditSummaryFinding &a,
                 const AuditSummaryFinding &b) {
                  const int la = levelOrdinal(a.level);
                  const int lb = levelOrdinal(b.level);
                  if (la != lb) return la > lb;
                  if (a.file != b.file) return a.file < b.file;
                  return a.line < b.line;
              });
    if (topN < 0) topN = 0;
    if (pool.size() > topN) pool.erase(pool.begin() + topN, pool.end());
    s.topFindings = std::move(pool);
    return s;
}

// ANTS-1111 — severity-tier shift on cross-tool corroboration.
void applyCorroborationShift(QList<Finding> &findings,
                             const QSet<QString> &noisyRules) {
    if (findings.isEmpty()) return;

    // Build (file:line) → set<checkId> coverage map.
    QHash<QString, QSet<QString>> coverage;
    coverage.reserve(findings.size());
    for (const Finding &f : findings) {
        if (f.file.isEmpty() || f.line <= 0) continue;
        const QString key = f.file + QChar(':') + QString::number(f.line);
        coverage[key].insert(f.checkId);
    }

    auto clamp = [](int v) {
        if (v < int(Severity::Info)) return int(Severity::Info);
        if (v > int(Severity::Blocker)) return int(Severity::Blocker);
        return v;
    };

    for (Finding &f : findings) {
        if (f.file.isEmpty() || f.line <= 0) continue;
        const QString key = f.file + QChar(':') + QString::number(f.line);
        const int distinct = coverage.value(key).size();
        int shift = 0;
        if (distinct >= 2) {
            shift = +1;
        } else if (distinct == 1 && noisyRules.contains(f.checkId)) {
            shift = -1;
        }
        if (shift != 0) {
            f.severity = static_cast<Severity>(
                clamp(int(f.severity) + shift));
        }
    }
}

// ANTS-1111 — render a fold-in subsection block per
// roadmap-format.md § 3.8 + § 3.5.
QString templateRoadmapFoldInBlock(const QList<Finding> &actionable,
                                   const QList<int> &allocatedIds,
                                   const QString &dateIso) {
    if (actionable.isEmpty() || actionable.size() != allocatedIds.size()) {
        return {};
    }

    auto laneFromPath = [](const QString &p) -> QString {
        if (p.startsWith(QStringLiteral("src/"))) {
            // src/foo/bar.cpp -> foo
            const int slash = p.indexOf('/', 4);
            if (slash > 4) return p.mid(4, slash - 4);
            // src/foo.cpp -> foo (basename without ext)
            QString rest = p.mid(4);
            const int dot = rest.lastIndexOf('.');
            if (dot > 0) rest.truncate(dot);
            return rest;
        }
        if (p.startsWith(QStringLiteral("tests/"))) return QStringLiteral("tests");
        if (p.startsWith(QStringLiteral("docs/"))) return QStringLiteral("docs");
        if (p.startsWith(QStringLiteral("packaging/"))) return QStringLiteral("packaging");
        return QStringLiteral("misc");
    };

    auto themeFromMessage = [](const QString &m) -> QString {
        // First sentence (up to '.') trimmed; cap at 80 chars.
        const int dot = m.indexOf('.');
        QString first = (dot > 0) ? m.left(dot) : m;
        first = first.trimmed();
        if (first.size() > 80) first = first.left(77) + QStringLiteral("...");
        return first;
    };

    // ANTS-2003 — finding text is interpolated into ROADMAP.md markdown. Escape
    // the inline actives so a `**`/backtick/bracket in a message can't break the
    // **bold** bullet or the [ANTS-NNNN] link (or inject a new list item).
    auto mdInline = [](const QString &s) -> QString {
        QString out;
        out.reserve(s.size() + 8);
        for (const QChar c : s) {
            if (c == '\\' || c == '`' || c == '*' || c == '_' ||
                c == '[' || c == ']')
                out += QChar('\\');
            out += c;
        }
        return out;
    };
    // A backtick cannot be backslash-escaped inside a `code span` — swap it for
    // an apostrophe so the path/rule span stays closed.
    auto mdCode = [](QString s) -> QString {
        return s.replace(QChar('`'), QChar('\''));
    };

    QString out;
    out.reserve(256 + actionable.size() * 200);
    out += QStringLiteral("### 🔍 Audit fold-in (");
    out += dateIso;
    out += QStringLiteral(")\n\n");

    for (int i = 0; i < actionable.size(); ++i) {
        const Finding &f = actionable.at(i);
        const int id = allocatedIds.at(i);
        const QString theme = themeFromMessage(f.message);
        const QString lane = laneFromPath(f.file);

        out += QStringLiteral("- 📋 [ANTS-");
        out += QString::number(id);
        out += QStringLiteral("] **");
        out += mdInline(theme);
        out += QStringLiteral(".**\n");
        if (!f.file.isEmpty() && f.line > 0) {
            out += QStringLiteral("  At `");
            out += mdCode(f.file);
            out += QChar(':');
            out += QString::number(f.line);
            out += QStringLiteral("` (rule `");
            out += mdCode(f.checkId);
            out += QStringLiteral("`).\n");
        } else if (!f.checkId.isEmpty()) {
            out += QStringLiteral("  Rule `");
            out += mdCode(f.checkId);
            out += QStringLiteral("`.\n");
        }
        out += QStringLiteral("  Kind: audit-fix.\n");
        out += QStringLiteral("  Source: audit-");
        out += dateIso;
        out += QStringLiteral(".\n");
        out += QStringLiteral("  Lanes: ");
        out += lane;
        out += QStringLiteral(".\n");
        if (i + 1 < actionable.size()) out += QChar('\n');
    }

    return out;
}

// ANTS-1576 — capture best-effort VCS state for SARIF writers. Forks
// up to three short git subprocesses (rev-parse, symbolic-ref,
// remote.origin.url) with a 2 s wall-clock cap each. Returns an
// empty array when rev-parse fails — callers omit the SARIF
// run.versionControlProvenance block in that case rather than
// emit a half-populated record.
QJsonArray buildVcsProvenanceBlock(const QString &rootCanonical) {
    if (rootCanonical.isEmpty()) return {};

    auto runShort = [&](const QStringList &argv) -> QString {
        QProcess p;
        p.setProcessChannelMode(QProcess::SeparateChannels);
        QStringList full;
        full << QStringLiteral("-C") << rootCanonical;
        full.append(argv);
        p.start(QStringLiteral("git"), full);
        if (!p.waitForStarted(1000)) return {};
        if (!p.waitForFinished(2000)) {
            p.kill();
            p.waitForFinished(500);
            return {};
        }
        if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
            return {};
        }
        return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
    };

    const QString head = runShort({QStringLiteral("rev-parse"),
                                   QStringLiteral("HEAD")});
    if (head.isEmpty()) return {};

    const QString branch = runShort({QStringLiteral("symbolic-ref"),
                                     QStringLiteral("--short"),
                                     QStringLiteral("HEAD")});
    QString remote = runShort({QStringLiteral("config"),
                               QStringLiteral("--get"),
                               QStringLiteral("remote.origin.url")});
    if (remote.isEmpty()) {
        remote = QStringLiteral("file://") + rootCanonical;
    }

    QJsonObject vcs;
    vcs[QStringLiteral("repositoryUri")] = remote;
    vcs[QStringLiteral("revisionId")]    = head;
    if (!branch.isEmpty()) vcs[QStringLiteral("branch")] = branch;

    QJsonArray arr;
    arr.append(vcs);
    return arr;
}

}  // namespace AuditEngine
