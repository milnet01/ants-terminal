#include "indiereviewengine.h"

#include "subsystemmap.h"

#include <QChar>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QPair>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace IndieReviewEngine {

namespace {

QString slurpUtf8(const QString &absPath) {
    QFile f(absPath);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

// Walk src/ to find files matching `<name>.{h,cpp}` + `<name>*.{h,cpp}`.
// Skip generated files.
QStringList sourcePathsForLane(const QString &projectPath,
                               const QString &laneName) {
    QStringList out;
    const QString src = projectPath + QStringLiteral("/src");
    QDir dir(src);
    if (!dir.exists()) return out;

    static const QStringList kGenPrefixes = {
        QStringLiteral("moc_"),
        QStringLiteral("ui_"),
        QStringLiteral("qrc_"),
    };

    const QStringList all = dir.entryList(
        QStringList{QStringLiteral("*.h"), QStringLiteral("*.cpp")},
        QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &entry : all) {
        bool generated = false;
        for (const QString &p : kGenPrefixes) {
            if (entry.startsWith(p)) { generated = true; break; }
        }
        if (generated) continue;

        // Match `<name>.h`, `<name>.cpp`, or `<name>*.{h,cpp}` (sibling).
        if (entry == laneName + QStringLiteral(".h")
            || entry == laneName + QStringLiteral(".cpp")
            || entry.startsWith(laneName + QChar('_'))
            || (entry.startsWith(laneName)
                && (entry.endsWith(QStringLiteral(".h"))
                    || entry.endsWith(QStringLiteral(".cpp"))))) {
            // Defensive: only the LAST entry.startsWith(laneName) branch
            // could grab a longer-prefix sibling like `claudestatuswidgets`
            // for lane `claude`. Tighten with a strict-prefix exact-match
            // first, fall through to startsWith for true siblings.
            if (entry.startsWith(laneName)) {
                const QString tail = entry.mid(laneName.size());
                // Accept if tail starts with `.`, `_`, or another lowercase
                // continuation of the same word. To avoid `claude` matching
                // `claudestatuswidgets`, restrict to `_` or `.` boundaries
                // for non-equal entries.
                if (entry == laneName + QStringLiteral(".h")
                    || entry == laneName + QStringLiteral(".cpp")
                    || tail.startsWith(QChar('_'))
                    || tail.startsWith(QChar('.'))) {
                    out << QStringLiteral("src/") + entry;
                }
            }
        }
    }
    out.removeDuplicates();
    return out;
}

QString readPartitionOverride(const QString &projectPath) {
    return slurpUtf8(projectPath
                     + QStringLiteral("/.indie-review/partition.json"));
}

QList<Lane> parsePartitionOverride(const QString &json) {
    QJsonParseError pe{};
    const auto doc = QJsonDocument::fromJson(json.toUtf8(), &pe);
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
        if (l.name.isEmpty()) continue;  // schema: name required
        for (const auto &sp : o.value(QStringLiteral("sourcePaths")).toArray()) {
            const QString s = sp.toString();
            if (!s.isEmpty() && s.startsWith(QStringLiteral("src/"))) {
                l.sourcePaths << s;
            }
        }
        out << l;
    }
    return out;
}

}  // namespace

QList<Lane> derivePartition(const QString &projectPath) {
    const QString override = readPartitionOverride(projectPath);
    if (!override.isEmpty()) {
        const auto lanes = parsePartitionOverride(override);
        if (!lanes.isEmpty()) return lanes;
        // fall-through: malformed override → try CLAUDE.md
    }

    const QString claudeMdPath =
        projectPath + QStringLiteral("/CLAUDE.md");
    const auto smLanes = SubsystemMap::cachedLanes(claudeMdPath);
    QList<Lane> out;
    out.reserve(smLanes.size());
    for (const auto &sm : smLanes) {
        Lane l;
        l.name        = sm.name;
        l.summary     = sm.summary;
        l.sourcePaths = sourcePathsForLane(projectPath, sm.name);
        out << l;
    }
    return out;
}

