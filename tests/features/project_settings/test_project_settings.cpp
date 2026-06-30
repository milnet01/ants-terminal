// Feature-conformance test for the per-project .ants/project.json settings
// file (ANTS-2160). Behavioural invariants drive the pure
// ProjectSettings::load loader and the settings-aware CodebaseIndex /
// DocsIndex walks; wiring invariants source-scrape the consumers.
// See spec.md + docs/specs/ANTS-2160.md.

#include "../../_support/expect.h"
#include "projectsettings.h"
#include "codebaseindex.h"
#include "docsindex.h"

#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#ifndef ANTS_SOURCE_DIR
#error "ANTS_SOURCE_DIR compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {

bool has(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}
std::string srcPath(const char *rel) {
    return std::string(ANTS_SOURCE_DIR) + "/" + rel;
}

void writeFile(const QString &path, const QString &content) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(content.toUtf8());
    f.close();
}

QString cFile(const QString &fn) {
    return QStringLiteral("int %1(int n) { return n + 1; }\n").arg(fn);
}

// Canonical root (isInsideProject canonicalises the candidate, so the root
// must be canonical for a consistent in-root verdict).
QString canon(const QTemporaryDir &d) {
    return QFileInfo(d.path()).canonicalFilePath();
}

int countPath(const QVector<CodebaseIndex::FileEntry> &files, const QString &rel) {
    int n = 0;
    for (const auto &fe : files) if (fe.path == rel) ++n;
    return n;
}
bool hasPath(const QVector<CodebaseIndex::FileEntry> &files, const QString &rel) {
    return countPath(files, rel) > 0;
}

}  // namespace

// INV-1 — absent file → all-nullopt; src/-only build unchanged.
TEST(ProjectSettings, AbsentFileNoOverride) {
    QTemporaryDir dir;
    const QString root = canon(dir);
    writeFile(root + "/src/a.c", cFile("a"));

    ProjectSettings::Settings s = ProjectSettings::load(root);
    EXPECT_FALSE(s.sourceRoots.has_value());
    EXPECT_FALSE(s.testRoots.has_value());
    EXPECT_FALSE(s.docsDir.has_value());
    EXPECT_FALSE(s.roadmap.has_value());
    EXPECT_FALSE(s.changelog.has_value());
    EXPECT_FALSE(s.specsDir.has_value());

    CodebaseIndex::Index idx = CodebaseIndex::build(root, 1000);
    EXPECT_TRUE(hasPath(idx.files, QStringLiteral("src/a.c")));
}

// INV-2 — source_roots redirects the walk to a non-src layout.
TEST(ProjectSettings, SourceRootsNonSrcLayout) {
    QTemporaryDir dir;
    const QString root = canon(dir);
    writeFile(root + "/engine/foo.c", cFile("foo"));
    writeFile(root + "/.ants/project.json",
              QStringLiteral("{\"source_roots\":[\"engine\"]}"));

    ProjectSettings::Settings s = ProjectSettings::load(root);
    ASSERT_TRUE(s.sourceRoots.has_value());
    EXPECT_EQ(*s.sourceRoots, QStringList{QStringLiteral("engine")});

    CodebaseIndex::Index idx = CodebaseIndex::build(root, 1000);
    EXPECT_GT(idx.files.size(), 0);
    EXPECT_TRUE(hasPath(idx.files, QStringLiteral("engine/foo.c")));
}

// INV-3 — declared keys surface from load; docs_dir redirects DocsIndex.
TEST(ProjectSettings, DeclaredKeysSurfaceAndDocsDirRedirects) {
    QTemporaryDir dir;
    const QString root = canon(dir);
    writeFile(root + "/ROADMAP.md", QStringLiteral("# r\n"));
    writeFile(root + "/HISTORY.md", QStringLiteral("# c\n"));
    writeFile(root + "/documentation/guide.md", QStringLiteral("# Guide\n"));
    writeFile(root + "/design/specs/X.md", QStringLiteral("# X\n"));
    writeFile(root + "/.ants/project.json",
              QStringLiteral("{\"roadmap\":\"ROADMAP.md\","
                             "\"changelog\":\"HISTORY.md\","
                             "\"docs_dir\":\"documentation\","
                             "\"specs_dir\":\"design/specs\"}"));

    ProjectSettings::Settings s = ProjectSettings::load(root);
    EXPECT_EQ(s.roadmap.value_or(QString()), QStringLiteral("ROADMAP.md"));
    EXPECT_EQ(s.changelog.value_or(QString()), QStringLiteral("HISTORY.md"));
    EXPECT_EQ(s.docsDir.value_or(QString()), QStringLiteral("documentation"));
    EXPECT_EQ(s.specsDir.value_or(QString()), QStringLiteral("design/specs"));

    // docs_dir redirects the DocsIndex walk to the declared dir.
    DocsIndex::Index didx = DocsIndex::build(root, 1000);
    bool sawGuide = false;
    for (const auto &de : didx.docs)
        if (de.path == QStringLiteral("documentation/guide.md")) sawGuide = true;
    EXPECT_TRUE(sawGuide);
}

