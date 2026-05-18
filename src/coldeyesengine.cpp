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
//   {"version": 1,
//    "lanes": [{"name": "...", "summary": "...",
//               "doc_paths": ["..."]}, ...]}
//
// ANTS-1412 — return a status alongside the parsed lanes so a
// malformed override can be surfaced to the caller instead of
// silently swallowing into the default partition. `Absent` is the
// no-override case (file not present). `Ok` means the file parsed
// and at least one valid lane survived path-rule filtering. Every
// other status carries a `warning` describing what failed.
struct OverrideReadResult {
    enum Status { Absent, ParseError, BadVersion, BadShape, AllLanesFiltered, Ok };
    Status      status = Absent;
    QString     warning;
    QList<Lane> lanes;
};

OverrideReadResult readPartitionOverride(const QString &projectPath) {
    OverrideReadResult r;
    const QString filePath = projectPath
                           + QStringLiteral("/.cold-eyes/partition.json");
    if (!QFileInfo::exists(filePath)) {
        return r;  // Status::Absent — no file, no warning.
    }
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        r.status = OverrideReadResult::ParseError;
        r.warning = QStringLiteral(
            ".cold-eyes/partition.json: cannot open for reading");
        return r;
    }
    const QByteArray raw = f.readAll();
    if (raw.trimmed().isEmpty()) {
        r.status = OverrideReadResult::ParseError;
        r.warning = QStringLiteral(
            ".cold-eyes/partition.json: empty file");
        return r;
    }
    QJsonParseError pe{};
    const auto doc = QJsonDocument::fromJson(raw, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        r.status = OverrideReadResult::ParseError;
        r.warning = QStringLiteral(
            ".cold-eyes/partition.json: parse error: ")
            + (pe.error != QJsonParseError::NoError
                  ? pe.errorString()
                  : QStringLiteral("root must be a JSON object"));
        return r;
    }
    const auto root = doc.object();
    const int v = root.value(QStringLiteral("version")).toInt(-1);
    if (v != 1) {
        r.status = OverrideReadResult::BadVersion;
        r.warning = QStringLiteral(
            ".cold-eyes/partition.json: \"version\" must be 1 (got %1). "
            "Expected schema: "
            "{\"version\":1,\"lanes\":[{\"name\":\"<id>\","
            "\"summary\":\"<text>\",\"doc_paths\":[\"<rel>\",...]}]}")
                .arg(v);
        return r;
    }
    if (!root.value(QStringLiteral("lanes")).isArray()) {
        r.status = OverrideReadResult::BadShape;
        r.warning = QStringLiteral(
            ".cold-eyes/partition.json: \"lanes\" must be an array of "
            "{name, summary, doc_paths[]} objects");
        return r;
    }
    const auto lanesArr = root.value(QStringLiteral("lanes")).toArray();
    int laneCount = 0;
    int filteredPaths = 0;
    int filteredLanes = 0;
    r.lanes.reserve(lanesArr.size());
    for (const auto &v2 : lanesArr) {
        ++laneCount;
        const auto o = v2.toObject();
        Lane l;
        l.name    = o.value(QStringLiteral("name")).toString();
        l.summary = o.value(QStringLiteral("summary")).toString();
        if (l.name.isEmpty()) { ++filteredLanes; continue; }
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
            if (QFileInfo(d).isAbsolute()) { ++filteredPaths; continue; }
            const QString joined = projectPath + QChar('/') + d;
            if (!isInsideProject(projectPath, joined)) {
                ++filteredPaths;
                continue;
            }
            l.docPaths << d;
        }
        if (l.docPaths.isEmpty()) { ++filteredLanes; continue; }
        r.lanes << l;
    }
    if (r.lanes.isEmpty() && laneCount > 0) {
        r.status = OverrideReadResult::AllLanesFiltered;
        r.warning = QStringLiteral(
            ".cold-eyes/partition.json: every lane was filtered out "
            "(rejected %1/%2 lanes; rejected %3 path entries by INV-13 "
            "absolute/escape rule). Falling back to the default "
            "partition.")
                .arg(filteredLanes).arg(laneCount).arg(filteredPaths);
        return r;
    }
    if (r.lanes.isEmpty()) {
        // version-1 + lanes:[] is a valid empty override; treat as
        // BadShape so the caller knows their config is a no-op.
        r.status = OverrideReadResult::BadShape;
        r.warning = QStringLiteral(
            ".cold-eyes/partition.json: \"lanes\" array is empty — "
            "no override applied");
        return r;
    }
    r.status = OverrideReadResult::Ok;
    return r;
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

