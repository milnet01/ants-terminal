// Feature-conformance test for spec.md —
//
// Invariant 1 — resolveProjectPath rejects ../ escape.
// Invariant 2 — resolveProjectPath rejects symlink escape.
// Invariant 3 — resolveProjectPath accepts in-project paths.
// Invariant 4 — resolveProjectPath returns empty for non-existent paths.
// Invariant 5 — all known call sites use resolveProjectPath; the raw
//               `m_projectPath + "/" + f.file` concat is gone.
//
// The behavioural checks call the production resolver,
// AuditEngine::resolveProjectPath, which AuditDialog::resolveProjectPath
// wraps. The source checks lock that every AuditDialog call site goes
// through the helper (no raw concat).

#include "../../_support/expect.h"
#include "auditengine.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QString>
#include <QTemporaryDir>

#include <cstdio>
#include <unistd.h>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

ANTS_TEST_SCOPE();

namespace {



QString resolve(const QString &projectPath, const QString &maybeRelative) {
    return AuditEngine::resolveProjectPath(maybeRelative, projectPath);
}

void runBehavioralChecks() {
    // Build a real on-disk layout:
    //   <temp>/audit-traversal-UUID/
    //   ├── project/
    //   │   ├── src/good.cpp
    //   │   └── link        → ../outside
    //   └── outside/
    //       └── sensitive.txt
    QTemporaryDir tmpRoot(QDir::tempPath()
                          + QStringLiteral("/audit-traversal-XXXXXX"));
    if (!tmpRoot.isValid()) {
        std::fprintf(stderr, "[FAIL] setup: could not create temp root\n");
        expect(false, "setup-error", "");
        return;
    }
    // RAII: tmpRoot's destructor recursively reclaims the tree at scope
    // exit — including on the setup-error early returns below, which the
    // old manual-rmdir teardown leaked. QDir::removeRecursively unlinks
    // symlinks rather than following them (ANTS-1602).
    const QString root = tmpRoot.path();

    const QString project = root + QStringLiteral("/project");
    const QString projectSrc = project + QStringLiteral("/src");
    const QString outsideDir = root + QStringLiteral("/outside");

    QDir().mkpath(projectSrc);
    QDir().mkpath(outsideDir);

    QFile good(projectSrc + QStringLiteral("/good.cpp"));
    if (!good.open(QIODevice::WriteOnly)) {
        std::fprintf(stderr, "[FAIL] setup: could not create good.cpp\n");
        expect(false, "setup-error", "");
        return;
    }
    good.write("int main() { return 0; }\n");
    good.close();

    QFile sensitive(outsideDir + QStringLiteral("/sensitive.txt"));
    if (!sensitive.open(QIODevice::WriteOnly)) {
        std::fprintf(stderr, "[FAIL] setup: could not create sensitive.txt\n");
        expect(false, "setup-error", "");
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
        expect(false, "setup-error", "");
    }

    // Invariant 3 — in-project path resolves to canonical absolute.
    const QString goodResolved =
        resolve(project, QStringLiteral("src/good.cpp"));
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
        resolve(project, QStringLiteral("../outside/sensitive.txt"));
    expect(escapeLiteral.isEmpty(),
           "I1/literal-dotdot-escape-rejected",
           QStringLiteral("expected empty, got '%1'").arg(escapeLiteral));

    // Invariant 2 — symlink target points outside the project.
    const QString escapeSymlink =
        resolve(project, QStringLiteral("link/sensitive.txt"));
    expect(escapeSymlink.isEmpty(),
           "I2/symlink-escape-rejected",
           QStringLiteral("expected empty, got '%1'").arg(escapeSymlink));

    // Invariant 4 — non-existent file returns empty.
    const QString ghost =
        resolve(project, QStringLiteral("nonexistent/file.txt"));
    expect(ghost.isEmpty(),
           "I4/nonexistent-returns-empty");

    // Additional corner: the project root itself resolves back to itself.
    const QString selfResolve = resolve(project, QStringLiteral("."));
    expect(!selfResolve.isEmpty() && selfResolve == canonProject,
           "I3b/project-root-self-resolve");

    // Additional corner: empty maybeRelative returns empty.
    const QString empty = resolve(project, QString());
    expect(empty.isEmpty(),
           "I4b/empty-input-returns-empty");

    // Teardown: unlink the boundary-crossing symlink *before* tmpRoot's
    // RAII recursive-remove runs, so the sweep never has a live cross-
    // project link in the tree (defence-in-depth — removeRecursively
    // already unlinks symlinks rather than following them). Everything
    // else under `root` is reclaimed by tmpRoot's destructor at scope exit.
    QFile::remove(QString::fromLocal8Bit(linkPath));
}

void runSourceChecks() {
    const QString src = QString::fromStdString(ants_test::slurpAuditDialog());
    if (src.isEmpty()) {
        std::fprintf(stderr,
                     "[FAIL] I5/source-open: cannot read the AuditDialog sources\n");
        expect(false, "setup-error", "");
        return;
    }

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
           QStringLiteral("expected ≥6 occurrences, found %1 in the AuditDialog sources")
               .arg(calls));

    // Invariant 5b — the unsafe raw-concat pattern is gone.
    //   The literal `m_projectPath + "/" + f.file` was the pre-fix shape
    //   at 5 call sites (enrichment, dropFindingsInCommentsOrStrings,
    //   inlineSuppressed, single- + batch-AI-triage snippet fallbacks).
    //   Any reappearance is a regression.
    expect(!src.contains(
               QStringLiteral("m_projectPath + \"/\" + f.file")),
           "I5b/raw-concat-removed",
           QStringLiteral("`m_projectPath + \"/\" + f.file` must not "
                          "appear in the AuditDialog sources — use "
                          "resolveProjectPath() instead"));

    // Invariant 5c — the regex-captured-relPath concat is also gone.
    //   dropIfContextContains read the file via
    //   `m_projectPath + "/" + relPath`; that call site must also use
    //   the helper now.
    expect(!src.contains(
               QStringLiteral("m_projectPath + \"/\" + relPath")),
           "I5c/context-filter-raw-concat-removed");

}

TEST(AuditPathTraversal, BehavioralChecks) {
    const int before = expect_failures();
    runBehavioralChecks();
    if (expect_failures() > before) FAIL();
}

TEST(AuditPathTraversal, SourceChecks) {
    const int before = expect_failures();
    runSourceChecks();
    if (expect_failures() > before) FAIL();
}

}  // namespace
