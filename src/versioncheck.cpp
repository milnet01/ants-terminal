// version_check (ANTS-5510). See versioncheck.h.

#include "versioncheck.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

namespace VersionCheck {
namespace {

constexpr int kMaxUnlistedFileBytes = 1024 * 1024;
constexpr int kMaxUnlistedFindings = 100;

QJsonObject refuse(const QString &code, const QString &message) {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("code"), code},
            {QStringLiteral("error"), QStringLiteral("version_check: ") + message}};
}

bool readFile(const QString &path, QByteArray *out) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    *out = f.readAll();
    return true;
}

int lineOf(const QString &text, qsizetype offset) {
    return static_cast<int>(text.left(offset).count(QLatin1Char('\n'))) + 1;
}

QJsonObject finding(const QString &kind, const QString &file, int line,
                    const QString &message) {
    return {{QStringLiteral("kind"), kind},
            {QStringLiteral("file"), file},
            {QStringLiteral("line"), line},
            {QStringLiteral("message"), message}};
}

// A `pattern` is a literal with {OLD} in it (bump-recipe.md), so everything else
// is escaped. The suffix group is what tells a stray `-rc.1` from a plain drift.
QRegularExpression fileRegex(const QString &pattern) {
    const QStringList parts = pattern.split(QStringLiteral("{OLD}"));
    QStringList escaped;
    for (const QString &p : parts) escaped.append(QRegularExpression::escape(p));
    return QRegularExpression(escaped.join(QStringLiteral(
        R"(([0-9]+\.[0-9]+\.[0-9]+)([-+~][0-9A-Za-z.+~-]*)?)")));
}

// bump-recipe.md § Validating a recipe's four tells, plus the required fields.
QString recipeDefect(const QJsonObject &recipe) {
    if (recipe.value(QStringLiteral("version_source")).toString().isEmpty())
        return QStringLiteral("version_source is required");
    if (recipe.value(QStringLiteral("version_pattern")).toString().isEmpty())
        return QStringLiteral("version_pattern is required");
    if (recipe.contains(QStringLiteral("post_bump")))
        return QStringLiteral("post_bump is not part of the recipe format");
    for (const QJsonValue &v : recipe.value(QStringLiteral("files")).toArray()) {
        const QJsonObject o = v.toObject();
        const QString pattern = o.value(QStringLiteral("pattern")).toString();
        if (o.value(QStringLiteral("path")).toString().isEmpty() || pattern.isEmpty())
            return QStringLiteral("each files[] entry needs path and pattern");
        if (o.contains(QStringLiteral("format")) ||
            pattern.contains(QLatin1String("(?P<")) ||
            pattern.startsWith(QLatin1Char('^')) || pattern.endsWith(QLatin1Char('$')))
            return QStringLiteral("files[] entry for %1 is a regex dialect; "
                                  "pattern is a literal with {OLD}")
                .arg(o.value(QStringLiteral("path")).toString());
    }
    return {};
}

void checkListedFile(const QString &root, const QJsonObject &entry,
                     const QString &version, QJsonArray *files, QJsonArray *findings) {
    const QString path = entry.value(QStringLiteral("path")).toString();
    const QString pattern = entry.value(QStringLiteral("pattern")).toString();
    QJsonObject row{{QStringLiteral("path"), path}};
    const auto report = [&](const QString &status, const QJsonObject &f) {
        row[QStringLiteral("status")] = status;
        findings->append(f);
    };

    QByteArray bytes;
    if (!pattern.contains(QLatin1String("{OLD}"))) {
        // Nothing here says which version the line holds.
        row[QStringLiteral("status")] = QStringLiteral("not_checked");
    } else if (!readFile(root + QLatin1Char('/') + path, &bytes)) {
        report(QStringLiteral("file_missing"),
               finding(QStringLiteral("file_missing"), path, 0,
                       QStringLiteral("the recipe lists this file and it cannot be read")));
    } else {
        const QString text = QString::fromUtf8(bytes);
        QList<QRegularExpressionMatch> hits;
        auto it = fileRegex(pattern).globalMatch(text);
        while (it.hasNext()) hits.append(it.next());
        row[QStringLiteral("status")] = QStringLiteral("ok");
        if (hits.isEmpty()) {
            report(QStringLiteral("pattern_not_found"),
                   finding(QStringLiteral("pattern_not_found"), path, 0,
                           QStringLiteral("no line matches the recipe's pattern: %1").arg(pattern)));
        } else if (hits.size() > 1 && !entry.value(QStringLiteral("replace_all")).toBool()) {
            report(QStringLiteral("pattern_ambiguous"),
                   finding(QStringLiteral("pattern_ambiguous"), path,
                           lineOf(text, hits.at(1).capturedStart()),
                           QStringLiteral("the pattern matches %1 places and replace_all is not set")
                               .arg(hits.size())));
        }
        for (const QRegularExpressionMatch &m : std::as_const(hits)) {
            const QString found = m.captured(1);
            const QString suffix = m.captured(2);
            if (found == version && suffix.isEmpty()) continue;
            const QString kind = found == version ? QStringLiteral("stray_suffix")
                                                  : QStringLiteral("version_drift");
            QJsonObject f = finding(kind, path, lineOf(text, m.capturedStart()),
                                    QStringLiteral("holds %1%2 where %3 is expected")
                                        .arg(found, suffix, version));
            f[QStringLiteral("found")] = found + suffix;
            f[QStringLiteral("expected")] = version;
            report(kind, f);
        }
    }
    files->append(row);
}