// ANTS-1571 — name-based standards fallback. When docs/standards/ doesn't
// exist, projects that ship flat standards files at docs/*.md (e.g.
// `docs/RETRODB_DESIGN_STANDARDS.md`) still get a standards lane. Filter:
// (1) name contains STANDARD / DESIGN / STYLE / GUIDE (case-insensitive);
// (2) ≥ kStandardsMinLines lines so stub helper notes don't get promoted.
// Returns project-relative paths sorted ascending for stable presentation.
constexpr int kStandardsMinLines = 100;

int countLines(const QString &absPath) {
    QFile f(absPath);
    if (!f.open(QIODevice::ReadOnly)) return 0;
    int n = 0;
    while (!f.atEnd()) {
        f.readLine();
        ++n;
        // Bail past the threshold — caller only needs to know
        // "≥ threshold" / "< threshold". Saves wall-time on large
        // standards files.
        if (n >= kStandardsMinLines) return n;
    }
    return n;
}

QStringList listStandardsByNameGlob(const QString &projectPath) {
    QStringList out;
    const QString docsDir =
        caseInsensitiveResolve(projectPath, QStringLiteral("docs"));
    if (docsDir.isEmpty()) return out;
    QDir d(projectPath + QChar('/') + docsDir);
    if (!d.exists()) return out;
    static const QRegularExpression rx(
        QStringLiteral(".*(STANDARD|DESIGN|STYLE|GUIDE).*\\.md$"),
        QRegularExpression::CaseInsensitiveOption);
    const auto entries =
        d.entryList(QStringList{QStringLiteral("*.md")},
                    QDir::Files | QDir::NoDotAndDotDot,
                    QDir::Name);
    for (const QString &e : entries) {
        if (!rx.match(e).hasMatch()) continue;
        const QString abs = projectPath + QChar('/') + docsDir
                          + QChar('/') + e;
        if (countLines(abs) < kStandardsMinLines) continue;
        out << docsDir + QChar('/') + e;
    }
    return out;
}

// ANTS-1440 — read the first ~8 KiB of a doc and extract the first
// markdown H1 line (skipping front-matter quote/blockquote prefixes).
// Used to enrich the brief manifest's summary for spec lanes: callers
// see the spec title ("ANTS-1435 — session_memory reads honour
// caller_cwd") instead of the generic "Single spec lane" placeholder.
// Caps the scan so a multi-megabyte mis-named .md file can't blow
// the cold_eyes_brief request budget.
QString readDocFirstH1(const QString &absPath) {
    QFile f(absPath);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QByteArray head = f.read(8192);
    const QString s = QString::fromUtf8(head);
    int pos = 0;
    while (pos < s.size()) {
        int nl = s.indexOf(QChar('\n'), pos);
        if (nl < 0) nl = s.size();
        QString line = s.mid(pos, nl - pos);
        // strip leading whitespace then a leading '> ' blockquote prefix
        // (matches the rare spec preamble shape).
        QString stripped = line;
        while (stripped.startsWith(QChar(' ')) || stripped.startsWith(QChar('\t'))) {
            stripped = stripped.mid(1);
        }
        if (stripped.startsWith(QStringLiteral("# "))
            && !stripped.startsWith(QStringLiteral("## "))) {
            return stripped.mid(2).trimmed();
        }
        pos = nl + 1;
    }
    return {};
}

