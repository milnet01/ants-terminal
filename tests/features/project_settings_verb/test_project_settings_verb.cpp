// Feature-conformance test for ANTS-2161 — the layout detector + the
// project_settings verb's pure helpers (ProjectSettings::detect /
// ProjectSettings::applyWrite). Behavioural invariants drive the pure
// helpers against QTemporaryDir fixtures; verb-layer + wiring invariants
// source-scrape the handler/registration. See spec.md + docs/specs/ANTS-2161.md.

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"
#include "projectsettings.h"

#include <string>

#include <gtest/gtest.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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
QString canon(const QTemporaryDir &d) {
    return QFileInfo(d.path()).canonicalFilePath();
}
QJsonObject jobj(std::initializer_list<std::pair<QString, QJsonValue>> kv) {
    QJsonObject o;
    for (const auto &p : kv) o[p.first] = p.second;
    return o;
}

}  // namespace

// INV-1 — standard src/ layout, no settings file → no suggestion.
TEST(ProjectSettingsVerb, StandardLayoutNoSuggestion) {
    QTemporaryDir dir;
    const QString root = canon(dir);
    writeFile(root + "/src/a.c", cFile("a"));
    writeFile(root + "/src/b.c", cFile("b"));
    writeFile(root + "/tests/t.c", cFile("t"));

    const ProjectSettings::Suggestion s = ProjectSettings::detect(root);
    EXPECT_FALSE(s.present);
    EXPECT_FALSE(s.sourceRoots.has_value());
    EXPECT_FALSE(s.reason.isEmpty());   // ANTS-3369 (INV-15): reason always set
    // INV-16: no-override path echoes whichever of src/tests hold source.
    ASSERT_TRUE(s.wouldUseRoots.has_value());
    EXPECT_TRUE(s.wouldUseRoots->contains(QStringLiteral("src")));
    EXPECT_TRUE(s.wouldUseRoots->contains(QStringLiteral("tests")));
}

// INV-2 — an existing settings file → present:true, no walk, no suggestion.
TEST(ProjectSettingsVerb, ConfiguredProjectShortCircuits) {
    QTemporaryDir dir;
    const QString root = canon(dir);
    writeFile(root + "/engine/a.c", cFile("a"));   // misplaced layout …
    writeFile(root + "/.ants/project.json",
              QStringLiteral("{\"source_roots\":[\"engine\"]}\n"));

    const ProjectSettings::Suggestion s = ProjectSettings::detect(root);
    EXPECT_TRUE(s.present);
    EXPECT_FALSE(s.sourceRoots.has_value());   // … but never second-guessed
    EXPECT_FALSE(s.reason.isEmpty());          // ANTS-3369 (INV-15): present branch
    // INV-16: present:true echoes the file's declared source_roots.
    ASSERT_TRUE(s.wouldUseRoots.has_value());
    EXPECT_EQ(*s.wouldUseRoots, QStringList{QStringLiteral("engine")});
}

// INV-3 — misplaced layout (code under engine/, no src/) → suggest engine.
TEST(ProjectSettingsVerb, MisplacedLayoutSuggestsSubdir) {
    QTemporaryDir dir;
    const QString root = canon(dir);
    writeFile(root + "/engine/a.c", cFile("a"));
    writeFile(root + "/engine/b.c", cFile("b"));

    const ProjectSettings::Suggestion s = ProjectSettings::detect(root);
    ASSERT_TRUE(s.sourceRoots.has_value());
    EXPECT_EQ(*s.sourceRoots, QStringList{QStringLiteral("engine")});
    EXPECT_EQ(s.defaultSourceCount, 0);
    EXPECT_EQ(s.totalSourceCount, 2);
    EXPECT_FALSE(s.reason.isEmpty());
}

