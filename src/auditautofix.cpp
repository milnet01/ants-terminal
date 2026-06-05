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

    // 1. cppcheck `unusedInclude` — NOT auto-removed (ANTS-2006). cppcheck has
    //    a well-known false-positive rate on Qt code (it doesn't model Qt's
    //    transitive includes / moc-generated needs), so deleting a "unused"
    //    header can break the build — violating the behaviour-neutral contract.
    //    cppcheck also can't see whether the finding came from a --library=qt
    //    run here, so there's no safe corroboration to gate on. Left for manual
    //    review rather than dropped silently.

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

    // ANTS-1744 — refuse an ambiguous removal. The exact-text guard above
    // can pass on a *different* line than the finding meant if an edit
    // moved a textually-identical line into r.line's slot (e.g. a dead
    // `#include <X>` and a still-used `#include <X>` elsewhere). When the
    // line text isn't unique we can't tell which occurrence the finding
    // referred to, so deleting it could drop a needed line — bail.
    if (r.removeLine && lines.count(r.original) > 1) return false;

    if (r.removeLine)
        lines.removeAt(r.line - 1);
    else
        lines[r.line - 1] = r.fixed;

    QString out = lines.join('\n');
    if (trailingNewline) out.append('\n');

    QSaveFile sf(r.file);
    if (!sf.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    // Refuse a short write rather than commit a truncated source file
    // (disk-full corruption guard). indie-review-2026-05-21.
    const QByteArray outBytes = out.toUtf8();
    if (sf.write(outBytes) != outBytes.size()) return false;
    if (!sf.commit()) return false;
    // ANTS-2006 — QSaveFile commits via rename(2), but the new directory entry
    // isn't durable until the parent dir is fsync'd (the ANTS-1141 pattern,
    // same as auditcache/auditrunner). A crash mid-commit could otherwise lose
    // the edit while reporting success.
    fsyncParentDir(r.file);
    return true;
}

bool logRepair(const QString &cacheDir, const Repair &r) {
    if (cacheDir.isEmpty()) return false;
    if (!ensurePrivateDir(cacheDir)) return false;  // ANTS-1988 — 0700, no 0755 window

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
    // ANTS-1824 — single O_APPEND write for record + newline so a
    // concurrent appender can't split the JSON from its '\n'.
    const QByteArray line =
        QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
    const bool wrote = f.write(line) == line.size();
    f.close();
    return wrote;
}

}  // namespace autofix
}  // namespace ants