// History and contracts quote versions on purpose: bump-recipe.md's Notes say a
// grep for the version finds both kinds and cannot tell them apart. So the hits
// are candidates, and a file the recipe names anywhere has been considered.
bool skipForUnlisted(const QString &path, const QString &recipeText) {
    return path.startsWith(QLatin1String("tests/")) ||
           path.startsWith(QLatin1String("test/")) ||
           path.startsWith(QLatin1String("docs/")) ||
           path.endsWith(QLatin1String(".md")) ||
           recipeText.contains(path);
}

bool isCommentLine(const QString &line) {
    const QString t = line.trimmed();
    return t.startsWith(QLatin1String("//")) || t.startsWith(QLatin1Char('*')) ||
           t.startsWith(QLatin1String("/*"));
}

void scanUnlisted(const QString &root, const QStringList &tracked, const QString &version,
                  const QString &recipeText, QJsonArray *findings, QJsonObject *out) {
    const QRegularExpression re(QStringLiteral(R"((?<![0-9.])%1(?![0-9]|\.[0-9]))")
                                    .arg(QRegularExpression::escape(version)));
    int scanned = 0;
    int reported = 0;
    bool capped = false;
    for (const QString &path : tracked) {
        if (skipForUnlisted(path, recipeText)) continue;
        QFile f(root + QLatin1Char('/') + path);
        if (f.size() > kMaxUnlistedFileBytes || !f.open(QIODevice::ReadOnly)) continue;
        const QByteArray bytes = f.readAll();
        if (bytes.contains('\0')) continue;
        ++scanned;
        const QStringList lines = QString::fromUtf8(bytes).split(QLatin1Char('\n'));
        for (int i = 0; i < lines.size(); ++i) {
            if (isCommentLine(lines[i]) || !re.match(lines[i]).hasMatch()) continue;
            if (reported++ >= kMaxUnlistedFindings) {
                capped = true;
                break;
            }
            QJsonObject hit = finding(
                QStringLiteral("unlisted_version_file"), path, i + 1,
                QStringLiteral("holds %1 and the recipe does not name this file; "
                               "list it, or add a $ note saying why not").arg(version));
            hit[QStringLiteral("candidate")] = true;
            findings->append(hit);
            break;   // one row per file: the file is the question
        }
        if (capped) break;
    }
    (*out)[QStringLiteral("unlisted_files_scanned")] = scanned;
    if (capped) (*out)[QStringLiteral("unlisted_truncated")] = true;
}

}  // namespace

QJsonObject run(const QString &root, const QStringList &trackedFiles) {
    QJsonObject out;
    QByteArray recipeBytes;
    if (!readFile(root + QStringLiteral("/.claude/bump.json"), &recipeBytes)) {
        out[QStringLiteral("ok")] = true;
        out[QStringLiteral("recipe_present")] = false;
        out[QStringLiteral("findings")] = QJsonArray();
        out[QStringLiteral("skipped")] =
            QJsonArray{QStringLiteral("lockstep"), QStringLiteral("unlisted")};
        out[QStringLiteral("hint")] = QStringLiteral(
            "no .claude/bump.json, so nothing was checked. This is not a clean result.");
        return out;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(recipeBytes, &parseError);
    if (!doc.isObject())
        return refuse(QStringLiteral("bad_recipe"),
                      QStringLiteral(".claude/bump.json is not a JSON object: %1")
                          .arg(parseError.errorString()));
    const QJsonObject recipe = doc.object();
    if (const QString defect = recipeDefect(recipe); !defect.isEmpty())
        return refuse(QStringLiteral("bad_recipe"), defect);

    const QString source = recipe.value(QStringLiteral("version_source")).toString();
    const QRegularExpression versionRe(recipe.value(QStringLiteral("version_pattern")).toString());
    if (!versionRe.isValid() || versionRe.captureCount() < 1)
        return refuse(QStringLiteral("bad_recipe"),
                      QStringLiteral("version_pattern must be a valid regex with a capture group"));
    QByteArray sourceBytes;
    if (!readFile(root + QLatin1Char('/') + source, &sourceBytes))
        return refuse(QStringLiteral("version_unreadable"),
                      QStringLiteral("cannot read version_source %1").arg(source));
    const QString version = versionRe.match(QString::fromUtf8(sourceBytes)).captured(1);
    if (version.isEmpty())
        return refuse(QStringLiteral("version_unreadable"),
                      QStringLiteral("version_pattern matches nothing in %1").arg(source));

    QJsonArray files;
    QJsonArray findings;
    for (const QJsonValue &v : recipe.value(QStringLiteral("files")).toArray())
        checkListedFile(root, v.toObject(), version, &files, &findings);

    QJsonArray skipped;
    if (trackedFiles.isEmpty())
        skipped.append(QStringLiteral("unlisted"));
    else
        scanUnlisted(root, trackedFiles, version, QString::fromUtf8(recipeBytes), &findings, &out);

    QJsonObject counts;
    for (const QJsonValue &v : std::as_const(findings)) {
        const QString kind = v.toObject().value(QStringLiteral("kind")).toString();
        counts[kind] = counts.value(kind).toInt() + 1;
    }
    out[QStringLiteral("ok")] = true;
    out[QStringLiteral("recipe_present")] = true;
    out[QStringLiteral("version")] = version;
    out[QStringLiteral("version_source")] = source;
    out[QStringLiteral("files")] = files;
    out[QStringLiteral("findings")] = findings;
    out[QStringLiteral("counts")] = counts;
    out[QStringLiteral("skipped")] = skipped;
    return out;
}

}  // namespace VersionCheck
