// Implementation: see debtsweepengine.h for the contract.
//
// Detector pattern: each is a top-level QString → QList<Finding>
// pure function, file IO constrained to projectPath, git invocations
// via QProcess with stdout caps + 30 s timeout. No persistent state.

#include "debtsweepengine.h"

#include "featurecoverage.h"
#include "roadmapfoldin.h"  // ANTS-3497 — shared renderId() for padded, project-prefixed IDs

#include <QByteArray>
#include <QChar>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStringList>

namespace DebtSweepEngine {

namespace {

// stdout cap for git invocations. Defensive — a massive diff or
// log would otherwise pin RAM during scanAll.
constexpr int kGitStdoutCap     = 1 * 1024 * 1024;   // 1 MiB
constexpr int kGitLogCap        = 4 * 1024 * 1024;   // 4 MiB
constexpr int kGitFilelistCap   = 256 * 1024;        // 256 KiB
constexpr int kGitTimeoutMs     = 30000;
constexpr int kPackagingScriptTimeoutMs = 30000;

// Maximum entries in a `git diff --name-only` result we'll iterate.
// 500 paths is generous — anything above is a release/refactor mega-
// diff that wants a different tool.
constexpr int kMaxDiffPaths = 500;

QString slurpUtf8(const QString &absPath) {
    // ANTS-1674 — cache by (absPath, mtime_ms). Single-threaded dispatcher.
    static QHash<QString, QPair<qint64, QString>> s_cache;
    const qint64 mtime =
        QFileInfo(absPath).lastModified().toMSecsSinceEpoch();
    auto it = s_cache.find(absPath);
    if (it != s_cache.end() && mtime != 0 && it->first == mtime)
        return it->second;
    QFile f(absPath);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QString content = QString::fromUtf8(f.readAll());
    s_cache.insert(absPath, {mtime, content});
    return content;
}

// Run a git command in projectPath; return stdout (capped).
// Returns empty QString on timeout / non-zero exit / process error.
QString runGit(const QString &projectPath,
               const QStringList &args,
               int stdoutCap = kGitStdoutCap) {
    QProcess p;
    p.setWorkingDirectory(projectPath);
    p.setProcessChannelMode(QProcess::SeparateChannels);
    p.start(QStringLiteral("git"), args);
    if (!p.waitForStarted(2000)) return {};
    if (!p.waitForFinished(kGitTimeoutMs)) {
        p.kill();
        return {};
    }
    if (p.exitStatus() != QProcess::NormalExit) return {};
    QByteArray out = p.readAllStandardOutput();
    if (out.size() > stdoutCap) out.truncate(stdoutCap);
    return QString::fromUtf8(out);
}

// Resolve sinceRef. Empty input → `git describe --tags --abbrev=0`,
// or `HEAD~10` if no tags exist. Returns the resolved ref.
QString resolveSinceRef(const QString &projectPath, const QString &sinceRef) {
    if (!sinceRef.isEmpty()) return sinceRef;
    const QString tag = runGit(
        projectPath,
        {QStringLiteral("describe"), QStringLiteral("--tags"),
         QStringLiteral("--abbrev=0")}).trimmed();
    if (!tag.isEmpty()) return tag;
    return QStringLiteral("HEAD~10");
}

// Diffed file list, capped + filtered by extension globs (raw `--`
// args passed through to git).
QStringList diffedFiles(const QString &projectPath,
                        const QString &since,
                        const QStringList &extGlobs) {
    QStringList args = {
        QStringLiteral("diff"),
        QStringLiteral("--name-only"),
        since + QStringLiteral("..HEAD"),
        QStringLiteral("--"),
    };
    for (const QString &g : extGlobs) args << g;
    const QString out = runGit(projectPath, args, kGitFilelistCap);
    QStringList rows;
    const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
    for (const QString &l : lines) {
        const QString t = l.trimmed();
        if (t.isEmpty()) continue;
        rows << t;
        if (rows.size() >= kMaxDiffPaths) break;
    }
    return rows;
}

// git ls-files restricted to the given pathspecs; capped + split.
QStringList lsFiles(const QString &projectPath, const QStringList &globs,
                    int cap = 5000) {
    QStringList args = {QStringLiteral("ls-files")};
    for (const QString &g : globs) args << g;
    const QString out = runGit(projectPath, args, kGitStdoutCap);
    if (out.isEmpty()) return {};
    QStringList rows = out.split('\n', Qt::SkipEmptyParts);
    if (rows.size() > cap) rows = rows.mid(0, cap);
    return rows;
}

}  // anonymous

// ---------------------------------------------------------------------------
// Code drift (a) — stale type-name comments
// ---------------------------------------------------------------------------

QList<Finding> detectStaleTypeComments(
    const QString &projectPath, const ScanOptions &opt) {
    const QString since = resolveSinceRef(projectPath, opt.sinceRef);
    static const QStringList kExts = {
        QStringLiteral("*.cpp"), QStringLiteral("*.h"),
        QStringLiteral("*.py"),  QStringLiteral("*.js"),
        QStringLiteral("*.ts"),  QStringLiteral("*.tsx"),
    };
    const QStringList files = diffedFiles(projectPath, since, kExts);
    if (files.isEmpty()) return {};

    const QString blob = FeatureCoverage::buildProjectSourceBlob(projectPath);
    if (blob.isEmpty()) return {};
    const QSet<QString> &stop = FeatureCoverage::specStopwords();

    // Bare-comment-token regex: leading-cap CamelCase ≥4 chars. The
    // capitalisation requirement is deliberate — bare lowercase words
    // in comments are usually prose, not type names.
    static const QRegularExpression kBareTokenRe(
        QStringLiteral(R"(\b([A-Z][A-Za-z0-9_]{3,})\b)"));
    static const QRegularExpression kLineCommentRe(
        QStringLiteral(R"(//(.*)$)"));
    // Block-comment: greedy match across lines is overkill here —
    // we scan line-by-line and treat any `/*` … `*/` segment as
    // comment text. For the v1 detector we keep it line-local
    // (multi-line block comments contribute their per-line content).
    static const QRegularExpression kBlockCommentChunkRe(
        QStringLiteral(R"(/\*(.*?)\*/)"));
    static const QRegularExpression kBlockCommentTailRe(
        QStringLiteral(R"(/\*(.*)$)"));
    static const QRegularExpression kBlockCommentHeadRe(
        QStringLiteral(R"(^(.*?)\*/)"));

    QList<Finding> out;
    for (const QString &rel : files) {
        const QString abs = projectPath + QChar('/') + rel;
        QFile f(abs);
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QString body = QString::fromUtf8(f.readAll());
        f.close();
        const QStringList lines = body.split('\n');

        bool inBlock = false;
        for (int i = 0; i < lines.size(); ++i) {
            // Skip the first 12 lines (typical license / copyright).
            if (i < 12) continue;
            const QString &raw = lines.at(i);

            QString commentText;
            if (inBlock) {
                const auto m = kBlockCommentHeadRe.match(raw);
                if (m.hasMatch()) {
                    commentText += m.captured(1);
                    inBlock = false;
                } else {
                    commentText += raw;
                }
            } else {
                // Block-comment chunks on this line.
                auto it = kBlockCommentChunkRe.globalMatch(raw);
                while (it.hasNext()) {
                    const auto m = it.next();
                    commentText += QChar(' ');
                    commentText += m.captured(1);
                }
                // Trailing block-comment opener (no close on same line).
                const auto t = kBlockCommentTailRe.match(raw);
                if (t.hasMatch()) {
                    commentText += QChar(' ');
                    commentText += t.captured(1);
                    inBlock = true;
                }
                // Line comment.
                const auto lc = kLineCommentRe.match(raw);
                if (lc.hasMatch()) {
                    commentText += QChar(' ');
                    commentText += lc.captured(1);
                }
            }
            if (commentText.isEmpty()) continue;

            QSet<QString> seenThisLine;
            auto it = kBareTokenRe.globalMatch(commentText);
            while (it.hasNext()) {
                const auto m = it.next();
                const QString tok = m.captured(1);
                if (stop.contains(tok)) continue;
                if (seenThisLine.contains(tok)) continue;
                seenThisLine.insert(tok);
                if (FeatureCoverage::existsInSource(blob, tok)) continue;
                Finding fnd;
                fnd.category    = QStringLiteral("code_drift");
                fnd.detectorId  = QStringLiteral("stale_type_comment");
                fnd.file        = rel;
                fnd.line        = i + 1;
                fnd.message     = QStringLiteral(
                    "comment references `%1` but no match in project source"
                ).arg(tok);
                out.append(fnd);
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Code drift (c) — TODO/FIXME added in scope
// ---------------------------------------------------------------------------

QList<Finding> detectAddedTodos(
    const QString &projectPath, const ScanOptions &opt) {
    const QString since = resolveSinceRef(projectPath, opt.sinceRef);
    static const QStringList kExts = {
        QStringLiteral("*.cpp"), QStringLiteral("*.h"),
        QStringLiteral("*.py"),  QStringLiteral("*.js"),
        QStringLiteral("*.ts"),  QStringLiteral("*.tsx"),
        QStringLiteral("*.go"),  QStringLiteral("*.rs"),
    };
    QStringList args = {
        QStringLiteral("diff"),
        QStringLiteral("--unified=0"),
        since + QStringLiteral("..HEAD"),
        QStringLiteral("--"),
    };
    for (const QString &g : kExts) args << g;
    const QString diff = runGit(projectPath, args, kGitStdoutCap);
    if (diff.isEmpty()) return {};

    static const QRegularExpression kTodoRe(
        QStringLiteral(R"(\b(TODO|FIXME|XXX|HACK)\b\s*[:(])"));
    static const QRegularExpression kFileRe(
        QStringLiteral(R"(^\+\+\+ b/(.+)$)"));
    static const QRegularExpression kHunkRe(
        QStringLiteral(R"(^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@)"));

    QList<Finding> out;
    QString currentFile;
    int currentLine = 0;
    const QStringList lines = diff.split('\n');
    for (const QString &raw : lines) {
        if (raw.startsWith(QStringLiteral("+++ "))) {
            const auto m = kFileRe.match(raw);
            currentFile = m.hasMatch() ? m.captured(1) : QString();
            currentLine = 0;
            continue;
        }
        if (raw.startsWith(QStringLiteral("@@"))) {
            const auto m = kHunkRe.match(raw);
            if (m.hasMatch()) {
                currentLine = m.captured(1).toInt();
            }
            continue;
        }
        if (raw.startsWith(QStringLiteral("+++ ")) ||
            raw.startsWith(QStringLiteral("--- "))) {
            continue;
        }
        if (currentFile.isEmpty()) continue;
        if (raw.startsWith(QChar('+'))) {
            const QString line = raw.mid(1);
            const auto m = kTodoRe.match(line);
            if (m.hasMatch()) {
                Finding fnd;
                fnd.category   = QStringLiteral("code_drift");
                fnd.detectorId = QStringLiteral("added_todo");
                fnd.file       = currentFile;
                fnd.line       = currentLine;
                QString preview = line.trimmed();
                if (preview.size() > 80) preview = preview.left(77) + QStringLiteral("...");
                fnd.message    = preview;
                out.append(fnd);
            }
            ++currentLine;
        } else if (!raw.startsWith(QChar('-'))) {
            // Context line in unified=0 mode (rare); advance.
            ++currentLine;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Code drift (d) — orphan Q_UNUSED / (void)x; markers
// ---------------------------------------------------------------------------

QList<Finding> detectOrphanQUnused(
    const QString &projectPath, const ScanOptions & /*opt*/) {
    const QString files = runGit(
        projectPath,
        {QStringLiteral("ls-files"),
         QStringLiteral("*.cpp"), QStringLiteral("*.h")},
        kGitStdoutCap);
    if (files.isEmpty()) return {};

    static const QRegularExpression kQUnusedRe(
        QStringLiteral(R"(Q_UNUSED\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\))"));
    static const QRegularExpression kVoidCastRe(
        QStringLiteral(R"(\(void\)\s*([A-Za-z_][A-Za-z0-9_]*)\s*;)"));

    QList<Finding> out;
    int processed = 0;
    const QStringList paths = files.split('\n', Qt::SkipEmptyParts);
    for (const QString &rel : paths) {
        if (++processed > 5000) break;
        const QString abs = projectPath + QChar('/') + rel;
        const QString body = slurpUtf8(abs);
        if (body.isEmpty()) continue;
        const QStringList lines = body.split('\n');

        // Per-marker pass: find Q_UNUSED(x) / (void)x;, then check
        // for any plausible declaration of `x` anywhere in this file.
        struct Marker { int line{}; QString varname; };
        QList<Marker> markers;
        for (int i = 0; i < lines.size(); ++i) {
            // Skip comment-only lines: doc comments that *name* the markers
            // (e.g. "// find Q_UNUSED(x) / (void)x;") are documentation, not
            // live markers — scanning them is a self-match false positive.
            const QString t = lines.at(i).trimmed();
            if (t.startsWith(QStringLiteral("//"))
                || t.startsWith(QStringLiteral("*"))
                || t.startsWith(QStringLiteral("/*")))
                continue;
            auto it = kQUnusedRe.globalMatch(lines.at(i));
            while (it.hasNext()) {
                const auto m = it.next();
                markers.append({i + 1, m.captured(1)});
            }
            auto vit = kVoidCastRe.globalMatch(lines.at(i));
            while (vit.hasNext()) {
                const auto m = vit.next();
                markers.append({i + 1, m.captured(1)});
            }
        }
        if (markers.isEmpty()) continue;

        for (const Marker &mk : markers) {
            // Plausible-declaration check. We look for any of:
            //   - `<typeword> <varname>` (auto/int/bool/.../typename Foo)
            //   - typed param/decl: `Foo &varname`, `Foo *varname`, etc.
            //   - lambda capture: `[..., varname, ...]` between `[` `]`
            // The check is conservative — we only need to confirm SOME
            // declaration exists; false negatives (missed declaration
            // shape) are acceptable since they'd just suppress a
            // legitimate orphan. False positives (claiming declared
            // when not) would be wrong, so we err on the side of
            // matching too much.
            const QString &v = mk.varname;
            const QRegularExpression typedDecl(
                QStringLiteral(R"(\b(?:auto|int|bool|float|double|char|short|long|size_t|int32_t|int64_t|uint32_t|uint64_t|qint8|qint16|qint32|qint64|quint8|quint16|quint32|quint64|qlonglong|qulonglong|qreal|qsizetype|uchar|ushort|uint|ulong|const|volatile|unsigned|signed|void)\b[^;{=,)]*\b%1\b)").arg(QRegularExpression::escape(v)));
            const QRegularExpression typedCustom(
                QStringLiteral(R"(\b[A-Z][A-Za-z_0-9]*\s*[*&]?\s*\b%1\b\s*[;,)=])").arg(QRegularExpression::escape(v)));
            const QRegularExpression lambdaCap(
                QStringLiteral(R"(\[[^\]]*\b%1\b[^\]]*\])").arg(QRegularExpression::escape(v)));
            bool declared = false;
            if (typedDecl.match(body).hasMatch()) declared = true;
            else if (typedCustom.match(body).hasMatch()) declared = true;
            else if (lambdaCap.match(body).hasMatch()) declared = true;
            if (declared) continue;

            Finding fnd;
            fnd.category    = QStringLiteral("code_drift");
            fnd.detectorId  = QStringLiteral("orphan_q_unused");
            fnd.file        = rel;
            fnd.line        = mk.line;
            fnd.message     = QStringLiteral(
                "Q_UNUSED(%1) — no declaration of %1 in this file").arg(v);
            fnd.suggestedFix = QStringLiteral("<delete the marker line>");
            fnd.autoFixable = true;
            out.append(fnd);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Code drift (e) — stale TODO/FIXME (git-blame age)  [ANTS-1358]
// ---------------------------------------------------------------------------

namespace {

// TODO/FIXME/XXX/HACK marker, same shape detectAddedTodos uses.
const QRegularExpression &todoMarkerRe() {
    static const QRegularExpression re(
        QStringLiteral(R"(\b(TODO|FIXME|XXX|HACK)\b\s*[:(])"));
    return re;
}

// Parse `git blame --line-porcelain` into final-line → committer-time
// (epoch seconds). With --line-porcelain the commit headers repeat for
// every line, so a single forward pass suffices.
QHash<int, qint64> blameCommitterTimes(const QString &projectPath,
                                       const QString &relPath) {
    const QString out = runGit(
        projectPath,
        {QStringLiteral("blame"), QStringLiteral("--line-porcelain"),
         QStringLiteral("--"), relPath},
        kGitLogCap);
    QHash<int, qint64> times;
    if (out.isEmpty()) return times;
    static const QRegularExpression headerRe(
        QStringLiteral(R"(^[0-9a-f]{40} \d+ (\d+))"));
    int curLine = -1;
    const QStringList lines = out.split('\n');
    for (const QString &l : lines) {
        const auto h = headerRe.match(l);
        if (h.hasMatch()) {
            curLine = h.captured(1).toInt();
            continue;
        }
        if (curLine > 0 && l.startsWith(QStringLiteral("committer-time "))) {
            times.insert(curLine,
                         l.mid(15).trimmed().toLongLong());
        }
    }
    return times;
}

}  // anonymous

QList<Finding> detectStaleTodos(
    const QString &projectPath, const ScanOptions &opt) {
    if (opt.staleTodoMaxAgeDays <= 0) return {};
    static const QStringList kExts = {
        QStringLiteral("*.cpp"), QStringLiteral("*.h"),
        QStringLiteral("*.py"),  QStringLiteral("*.js"),
        QStringLiteral("*.ts"),  QStringLiteral("*.tsx"),
        QStringLiteral("*.go"),  QStringLiteral("*.rs"),
    };
    const QStringList files = lsFiles(projectPath, kExts);
    if (files.isEmpty()) return {};

    const qint64 nowSec = QDateTime::currentSecsSinceEpoch();
    const qint64 thresholdSec =
        static_cast<qint64>(opt.staleTodoMaxAgeDays) * 86400;

    QList<Finding> out;
    for (const QString &rel : files) {
        const QString body = slurpUtf8(projectPath + QChar('/') + rel);
        if (body.isEmpty()) continue;
        const QStringList lines = body.split('\n');
        QList<int> hitLines;
        for (int i = 0; i < lines.size(); ++i) {
            if (todoMarkerRe().match(lines.at(i)).hasMatch())
                hitLines << (i + 1);
        }
        if (hitLines.isEmpty()) continue;

        const QHash<int, qint64> times = blameCommitterTimes(projectPath, rel);
        if (times.isEmpty()) continue;  // non-git / blame failure → skip file
        for (int ln : std::as_const(hitLines)) {
            const auto it = times.constFind(ln);
            if (it == times.constEnd()) continue;
            const qint64 ageSec = nowSec - it.value();
            if (ageSec < thresholdSec) continue;
            Finding fnd;
            fnd.category   = QStringLiteral("code_drift");
            fnd.detectorId = QStringLiteral("stale_todo");
            fnd.file       = rel;
            fnd.line       = ln;
            QString preview = lines.at(ln - 1).trimmed();
            if (preview.size() > 80) preview = preview.left(77) + QStringLiteral("...");
            fnd.message    = QStringLiteral("%1 (unchanged for %2 days)")
                                 .arg(preview)
                                 .arg(ageSec / 86400);
            out.append(fnd);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Code drift (f) — duplicate #include  [ANTS-1358]
// ---------------------------------------------------------------------------

namespace detail {

QList<Finding> scanDuplicateIncludes(const QString &relPath,
                                     const QString &body) {
    static const QRegularExpression kIncludeRe(
        QStringLiteral(R"(^\s*#\s*include\s+(["<][^">]*[">]))"));
    QList<Finding> out;
    QHash<QString, int> firstLineOf;
    int ppDepth = 0;
    const QStringList lines = body.split('\n');
    for (int i = 0; i < lines.size(); ++i) {
        const QString &raw = lines.at(i);
        const QString t = raw.trimmed();
        // Track preprocessor conditional nesting so we never flag an
        // include duplicated across mutually-exclusive #if branches.
        if (t.startsWith(QStringLiteral("#if"))) { ++ppDepth; continue; }
        if (t.startsWith(QStringLiteral("#endif"))) {
            if (ppDepth > 0) --ppDepth;
            continue;
        }
        if (t.startsWith(QStringLiteral("#el"))) continue;  // #else/#elif
        if (ppDepth != 0) continue;
        const auto m = kIncludeRe.match(raw);
        if (!m.hasMatch()) continue;
        const QString key = m.captured(1);
        const auto it = firstLineOf.constFind(key);
        if (it == firstLineOf.constEnd()) {
            firstLineOf.insert(key, i + 1);
            continue;
        }
        Finding fnd;
        fnd.category     = QStringLiteral("code_drift");
        fnd.detectorId   = QStringLiteral("duplicate_include");
        fnd.file         = relPath;
        fnd.line         = i + 1;
        fnd.message      = QStringLiteral(
            "duplicate #include %1 (first at line %2)")
                               .arg(key)
                               .arg(it.value());
        fnd.suggestedFix = QStringLiteral("<delete the redundant include line>");
        fnd.autoFixable  = true;
        out.append(fnd);
    }
    return out;
}

}  // namespace detail

QList<Finding> detectDuplicateIncludes(
    const QString &projectPath, const ScanOptions & /*opt*/) {
    const QStringList files = lsFiles(
        projectPath, {QStringLiteral("*.cpp"), QStringLiteral("*.h")});
    QList<Finding> out;
    for (const QString &rel : files) {
        const QString body = slurpUtf8(projectPath + QChar('/') + rel);
        if (body.isEmpty()) continue;
        out += detail::scanDuplicateIncludes(rel, body);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Code drift (g) — obsolete Qt6 QString idioms  [ANTS-1358]
// ---------------------------------------------------------------------------

namespace {

// Removed-in-Qt6 idiom → mechanical replacement. The find patterns are
// anchored to *code* usage — `(?=\s*\()` for the call forms, a
// quote-excluding lookbehind for `QString::null` — so a quoted mention
// of the idiom name (e.g. this very table, parsed as source) is not a
// match. Callers additionally strip `//` line-comments before scanning.
struct QtIdiom {
    QRegularExpression find;
    QString            replace;
    QString            label;
};
const QList<QtIdiom> &obsoleteQtIdioms() {
    static const QList<QtIdiom> tbl = {
        {QRegularExpression(QStringLiteral(R"((?<![\w"])QString::null\b(?!"))")),
         QStringLiteral("QString()"), QStringLiteral("QString::null")},
        {QRegularExpression(QStringLiteral(R"(\btoAscii\b(?=\s*\())")),
         QStringLiteral("toLatin1"),  QStringLiteral("toAscii")},
        {QRegularExpression(QStringLiteral(R"(\bfromAscii\b(?=\s*\())")),
         QStringLiteral("fromLatin1"), QStringLiteral("fromAscii")},
    };
    return tbl;
}

// Drop a trailing `//` line-comment (best-effort: ignores `//` that
// appears after the first double-quote, to avoid cutting at a `//`
// inside a string literal).
QString stripLineComment(const QString &s) {
    const int q = s.indexOf(QChar('"'));
    const int c = s.indexOf(QStringLiteral("//"));
    if (c >= 0 && (q < 0 || c < q)) return s.left(c);
    return s;
}

}  // anonymous

namespace detail {

QList<Finding> scanObsoleteQStringIdioms(const QString &relPath,
                                         const QString &body) {
    QList<Finding> out;
    const QStringList lines = body.split('\n');
    for (int i = 0; i < lines.size(); ++i) {
        const QString code = stripLineComment(lines.at(i));
        QString hit;
        QString repl;
        for (const QtIdiom &idiom : obsoleteQtIdioms()) {
            if (idiom.find.match(code).hasMatch()) {
                hit = idiom.label;
                repl = idiom.replace;
                break;
            }
        }
        if (hit.isEmpty()) continue;
        Finding fnd;
        fnd.category     = QStringLiteral("code_drift");
        fnd.detectorId   = QStringLiteral("obsolete_qstring_idiom");
        fnd.file         = relPath;
        fnd.line         = i + 1;
        fnd.message      = QStringLiteral("obsolete Qt6 idiom `%1` → `%2`")
                               .arg(hit, repl);
        fnd.suggestedFix = repl;
        fnd.autoFixable  = true;
        out.append(fnd);
    }
    return out;
}

}  // namespace detail

QList<Finding> detectObsoleteQStringIdioms(
    const QString &projectPath, const ScanOptions & /*opt*/) {
    const QStringList files = lsFiles(
        projectPath, {QStringLiteral("*.cpp"), QStringLiteral("*.h")});
    QList<Finding> out;
    for (const QString &rel : files) {
        // This engine's own source *names* the obsolete idioms as data
        // (the obsoleteQtIdioms() table + its anchored regex literals).
        // Those are definitions, not occurrences-in-use, so excluding
        // the engine file is the correct semantics — not a silenced
        // finding. No other detector has this definitional self-match.
        if (rel.endsWith(QStringLiteral("src/debtsweepengine.cpp"))
            || rel.endsWith(QStringLiteral("src/debtsweepengine.h")))
            continue;
        const QString body = slurpUtf8(projectPath + QChar('/') + rel);
        if (body.isEmpty()) continue;
        out += detail::scanObsoleteQStringIdioms(rel, body);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Code drift (h) — statement after unconditional control-flow exit
// ---------------------------------------------------------------------------

namespace detail {

QList<Finding> scanDeadBranchAfterReturn(const QString &relPath,
                                         const QString &body) {
    // The whole line (after stripping a trailing // comment) is solely
    // an unconditional exit. Conditional forms (`if (x) return;`) carry
    // a leading keyword and won't match.
    static const QRegularExpression kExitRe(
        QStringLiteral(R"(^(return(\s+[^;]*)?;|break;|continue;|throw(\s+[^;]*)?;)$)"));

    const QStringList lines = body.split('\n');
    // Strip a trailing line-comment conservatively (skips when a quote
    // precedes the //, to avoid butchering string content).
    auto stripComment = [](const QString &s) -> QString {
        const int q = s.indexOf(QChar('"'));
        const int c = s.indexOf(QStringLiteral("//"));
        if (c >= 0 && (q < 0 || c < q)) return s.left(c);
        return s;
    };
    auto isCommentOrBlank = [](const QString &t) -> bool {
        return t.isEmpty() || t.startsWith(QStringLiteral("//"))
               || t.startsWith(QStringLiteral("/*"))
               || t.startsWith(QStringLiteral("*"));
    };
    // A structural token that legitimately follows an exit (block close,
    // switch label, preprocessor, else, continuation).
    static const QRegularExpression kLabelOnlyRe(
        QStringLiteral(R"(^[A-Za-z_]\w*\s*:\s*$)"));
    auto isStructural = [&](const QString &t) -> bool {
        if (t.startsWith(QChar('}')) || t.startsWith(QChar('#'))
            || t.startsWith(QChar(')')) || t.startsWith(QChar(','))
            || t.startsWith(QStringLiteral("*/"))
            || t.startsWith(QStringLiteral("else"))
            || t.startsWith(QStringLiteral("case"))
            || t.startsWith(QStringLiteral("default")))
            return true;
        // A bare `label:` (goto target) — but not `foo::bar` (note ::).
        if (kLabelOnlyRe.match(t).hasMatch()
            && !t.contains(QStringLiteral("::")))
            return true;
        return false;
    };

    QList<Finding> out;
    for (int i = 0; i < lines.size(); ++i) {
        const QString exitLine = stripComment(lines.at(i)).trimmed();
        if (!kExitRe.match(exitLine).hasMatch()) continue;
        // The exit may be the brace-less body of a preceding control-flow
        // header (`if (..)\n  return;`): the following statement then runs
        // when the branch is not taken, so it is NOT dead. Detect via the
        // previous non-blank/non-comment line — a header ends in `)` (an
        // `if`/`for`/`while` condition, possibly multi-line) or is `else`/`do`.
        {
            int p = i - 1;
            while (p >= 0
                   && isCommentOrBlank(stripComment(lines.at(p)).trimmed()))
                --p;
            if (p >= 0) {
                const QString prev = stripComment(lines.at(p)).trimmed();
                if (prev.endsWith(QChar(')'))
                    || prev == QStringLiteral("else")
                    || prev.endsWith(QStringLiteral("else"))
                    || prev == QStringLiteral("do"))
                    continue;
            }
        }
        // Find the next non-blank, non-comment line.
        int j = i + 1;
        while (j < lines.size() && isCommentOrBlank(lines.at(j).trimmed()))
            ++j;
        if (j >= lines.size()) continue;
        const QString next = lines.at(j).trimmed();
        if (isStructural(next)) continue;
        Finding fnd;
        fnd.category   = QStringLiteral("code_drift");
        fnd.detectorId = QStringLiteral("dead_branch_after_return");
        fnd.file       = relPath;
        fnd.line       = j + 1;
        QString preview = next;
        if (preview.size() > 80) preview = preview.left(77) + QStringLiteral("...");
        fnd.message    = QStringLiteral(
            "statement after `%1` is unreachable: %2")
                             .arg(exitLine, preview);
        out.append(fnd);
    }
    return out;
}

}  // namespace detail

QList<Finding> detectDeadBranchAfterReturn(
    const QString &projectPath, const ScanOptions & /*opt*/) {
    const QStringList files = lsFiles(
        projectPath, {QStringLiteral("*.cpp"), QStringLiteral("*.h")});
    QList<Finding> out;
    for (const QString &rel : files) {
        const QString body = slurpUtf8(projectPath + QChar('/') + rel);
        if (body.isEmpty()) continue;
        out += detail::scanDeadBranchAfterReturn(rel, body);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Test coverage gap — INV-N markers without test references
// ---------------------------------------------------------------------------

QList<Finding> detectMissingInvariantTests(
    const QString &projectPath, const ScanOptions & /*opt*/) {
    const QString featuresRoot =
        projectPath + QStringLiteral("/tests/features");
    QDir d(featuresRoot);
    if (!d.exists()) return {};

    static const QRegularExpression kInvRe(
        QStringLiteral(R"(\bINV-([0-9][0-9a-zA-Z]*)\b)"));

    QList<Finding> out;
    const QFileInfoList subdirs = d.entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &sub : subdirs) {
        const QString specPath = sub.filePath() + QStringLiteral("/spec.md");
        if (!QFileInfo::exists(specPath)) continue;
        const QString specText = slurpUtf8(specPath);
        if (specText.isEmpty()) continue;

        // Collect (id → first line in spec).
        QStringList ids;
        QList<int> lineOf;
        QSet<QString> seen;
        const QStringList specLines = specText.split('\n');
        for (int i = 0; i < specLines.size(); ++i) {
            auto it = kInvRe.globalMatch(specLines.at(i));
            while (it.hasNext()) {
                const auto m = it.next();
                const QString id = m.captured(1);
                if (seen.contains(id)) continue;
                seen.insert(id);
                ids << id;
                lineOf << (i + 1);
            }
        }
        if (ids.isEmpty()) continue;

        // Concatenate all test_* files in the dir.
        QString testsBlob;
        QDir tdir(sub.filePath());
        const QStringList testGlobs = {
            QStringLiteral("test_*.cpp"), QStringLiteral("test_*.py"),
            QStringLiteral("test_*.js"),  QStringLiteral("test_*.go"),
            QStringLiteral("test_*.rs"),  QStringLiteral("test_*.sh"),
            QStringLiteral("test_*.bash"),
        };
        const QFileInfoList testFiles =
            tdir.entryInfoList(testGlobs,
                               QDir::Files | QDir::NoDotAndDotDot);
        for (const QFileInfo &tf : testFiles) {
            testsBlob += slurpUtf8(tf.filePath());
            testsBlob += QChar('\n');
        }

        const QString relSpec =
            QDir(projectPath).relativeFilePath(specPath);
        for (int i = 0; i < ids.size(); ++i) {
            const QString needle = QStringLiteral("INV-") + ids.at(i);
            if (testsBlob.contains(needle)) continue;
            Finding fnd;
            fnd.category    = QStringLiteral("test_coverage");
            fnd.detectorId  = QStringLiteral("missing_inv_test");
            fnd.file        = relSpec;
            fnd.line        = lineOf.at(i);
            fnd.message     = QStringLiteral(
                "INV-%1 declared in spec.md but no test_*.{cpp,py,js,go,rs,sh,bash} in this dir mentions it"
            ).arg(ids.at(i));
            out.append(fnd);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Doc drift (a) — ROADMAP ✅ items without commit subject mention
// ---------------------------------------------------------------------------

QList<Finding> detectRoadmapShippedWithoutCommit(
    const QString &projectPath, const ScanOptions & /*opt*/) {
    const QString roadmapPath = projectPath + QStringLiteral("/ROADMAP.md");
    if (!QFileInfo::exists(roadmapPath)) return {};
    const QString body = slurpUtf8(roadmapPath);
    if (body.isEmpty()) return {};

    // U+2705 ✅ embedded directly; QString literal is UTF-16 so the
    // emoji is one code-point match. (Earlier UTF-8-byte variant
    // `kShippedRe` was dead — removed.)
    static const QRegularExpression kShippedReUtf16(
        QStringLiteral("^- ✅ \\[(ANTS-\\d+)\\]"));

    QList<QPair<QString, int>> ids;
    const QStringList lines = body.split('\n');
    for (int i = 0; i < lines.size(); ++i) {
        const auto m = kShippedReUtf16.match(lines.at(i));
        if (m.hasMatch()) {
            ids.append({m.captured(1), i + 1});
        }
    }
    if (ids.isEmpty()) return {};

    const QString log = runGit(
        projectPath,
        {QStringLiteral("log"), QStringLiteral("--all"),
         QStringLiteral("--format=%s")},
        kGitLogCap);
    if (log.isEmpty()) return {};  // not a git checkout

    QList<Finding> out;
    for (const auto &p : std::as_const(ids)) {
        if (log.contains(p.first)) continue;
        Finding fnd;
        fnd.category    = QStringLiteral("doc_drift");
        fnd.detectorId  = QStringLiteral("shipped_without_commit");
        fnd.file        = QStringLiteral("ROADMAP.md");
        fnd.line        = p.second;
        fnd.message     = QStringLiteral(
            "%1 is ✅ in ROADMAP but no commit subject mentions it"
        ).arg(p.first);
        out.append(fnd);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Doc drift (b) — CHANGELOG [Unreleased] bullets citing files not in diff
// ---------------------------------------------------------------------------

QList<Finding> detectChangelogStaleBullets(
    const QString &projectPath, const ScanOptions &opt) {
    const QString clogPath = projectPath + QStringLiteral("/CHANGELOG.md");
    if (!QFileInfo::exists(clogPath)) return {};
    const QString body = slurpUtf8(clogPath);
    if (body.isEmpty()) return {};

    // Find [Unreleased] section. If absent → silent self-disable.
    const QStringList lines = body.split('\n');
    int start = -1, end = lines.size();
    for (int i = 0; i < lines.size(); ++i) {
        if (lines.at(i).startsWith(QStringLiteral("## [Unreleased]"))) {
            start = i;
        } else if (start >= 0 && lines.at(i).startsWith(QStringLiteral("## "))) {
            end = i;
            break;
        }
    }
    if (start < 0) return {};

    // Resolve diff scope.
    const QString since = resolveSinceRef(projectPath, opt.sinceRef);
    const QStringList diffPaths = diffedFiles(projectPath, since, {});
    QSet<QString> diffSet(diffPaths.begin(), diffPaths.end());

    static const QRegularExpression kFilePathInBacktick(
        QStringLiteral(R"(`([a-zA-Z0-9_./-]+\.(?:cpp|h|md|json|yml|yaml|py|sh))`)"));

    QList<Finding> out;
    for (int i = start + 1; i < end; ++i) {
        const QString s = lines.at(i).trimmed();
        if (!s.startsWith(QStringLiteral("- "))) continue;
        QStringList cited;
        auto it = kFilePathInBacktick.globalMatch(s);
        while (it.hasNext()) {
            const auto m = it.next();
            cited << m.captured(1);
        }
        if (cited.isEmpty()) continue;
        bool any = false;
        QString flagged;
        for (const QString &c : std::as_const(cited)) {
            if (diffSet.contains(c)) { any = true; break; }
            if (flagged.isEmpty()) flagged = c;
        }
        if (any) continue;
        Finding fnd;
        fnd.category    = QStringLiteral("doc_drift");
        fnd.detectorId  = QStringLiteral("stale_changelog_bullet");
        fnd.file        = QStringLiteral("CHANGELOG.md");
        fnd.line        = i + 1;
        fnd.message     = QStringLiteral(
            "[Unreleased] bullet cites `%1` but it's not in git diff %2..HEAD"
        ).arg(flagged, since);
        out.append(fnd);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Packaging drift — wraps packaging/check-version-drift.sh
// ---------------------------------------------------------------------------

QList<Finding> runPackagingDrift(
    const QString &projectPath, const ScanOptions & /*opt*/) {
    const QString script =
        projectPath + QStringLiteral("/packaging/check-version-drift.sh");
    QFileInfo fi(script);
    if (!fi.exists() || !fi.isFile() || !fi.isExecutable()) return {};

    QProcess p;
    p.setWorkingDirectory(projectPath);
    p.setProcessChannelMode(QProcess::SeparateChannels);
    p.start(QStringLiteral("bash"), {QStringLiteral("packaging/check-version-drift.sh")});
    if (!p.waitForStarted(2000)) return {};
    if (!p.waitForFinished(kPackagingScriptTimeoutMs)) {
        p.kill();
        return {};
    }
    QByteArray out = p.readAllStandardOutput();
    if (out.size() > kGitStdoutCap) out.truncate(kGitStdoutCap);
    const QString s = QString::fromUtf8(out);

    static const QRegularExpression kDriftLine(
        QStringLiteral(R"(^([^:]+):(\d+):\s*(.*)$)"));
    QList<Finding> rows;
    const QStringList lines = s.split('\n', Qt::SkipEmptyParts);
    for (const QString &l : lines) {
        const auto m = kDriftLine.match(l);
        if (!m.hasMatch()) continue;
        Finding fnd;
        fnd.category    = QStringLiteral("packaging_drift");
        fnd.detectorId  = QStringLiteral("version_drift");
        fnd.file        = m.captured(1);
        fnd.line        = m.captured(2).toInt();
        fnd.message     = m.captured(3).trimmed();
        rows.append(fnd);
    }
    return rows;
}

// ---------------------------------------------------------------------------
// scanAll
// ---------------------------------------------------------------------------

QList<Finding> scanAll(
    const QString &projectPath, const ScanOptions &opt) {
    QList<Finding> out;
    if (opt.includeCodeDrift) {
        out += detectStaleTypeComments(projectPath, opt);
        out += detectAddedTodos(projectPath, opt);
        out += detectOrphanQUnused(projectPath, opt);
        out += detectStaleTodos(projectPath, opt);
        out += detectDuplicateIncludes(projectPath, opt);
        out += detectObsoleteQStringIdioms(projectPath, opt);
        out += detectDeadBranchAfterReturn(projectPath, opt);
    }
    if (opt.includeTestCoverage) {
        out += detectMissingInvariantTests(projectPath, opt);
    }
    if (opt.includeDocDrift) {
        out += detectRoadmapShippedWithoutCommit(projectPath, opt);
        out += detectChangelogStaleBullets(projectPath, opt);
    }
    if (opt.includePackagingDrift) {
        out += runPackagingDrift(projectPath, opt);
    }
    return out;
}

// ---------------------------------------------------------------------------
// applyMechanicalFix — orphan_q_unused only in v1
// ---------------------------------------------------------------------------

ApplyVerdict applyMechanicalFix(
    const QString &projectPath, const Finding &finding, bool dryRun) {
    ApplyVerdict v;
    if (!finding.autoFixable) {
        v.errorCode = QStringLiteral("not_fixable");
        v.errorMessage = QStringLiteral(
            "detector did not flag this finding as auto-fixable");
        return v;
    }
    // Auto-fix table: the detectors whose fix is byte-deterministic.
    // stale_todo / dead_branch_after_return are flag-only (no entry).
    static const QSet<QString> kFixable = {
        QStringLiteral("orphan_q_unused"),
        QStringLiteral("duplicate_include"),
        QStringLiteral("obsolete_qstring_idiom"),
    };
    if (!kFixable.contains(finding.detectorId)) {
        v.errorCode = QStringLiteral("not_fixable");
        v.errorMessage = QStringLiteral(
            "no fix table entry for detector '%1'").arg(finding.detectorId);
        return v;
    }

    const QString abs = projectPath + QChar('/') + finding.file;
    QFile in(abs);
    if (!in.open(QIODevice::ReadOnly)) {
        v.errorCode = QStringLiteral("io_error");
        v.errorMessage = in.errorString();
        return v;
    }
    const QByteArray raw = in.readAll();
    in.close();
    const QString body = QString::fromUtf8(raw);
    const QFile::Permissions origPerms = QFileInfo(abs).permissions();

    QStringList kept = body.split('\n');
    if (finding.line < 1 || finding.line > kept.size()) {
        v.errorCode = QStringLiteral("file_changed");
        v.errorMessage = QStringLiteral(
            "marker no longer on line %1; re-scan required").arg(finding.line);
        return v;
    }
    const int idx = finding.line - 1;
    const QString &target = kept.at(idx);

    auto staleVerdict = [&]() {
        v.errorCode = QStringLiteral("file_changed");
        v.errorMessage = QStringLiteral(
            "marker no longer on line %1; re-scan required").arg(finding.line);
    };

    if (finding.detectorId == QStringLiteral("orphan_q_unused")) {
        static const QRegularExpression kMarkerRe(
            QStringLiteral(R"(Q_UNUSED\(\s*[A-Za-z_][A-Za-z0-9_]*\s*\)|\(void\)\s*[A-Za-z_][A-Za-z0-9_]*\s*;)"));
        if (!kMarkerRe.match(target).hasMatch()) { staleVerdict(); return v; }
        kept.removeAt(idx);
    } else if (finding.detectorId == QStringLiteral("duplicate_include")) {
        static const QRegularExpression kIncludeRe(
            QStringLiteral(R"(^\s*#\s*include\s+(["<][^">]*[">]))"));
        const auto m = kIncludeRe.match(target);
        if (!m.hasMatch()) { staleVerdict(); return v; }
        // Confirm the include is still a duplicate (an earlier line
        // includes the same header) before deleting.
        const QString key = m.captured(1);
        bool earlierDup = false;
        for (int i = 0; i < idx; ++i) {
            const auto e = kIncludeRe.match(kept.at(i));
            if (e.hasMatch() && e.captured(1) == key) { earlierDup = true; break; }
        }
        if (!earlierDup) { staleVerdict(); return v; }
        kept.removeAt(idx);
    } else {  // obsolete_qstring_idiom
        QString rewritten = target;
        bool any = false;
        for (const QtIdiom &idiom : obsoleteQtIdioms()) {
            if (idiom.find.match(rewritten).hasMatch()) {
                rewritten.replace(idiom.find, idiom.replace);
                any = true;
            }
        }
        if (!any) { staleVerdict(); return v; }
        kept[idx] = rewritten;
    }

    const QByteArray newBody = kept.join(QChar('\n')).toUtf8();

    // ANTS-2227 — dry_run: the patch passed every guard and a patched
    // body was computed; report it would apply but skip the write. Shares
    // the full validate-and-patch path above so the preview can't drift
    // from the real fix.
    if (dryRun) {
        v.wouldApply = true;
        return v;
    }

    QSaveFile out(abs);
    if (!out.open(QIODevice::WriteOnly)) {
        v.errorCode = QStringLiteral("io_error");
        v.errorMessage = out.errorString();
        return v;
    }
    if (out.write(newBody) != newBody.size()) {
        v.errorCode = QStringLiteral("io_error");
        v.errorMessage = out.errorString();
        return v;
    }
    if (!out.commit()) {
        v.errorCode = QStringLiteral("io_error");
        v.errorMessage = out.errorString();
        return v;
    }
    QFile::setPermissions(abs, origPerms);

    v.applied = true;
    return v;
}

// ---------------------------------------------------------------------------
// templateDebtSweepFoldInBlock
// ---------------------------------------------------------------------------

QString templateDebtSweepFoldInBlock(
    const QList<Finding> &deferred,
    const QList<int> &allocatedIds,
    const QString &dateIso,
    const QString &idPrefix) {
    if (deferred.isEmpty() || deferred.size() != allocatedIds.size()) {
        return {};
    }

    QString out;
    out.reserve(256 + deferred.size() * 200);
    out += QStringLiteral("### 🧹 Debt-sweep fold-in (");
    out += dateIso;
    out += QStringLiteral(")\n\n");

    for (int i = 0; i < deferred.size(); ++i) {
        const Finding &f = deferred.at(i);
        const int id = allocatedIds.at(i);
        out += QStringLiteral("- 📋 [");
        out += RoadmapFoldIn::renderId(idPrefix, id);
        out += QStringLiteral("] **");
        out += f.message;
        out += QStringLiteral("** at ");
        out += f.file;
        if (f.line > 0) {
            out += QChar(':');
            out += QString::number(f.line);
        }
        out += QStringLiteral(".\n");
        out += QStringLiteral("  Kind: chore.\n");
        out += QStringLiteral("  Source: debt-sweep-");
        out += dateIso;
        out += QStringLiteral(".\n");
        if (i + 1 < deferred.size()) out += QChar('\n');
    }
    return out;
}

// ---------------------------------------------------------------------------
// triagePrompt
// ---------------------------------------------------------------------------

QString triagePrompt(const QList<Finding> &llmShaped) {
    QString out;
    out.reserve(2048);
    out += QStringLiteral(
        "You are triaging mechanical findings from a debt-sweep run.\n"
        "The mechanical scanners flagged ");
    out += QString::number(llmShaped.size());
    out += QStringLiteral(
        " potential issues that need\n"
        "human judgment. For each, decide one of:\n\n"
        "  KEEP   — finding is real; suggest a fix\n"
        "  DROP   — false positive; explain why\n"
        "  DEFER  — real but not worth fixing now\n\n"
        "=== Findings ===\n\n");
    for (const Finding &f : llmShaped) {
        out += QChar('[');
        out += f.category;
        out += QStringLiteral(" / ");
        out += f.detectorId;
        out += QStringLiteral("] ");
        out += f.file;
        if (f.line > 0) {
            out += QChar(':');
            out += QString::number(f.line);
        }
        out += QChar('\n');
        out += f.message;
        out += QStringLiteral("\n\n");
    }
    out += QStringLiteral(
        "=== Per-category guidance ===\n\n"
        "- stale_type_comment: is the cited identifier merely a description\n"
        "  in prose, or genuinely a stale code reference?\n"
        "- added_todo: TODOs added in this scope — are they tracked elsewhere\n"
        "  (issue tracker, ROADMAP), or floating?\n"
        "- missing_inv_test: was the INV intentionally untested (judgment-\n"
        "  only invariant), or is this a real coverage gap?\n"
        "- shipped_without_commit: was the commit message truncated, the\n"
        "  ID renamed, or the ROADMAP item flipped prematurely?\n"
        "- stale_changelog_bullet: was the bullet for an earlier baseline,\n"
        "  or does the diff actually not match?\n\n"
        "Format: markdown. <= 500 words.\n");
    return out;
}

// ANTS-3346 — see header. The gate fires on the TOTAL deferred count, not
// composition: a raw scan is a mix of auto-fixable, FP-prone, and real
// findings, and deferring the whole lot writes every one as a tracked ROADMAP
// item. Auto-fixability is NOT a safety signal — a fixture's deliberate
// `Q_UNUSED(w)` in good.cpp is a false positive yet auto-fixable — so bulk
// size is the honest measure of "raw scan output dumped un-reviewed" (the
// class that once folded 1106 findings into ROADMAP.md). `nonAutoFixable` is
// carried for the caller's message only. `triaged=true` is the reviewed-this-
// batch assertion and always passes.
TriageGateVerdict evaluateTriageGate(const QList<Finding> &deferred,
                                     bool triaged) {
    TriageGateVerdict v;
    v.total     = deferred.size();
    v.threshold = kBulkDeferTriageThreshold;
    for (const auto &f : deferred) {
        if (!f.autoFixable) ++v.nonAutoFixable;
    }
    if (triaged) return v;   // reviewed: allowed regardless of size
    if (v.total > v.threshold) {
        v.allowed = false;
        v.reason = QStringLiteral(
            "%1 findings exceed the un-triaged bulk-defer threshold of %2 "
            "(%3 of them judgment-required). A raw scan is a mix of real, "
            "FP-prone, and mechanical findings — review it (Triage with AI) "
            "and pass triaged:true, or defer a reviewed subset, rather than "
            "folding the whole scan into ROADMAP.")
            .arg(v.total).arg(v.threshold).arg(v.nonAutoFixable);
    }
    return v;
}

}  // namespace DebtSweepEngine