// ANTS-1440 — extract `[ANTS-NNNN]` / `ANTS-NNNN` tokens from the
// spec's header preamble (first ~8 KiB) and resolve them to existing
// `docs/specs/ANTS-NNNN.md` paths. Used to populate the brief's
// cross-reference docs from the spec's own "Pairs with:" / "Cross-
// refs:" markers — saves a reviewer re-reading the spec to discover
// related-spec links. Returns project-relative paths that exist on
// disk; missing references silently filtered.
QStringList extractRelatedSpecsFromHeader(const QString &projectPath,
                                          const QString &specAbsPath) {
    QFile f(specAbsPath);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray head = f.read(8192);
    const QString s = QString::fromUtf8(head);
    // Locate the header preamble. Scan up to the first `## ` heading
    // (or end of buffer) — Pairs-with markers live in the metadata
    // block at the top, body usage doesn't count as a header link.
    int headerEnd = s.indexOf(QStringLiteral("\n## "));
    if (headerEnd < 0) headerEnd = s.size();
    const QString header = s.left(headerEnd);
    static const QRegularExpression rxId(
        QStringLiteral("\\bANTS-(\\d+)\\b"));
    auto it = rxId.globalMatch(header);
    QStringList out;
    QSet<int> seen;
    while (it.hasNext()) {
        const int id = it.next().captured(1).toInt();
        if (seen.contains(id)) continue;
        seen.insert(id);
        const QString rel = QStringLiteral("docs/specs/ANTS-%1.md").arg(id);
        if (QFileInfo::exists(projectPath + QChar('/') + rel)) {
            out << rel;
        }
    }
    return out;
}