// INV-4 — root-escape dropped; mixed array keeps valid entries.
TEST(ProjectSettings, EscapeDroppedMixedArrayKept) {
    QTemporaryDir dir;
    const QString root = canon(dir);
    writeFile(root + "/engine/foo.c", cFile("foo"));

    writeFile(root + "/.ants/project.json",
              QStringLiteral("{\"roadmap\":\"../outside.md\","
                             "\"source_roots\":[\"engine\",\"/etc\"]}"));
    ProjectSettings::Settings s = ProjectSettings::load(root);
    EXPECT_FALSE(s.roadmap.has_value());               // escaping path dropped
    ASSERT_TRUE(s.sourceRoots.has_value());
    EXPECT_EQ(*s.sourceRoots, QStringList{QStringLiteral("engine")});  // /etc dropped
}

// INV-5 — non-existent dropped; file-typed source_root → fallback to src.
TEST(ProjectSettings, NonExistentDroppedFileTypedFallsBack) {
    QTemporaryDir dir;
    const QString root = canon(dir);
    writeFile(root + "/src/keep.c", cFile("keep"));
    writeFile(root + "/notadir.txt", QStringLiteral("x\n"));

    writeFile(root + "/.ants/project.json",
              QStringLiteral("{\"roadmap\":\"does/not/exist.md\","
                             "\"source_roots\":[\"notadir.txt\"]}"));
    ProjectSettings::Settings s = ProjectSettings::load(root);
    EXPECT_FALSE(s.roadmap.has_value());   // non-existent dropped at load

    // notadir.txt exists + in-root so load keeps it; candidates() rejects the
    // non-dir → source_roots effectively unset → fall back to src default.
    CodebaseIndex::Index idx = CodebaseIndex::build(root, 1000);
    EXPECT_TRUE(hasPath(idx.files, QStringLiteral("src/keep.c")));
}

// INV-6 — malformed / non-object / wrong-type / null → all-nullopt.
TEST(ProjectSettings, MalformedFailSafe) {
    auto allNull = [](const ProjectSettings::Settings &s) {
        return !s.sourceRoots && !s.testRoots && !s.docsDir &&
               !s.roadmap && !s.changelog && !s.specsDir;
    };
    {
        QTemporaryDir d; const QString r = canon(d);
        writeFile(r + "/.ants/project.json", QStringLiteral("{ truncated"));
        EXPECT_TRUE(allNull(ProjectSettings::load(r)));
    }
    {
        QTemporaryDir d; const QString r = canon(d);
        writeFile(r + "/.ants/project.json", QStringLiteral("[]"));  // non-object
        EXPECT_TRUE(allNull(ProjectSettings::load(r)));
    }
    {
        QTemporaryDir d; const QString r = canon(d);
        writeFile(r + "/.ants/project.json",
                  QStringLiteral("{\"source_roots\":\"src\"}"));  // string not array
        EXPECT_FALSE(ProjectSettings::load(r).sourceRoots.has_value());
    }
    {
        QTemporaryDir d; const QString r = canon(d);
        writeFile(r + "/.ants/project.json",
                  QStringLiteral("{\"roadmap\":null}"));  // null value
        EXPECT_FALSE(ProjectSettings::load(r).roadmap.has_value());
    }
}

// INV-7 — partial settings: only-present keys override.
TEST(ProjectSettings, PartialPerKeyIndependence) {
    QTemporaryDir dir;
    const QString root = canon(dir);
    writeFile(root + "/engine/foo.c", cFile("foo"));
    writeFile(root + "/.ants/project.json",
              QStringLiteral("{\"source_roots\":[\"engine\"]}"));
    ProjectSettings::Settings s = ProjectSettings::load(root);
    EXPECT_TRUE(s.sourceRoots.has_value());
    EXPECT_FALSE(s.roadmap.has_value());     // unset key stays nullopt
    EXPECT_FALSE(s.testRoots.has_value());
}

