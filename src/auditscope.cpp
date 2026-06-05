// ANTS-1504 — changed-file resolution for `audit_run` narrowing scopes.

#include "auditscope.h"

#include <QProcess>
#include <QSet>

namespace AuditScope {

namespace {

constexpr int kGitTimeoutMs = 1'500;  // matches AuditCache::runGit

// Run git, return trimmed stdout ({} on non-zero exit / timeout). Mirrors
// the AuditCache::runGit pattern (auditcache.cpp:39); duplicated rather than
// coupling auditscope to auditcache internals.
QString runGit(const QString &root, const QStringList &args) {
    QProcess p;
    p.setWorkingDirectory(root);
    p.setProcessChannelMode(QProcess::SeparateChannels);
    p.start(QStringLiteral("git"), args);
    if (!p.waitForStarted(kGitTimeoutMs)) return {};
    if (!p.waitForFinished(kGitTimeoutMs)) {
        p.kill();
        p.waitForFinished(500);
        return {};
    }
    if (p.exitCode() != 0) return {};
    return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
}

// Like runGit but WITHOUT trimming — `git status --porcelain` leads each
// line with a significant 2-column XY status, and a whole-output trim would
// eat the first line's leading space and mis-parse that entry.
QString runGitRaw(const QString &root, const QStringList &args) {
    QProcess p;
    p.setWorkingDirectory(root);
    p.setProcessChannelMode(QProcess::SeparateChannels);
    p.start(QStringLiteral("git"), args);
    if (!p.waitForStarted(kGitTimeoutMs)) return {};
    if (!p.waitForFinished(kGitTimeoutMs)) {
        p.kill();
        p.waitForFinished(500);
        return {};
    }
    if (p.exitCode() != 0) return {};
    return QString::fromUtf8(p.readAllStandardOutput());
}

// True iff git exits 0 (for predicate commands like `cat-file -e` that emit
// no stdout). runGit can't be used — empty stdout is ambiguous with failure.
bool runGitSucceeds(const QString &root, const QStringList &args) {
    QProcess p;
    p.setWorkingDirectory(root);
    p.setProcessChannelMode(QProcess::SeparateChannels);
    p.start(QStringLiteral("git"), args);
    if (!p.waitForStarted(kGitTimeoutMs)) return false;
    if (!p.waitForFinished(kGitTimeoutMs)) {
        p.kill();
        p.waitForFinished(500);
        return false;
    }
    return p.exitCode() == 0;
}

const QStringList &cppExts() {
    static const QStringList e = {QStringLiteral(".c"),   QStringLiteral(".cc"),
                                  QStringLiteral(".cpp"), QStringLiteral(".cxx"),
                                  QStringLiteral(".c++"), QStringLiteral(".h"),
                                  QStringLiteral(".hh"),  QStringLiteral(".hpp"),
                                  QStringLiteral(".hxx")};
    return e;
}
const QStringList &pyExts() {
    static const QStringList e = {QStringLiteral(".py"), QStringLiteral(".pyi")};
    return e;
}
const QStringList &shExts() {
    static const QStringList e = {QStringLiteral(".sh"), QStringLiteral(".bash")};
    return e;
}

bool hasExt(const QString &path, const QStringList &exts) {
    const QString lower = path.toLower();
    for (const QString &e : exts)
        if (lower.endsWith(e)) return true;
    return false;
}

}  // namespace

bool isFileScopedTool(const QString &tool) {
    return tool != QLatin1String("gitleaks") && tool != QLatin1String("trivy");
}

QStringList filterForTool(const QStringList &files, const QString &tool) {
    const QStringList *exts = nullptr;
    if (tool == QLatin1String("cppcheck") || tool == QLatin1String("clazy")
        || tool == QLatin1String("clang-tidy"))
        exts = &cppExts();
    else if (tool == QLatin1String("ruff") || tool == QLatin1String("bandit")
             || tool == QLatin1String("mypy"))
        exts = &pyExts();
    else if (tool == QLatin1String("shellcheck"))
        exts = &shExts();

    // semgrep + any unknown file-scoped tool self-select → take every file.
    if (!exts) return files;

    QStringList out;
    for (const QString &f : files)
        if (hasExt(f, *exts)) out.append(f);
    return out;
}

QStringList parseChangedFiles(const QString &diffNameOnly,
                              const QString &statusPorcelain,
                              bool includeWorkingTree) {
    QStringList out;
    QSet<QString> seen;
    auto add = [&](const QString &path) {
        if (path.isEmpty()) return;
        if (seen.contains(path)) return;
        seen.insert(path);
        out.append(path);
    };

    // `git diff --name-only` output: one project-relative path per line
    // (callers pass --diff-filter=ACMR, so deletions are already excluded).
    const auto diffLines = diffNameOnly.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &raw : diffLines) add(raw.trimmed());