// ANTS-1411 — generalised spec-lane candidate walker. Walks every
// `*.md` under `docs/specs/` regardless of filename shape. For
// `ANTS-NNNN.md` shapes, gates on the ROADMAP active set (preserves
// the legacy Ants behaviour: shipped specs don't surface as lanes).
// For other shapes (e.g. MAME Curator's `DS01.md`, `FP05.md`,
// `P04.md`), include all — projects without an ANTS-style ROADMAP
// get every spec file as a candidate. mtime + relative-path pairs;
// caller sorts + caps.
QList<QPair<qint64, QString>> listSpecLaneCandidates(
    const QString &projectPath,
    const QSet<int> &activeAntsIds) {
    QList<QPair<qint64, QString>> out;
    const QString specsDir = caseInsensitiveResolve(
        projectPath, QStringLiteral("docs/specs"));
    if (specsDir.isEmpty()) return out;
    QDir d(projectPath + QChar('/') + specsDir);
    if (!d.exists()) return out;
    const auto entries = d.entryList(
        QStringList{QStringLiteral("*.md")},
        QDir::Files | QDir::NoDotAndDotDot,
        QDir::Name);
    // The legacy Ants filter only knew `ANTS-NNNN.md`. Match strictly
    // so unrelated filename shapes are never accidentally gated.
    static const QRegularExpression rxAntsId(
        QStringLiteral("^ANTS-(\\d+)\\.md$"));
    for (const QString &e : entries) {
        const auto am = rxAntsId.match(e);
        if (am.hasMatch() && !activeAntsIds.isEmpty()) {
            const int id = am.captured(1).toInt();
            if (!activeAntsIds.contains(id)) continue;
        }
        const QString rel = specsDir + QChar('/') + e;
        QFileInfo fi(projectPath + QChar('/') + rel);
        if (!fi.exists()) continue;
        out.append({fi.lastModified().toMSecsSinceEpoch(), rel});
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
    // hand-written partition). ANTS-1412: malformed overrides fall
    // through to the default partition but surface a warning so the
    // caller knows their config was ignored and why.
    const auto override = readPartitionOverride(projectPath);
    if (override.status == OverrideReadResult::Ok) {
        result.lanes        = override.lanes;
        result.overridePath = QStringLiteral(".cold-eyes/partition.json");
        result.scopedCount  = override.lanes.size();
        return result;
    }
    if (override.status != OverrideReadResult::Absent) {
        result.overrideWarning = override.warning;
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
        // ANTS-1571 — fold community-contract docs (CONTRIBUTING /
        // SECURITY / LEGAL / CODE_OF_CONDUCT) into the contracts lane.
        // Standard names on most modern projects; cold-eyes lanes
        // should see them rather than dropping them off the side.
        // Case-insensitive resolution via caseInsensitiveResolve.
        static const QStringList kCommunityContracts = {
            QStringLiteral("CONTRIBUTING.md"),
            QStringLiteral("SECURITY.md"),
            QStringLiteral("LEGAL.md"),
            QStringLiteral("CODE_OF_CONDUCT.md"),
        };
        for (const QString &name : kCommunityContracts) {
            const QString resolved =
                caseInsensitiveResolve(projectPath, name);
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
    // ANTS-1571 — when docs/standards/ doesn't exist, fall back to
    // top-level docs/*.md whose names match STANDARD|DESIGN|STYLE|GUIDE
    // so projects that ship flat standards files (e.g. RetroDB's
    // docs/RETRODB_DESIGN_STANDARDS.md) still get a standards lane.
    {
        Lane stds;
        stds.name     = QStringLiteral("standards");
        stds.summary  = QStringLiteral(
            "docs/standards/*.md (project-wide contract docs)");
        const QString stdDir = caseInsensitiveResolve(
            projectPath, QStringLiteral("docs/standards"));
        if (!stdDir.isEmpty()) {
            stds.docPaths = listMdInDir(projectPath, stdDir);
        } else {
            stds.docPaths = listStandardsByNameGlob(projectPath);
            if (!stds.docPaths.isEmpty()) {
                stds.summary = QStringLiteral(
                    "docs/*STANDARD*|*DESIGN*|*STYLE*|*GUIDE*.md "
                    "(>= 100 lines, name-based standards fallback)");
            }
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

    // Active-spec lanes. ANTS-1411: scanner walks every `*.md` under
    // `docs/specs/` regardless of filename shape. For `ANTS-NNNN.md`
    // files we keep the legacy active-only filter (drops shipped ✅
    // specs from the lane set). For other naming conventions
    // (DS01.md, FP05.md, generic.md) every file is a candidate.
    const auto allActive = activeSpecIds(projectPath);
    const QSet<int> activeAntsIds(allActive.begin(), allActive.end());
    auto mtimes = listSpecLaneCandidates(projectPath, activeAntsIds);
    // INV-2: pick the kMaxSpecLanes most-recently-modified, then
    // sort by basename for stable presentation order.
    // ANTS-1345 — deterministic tiebreak. Bucket mtimes into 1 s
    // windows so a cluster of edits within a second sorts by path-lex
    // (alphabetic) rather than by filesystem enumeration order. The
    // bucketing form keeps the comparator a strict weak ordering — a
    // tolerance-window approach (|a-b| < N) breaks transitivity when
    // chains cross the window boundary and triggers undefined sort
    // behaviour.
    result.scopedCount = mtimes.size();
    std::sort(mtimes.begin(), mtimes.end(),
              [](const auto &a, const auto &b) {
                  const qint64 aBucket = a.first / 1000;
                  const qint64 bBucket = b.first / 1000;
                  if (aBucket != bBucket) return aBucket > bBucket;
                  return a.second < b.second;
              });
    if (mtimes.size() > kMaxSpecLanes) {
        mtimes = mtimes.mid(0, kMaxSpecLanes);
        result.truncated = true;
    }
    QStringList chosen;
    chosen.reserve(mtimes.size());
    for (const auto &p : mtimes) chosen << p.second;
    std::sort(chosen.begin(), chosen.end());

    for (const QString &rel : chosen) {
        QFileInfo fi(projectPath + QChar('/') + rel);
        Lane l;
        l.name    = QStringLiteral("spec/") + fi.completeBaseName();
        l.summary = QStringLiteral("Single spec lane");
        l.docPaths << rel;
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

    // ANTS-1440 — spec-lane enrichment. For lanes named `spec/...`
    // (single-spec lanes from derivePartition or caller-supplied),
    // replace the generic "Single spec lane (active)" summary with
    // the spec's first H1 line and add any related specs referenced
    // in the header preamble to crossReferenceDocs.
    m.summary = lane.summary;
    if (lane.name.startsWith(QStringLiteral("spec/"))
        && !lane.docPaths.isEmpty()) {
        const QString primaryAbs = projectPath + QChar('/')
                                 + lane.docPaths.first();
        const QString h1 = readDocFirstH1(primaryAbs);
        if (!h1.isEmpty()) m.summary = h1;
        const auto related = extractRelatedSpecsFromHeader(
            projectPath, primaryAbs);
        for (const QString &rel : related) {
            if (laneDocSet.contains(rel)) continue;
            if (m.crossReferenceDocs.contains(rel)) continue;
            m.crossReferenceDocs << rel;
        }
    }

    // INV-3: paths-only brief. Subagent reads each doc itself.
    QString b;
    b += QStringLiteral("# Cold-eyes brief — lane: ") + lane.name + QChar('\n');
    b += QStringLiteral("\n## Summary\n\n") + m.summary + QChar('\n');
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

namespace {

// Build the root-contract doc set the same way derivePartition's
// contracts lane does. Surfaced as a private helper so
// assembleSingleDocBrief (ANTS-1413) doesn't duplicate the table.
QStringList resolveRootContractDocs(const QString &projectPath) {
    QStringList out;
    static const std::vector<std::pair<QString, QStringList>> kContracts = {
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
        const QString resolved = resolveContractStem(projectPath, stem, exts);
        if (!resolved.isEmpty()) out << resolved;
    }
    static const QStringList kCommunityContracts = {
        QStringLiteral("CONTRIBUTING.md"),
        QStringLiteral("SECURITY.md"),
        QStringLiteral("LEGAL.md"),
        QStringLiteral("CODE_OF_CONDUCT.md"),
    };
    for (const QString &name : kCommunityContracts) {
        const QString resolved = caseInsensitiveResolve(projectPath, name);
        if (!resolved.isEmpty()) out << resolved;
    }
    return out;
}

// Build the standards doc set the same way derivePartition does:
// canonical `docs/standards/*.md` first, name-glob fallback when
// that directory is absent (ANTS-1571).
QStringList resolveStandardsDocs(const QString &projectPath) {
    const QString stdDir = caseInsensitiveResolve(
        projectPath, QStringLiteral("docs/standards"));
    if (!stdDir.isEmpty()) {
        return listMdInDir(projectPath, stdDir);
    }
    return listStandardsByNameGlob(projectPath);
}

}  // namespace

SingleDocBrief assembleSingleDocBrief(const QString &projectPath,
                                      const QString &docPathRel) {
    SingleDocBrief b;
    b.docPath = docPathRel;
    const QString absDoc = projectPath + QChar('/') + docPathRel;
    b.summary = readDocFirstH1(absDoc);

    // Same-dir siblings — every other `*.md` next to docPathRel.
    // Capped at 12 (mirrors INV-2's lane cap so payload stays
    // bounded even on a docs/specs/ directory with hundreds of
    // specs). Sorted alphabetically for stable presentation.
    const QFileInfo fi(absDoc);
    const QString  parentRel = QFileInfo(docPathRel).path();
    QDir parentDir(fi.dir());
    if (parentDir.exists()) {
        const auto entries = parentDir.entryList(
            QStringList{QStringLiteral("*.md")},
            QDir::Files | QDir::NoDotAndDotDot,
            QDir::Name);
        for (const QString &e : entries) {
            if (e == fi.fileName()) continue;
            const QString rel = parentRel.isEmpty()
                ? e
                : (parentRel + QChar('/') + e);
            b.sameDirSiblings << rel;
            if (b.sameDirSiblings.size() >= kMaxSpecLanes) break;
        }
    }

    b.standards     = resolveStandardsDocs(projectPath);
    b.rootContracts = resolveRootContractDocs(projectPath);

    // Default reviewer-role list — orchestrator dispatches one
    // subagent per label, each reviewing the same doc with a
    // different lens. Three is the typical /cold-eyes fanout.
    b.recommendedReviewers = QStringList{
        QStringLiteral("primary-correctness-reviewer"),
        QStringLiteral("cross-doc-consistency-reviewer"),
        QStringLiteral("security-implications-reviewer"),
    };

    return b;
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

QString templateColdEyesFoldInBlockFreeform(
    const QList<IndieReviewEngine::CorroboratedFinding> &actionable,
    const QString &dateIso) {
    // ANTS-1510 — no `[PROJ-NNNN]` prefix and no ID-keyed Source/Kind
    // metadata. Projects that don't use the shareable roadmap-format.md
    // ID scheme still get a structured `- ` bullet per finding plus a
    // single `Source: cold-eyes-<DATE>` provenance line at the bottom of
    // the block.
    QString out;
    out += QStringLiteral("### 📝 Cold-eyes ") + dateIso + QChar('\n');
    out += QChar('\n');
    for (const auto &f : actionable) {
        QString lanesJoined;
        for (int j = 0; j < f.citingLanes.size(); ++j) {
            if (j) lanesJoined += QStringLiteral(", ");
            lanesJoined += f.citingLanes[j];
        }
        out += QStringLiteral("- **Cold-eyes finding:** ");
        out += f.file;
        if (f.line > 0) {
            out += QChar(':');
            out += QString::number(f.line);
        }
        out += QStringLiteral(" — cited across [");
        out += lanesJoined;
        out += QStringLiteral("].\n");
    }
    out += QChar('\n');
    out += QStringLiteral("Source: cold-eyes-") + dateIso
         + QStringLiteral(".\n");
    return out;
}

}  // namespace ColdEyesEngine
