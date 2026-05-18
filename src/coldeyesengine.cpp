#include "coldeyesengine.h"

#include "falseposledger.h"
#include "indiereviewengine.h"

#include <QChar>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPair>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace ColdEyesEngine {

namespace {

QString slurpUtf8(const QString &absPath) {
    QFile f(absPath);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

// INV-13 path-rule defence: canonicalise candidate, accept only if it
// remains under projectPath's canonical form.
bool isInsideProject(const QString &projectPath, const QString &candidate) {
    const QString rootCanon = QFileInfo(projectPath).canonicalFilePath();
    if (rootCanon.isEmpty()) return false;
    const QString candCanon = QFileInfo(candidate).canonicalFilePath();
    if (candCanon.isEmpty()) return false;
    return candCanon == rootCanon
           || candCanon.startsWith(rootCanon + QChar('/'));
}

bool fileExists(const QString &projectPath, const QString &rel) {
    return QFileInfo::exists(projectPath + QChar('/') + rel);
}

// ANTS-1506 — case-insensitive contract-doc resolver. Mirrors the
// projectlayoutengine helper: walks each path component against the
// real on-disk entries so `Claude.md` / `readme.md` / `Roadmap.md`
// resolve to the canonical contract-doc names instead of silently
// missing. Returns the actual on-disk relative path that matched,
// or QString() on miss.
QString caseInsensitiveResolve(const QString &projectPath,
                               const QString &relPath) {
    if (QFileInfo::exists(projectPath + QChar('/') + relPath)) {
        return relPath;
    }
    const QStringList parts = relPath.split(QChar('/'), Qt::SkipEmptyParts);
    QString cur;
    for (const QString &p : parts) {
        const QString parent = cur.isEmpty()
            ? projectPath
            : (projectPath + QChar('/') + cur);
        QDir d(parent);
        if (!d.exists()) return {};
        const QStringList entries =
            d.entryList(QDir::AllEntries | QDir::NoDotAndDotDot,
                        QDir::Name);
        QString hit;
        for (const QString &e : entries) {
            if (e.compare(p, Qt::CaseInsensitive) == 0) { hit = e; break; }
        }
        if (hit.isEmpty()) return {};
        if (!cur.isEmpty()) cur += QChar('/');
        cur += hit;
    }
    return cur;
}

// ANTS-1529 — extension-tolerant contract-doc resolver. Extends the
// case-insensitive lookup with a list of common alternate extensions
// per contract stem (README.md / README.rst / README.txt; CHANGELOG.md /
// CHANGELOG.yaml / CHANGELOG.json — RetroDB ships YAML, some Rust
// crates ship plain text, ASCIIDoc projects ship .adoc). Returns the
// first hit's on-disk relative path, or QString() if no extension
// resolves. Cheap: caseInsensitiveResolve already short-circuits when
// the literal path exists, so the common .md case is one syscall.
QString resolveContractStem(const QString &projectPath,
                            const QString &stem,
                            const QStringList &extensions) {
    for (const QString &ext : extensions) {
        const QString rel = stem + ext;
        const QString resolved = caseInsensitiveResolve(projectPath, rel);
        if (!resolved.isEmpty()) return resolved;
    }
    return {};
}

// Read .cold-eyes/partition.json if present. Schema:
//   {"version": 1, "lanes": [{"name": "...", "summary": "...",
//                             "doc_paths": ["..."]}, ...]}
QList<Lane> readPartitionOverride(const QString &projectPath) {
    const QString raw = slurpUtf8(projectPath
                                  + QStringLiteral("/.cold-eyes/partition.json"));
    if (raw.isEmpty()) return {};
    QJsonParseError pe{};
    const auto doc = QJsonDocument::fromJson(raw.toUtf8(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) return {};
    const auto root = doc.object();
    if (root.value(QStringLiteral("version")).toInt() != 1) return {};
    const auto lanesArr = root.value(QStringLiteral("lanes")).toArray();
    QList<Lane> out;
    out.reserve(lanesArr.size());
    for (const auto &v : lanesArr) {
        const auto o = v.toObject();
        Lane l;
        l.name    = o.value(QStringLiteral("name")).toString();
        l.summary = o.value(QStringLiteral("summary")).toString();
        if (l.name.isEmpty()) continue;
        for (const auto &dv :
             o.value(QStringLiteral("doc_paths")).toArray()) {
            const QString d = dv.toString();
            if (d.isEmpty()) continue;
            // Indie-review-2026-05-14 lane-5 CR-1: anchor each
            // doc_paths entry inside projectPath. A hostile clone's
            // .cold-eyes/partition.json with doc_paths=["../../etc/passwd"]
            // would otherwise be slurped on the cold_eyes_brief code
            // path (extractCitedCodePaths) AND echoed back in the MCP
            // response — a real disclosure vector. ANTS-1295's MCP-
            // layer chokepoint doesn't cover this read because the
            // path doesn't come from the MCP arg; it comes from disk.
            // Reject absolute paths outright; canonicalise relative
            // entries and require they resolve inside the project.
            if (QFileInfo(d).isAbsolute()) continue;
            const QString joined = projectPath + QChar('/') + d;
            if (!isInsideProject(projectPath, joined)) continue;
            l.docPaths << d;
        }
        out << l;
    }
    return out;
}

// docs/standards/*.md
QStringList listMdInDir(const QString &projectPath, const QString &relDir) {
    QStringList out;
    QDir d(projectPath + QChar('/') + relDir);
    if (!d.exists()) return out;
    const auto entries = d.entryList(QStringList{QStringLiteral("*.md")},
                                     QDir::Files | QDir::NoDotAndDotDot,
                                     QDir::Name);
    for (const QString &e : entries) {
        out << relDir + QChar('/') + e;
    }
    return out;
}

}  // namespace

bool parseScope(const QString &raw, Scope *out) {
    const QString s = raw.trimmed().toLower();
    if (s.isEmpty() || s == QStringLiteral("default")) {
        if (out) *out = Scope::Default;
        return true;
    }
    if (s == QStringLiteral("docs_only")) {
        if (out) *out = Scope::DocsOnly;
        return true;
    }
    if (s == QStringLiteral("contracts_only")) {
        if (out) *out = Scope::ContractsOnly;
        return true;
    }
    return false;
}

QList<int> activeSpecIds(const QString &projectPath) {
    const QString raw = slurpUtf8(projectPath
                                  + QStringLiteral("/ROADMAP.md"));
    if (raw.isEmpty()) return {};
    // Regex matches `^- (📋|🚧) [ANTS-NNNN]`. Emoji written as literal
    // UTF-8 source characters — QStringLiteral's `\xNN` escape path
    // does Latin-1 byte promotion (per-byte → per-codepoint), which
    // mismatches the correctly-UTF-8-decoded file content.
    static const QRegularExpression rx(
        QStringLiteral("^- (📋|🚧)\\s+\\[ANTS-(\\d+)\\]"),
        QRegularExpression::MultilineOption);
    QList<int> out;
    QSet<int>  seen;
    auto it = rx.globalMatch(raw);
    while (it.hasNext()) {
        const auto m  = it.next();
        bool ok = false;
        const int id = m.captured(2).toInt(&ok);
        if (ok && !seen.contains(id)) {
            seen.insert(id);
            out << id;
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

PartitionResult derivePartition(const QString &projectPath, Scope scope) {
    PartitionResult result;
    result.scope        = scope;
    result.overridePath = QStringLiteral("<default>");

    // Override path wins regardless of scope (the user committed to a
    // hand-written partition).
    const auto override = readPartitionOverride(projectPath);
    if (!override.isEmpty()) {
        result.lanes        = override;
        result.overridePath = QStringLiteral(".cold-eyes/partition.json");
        result.scopedCount  = override.size();
        return result;
    }

    // Contracts lane — emitted under Default + ContractsOnly.
    if (scope != Scope::DocsOnly) {
        Lane contracts;
        contracts.name = QStringLiteral("contracts");
        // ANTS-1506 — case-insensitive: ship doc names like `Claude.md`
        // / `readme.md` resolve to the contract slot they match instead
        // of being treated as missing.
        // ANTS-1529 — also tolerate non-.md extensions per RetroDB
        // (CHANGELOG as YAML/JSON, README as .rst/.txt/.adoc). Per
        // stem the first existing alternative wins. Keeps .md as the
        // primary so the common case is a single syscall.
        const std::vector<std::pair<QString, QStringList>> kContracts = {
            { QStringLiteral("CLAUDE"),
              { QStringLiteral(".md") } },
            { QStringLiteral("README"),
              { QStringLiteral(".md"), QStringLiteral(".rst"),
                QStringLiteral(".adoc"), QStringLiteral(".txt") } },
            { QStringLiteral("ROADMAP"),
              { QStringLiteral(".md"), QStringLiteral(".yaml"),
                QStringLiteral(".yml"), QStringLiteral(".json") } },
            { QStringLiteral("CHANGELOG"),
              { QStringLiteral(".md"), QStringLiteral(".yaml"),
                QStringLiteral(".yml"), QStringLiteral(".json"),
                QStringLiteral(".rst"), QStringLiteral(".txt") } },
        };
        for (const auto &[stem, exts] : kContracts) {
            const QString resolved =
                resolveContractStem(projectPath, stem, exts);
            if (!resolved.isEmpty()) contracts.docPaths << resolved;
        }
        // ANTS-1506 — summary now mirrors the actually-matched docs,
        // so callers don't see "CLAUDE.md + README.md + ROADMAP.md +
        // CHANGELOG.md" while doc_paths only carries two of those.
        if (!contracts.docPaths.isEmpty()) {
            contracts.summary = contracts.docPaths.join(QStringLiteral(" + "))
                              + QStringLiteral(" (cross-cutting)");
            result.lanes << contracts;
        }

        if (scope == Scope::ContractsOnly) {
            result.scopedCount = result.lanes.size();
            return result;
        }
    }

    // Standards bundle. ANTS-1506 — resolve dir case-insensitively.
    {
        Lane stds;
        stds.name     = QStringLiteral("standards");
        stds.summary  = QStringLiteral(
            "docs/standards/*.md (project-wide contract docs)");
        const QString stdDir = caseInsensitiveResolve(
            projectPath, QStringLiteral("docs/standards"));
        if (!stdDir.isEmpty()) {
            stds.docPaths = listMdInDir(projectPath, stdDir);
        }
        if (!stds.docPaths.isEmpty()) result.lanes << stds;
    }

    // Decisions bundle. ANTS-1506 — resolve dir case-insensitively.
    {
        Lane decs;
        decs.name     = QStringLiteral("decisions");
        decs.summary  = QStringLiteral(
            "docs/decisions/*.md (ADRs, Michael Nygard format)");
        const QString decDir = caseInsensitiveResolve(
            projectPath, QStringLiteral("docs/decisions"));
        if (!decDir.isEmpty()) {
            decs.docPaths = listMdInDir(projectPath, decDir);
        }
        if (!decs.docPaths.isEmpty()) result.lanes << decs;
    }

    // Plugins.md. ANTS-1506 — case-insensitive contract resolution.
    {
        const QString pluginsRel = caseInsensitiveResolve(
            projectPath, QStringLiteral("PLUGINS.md"));
        if (!pluginsRel.isEmpty()) {
            Lane plugins;
            plugins.name     = QStringLiteral("plugins");
            plugins.summary  = pluginsRel
                + QStringLiteral(" (plugin-author contract)");
            plugins.docPaths << pluginsRel;
            result.lanes << plugins;
        }
    }

    // Active-spec lanes.
    const auto allActive = activeSpecIds(projectPath);
    // INV-2: pick the kMaxSpecLanes most-recently-modified, then sort
    // by ID ascending for stable presentation order.
    QList<QPair<qint64, int>> mtimes;
    mtimes.reserve(allActive.size());
    for (int id : allActive) {
        const QString rel = QStringLiteral("docs/specs/ANTS-%1.md").arg(id);
        const QString abs = projectPath + QChar('/') + rel;
        QFileInfo fi(abs);
        if (!fi.exists()) continue;  // active in ROADMAP but no spec file
        mtimes.append({fi.lastModified().toMSecsSinceEpoch(), id});
    }
    result.scopedCount = mtimes.size();
    std::sort(mtimes.begin(), mtimes.end(),
              [](const auto &a, const auto &b) { return a.first > b.first; });
    if (mtimes.size() > kMaxSpecLanes) {
        mtimes = mtimes.mid(0, kMaxSpecLanes);
        result.truncated = true;
    }
    QList<int> chosen;
    chosen.reserve(mtimes.size());
    for (const auto &p : mtimes) chosen << p.second;
    std::sort(chosen.begin(), chosen.end());

    for (int id : chosen) {
        Lane l;
        l.name    = QStringLiteral("spec/ANTS-%1").arg(id);
        l.summary = QStringLiteral("Single spec lane (active)");
        l.docPaths << QStringLiteral("docs/specs/ANTS-%1.md").arg(id);
        result.lanes << l;
    }

    return result;
}

QStringList extractCitedCodePaths(const QString &projectPath,
                                  const QStringList &docPaths) {
    QSet<QString> seen;
    QStringList   out;

    static const QRegularExpression rx(
        // src/foo.h, src/foo.cpp, src/foo.{h,cpp}
        QStringLiteral("\\bsrc/([A-Za-z0-9_./-]+\\.(?:h|cpp))"));

    for (const QString &rel : docPaths) {
        const QString body = slurpUtf8(projectPath + QChar('/') + rel);
        if (body.isEmpty()) continue;
        auto it = rx.globalMatch(body);
        while (it.hasNext()) {
            const QString hit = QStringLiteral("src/")
                              + it.next().captured(1);
            if (seen.contains(hit)) continue;
            seen.insert(hit);
            // INV-13 defence.
            const QString abs = projectPath + QChar('/') + hit;
            if (!isInsideProject(projectPath, abs)) continue;
            if (!QFileInfo::exists(abs)) continue;
            out << hit;
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

BriefManifest assembleBriefManifest(const QString &projectPath,
                                    const Lane &lane) {
    BriefManifest m;
    m.docPaths = lane.docPaths;

    // INV-4 fixed cross-reference docs. De-dup against lane.docPaths.
    static const QStringList kCrossRefs = {
        QStringLiteral("CLAUDE.md"),
        QStringLiteral("README.md"),
        QStringLiteral("ROADMAP.md"),
        QStringLiteral("CHANGELOG.md"),
    };
    const QSet<QString> laneDocSet(lane.docPaths.begin(), lane.docPaths.end());
    for (const QString &x : kCrossRefs) {
        if (!laneDocSet.contains(x) && fileExists(projectPath, x)) {
            m.crossReferenceDocs << x;
        }
    }

    m.citedCodePaths = extractCitedCodePaths(projectPath, lane.docPaths);

    // INV-3: paths-only brief. Subagent reads each doc itself.
    QString b;
    b += QStringLiteral("# Cold-eyes brief — lane: ") + lane.name + QChar('\n');
    b += QStringLiteral("\n## Summary\n\n") + lane.summary + QChar('\n');
    b += QStringLiteral("\n## Doc files (this lane)\n\n");
    for (const QString &p : lane.docPaths) {
        b += QStringLiteral("- ") + p + QChar('\n');
    }
    if (!m.crossReferenceDocs.isEmpty()) {
        b += QStringLiteral("\n## Cross-reference docs "
                            "(always cross-check for drift)\n\n");
        for (const QString &p : m.crossReferenceDocs) {
            b += QStringLiteral("- ") + p + QChar('\n');
        }
    }
    if (!m.citedCodePaths.isEmpty()) {
        b += QStringLiteral("\n## Cited code files (read-only, "
                            "verify doc-vs-code accuracy)\n\n");
        for (const QString &p : m.citedCodePaths) {
            b += QStringLiteral("- ") + p + QChar('\n');
        }
    }
    // ANTS-1457 — previously-rejected findings (do not re-raise).
    // Inserted before the Instructions section so the reviewer
    // reads it before they begin.
    {
        const auto fpEntries = ants::falsepos::filter(
            ants::falsepos::loadEntries(projectPath),
            QStringLiteral("cold-eyes"), lane.name);
        const QString block = ants::falsepos::formatForBrief(fpEntries);
        if (!block.isEmpty()) {
            b += QChar('\n');
            b += block;
            if (!b.endsWith(QChar('\n'))) b += QChar('\n');
        }
    }
    b += QStringLiteral(
        "\n## Instructions\n\n"
        "Read each doc and cross-reference file via your Read tool. "
        "Do NOT trust prior summarisations of them — the doc set may "
        "have shifted since the brief was assembled. Flag findings "
        "with file:line citations against the doc whose claim you "
        "dispute (or the code whose behaviour the doc misstates).\n");
    m.brief = b;
    return m;
}

QList<IndieReviewEngine::CorroboratedFinding>
crossDocDiffFromDir(const QString &projectPath,
                    const QString &reportsDirRelative,
                    int minLanes,
                    int *reportsRead) {
    // INV-5: thin wrapper. Same disk-input contract, same 64 KiB
    // truncate, same `(file, line)` keying, same min_lanes semantics.
    return IndieReviewEngine::corroboratedFindingsFromDir(
        projectPath, reportsDirRelative, minLanes, reportsRead);
}

QList<IndieReviewEngine::CorroboratedFinding>
crossDocDiffFromReports(const QString &projectPath,
                        const QHash<QString, QString> &reports,
                        int minLanes) {
    // ANTS-1509 — same in-memory contract as
    // IndieReviewEngine::corroboratedFindings; engine reuses the
    // shared (file, line) keying so the regex set is identical.
    return IndieReviewEngine::corroboratedFindings(
        projectPath, reports, minLanes);
}

QString templateColdEyesFoldInBlock(
    const QList<IndieReviewEngine::CorroboratedFinding> &actionable,
    const QList<int> &allocatedIds,
    const QString &dateIso) {
    QString out;
    out += QStringLiteral("### 📝 Cold-eyes ") + dateIso + QChar('\n');
    out += QChar('\n');
    const int n = std::min(actionable.size(), allocatedIds.size());
    for (int i = 0; i < n; ++i) {
        const auto &f = actionable[i];
        const int   id = allocatedIds[i];
        QString lanesJoined;
        for (int j = 0; j < f.citingLanes.size(); ++j) {
            if (j) lanesJoined += QStringLiteral(", ");
            lanesJoined += f.citingLanes[j];
        }
        out += QStringLiteral("- 📋 [ANTS-%1] **Cold-eyes finding:** ")
                   .arg(id);
        out += f.file;
        if (f.line > 0) {
            out += QChar(':');
            out += QString::number(f.line);
        }
        out += QStringLiteral(" — cited across [");
        out += lanesJoined;
        out += QStringLiteral("].\n");
        out += QStringLiteral("  Kind: review-fix.\n");
        out += QStringLiteral("  Source: cold-eyes-")
             + dateIso + QStringLiteral(".\n");
    }
    return out;
}

}  // namespace ColdEyesEngine