QString assembleBrief(const QString &projectPath, const Lane &lane) {
    QString out;
    out.reserve(8 * 1024);
    out += QStringLiteral("=== Lane: ");
    out += lane.name;
    out += QStringLiteral(" ===\n\n");
    out += QStringLiteral("Summary: ");
    out += lane.summary;
    out += QStringLiteral("\n\n");
    out += QStringLiteral("Source files (");
    out += QString::number(lane.sourcePaths.size());
    out += QStringLiteral("):\n");
    for (const QString &sp : lane.sourcePaths) {
        out += QStringLiteral("- ");
        out += sp;
        out += QChar('\n');
    }
    out += QChar('\n');

    for (const QString &sp : lane.sourcePaths) {
        const QString abs = projectPath + QChar('/') + sp;
        // Path-traversal guard: canonicalise + ensure under projectPath.
        const QFileInfo fi(abs);
        const QString canon = fi.canonicalFilePath();
        const QFileInfo rootInfo(projectPath);
        const QString rootCanon = rootInfo.canonicalFilePath();
        if (canon.isEmpty() || rootCanon.isEmpty()
            || !canon.startsWith(rootCanon + QChar('/'))) {
            continue;
        }
        out += QStringLiteral("=== file: ");
        out += sp;
        out += QStringLiteral(" ===\n");
        out += slurpUtf8(canon);
        if (!out.endsWith(QChar('\n'))) out += QChar('\n');
        out += QChar('\n');
    }

    // ROADMAP slice — grep lines that mention the lane name as
    // `Lanes:` value or basename token.
    const QString roadmap = slurpUtf8(projectPath
                                      + QStringLiteral("/ROADMAP.md"));
    if (!roadmap.isEmpty()) {
        out += QStringLiteral("=== ROADMAP slice ===\n");
        const QStringList lines = roadmap.split(QChar('\n'));
        const QString needle = lane.name;
        for (const QString &line : lines) {
            if (line.contains(QStringLiteral("Lanes:"), Qt::CaseInsensitive)
                && line.contains(needle, Qt::CaseInsensitive)) {
                out += line;
                out += QChar('\n');
            } else if (line.contains(QStringLiteral("`")
                                     + needle + QStringLiteral("`"))) {
                out += line;
                out += QChar('\n');
            }
        }
        out += QChar('\n');
    }

    out += QStringLiteral("=== Standards reference (not inlined; reviewer fetches if needed) ===\n");
    out += QStringLiteral("- docs/standards/coding.md\n");
    out += QStringLiteral("- docs/standards/testing.md\n");
    out += QStringLiteral("- docs/standards/documentation.md\n");
    return out;
}