// INV-4 — applyWrite + a real write round-trips through load(); file 0644.
TEST(ProjectSettingsVerb, WriteRoundTripWorldReadable) {
    QTemporaryDir dir;
    const QString root = canon(dir);
    writeFile(root + "/engine/a.c", cFile("a"));

    QString ec, ek, ev;
    const auto merged = ProjectSettings::applyWrite(
        QJsonObject{}, jobj({{QStringLiteral("source_roots"),
                              QJsonArray{QStringLiteral("engine")}}}),
        root, &ec, &ek, &ev);
    ASSERT_TRUE(merged.has_value());

    const QString path = root + "/.ants/project.json";
    QDir().mkpath(root + "/.ants");
    writeFile(path, QString::fromUtf8(
        QJsonDocument(*merged).toJson(QJsonDocument::Indented)));
    ASSERT_TRUE(QFile::setPermissions(
        path, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                  | QFileDevice::ReadGroup | QFileDevice::ReadOther));

    const ProjectSettings::Settings loaded = ProjectSettings::load(root);
    ASSERT_TRUE(loaded.sourceRoots.has_value());
    EXPECT_EQ(*loaded.sourceRoots, QStringList{QStringLiteral("engine")});
    const QFileDevice::Permissions p = QFile(path).permissions();
    EXPECT_TRUE(p.testFlag(QFileDevice::ReadOther));   // world-readable
    EXPECT_TRUE(p.testFlag(QFileDevice::ReadGroup));
}

// INV-7 — set merges, preserving keys it didn't touch incl. unknown ones.
TEST(ProjectSettingsVerb, MergePreservesUnknownKeys) {
    QTemporaryDir dir;
    const QString root = canon(dir);
    writeFile(root + "/a/x.c", cFile("x"));
    writeFile(root + "/docs/readme.md", QStringLiteral("# d\n"));

    const QJsonObject existing = jobj({
        {QStringLiteral("docs_dir"), QStringLiteral("docs")},
        {QStringLiteral("future_key"), 1}});
    QString ec, ek, ev;
    const auto merged = ProjectSettings::applyWrite(
        existing, jobj({{QStringLiteral("source_roots"),
                         QJsonArray{QStringLiteral("a")}}}),
        root, &ec, &ek, &ev);
    ASSERT_TRUE(merged.has_value());
    EXPECT_TRUE(merged->contains(QStringLiteral("source_roots")));
    EXPECT_TRUE(merged->contains(QStringLiteral("docs_dir")));
    EXPECT_TRUE(merged->contains(QStringLiteral("future_key")));   // forward-compat
}

// INV-8 — a null value removes a key; a null-only call on an absent key is a
// valid no-op (returns an object, never bad_args).
TEST(ProjectSettingsVerb, NullClearsAndNullOnlyIsNoOp) {
    QTemporaryDir dir;
    const QString root = canon(dir);

    QString ec, ek, ev;
    const auto cleared = ProjectSettings::applyWrite(
        jobj({{QStringLiteral("docs_dir"), QStringLiteral("docs")}}),
        jobj({{QStringLiteral("docs_dir"), QJsonValue::Null}}),
        root, &ec, &ek, &ev);
    ASSERT_TRUE(cleared.has_value());
    EXPECT_FALSE(cleared->contains(QStringLiteral("docs_dir")));   // removed

    const auto noop = ProjectSettings::applyWrite(
        QJsonObject{},
        jobj({{QStringLiteral("docs_dir"), QJsonValue::Null}}),
        root, &ec, &ek, &ev);
    ASSERT_TRUE(noop.has_value());                                 // not bad_args
    EXPECT_FALSE(noop->contains(QStringLiteral("docs_dir")));
}

// INV-9 — write-time validation: escape/absent + wrong-type → bad_path;
// wrong-shape (non-array) → bad_args; the merge never writes on failure.
TEST(ProjectSettingsVerb, WriteTimeValidation) {
    QTemporaryDir dir;
    const QString root = canon(dir);
    writeFile(root + "/src/a.c", cFile("a"));   // src/ exists (a dir)

    QString ec, ek, ev;
    // Absent dir → bad_path.
    EXPECT_FALSE(ProjectSettings::applyWrite(
        QJsonObject{}, jobj({{QStringLiteral("source_roots"),
                              QJsonArray{QStringLiteral("nope")}}}),
        root, &ec, &ek, &ev).has_value());
    EXPECT_EQ(ec, QStringLiteral("bad_path"));
    EXPECT_EQ(ek, QStringLiteral("source_roots"));

    // A dir where a FILE is expected → bad_path.
    EXPECT_FALSE(ProjectSettings::applyWrite(
        QJsonObject{}, jobj({{QStringLiteral("roadmap"), QStringLiteral("src")}}),
        root, &ec, &ek, &ev).has_value());
    EXPECT_EQ(ec, QStringLiteral("bad_path"));

    // Wrong shape (string, not array) → bad_args.
    EXPECT_FALSE(ProjectSettings::applyWrite(
        QJsonObject{}, jobj({{QStringLiteral("source_roots"), QStringLiteral("src")}}),
        root, &ec, &ek, &ev).has_value());
    EXPECT_EQ(ec, QStringLiteral("bad_args"));

    // Root escape → bad_path.
    EXPECT_FALSE(ProjectSettings::applyWrite(
        QJsonObject{}, jobj({{QStringLiteral("docs_dir"), QStringLiteral("../escape")}}),
        root, &ec, &ek, &ev).has_value());
    EXPECT_EQ(ec, QStringLiteral("bad_path"));
}

