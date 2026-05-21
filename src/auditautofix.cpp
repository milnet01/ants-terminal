// ANTS-1719 — see auditautofix.h.

#include "auditautofix.h"

#include "secureio.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringList>

namespace ants {
namespace autofix {

bool versionLE(const QString &a, const QString &b) {
    static const QRegularExpression ver(QStringLiteral(R"(^(\d+)\.(\d+)\.(\d+)$)"));
    const QRegularExpressionMatch ma = ver.match(a.trimmed());
    const QRegularExpressionMatch mb = ver.match(b.trimmed());
    if (!ma.hasMatch() || !mb.hasMatch()) return false;
    for (int i = 1; i <= 3; ++i) {
        const int x = ma.captured(i).toInt();
        const int y = mb.captured(i).toInt();
        if (x != y) return x < y;
    }
    return true;  // equal
}

std::optional<Repair> planRepair(const Finding &f,
                                 const QString &absPath,
                                 const QString &fileContents,
                                 const QString &currentVersion) {
    if (f.line < 1 || absPath.isEmpty()) return std::nullopt;
    const QStringList lines = fileContents.split('\n');
    if (f.line > lines.size()) return std::nullopt;
    const QString line = lines.at(f.line - 1);

    auto make = [&](const QString &rule, const QString &fixed,
                    bool remove) -> std::optional<Repair> {
        Repair r;
        r.rule       = rule;
        r.file       = absPath;
        r.line       = f.line;
        r.original   = line;
        r.fixed      = fixed;
        r.removeLine = remove;
        return r;
    };

    // 1. cppcheck `unusedInclude` -> remove the #include line. Trust the
    //    tool's verdict, but only when the flagged line really is an
    //    include directive (a moved finding must not delete code).
    if (f.checkId.contains(QStringLiteral("cppcheck"), Qt::CaseInsensitive) &&
        f.message.contains(QStringLiteral("unusedInclude"), Qt::CaseInsensitive)) {
        static const QRegularExpression inc(QStringLiteral(R"(^\s*#\s*include\b)"));
        if (inc.match(line).hasMatch())
            return make(QStringLiteral("autofix.unused_include"), QString(), true);
        return std::nullopt;
    }

    // 2. dead Q_UNUSED — only a standalone `Q_UNUSED(...);` statement.
    if (f.message.contains(QStringLiteral("Q_UNUSED"), Qt::CaseInsensitive)) {
        static const QRegularExpression qun(
            QStringLiteral(R"(^\s*Q_UNUSED\s*\([^()]*\)\s*;\s*$)"));
        if (qun.match(line).hasMatch())
            return make(QStringLiteral("autofix.dead_q_unused"), QString(), true);
        return std::nullopt;
    }

    // 3. versioned TODO/FIXME — a standalone comment whose "remove after
    //    <ver>" is <= the current build version. Removing a comment is
    //    behaviour-neutral; gated on the line shape (self-verifying).
    {
        static const QRegularExpression todo(
            QStringLiteral(
                R"(^\s*//.*\b(?:TODO|FIXME)\b.*\bremove after\b\s*v?(\d+\.\d+\.\d+))"),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch m = todo.match(line);
        if (m.hasMatch()) {
            if (versionLE(m.captured(1), currentVersion))
                return make(QStringLiteral("autofix.stale_todo"), QString(), true);
            return std::nullopt;  // marker references a future release
        }
    }

    // 4. comment-style `//word` -> `// word` on a standalone comment line.
    //    Gated on the finding flagging a comment issue so an unrelated
    //    finding that happens to sit on a `//x` line isn't reformatted.
    if (f.message.contains(QStringLiteral("comment"), Qt::CaseInsensitive)) {
        static const QRegularExpression cmt(QStringLiteral(R"(^(\s*)//([^/!\s].*)$)"));
        const QRegularExpressionMatch m = cmt.match(line);
        if (m.hasMatch())
            return make(QStringLiteral("autofix.comment_space"),
                        m.captured(1) + QStringLiteral("// ") + m.captured(2),
                        false);
    }

    return std::nullopt;
}

bool applyRepair(const Repair &r) {
    if (r.file.isEmpty() || r.line < 1) return false;

    QFile in(r.file);
    if (!in.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    const QString contents = QString::fromUtf8(in.readAll());
    in.close();

    // Preserve a trailing newline if present so a removal/replace doesn't
    // strip the file's final EOL.
    const bool trailingNewline = contents.endsWith('\n');
    QStringList lines = contents.split('\n');
    if (trailingNewline && !lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast();

    if (r.line > lines.size()) return false;
    if (lines.at(r.line - 1) != r.original) return false;  // stale plan

    if (r.removeLine)
        lines.removeAt(r.line - 1);
    else
        lines[r.line - 1] = r.fixed;

    QString out = lines.join('\n');
    if (trailingNewline) out.append('\n');

    QSaveFile sf(r.file);
    if (!sf.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    sf.write(out.toUtf8());
    return sf.commit();
}

bool logRepair(const QString &cacheDir, const Repair &r) {
    if (cacheDir.isEmpty()) return false;
    if (!QDir().mkpath(cacheDir)) return false;

    const QString day = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-dd"));
    const QString path = cacheDir + QStringLiteral("/autofix-") + day + QStringLiteral(".jsonl");

    QFile f(path);
    const bool fresh = !QFileInfo::exists(path) || QFileInfo(path).size() == 0;
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append)) return false;
    if (fresh) {
        setOwnerOnlyPerms(f);
        f.write("# ants-audit auto-fix log (JSONL, ANTS-1719). One entry "
                "per applied behaviour-neutral repair; append-only.\n");
    }

    QJsonObject o;
    o[QStringLiteral("file")]      = r.file;
    o[QStringLiteral("line")]      = r.line;
    o[QStringLiteral("rule")]      = r.rule;
    o[QStringLiteral("original")]  = r.original;
    o[QStringLiteral("fixed")]     = r.removeLine ? QString() : r.fixed;
    o[QStringLiteral("timestamp")] =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
    f.write("\n");
    f.close();
    return true;
}

}  // namespace autofix
}  // namespace ants