QList<Citation> extractFileLineCitations(const QString &projectPath,
                                         const QString &report) {
    QList<Citation> out;
    if (report.isEmpty()) return out;

    // 64 KiB defensive truncation (matches MAX_TOOL_OUTPUT_BYTES
    // convention; report longer than that scans only the first 64K).
    static constexpr int kMaxScanBytes = 64 * 1024;
    QString scope = report;
    if (scope.size() > kMaxScanBytes) scope = scope.left(kMaxScanBytes);

    // Longest-first extension alternation (cpp before c, hpp before h).
    static const QRegularExpression reLine(
        QStringLiteral(R"(\b([A-Za-z0-9_./-]+\.(?:cpp|hpp|cc|yaml|json|yml|md|py|sh|h|c)):(\d+))"));
    static const QRegularExpression reFile(
        QStringLiteral(R"(\b([A-Za-z0-9_./-]+\.(?:cpp|hpp|cc|yaml|json|yml|md|py|sh|h|c))\b)"));

    QFileInfo rootInfo(projectPath);
    const QString rootCanon = rootInfo.canonicalFilePath();
    if (rootCanon.isEmpty()) return out;

    auto resolveOk = [&](const QString &rel) -> QString {
        // Resolve `rel` against projectPath; reject if it escapes.
        const QString abs = QDir(projectPath).filePath(rel);
        const QString canon = QFileInfo(abs).canonicalFilePath();
        if (canon.isEmpty()) return {};
        if (canon != rootCanon
            && !canon.startsWith(rootCanon + QChar('/'))) return {};
        return rel;
    };

    auto contextAt = [&](int pos, int matchLen) -> QString {
        const int start = std::max(0, pos - 40);
        const int len   = std::min<int>(scope.size() - start, matchLen + 80);
        return scope.mid(start, len).simplified();
    };

    QSet<QString> lineKeys;
    auto it = reLine.globalMatch(scope);
    while (it.hasNext()) {
        const auto m = it.next();
        const QString rel = resolveOk(m.captured(1));
        if (rel.isEmpty()) continue;
        const int line = m.captured(2).toInt();
        const QString k = rel + QChar(':') + QString::number(line);
        if (lineKeys.contains(k)) continue;
        lineKeys.insert(k);
        Citation c;
        c.file    = rel;
        c.line    = line;
        c.context = contextAt(m.capturedStart(), m.capturedLength());
        out.append(c);
    }

    QSet<QString> fileKeys;
    auto fit = reFile.globalMatch(scope);
    while (fit.hasNext()) {
        const auto m = fit.next();
        const QString rel = resolveOk(m.captured(1));
        if (rel.isEmpty()) continue;
        // Skip if a line-level citation for the same file already
        // exists (avoids double-counting `foo.cpp` mentioned both
        // bare and at `foo.cpp:42`).
        bool covered = false;
        for (const QString &lk : lineKeys) {
            if (lk.startsWith(rel + QChar(':'))) { covered = true; break; }
        }
        if (covered) continue;
        if (fileKeys.contains(rel)) continue;
        fileKeys.insert(rel);
        Citation c;
        c.file    = rel;
        c.line    = -1;
        c.context = contextAt(m.capturedStart(), m.capturedLength());
        out.append(c);
    }

    return out;
}

QList<CorroboratedFinding> corroboratedFindings(
    const QString &projectPath,
    const QHash<QString, QString> &reports,
    int minLanes) {
    if (minLanes < 1) minLanes = 1;

    // (file, line) → set<lane>; (file, line) → ordered <lane, context>
    QHash<QPair<QString, int>, QSet<QString>> coverage;
    QHash<QPair<QString, int>, QHash<QString, QString>> contexts;

    for (auto it = reports.constBegin(); it != reports.constEnd(); ++it) {
        const QString lane = it.key();
        const auto cites = extractFileLineCitations(projectPath, it.value());
        for (const Citation &c : cites) {
            const QPair<QString, int> key{c.file, c.line};
            coverage[key].insert(lane);
            contexts[key][lane] = c.context;
        }
    }

    QList<CorroboratedFinding> out;
    for (auto it = coverage.constBegin(); it != coverage.constEnd(); ++it) {
        if (it.value().size() < minLanes) continue;
        CorroboratedFinding cf;
        cf.file = it.key().first;
        cf.line = it.key().second;
        cf.citingLanes = it.value().values();
        std::sort(cf.citingLanes.begin(), cf.citingLanes.end());
        const auto &ctxMap = contexts[it.key()];
        for (const QString &ln : std::as_const(cf.citingLanes)) {
            cf.contexts << ctxMap.value(ln);
        }
        out.append(cf);
    }

    std::sort(out.begin(), out.end(),
              [](const CorroboratedFinding &a, const CorroboratedFinding &b) {
        if (a.citingLanes.size() != b.citingLanes.size())
            return a.citingLanes.size() > b.citingLanes.size();
        if (a.file != b.file) return a.file < b.file;
        return a.line > b.line;
    });
    return out;
}