// INV-11 — the walk skips noise dirs; node_modules neither suggested nor
// counted toward the total.
TEST(ProjectSettingsVerb, NoiseDirsSkipped) {
    QTemporaryDir dir;
    const QString root = canon(dir);
    writeFile(root + "/engine/a.c", cFile("a"));
    for (int i = 0; i < 10; ++i)   // more JS files than engine has .c
        writeFile(root + QStringLiteral("/node_modules/m%1.js").arg(i),
                  QStringLiteral("function f%1(){}\n").arg(i));

    const ProjectSettings::Suggestion s = ProjectSettings::detect(root);
    ASSERT_TRUE(s.sourceRoots.has_value());
    EXPECT_EQ(*s.sourceRoots, QStringList{QStringLiteral("engine")});
    EXPECT_EQ(s.totalSourceCount, 1);   // node_modules excluded from the total
}

// INV-12 (amended ANTS-3390) — source at the repo root with NO subdir now
// yields a whole-root ["."] suggestion (was: no suggestion). ["."] is the
// degenerate rootLevel>0 case of INV-17; it lets codebase_index reach the
// depth-0 files that a subdirs-only suggestion could never index.
TEST(ProjectSettingsVerb, RepoRootSourceSuggestsWholeRoot) {
    QTemporaryDir dir;
    const QString root = canon(dir);
    writeFile(root + "/a.c", cFile("a"));
    writeFile(root + "/b.c", cFile("b"));

    const ProjectSettings::Suggestion s = ProjectSettings::detect(root);
    ASSERT_TRUE(s.sourceRoots.has_value());
    EXPECT_EQ(*s.sourceRoots, QStringList{QStringLiteral(".")});
    EXPECT_EQ(s.totalSourceCount, 2);
    EXPECT_FALSE(s.reason.isEmpty());
}

// INV-14 — ANTS-3369: on a miss with NO root source (rootLevel==0), suggest
// ALL first-party source subdirs (not a dominant-cover subset), sorted count
// desc / name asc. Pure-subdir spread here (no root file) so the rootLevel==0
// branch is exercised — a miss WITH root source is INV-17's ["."] case.
TEST(ProjectSettingsVerb, MissSuggestsAllSourceSubdirs) {
    QTemporaryDir dir;
    const QString root = canon(dir);
    writeFile(root + "/app/a.c", cFile("a"));      // subdir (no root file)
    writeFile(root + "/engine/b.c", cFile("b"));   // subdir

    const ProjectSettings::Suggestion s = ProjectSettings::detect(root);
    EXPECT_EQ(s.defaultSourceCount, 0);   // a miss …
    ASSERT_TRUE(s.sourceRoots.has_value());  // … all subdirs suggested
    EXPECT_EQ(*s.sourceRoots,
              (QStringList{QStringLiteral("app"), QStringLiteral("engine")}));
    EXPECT_EQ(s.totalSourceCount, 2);
    EXPECT_FALSE(s.reason.isEmpty());
}

