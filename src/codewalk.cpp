// The source files a code-reading verb looks at (ANTS-5785, 5786, 5787).

#include "codewalk.h"

#include "codebaseindex.h"
#include "pathvalidation.h"
#include "projectsettings.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace CodeWalk {

namespace {

QJsonObject refusal(const QString &code, const QString &error) {
    QJsonObject e;
    e[QStringLiteral("ok")] = false;
    e[QStringLiteral("code")] = code;
    e[QStringLiteral("error")] = error;
    return e;
}

// Every indexable file under `dir`, as root-relative paths, pruning noise
// directories by name. Stops once `out` holds kMaxFiles + 1 entries, so the
// caller can tell a full walk from a cut one.
void walk(const QDir &root, const QString &dir, QStringList &out) {
    const QFileInfoList entries = QDir(dir).entryInfoList(
        QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks,
        QDir::Name);
    for (const QFileInfo &fi : entries) {
        if (out.size() > kMaxFiles) return;
        if (fi.isDir()) {
            if (fi.fileName().startsWith(QLatin1Char('.'))
                || ProjectSettings::isNoiseDir(fi.fileName()))
                continue;
            walk(root, fi.absoluteFilePath(), out);
        } else if (CodebaseIndex::isIndexableSuffix(fi.suffix().toLower())) {
            out.append(root.relativeFilePath(fi.absoluteFilePath()));
        }
    }
}

} // namespace

// ANTS-4096 — a generated source file is not review material: a lane briefed
// from one reviews a compiler's output. The prefix-only test this replaces
// (`moc_`/`ui_`/`qrc_`) missed every SUFFIX-shaped generator, which is how a
// shaders/ directory partitioned into 22 `*.spv.h` byte-array headers and none
// of the 19 hand-written GLSL sources beside them.
//
// Deliberately NOT AuditDialog::isGeneratedFile: that one lives on a
// Widgets-linked class this core library must not see, and it carries
// audit-artifact rules (`.audit_cache/`, `audit_fixtures/`) that have no
// meaning for a review partition. It also predates `.spv.h`. Moved here from
// indiereviewengine.cpp (ANTS-4120); auditdialog and findsources still keep
// their own copies.
bool isGeneratedSource(const QString &fileName) {
    static const QStringList kPrefixes = {
        QStringLiteral("moc_"), QStringLiteral("ui_"), QStringLiteral("qrc_"),
    };
    for (const QString &p : kPrefixes)
        if (fileName.startsWith(p)) return true;

    // Suffix-shaped generators: shader byte arrays (glslc/spirv `-mfs`),
    // protobuf/gRPC, flex/bison, and the `*_generated.<ext>` convention.
    static const QStringList kSuffixes = {
        QStringLiteral(".spv.h"),  QStringLiteral(".spv.hpp"),
        QStringLiteral(".pb.h"),   QStringLiteral(".pb.cc"),
        QStringLiteral(".yy.cc"),  QStringLiteral(".tab.cc"),
        QStringLiteral(".ttf.h"),  QStringLiteral(".moc"),
    };
    for (const QString &s : kSuffixes)
        if (fileName.endsWith(s, Qt::CaseInsensitive)) return true;

    static const QRegularExpression reGenerated(
        QStringLiteral(R"(_generated\.[A-Za-z0-9]+$)"),
        QRegularExpression::CaseInsensitiveOption);
    return reGenerated.match(fileName).hasMatch();
}

bool isTestPath(const QString &relPath, const QStringList &testRoots) {
    for (const QString &t : testRoots) {
        const QString root = QDir::cleanPath(t);
        if (root == QLatin1String(".")) continue;
        if (relPath == root || relPath.startsWith(root + QLatin1Char('/')))
            return true;
    }
    const QStringList parts = relPath.split(QLatin1Char('/'));
    for (int i = 0; i + 1 < parts.size(); ++i) {
        const QString &p = parts.at(i);
        if (p == QLatin1String("tests") || p == QLatin1String("test")
            || p == QLatin1String("__tests__") || p == QLatin1String("spec"))
            return true;
    }
    static const QRegularExpression reTestName(QStringLiteral(
        R"(^test_|_tests?\.[^.]+$|\.(test|spec)\.[^.]+$)"));
    return reTestName.match(parts.last()).hasMatch();
}

Result collect(const QString &rootCanonical, const QJsonArray &paths,
               const Options &opts, const QString &verb) {
    Result r;
    const QDir root(rootCanonical);
    const ProjectSettings::Settings settings = ProjectSettings::load(rootCanonical);

    QStringList requested;
    for (const QJsonValue v : paths) {
        if (!v.isString()) {
            r.refusal = refusal(QStringLiteral("bad_args"),
                                QStringLiteral("%1: \"paths\" entries must be strings")
                                    .arg(verb));
            return r;
        }
        requested.append(v.toString());
    }
    if (requested.isEmpty()) {
        requested = settings.sourceRoots.value_or(QStringList());
        if (requested.isEmpty())
            requested.append(root.exists(QStringLiteral("src")) ? QStringLiteral("src")
                                                                 : QStringLiteral("."));
    }

    QStringList named;   // files asked for by name: kept as asked
    QStringList walked;  // files found under a directory: filtered below
    for (const QString &raw : requested) {
        const PathValidation::Check check =
            PathValidation::validatePath(raw, rootCanonical, verb, QStringLiteral("paths"));
        if (check.bad) {
            r.refusal = check.err;
            return r;
        }
        const QFileInfo fi(check.resolved);
        if (check.resolved.isEmpty() || !fi.exists()) {
            r.refusal = refusal(QStringLiteral("bad_path"),
                                QStringLiteral("%1: \"paths\" entry \"%2\" does not exist")
                                    .arg(verb, raw));
            return r;
        }
        if (fi.isFile())
            named.append(root.relativeFilePath(fi.canonicalFilePath()));
        else
            walk(root, fi.canonicalFilePath(), walked);
    }

    // A walk that stopped at kMaxFiles + 1 left files unseen. The cap below
    // only sees what survived the filters, so a cut walk whose tests or
    // generated files were then dropped would read as complete.
    if (walked.size() > kMaxFiles) r.truncated = true;

    const QSet<QString> ignored = ProjectSettings::gitIgnoredPaths(rootCanonical, walked);
    const QStringList testRoots = settings.testRoots.value_or(QStringList());
    QSet<QString> kept(named.cbegin(), named.cend());
    for (const QString &rel : std::as_const(walked)) {
        if (ignored.contains(rel)) continue;
        if (!opts.includeGenerated && isGeneratedSource(QFileInfo(rel).fileName())) {
            ++r.skippedGenerated;
            continue;
        }
        if (!opts.includeTests && isTestPath(rel, testRoots)) {
            ++r.skippedTests;
            continue;
        }
        kept.insert(rel);
    }

    QStringList rels(kept.cbegin(), kept.cend());
    std::sort(rels.begin(), rels.end());
    if (rels.size() > kMaxFiles) {
        rels = rels.mid(0, kMaxFiles);
        r.truncated = true;
    }
    for (const QString &rel : std::as_const(rels))
        r.files.append(root.absoluteFilePath(rel));
    return r;
}

QJsonObject scannedSummary(const Result &r) {
    QJsonObject s;
    s[QStringLiteral("files")] = int(r.files.size());
    s[QStringLiteral("skipped_tests")] = r.skippedTests;
    s[QStringLiteral("skipped_generated")] = r.skippedGenerated;
    s[QStringLiteral("truncated")] = r.truncated;
    return s;
}

} // namespace CodeWalk