// INV-8 — case-mismatched declared path → treated as non-existent.
TEST(ProjectSettings, CaseSensitivity) {
    QTemporaryDir dir;
    const QString root = canon(dir);
    writeFile(root + "/ROADMAP.md", QStringLiteral("# r\n"));
    writeFile(root + "/.ants/project.json",
              QStringLiteral("{\"roadmap\":\"roadmap.md\"}"));  // wrong case
    EXPECT_FALSE(ProjectSettings::load(root).roadmap.has_value());
}

// INV-9 — nested overlapping source_roots → file de-duped (appears once).
TEST(ProjectSettings, NestedRootsDedup) {
    QTemporaryDir dir;
    const QString root = canon(dir);
    writeFile(root + "/a/b/foo.c", cFile("foo"));
    writeFile(root + "/.ants/project.json",
              QStringLiteral("{\"source_roots\":[\"a\",\"a/b\"]}"));
    CodebaseIndex::Index idx = CodebaseIndex::build(root, 1000);
    EXPECT_EQ(countPath(idx.files, QStringLiteral("a/b/foo.c")), 1);
}

// INV-10 — empty array / blank string → treated as absent (fall back).
TEST(ProjectSettings, EmptyArrayBlankStringAsAbsent) {
    QTemporaryDir dir;
    const QString root = canon(dir);
    writeFile(root + "/src/keep.c", cFile("keep"));
    writeFile(root + "/.ants/project.json",
              QStringLiteral("{\"source_roots\":[],\"docs_dir\":\"\"}"));
    ProjectSettings::Settings s = ProjectSettings::load(root);
    EXPECT_FALSE(s.sourceRoots.has_value());
    EXPECT_FALSE(s.docsDir.has_value());

    CodebaseIndex::Index idx = CodebaseIndex::build(root, 1000);
    EXPECT_TRUE(hasPath(idx.files, QStringLiteral("src/keep.c")));  // src default
}

// INV-11 — source_roots replaces src (not union); tests/ still walked.
TEST(ProjectSettings, ReplacementNotUnionTestsIndependent) {
    QTemporaryDir dir;
    const QString root = canon(dir);
    writeFile(root + "/a/x.c", cFile("x"));
    writeFile(root + "/src/y.c", cFile("y"));
    writeFile(root + "/tests/t.c", cFile("t"));
    writeFile(root + "/.ants/project.json",
              QStringLiteral("{\"source_roots\":[\"a\"]}"));
    CodebaseIndex::Index idx = CodebaseIndex::build(root, 1000);
    EXPECT_TRUE(hasPath(idx.files, QStringLiteral("a/x.c")));
    EXPECT_FALSE(hasPath(idx.files, QStringLiteral("src/y.c")));   // src not walked
    EXPECT_TRUE(hasPath(idx.files, QStringLiteral("tests/t.c")));  // tests default kept
}

// INV-12 — settings honoured on the warm refresh() path (no spurious churn).
TEST(ProjectSettings, RefreshPathHonoursSettings) {
    QTemporaryDir dir;
    const QString root = canon(dir);
    writeFile(root + "/a/x.c", cFile("x"));
    writeFile(root + "/src/y.c", cFile("y"));
    writeFile(root + "/.ants/project.json",
              QStringLiteral("{\"source_roots\":[\"a\"]}"));

    CodebaseIndex::Index prev = CodebaseIndex::build(root, 1000);
    ASSERT_TRUE(hasPath(prev.files, QStringLiteral("a/x.c")));
    ASSERT_FALSE(hasPath(prev.files, QStringLiteral("src/y.c")));

    int refreshed = -1;
    CodebaseIndex::Index cur =
        CodebaseIndex::refresh(prev, root, 2000,
                               CodebaseIndex::mapSourceMtimeMs(root),
                               CodebaseIndex::Options{}, &refreshed);
    EXPECT_EQ(refreshed, 0);  // unchanged tree → no spurious re-outline
    EXPECT_TRUE(hasPath(cur.files, QStringLiteral("a/x.c")));
    EXPECT_FALSE(hasPath(cur.files, QStringLiteral("src/y.c")));  // still scoped to a/
}

