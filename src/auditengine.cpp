#include "auditengine.h"

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
#include <QJsonValue>
#include <QRegularExpression>
#include <QXmlStreamReader>

#include <algorithm>
#include <cstdlib>

namespace AuditEngine {

// Catastrophic-regex shape detector (ANTS-1123 indie-review
// C1/C2/C3 unification: previously this lived in two places —
// engine's anonymous-namespace local + AuditDialog's static. The
// shapes detected drifted between them. Single definition now.)
bool isCatastrophicRegex(const QString &pattern) {
    // Shape A — quantifier-under-quantifier inside a group:
    // `(.+)+`, `(a*)+`, `(a+b)*`, etc.
    static const QRegularExpression nestedQuant(
        QStringLiteral(R"(\([^()]*[+*][^()]*\)[?*+])"));
    // Indie-review-2026-05-14 lane-4 M4 — Shape B —
    // alternation-under-quantifier inside a group: `(a|b)+`,
    // `(a|aa)*`, `(x|y|z)+`. The pre-fix detector advertised in
    // its comment that it catches these too, but the regex above
    // requires `[+*]` inside the parens — alternation alone
    // wouldn't match. PCRE2's LIMIT_MATCH was the only guard.
    // Now the conservative detector matches its docstring.
    static const QRegularExpression altQuant(
        QStringLiteral(R"(\([^()]*\|[^()]*\)[?*+])"));
    return nestedQuant.match(pattern).hasMatch()
           || altQuant.match(pattern).hasMatch();
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
        // ANTS-1123 indie-review M2: empty lines are dropped silently
        // — most checker outputs separate findings with blank lines and
        // those carry no signal for the dedup/SARIF pipeline downstream.
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
        if (auto m1 = reFileLineCol.match(line); m1.hasMatch()) {
            f.file = m1.captured(1);
            f.line = m1.captured(2).toInt();
        } else if (auto m2 = reFileLine.match(line); m2.hasMatch()) {
            f.file = m2.captured(1);
            f.line = m2.captured(2).toInt();
        } else if (auto m3 = reJustFile.match(line); m3.hasMatch()) {
            f.file = m3.captured(1);
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

    const QJsonObject run = runs.first().toObject();

    AuditSummary s;
    s.sarifPath    = sarifPath;
    s.sourceFormat = QStringLiteral("sarif");  // ANTS-1459

    // INV-1 single-run; ignore runs[1..]. invocations[0].startTimeUtc.
    const QJsonArray invocations = run.value(QStringLiteral("invocations")).toArray();
    if (!invocations.isEmpty()) {
        s.runAtIso = invocations.first().toObject()
                         .value(QStringLiteral("startTimeUtc")).toString();
    }

    // Spec § 3.1 step 3 — build rule index keyed by id → severity string.
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

    const int floorOrd = levelOrdinal(levelFloor);

    // Spec § 3.1 step 4 — walk results, count, filter, build pool.
    QList<AuditSummaryFinding> pool;
    const QJsonArray results = run.value(QStringLiteral("results")).toArray();
    pool.reserve(results.size());
    for (const QJsonValue &rv : results) {
        const QJsonObject res = rv.toObject();
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
        out += theme;
        out += QStringLiteral(".**\n");
        if (!f.file.isEmpty() && f.line > 0) {
            out += QStringLiteral("  At `");
            out += f.file;
            out += QChar(':');
            out += QString::number(f.line);
            out += QStringLiteral("` (rule `");
            out += f.checkId;
            out += QStringLiteral("`).\n");
        } else if (!f.checkId.isEmpty()) {
            out += QStringLiteral("  Rule `");
            out += f.checkId;
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

}  // namespace AuditEngine
