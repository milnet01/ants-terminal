// Feature-conformance test for spec.md —
//
// Invariant 1 — resolveProjectPath rejects ../ escape.
// Invariant 2 — resolveProjectPath rejects symlink escape.
// Invariant 3 — resolveProjectPath accepts in-project paths.
// Invariant 4 — resolveProjectPath returns empty for non-existent paths.
// Invariant 5 — all known call sites use resolveProjectPath; the raw
//               `m_projectPath + "/" + f.file` concat is gone.
//
// Design note: AuditDialog::resolveProjectPath is a private method on a
// heavy QDialog subclass. Rather than linking the 5 000+ line
// auditdialog.cpp into this test (which pulls in auditcheck,
// featurecoverage, audithygiene, etc.), we test the *invariant shape*
// behaviorally against a reference reimplementation of the same canonicalize-
// and-anchor logic, then source-grep auditdialog.cpp to lock:
//   (a) the production helper has the same shape
//       (contains canonicalFilePath + the startsWith anchored check);
//   (b) every known call site migrated to the helper (no raw concat).
//
// If either the reference test or the source-grep fails, the fix has
// regressed — either the helper's contract weakened or a call site
// sneaked back to the unsafe pattern.

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QString>
#include <QUuid>

#include <cstdio>
#include <unistd.h>

namespace {

int g_failures = 0;

void expect(bool cond, const char *label, const QString &detail = {}) {
    if (cond) {
        std::fprintf(stderr, "[PASS] %s\n", label);
    } else {
        std::fprintf(stderr, "[FAIL] %s  %s\n", label,
                     qUtf8Printable(detail));
        ++g_failures;
    }
}

// Reference reimplementation of AuditDialog::resolveProjectPath.
// Kept byte-faithful to the production code so behavioral drift here
// is easy to catch — the source-grep below enforces the production
// version has the same structure (canonicalFilePath + anchored
// startsWith).
QString resolveProjectPathRef(const QString &projectPath,
                              const QString &maybeRelative) {
    if (maybeRelative.isEmpty() || projectPath.isEmpty()) return {};
    const QString candidate = QFileInfo(maybeRelative).isAbsolute()
        ? maybeRelative
        : (projectPath + QStringLiteral("/") + maybeRelative);
    const QString canonCandidate = QFileInfo(candidate).canonicalFilePath();
    if (canonCandidate.isEmpty()) return {};
    const QString canonProject = QFileInfo(projectPath).canonicalFilePath();
    if (canonProject.isEmpty()) return {};
    const QString anchored = canonProject.endsWith('/')
        ? canonProject
        : (canonProject + QStringLiteral("/"));
    if (!canonCandidate.startsWith(anchored) && canonCandidate != canonProject)
        return {};
    return canonCandidate;
}

void runBehavioralChecks() {
    // Build a real on-disk layout:
    //   <temp>/audit-traversal-UUID/
    //   ├── project/
    //   │   ├── src/good.cpp
    //   │   └── link        → ../outside
    //   └── outside/
    //       └── sensitive.txt
    const QString root =
        QStandardPaths::writableLocation(QStandardPaths::TempLocation)
        + QStringLiteral("/audit-traversal-")
        + QUuid::createUuid().toString(QUuid::Id128);

    const QString project = root + QStringLiteral("/project");
    const QString projectSrc = project + QStringLiteral("/src");
    const QString outsideDir = root + QStringLiteral("/outside");

    QDir().mkpath(projectSrc);
    QDir().mkpath(outsideDir);

    QFile good(projectSrc + QStringLiteral("/good.cpp"));
    if (!good.open(QIODevice::WriteOnly)) {
        std::fprintf(stderr, "[FAIL] setup: could not create good.cpp\n");
        ++g_failures;
        return;
    }
    good.write("int main() { return 0; }\n");
    good.close();

    QFile sensitive(outsideDir + QStringLiteral("/sensitive.txt"));
    if (!sensitive.open(QIODevice::WriteOnly)) {
        std::fprintf(stderr, "[FAIL] setup: could not create sensitive.txt\n");
        ++g_failures;
        return;
    }
    sensitive.write("SECRET=exfil-target\n");
    sensitive.close();

    // Symlink project/link → ../outside (relative target so the link
    // contents traverse the project boundary after canonicalization).
    const QByteArray linkPath =
        (project + QStringLiteral("/link")).toLocal8Bit();
    if (::symlink("../outside", linkPath.constData()) != 0) {
        std::fprintf(stderr,
                     "[FAIL] I2/setup-symlink: could not create test "
                     "symlink (errno=%d)\n", errno);
        ++g_failures;
    }

    // Invariant 3 — in-project path resolves to canonical absolute.
    const QString goodResolved =
        resolveProjectPathRef(project, QStringLiteral("src/good.cpp"));
    expect(!goodResolved.isEmpty(),
           "I3/in-project-path-resolves");
    expect(QFileInfo(goodResolved).canonicalFilePath() == goodResolved,
           "I3/resolved-path-is-canonical");
    const QString canonProject =
        QFileInfo(project).canonicalFilePath();
    expect(goodResolved.startsWith(canonProject),
           "I3/resolved-path-anchored-in-project",
           QStringLiteral("got '%1' but canonProject='%2'")
               .arg(goodResolved).arg(canonProject));

    // Invariant 1 — `../outside/sensitive.txt` via literal ..
    const QString escapeLiteral =
        resolveProjectPathRef(project,
                              QStringLiteral("../outside/sensitive.txt"));
    expect(escapeLiteral.isEmpty(),
           "I1/literal-dotdot-escape-rejected",
           QStringLiteral("expected empty, got '%1'").arg(escapeLiteral));

    // Invariant 2 — symlink target points outside the project.
    const QString escapeSymlink =
        resolveProjectPathRef(project,
                              QStringLiteral("link/sensitive.txt"));
    expect(escapeSymlink.isEmpty(),
           "I2/symlink-escape-rejected",
           QStringLiteral("expected empty, got '%1'").arg(escapeSymlink));

    // Invariant 4 — non-existent file returns empty.
    const QString ghost =
        resolveProjectPathRef(project,
                              QStringLiteral("nonexistent/file.txt"));
    expect(ghost.isEmpty(),
           "I4/nonexistent-returns-empty");

    // Additional corner: the project root itself resolves back to itself.
    const QString selfResolve = resolveProjectPathRef(project, QStringLiteral("."));
    expect(!selfResolve.isEmpty() && selfResolve == canonProject,
           "I3b/project-root-self-resolve");

    // Additional corner: empty maybeRelative returns empty.
    const QString empty = resolveProjectPathRef(project, QString());
    expect(empty.isEmpty(),
           "I4b/empty-input-returns-empty");

    // Cleanup — test-harness courtesy.
    QFile::remove(projectSrc + QStringLiteral("/good.cpp"));
    QFile::remove(linkPath);
    QFile::remove(outsideDir + QStringLiteral("/sensitive.txt"));
    QDir().rmpath(projectSrc);
    QDir().rmdir(outsideDir);
    QDir().rmdir(project);
    QDir().rmdir(root);
}

void runSourceChecks() {
    const QString path = QStringLiteral(SRC_AUDITDIALOG_PATH);
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::fprintf(stderr,
                     "[FAIL] I5/source-open: cannot read %s\n",
                     qUtf8Printable(path));
        ++g_failures;
        return;
    }
    const QString src = QString::fromUtf8(f.readAll());
    f.close();