QString synthesisPrompt(const QHash<QString, QString> &reports,
                        const QString &threatModelExtras) {
    QString out;
    out.reserve(2048);
    out += QStringLiteral(
        "You are reviewing an Ants Terminal codebase that has just been\n"
        "audited by N independent reviewers. Each reviewer was given one\n"
        "subsystem in isolation. Your job: cross-cutting synthesis.\n\n"
        "=== Per-lane reports ===\n\n");

    QStringList laneNames = reports.keys();
    std::sort(laneNames.begin(), laneNames.end());
    for (const QString &ln : laneNames) {
        out += QStringLiteral("## Lane: ");
        out += ln;
        out += QStringLiteral("\n");
        out += reports.value(ln);
        if (!out.endsWith(QChar('\n'))) out += QChar('\n');
        out += QChar('\n');
    }

    out += QStringLiteral("=== Threat model extras ===\n\n");
    if (threatModelExtras.isEmpty()) {
        out += QStringLiteral("(none provided)\n\n");
    } else {
        out += threatModelExtras;
        if (!out.endsWith(QChar('\n'))) out += QChar('\n');
        out += QChar('\n');
    }

    out += QStringLiteral(
        "=== Your tasks ===\n\n"
        "1. Cross-cutting themes: which findings appear in 2+ lane reports?\n"
        "   Group them and explain.\n"
        "2. Threat-model calibration: which corroborated findings are\n"
        "   genuinely high-impact given the project's threat model\n"
        "   (above)? Which are noise that the project has explicitly\n"
        "   accepted?\n"
        "3. Recommended action order: rank the actionable findings by\n"
        "   (impact × effort^-1). Top 5 only.\n"
        "4. Anti-rec: anything you would NOT do based on these reports\n"
        "   (cases where two lanes recommend opposite changes).\n\n"
        "Format: markdown. No code generation. <= 800 words.\n");
    return out;
}

QString templateIndieReviewFoldInBlock(
    const QList<CorroboratedFinding> &actionable,
    const QList<int> &allocatedIds,
    const QString &dateIso) {
    if (actionable.isEmpty() || actionable.size() != allocatedIds.size()) {
        return {};
    }

    auto laneFromPath = [](const QString &p) -> QString {
        if (p.startsWith(QStringLiteral("src/"))) {
            const int slash = p.indexOf('/', 4);
            if (slash > 4) return p.mid(4, slash - 4);
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

    QString out;
    out.reserve(256 + actionable.size() * 200);
    out += QStringLiteral("### 🔍 Indie-review fold-in (");
    out += dateIso;
    out += QStringLiteral(")\n\n");

    for (int i = 0; i < actionable.size(); ++i) {
        const CorroboratedFinding &f = actionable.at(i);
        const int id = allocatedIds.at(i);
        const QString lane = laneFromPath(f.file);

        out += QStringLiteral("- 📋 [ANTS-");
        out += QString::number(id);
        out += QStringLiteral("] **Cited by ");
        out += QString::number(f.citingLanes.size());
        out += QStringLiteral(" lanes at `");
        out += f.file;
        if (f.line > 0) {
            out += QChar(':');
            out += QString::number(f.line);
        }
        out += QStringLiteral("`.**\n");
        out += QStringLiteral("  Lanes: ");
        out += f.citingLanes.join(QStringLiteral(", "));
        out += QStringLiteral(".\n");
        out += QStringLiteral("  Kind: review-fix.\n");
        out += QStringLiteral("  Source: indie-review-");
        out += dateIso;
        out += QStringLiteral(".\n");
        out += QStringLiteral("  Lanes: ");  // roadmap-format Lanes: field
        out += lane;
        out += QStringLiteral(".\n");
        if (i + 1 < actionable.size()) out += QChar('\n');
    }
    return out;
}

QString assembleThreatModelExtras(const QString &projectPath) {
    QString out;
    out.reserve(8 * 1024);
    auto append = [&](const QString &header, const QString &relPath) {
        out += QStringLiteral("=== ");
        out += header;
        out += QStringLiteral(" ===\n");
        const QString abs = projectPath + QChar('/') + relPath;
        const QString body = slurpUtf8(abs);
        out += body;
        if (!out.endsWith(QChar('\n'))) out += QChar('\n');
        out += QChar('\n');
    };
    append(QStringLiteral("CLAUDE.md"),     QStringLiteral("CLAUDE.md"));
    append(QStringLiteral("SECURITY.md"),   QStringLiteral("SECURITY.md"));
    append(QStringLiteral(".semgrep.yml"),  QStringLiteral(".semgrep.yml"));
    return out;
}

}  // namespace IndieReviewEngine