// INV-13 — ANTS-3357: op:detect discounts vendored / third-party trees so
// a bundled-dependency dir (DOOM ships SDL2 + Vulkan-Headers under
// `mingw-deps/`) is never ranked as the dominant source_root and never
// inflates the repo-wide total. The project's own code dir is suggested.
TEST(ProjectSettings, DetectDiscountsVendoredDeps) {
    QTemporaryDir dir;
    const QString root = canon(dir);
    // Bundled deps: a *-deps staging tree + a conventional third_party name,
    // together holding far more files than the first-party code.
    for (int i = 0; i < 30; ++i)
        writeFile(root + QStringLiteral("/mingw-deps/SDL2/src/f%1.c").arg(i),
                  cFile(QStringLiteral("dep%1").arg(i)));
    for (int i = 0; i < 12; ++i)
        writeFile(root + QStringLiteral("/third_party/vk/g%1.c").arg(i),
                  cFile(QStringLiteral("vk%1").arg(i)));
    // First-party code in a non-src layout dir (the DOOM shape).
    for (int i = 0; i < 5; ++i)
        writeFile(root + QStringLiteral("/linuxdoom/h%1.c").arg(i),
                  cFile(QStringLiteral("game%1").arg(i)));

    ProjectSettings::Suggestion s = ProjectSettings::detect(root);
    // Vendored dirs are excluded from both the suggestion and the total.
    EXPECT_EQ(s.totalSourceCount, 5);  // only linuxdoom/ counts
    ASSERT_TRUE(s.sourceRoots.has_value());
    EXPECT_FALSE(s.sourceRoots->contains(QStringLiteral("mingw-deps")));
    EXPECT_FALSE(s.sourceRoots->contains(QStringLiteral("third_party")));
    EXPECT_TRUE(s.sourceRoots->contains(QStringLiteral("linuxdoom")));
}

// ANTS-3393 — a committed Python virtualenv (`venv/`, NOT `.venv/`) + a
// bytecode cache (`__pycache__/`) must be discounted exactly like the
// vendored *-deps trees. The Contact_List session saw op:detect suggest
// source_roots=["venv"] because the virtualenv held 2340 of 2354 files.
// venv/env/__pycache__ are now in the isNoiseDir set.
TEST(ProjectSettings, DetectDiscountsPythonVirtualenv) {
    QTemporaryDir dir;
    const QString root = canon(dir);
    for (int i = 0; i < 40; ++i)
        writeFile(root + QStringLiteral("/venv/lib/p%1.c").arg(i),
                  cFile(QStringLiteral("vend%1").arg(i)));
    for (int i = 0; i < 6; ++i)
        writeFile(root + QStringLiteral("/__pycache__/c%1.c").arg(i),
                  cFile(QStringLiteral("cache%1").arg(i)));
    // First-party code in a real subdir, so a suggestion can still be made.
    for (int i = 0; i < 5; ++i)
        writeFile(root + QStringLiteral("/app/m%1.c").arg(i),
                  cFile(QStringLiteral("route%1").arg(i)));

    ProjectSettings::Suggestion s = ProjectSettings::detect(root);
    EXPECT_EQ(s.totalSourceCount, 5);  // venv/ + __pycache__/ excluded from the total
    EXPECT_TRUE(s.excluded.contains(QStringLiteral("venv")));
    EXPECT_TRUE(s.excluded.contains(QStringLiteral("__pycache__")));
    ASSERT_TRUE(s.sourceRoots.has_value());
    EXPECT_FALSE(s.sourceRoots->contains(QStringLiteral("venv")));
    EXPECT_TRUE(s.sourceRoots->contains(QStringLiteral("app")));
}

// Wiring — every consumer calls ProjectSettings::load.
TEST(ProjectSettings, ConsumerWiring) {
    const std::string ci = ants_test::slurpFile(srcPath("src/codebaseindex.cpp"));
    const std::string di = ants_test::slurpFile(srcPath("src/docsindex.cpp"));
    const std::string rc = ants_test::slurpFile(srcPath("src/remotecontrol.cpp"));
    const std::string pl = ants_test::slurpFile(srcPath("src/projectlayoutengine.cpp"));
    EXPECT_TRUE(has(ci, "ProjectSettings::load"));   // candidates()
    EXPECT_TRUE(has(di, "ProjectSettings::load"));   // walkDocs()
    EXPECT_TRUE(has(rc, "ProjectSettings::load"));   // finders + spec routing
    EXPECT_TRUE(has(pl, "ProjectSettings::load"));   // scanLayout()
}