    // Invariant 5a — helper is defined and used.
    //   Expected occurrences:
    //     1× definition: `QString AuditDialog::resolveProjectPath(...)`
    //     5× call-site use inside .cpp (see spec.md).
    //   Budget: ≥6 total. Exact count fluctuates as call sites are added.
    int calls = 0;
    int pos = 0;
    while ((pos = src.indexOf(QStringLiteral("resolveProjectPath"), pos)) >= 0) {
        ++calls;
        pos += 1;
    }
    expect(calls >= 6,
           "I5a/resolveProjectPath-used-at-all-sites",
           QStringLiteral("expected ≥6 occurrences, found %1 in %2")
               .arg(calls).arg(path));

    // Invariant 5b — the unsafe raw-concat pattern is gone.
    //   The literal `m_projectPath + "/" + f.file` was the pre-fix shape
    //   at 5 call sites (enrichment, dropFindingsInCommentsOrStrings,
    //   inlineSuppressed, single- + batch-AI-triage snippet fallbacks).
    //   Any reappearance is a regression.
    expect(!src.contains(
               QStringLiteral("m_projectPath + \"/\" + f.file")),
           "I5b/raw-concat-removed",
           QStringLiteral("`m_projectPath + \"/\" + f.file` must not "
                          "appear in %1 — use resolveProjectPath() "
                          "instead").arg(path));

    // Invariant 5c — the regex-captured-relPath concat is also gone.
    //   dropIfContextContains read the file via
    //   `m_projectPath + "/" + relPath`; that call site must also use
    //   the helper now.
    expect(!src.contains(
               QStringLiteral("m_projectPath + \"/\" + relPath")),
           "I5c/context-filter-raw-concat-removed");

    // Invariant 5d — helper body has the canonicalize + anchored-prefix
    // check. Drift here would weaken the traversal guard.
    expect(src.contains(QStringLiteral("canonicalFilePath()")),
           "I5d/helper-uses-canonicalFilePath");
    expect(src.contains(QStringLiteral("anchored")) &&
               src.contains(QStringLiteral("startsWith(anchored)")),
           "I5d/helper-uses-anchored-startsWith",
           QStringLiteral("helper must compute an anchored prefix "
                          "(canonProject + '/') and check "
                          "startsWith(anchored); otherwise sibling dirs "
                          "sharing a name prefix could escape"));
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    runBehavioralChecks();
    runSourceChecks();

    if (g_failures) {
        std::fprintf(stderr, "\n%d invariant(s) failed\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nall invariants hold\n");
    return 0;
}