// INV-17 (ANTS-3390) — a miss with source loose AT the repo root (rootLevel>0)
// suggests the whole-root ["."] walk, which subsumes both the depth-0 files
// and the subdir library in one entry (the RetroArch-class layout). Takes the
// rootLevel>0 branch, mutually exclusive with INV-14's subdir list.
TEST(ProjectSettingsVerb, MissWithRootSourceSuggestsWholeRoot) {
    QTemporaryDir dir;
    const QString root = canon(dir);
    writeFile(root + "/retroarch.c", cFile("retroarch"));    // loose at the root
    writeFile(root + "/libretro-common/x.c", cFile("x"));    // subdir library

    const ProjectSettings::Suggestion s = ProjectSettings::detect(root);
    EXPECT_EQ(s.defaultSourceCount, 0);   // a miss
    ASSERT_TRUE(s.sourceRoots.has_value());
    EXPECT_EQ(*s.sourceRoots, QStringList{QStringLiteral(".")});
    EXPECT_EQ(s.totalSourceCount, 2);
    EXPECT_FALSE(s.reason.isEmpty());
}

// INV-15 — ANTS-3369: detect() always sets a non-empty reason even when
// there is no suggestion (empty repo here; the present / no-override
// branches are covered by INV-2 / INV-1). nullopt sourceRoots — not an
// empty reason — is the "no suggestion" signal.
TEST(ProjectSettingsVerb, AlwaysSetsReasonEmptyRepo) {
    QTemporaryDir dir;
    const QString root = canon(dir);   // no source files at all
    const ProjectSettings::Suggestion s = ProjectSettings::detect(root);
    EXPECT_FALSE(s.sourceRoots.has_value());
    EXPECT_FALSE(s.reason.isEmpty());
}

// INV-16 — ANTS-3369: `excluded` echoes the skipped noise/vendored dirs
// (present on disk, minus dot-dirs) by name — covering a plain vendored
// name AND the *-deps suffix rule. (The present:true → wouldUseRoots echo
// is asserted in INV-2.)
TEST(ProjectSettingsVerb, ExcludedEchoesVendoredDirs) {
    QTemporaryDir dir;
    const QString root = canon(dir);
    writeFile(root + "/engine/a.c", cFile("a"));
    for (int i = 0; i < 5; ++i)   // a plain vendored name
        writeFile(root + QStringLiteral("/node_modules/m%1.js").arg(i),
                  QStringLiteral("function f%1(){}\n").arg(i));
    writeFile(root + "/mingw-deps/SDL2/x.c", cFile("x"));   // the *-deps suffix rule

    const ProjectSettings::Suggestion s = ProjectSettings::detect(root);
    EXPECT_TRUE(s.excluded.contains(QStringLiteral("node_modules")));
    EXPECT_TRUE(s.excluded.contains(QStringLiteral("mingw-deps")));
    // engine/ is first-party → suggested, not excluded.
    EXPECT_FALSE(s.excluded.contains(QStringLiteral("engine")));
}

// INV-5 / INV-6 / INV-10 / INV-13 — verb-layer + registration wiring
// (source-grep; the verb glue isn't unit-testable without RemoteControl).
TEST(ProjectSettingsVerb, VerbAndRegistrationWiring) {
    const std::string rc = ants_test::slurpFile(srcPath("src/remotecontrol.cpp"));
    const std::string ci = ants_test::slurpFile(srcPath("src/claudeintegration.cpp"));
    const std::string mw = ants_test::slurpFile(srcPath("src/mainwindow.cpp"));

    // INV-13 — Required contract registered in callerCwdContractFor(), and
    // registered as a tool provider; the write target is .ants/project.json.
    EXPECT_TRUE(has(ci, "if (toolName == QStringLiteral(\"project_settings\"))"));
    EXPECT_TRUE(has(ci, "C::Required"));
    EXPECT_TRUE(has(mw, "registerToolProvider(\"project_settings\""));
    EXPECT_TRUE(has(rc, "/.ants/project.json"));

    // INV-5 — init refuses settings_exists (no clobber).
    EXPECT_TRUE(has(rc, "settings_exists"));
    // INV-6 — init with nothing to write returns written:false.
    EXPECT_TRUE(has(rc, "\"written\""));
    // INV-10 — set on a malformed existing file refuses unrecognised_format.
    EXPECT_TRUE(has(rc, "unrecognised_format"));
    // set's "no recognised key" guard uses bad_args.
    EXPECT_TRUE(has(rc, "bad_args"));
}