    if (!includeWorkingTree) return out;

    // `git status --porcelain` v1 line: XY<space>path. Drop deletions
    // (X=='D' or Y=='D'); keep untracked ("?? path") and renames
    // ("R  old -> new" → take the new path).
    const auto statusLines = statusPorcelain.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : statusLines) {
        if (line.size() < 4) continue;
        const QChar x = line.at(0);
        const QChar y = line.at(1);
        if (x == QLatin1Char('D') || y == QLatin1Char('D')) continue;  // deleted
        QString path = line.mid(3).trimmed();
        const int arrow = path.indexOf(QStringLiteral(" -> "));
        if (arrow >= 0) path = path.mid(arrow + 4).trimmed();  // rename → new name
        add(path);
    }
    return out;
}

Resolution resolveChangedFiles(const QString &canonProject,
                               const QString &scope,
                               const QString &priorCommit) {
    Resolution res;

    // Non-narrowing scopes: leave narrowed=false → caller runs a full scan.
    if (scope.isEmpty() || scope == QLatin1String("auto")) return res;
    res.narrowed = true;

    // Every narrowing scope needs a git work tree.
    if (runGit(canonProject, {QStringLiteral("rev-parse"),
                              QStringLiteral("--is-inside-work-tree")})
            != QLatin1String("true")) {
        res.demotedReason = QStringLiteral("not_git");
        return res;
    }

    const QString diffFilter = QStringLiteral("--diff-filter=ACMR");
    QString base;             // commit/ref to diff HEAD against
    bool includeWorkTree = true;

    if (scope == QLatin1String("since-last-run")) {
        if (priorCommit.isEmpty()) {
            res.demotedReason = QStringLiteral("no_prior_run");
            return res;
        }
        if (priorCommit == QLatin1String("nogit")) {
            res.demotedReason = QStringLiteral("no_prior_commit");
            return res;
        }
        // Reachable in current history? (rebase / force-push drops it.)
        if (!runGitSucceeds(canonProject,
                {QStringLiteral("cat-file"), QStringLiteral("-e"),
                 priorCommit + QStringLiteral("^{commit}")})) {
            res.demotedReason = QStringLiteral("prior_commit_unreachable");
            return res;
        }
        base = priorCommit;
        res.anchorCommit = priorCommit;
    } else if (scope == QLatin1String("files")) {
        // merge-base with the remote default branch, then HEAD~1, then demote.
        const QString originHead = runGit(canonProject,
            {QStringLiteral("rev-parse"), QStringLiteral("--abbrev-ref"),
             QStringLiteral("origin/HEAD")});
        if (!originHead.isEmpty())
            base = runGit(canonProject,
                {QStringLiteral("merge-base"), QStringLiteral("HEAD"), originHead});
        if (base.isEmpty()) {
            base = runGit(canonProject,
                {QStringLiteral("rev-parse"), QStringLiteral("--verify"),
                 QStringLiteral("--quiet"), QStringLiteral("HEAD~1")});
        }
        if (base.isEmpty()) {
            res.demotedReason = QStringLiteral("no_merge_base");
            return res;
        }
        res.anchorCommit = base;
    } else if (scope == QLatin1String("branch-diff")) {
        base = QStringLiteral("main");
        includeWorkTree = false;  // committed-branch comparison only
    } else if (scope.startsWith(QLatin1String("since-tag:"))) {
        base = scope.mid(10);  // strip "since-tag:" (matches auditrunner.cpp:1102)
        res.anchorCommit = base;
    } else {
        // Unknown narrowing scope — treat as full scan rather than refuse.
        res.narrowed = false;
        return res;
    }

    const QString diff = runGit(canonProject,
        {QStringLiteral("diff"), QStringLiteral("--name-only"), diffFilter,
         base + QStringLiteral("..HEAD")});
    const QString status = includeWorkTree
        ? runGitRaw(canonProject, {QStringLiteral("status"),
                                   QStringLiteral("--porcelain")})
        : QString();

    res.files = parseChangedFiles(diff, status, includeWorkTree);
    res.noChanges = res.files.isEmpty();
    return res;
}

QStringList enumerateSourceFiles(const QString &canonProject) {
    // Impure (shells git) — lives below resolveChangedFiles so the
    // pure-helpers region stays git-free (audit_run_since_last_run INV-9).
    // `git ls-files` returns one project-relative path per line with no
    // leading whitespace, so the trimming runGit is correct here.
    const QString out = runGit(canonProject, {QStringLiteral("ls-files"),
                                              QStringLiteral("src/")});
    return out.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

}  // namespace AuditScope
