// Feature-conformance test for ANTS-2161 — the layout detector + the
// project_settings verb's pure helpers (ProjectSettings::detect /
// ProjectSettings::applyWrite). Behavioural invariants drive the pure
// helpers against QTemporaryDir fixtures; verb-layer + wiring invariants
// source-scrape the handler/registration. See spec.md + docs/specs/ANTS-2161.md.

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"
#include "projectsettings.h"
#include "remotecontrol.h"

#include <string>

#include <gtest/gtest.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
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

// INV-19 (ANTS-3588) — a source_roots suggestion (misplaced layout) also
// proposes the conventional aux layout keys present on disk, so one op:init
// writes the whole block. engine/ holds 2 .c files (a miss: defaultSourceCount
// 1 < kMissRatio*3); the .md / root files are not indexable so docs/ is never
// proposed as a source subdir.
TEST(ProjectSettingsVerb, MisplacedLayoutProposesAuxKeys) {
    QTemporaryDir dir;
    const QString root = canon(dir);
    writeFile(root + "/engine/a.c", cFile("a"));
    writeFile(root + "/engine/b.c", cFile("b"));
    writeFile(root + "/tests/t.c", cFile("t"));
    writeFile(root + "/docs/x.md", QStringLiteral("# d\n"));
    writeFile(root + "/docs/specs/s.md", QStringLiteral("# s\n"));
    writeFile(root + "/ROADMAP.md", QStringLiteral("# r\n"));
    writeFile(root + "/CHANGELOG.md", QStringLiteral("# c\n"));

    const ProjectSettings::Suggestion s = ProjectSettings::detect(root);
    ASSERT_TRUE(s.sourceRoots.has_value());
    EXPECT_EQ(*s.sourceRoots, QStringList{QStringLiteral("engine")});  // docs/ NOT a source subdir
    ASSERT_TRUE(s.testRoots.has_value());
    EXPECT_EQ(*s.testRoots, QStringList{QStringLiteral("tests")});
    ASSERT_TRUE(s.docsDir.has_value());
    EXPECT_EQ(*s.docsDir, QStringLiteral("docs"));
    ASSERT_TRUE(s.specsDir.has_value());
    EXPECT_EQ(*s.specsDir, QStringLiteral("docs/specs"));
    ASSERT_TRUE(s.roadmap.has_value());
    EXPECT_EQ(*s.roadmap, QStringLiteral("ROADMAP.md"));
    ASSERT_TRUE(s.changelog.has_value());
    EXPECT_EQ(*s.changelog, QStringLiteral("CHANGELOG.md"));
}

// INV-19 negative — a standard layout makes no suggestion, so NO aux key is
// proposed even when CHANGELOG.md is present (the ride-along requires a
// source_roots suggestion).
TEST(ProjectSettingsVerb, StandardLayoutProposesNoAuxKeys) {
    QTemporaryDir dir;
    const QString root = canon(dir);
    writeFile(root + "/src/a.c", cFile("a"));
    writeFile(root + "/src/b.c", cFile("b"));
    writeFile(root + "/tests/t.c", cFile("t"));
    writeFile(root + "/CHANGELOG.md", QStringLiteral("# c\n"));

    const ProjectSettings::Suggestion s = ProjectSettings::detect(root);
    EXPECT_FALSE(s.sourceRoots.has_value());   // default walk covers → no suggestion
    // INV-21 (ANTS-4093) — but the aux keys ARE proposed here now. They used
    // to ride along on a source_roots suggestion only, so this project got a
    // reply about source_roots and nothing else while five keys sat
    // undeclared and unmentioned.
    EXPECT_TRUE(s.testRoots.has_value());
    EXPECT_EQ(s.changelog.value_or(QString()), QStringLiteral("CHANGELOG.md"))
        << "CHANGELOG.md is on disk and undeclared — say so";
    EXPECT_FALSE(s.docsDir.has_value());       // no docs/ in this fixture
    EXPECT_FALSE(s.specsDir.has_value());
    EXPECT_FALSE(s.roadmap.has_value());
    // …and the verdict names what it is about.
    EXPECT_TRUE(s.reason.contains(QStringLiteral("no source_roots override needed")))
        << "\"no override needed\" was only ever true of source_roots";
}

// ANTS-4815 — a recognised key with no declaration is split by whether the
// caller can act on it. op:set refuses a path that is not on disk, so before
// the split a project that legitimately keeps no docs/ or docs/specs carried
// those keys in `undeclared[]` forever, clearable only by creating
// directories it did not want. That is indistinguishable from a genuine
// to-do, so the array as a whole became ignorable.
TEST(ProjectSettingsVerb, Ants4815UndeclaredSplitsOnActionability) {
    QTemporaryDir dir;
    const QString root = canon(dir);
    writeFile(root + "/src/a.c", cFile("a"));
    writeFile(root + "/CHANGELOG.md", QStringLiteral("# c\n"));
    // Deliberately absent: docs/, docs/specs/, ROADMAP.md, tests/.

    static const QStringList kKeys = {
        QStringLiteral("source_roots"), QStringLiteral("test_roots"),
        QStringLiteral("docs_dir"), QStringLiteral("specs_dir"),
        QStringLiteral("roadmap"), QStringLiteral("changelog")};

    const ProjectSettings::Suggestion s = ProjectSettings::detect(root);
    const ProjectSettings::UndeclaredSplit split =
        ProjectSettings::splitUndeclared(root, s, kKeys, QStringList{});

    // On disk, so op:set would accept a declaration for these.
    EXPECT_TRUE(split.undeclared.contains(QStringLiteral("changelog")))
        << "CHANGELOG.md exists — declaring it is actionable";
    EXPECT_TRUE(split.undeclared.contains(QStringLiteral("source_roots")))
        << "src/ exists — declaring source_roots is actionable";

    // Not on disk: op:set refuses bad_path, so these are not a to-do list.
    EXPECT_TRUE(split.unavailable.contains(QStringLiteral("docs_dir")));
    EXPECT_TRUE(split.unavailable.contains(QStringLiteral("specs_dir")));
    EXPECT_TRUE(split.unavailable.contains(QStringLiteral("roadmap")));
    EXPECT_TRUE(split.unavailable.contains(QStringLiteral("test_roots")));

    // The partition is exactly the undeclared set — nothing invented, nothing
    // dropped, and no key in both halves.
    EXPECT_EQ(split.undeclared.size() + split.unavailable.size(), kKeys.size());
    for (const QString &k : split.undeclared)
        EXPECT_FALSE(split.unavailable.contains(k)) << k.toStdString();

    // A declared key appears in neither half.
    const ProjectSettings::UndeclaredSplit declaredOut =
        ProjectSettings::splitUndeclared(
            root, s, kKeys, QStringList{QStringLiteral("changelog")});
    EXPECT_FALSE(declaredOut.undeclared.contains(QStringLiteral("changelog")));
    EXPECT_FALSE(declaredOut.unavailable.contains(QStringLiteral("changelog")));
}

// ANTS-4815 — detect returns early on a configured project (INV-2) having
// proposed no aux keys at all, so classifying from the Suggestion alone would
// call every key unavailable there. The split probes the conventional path as
// well, which is what keeps that case honest.
TEST(ProjectSettingsVerb, Ants4815ConfiguredProjectStillSeesPathsOnDisk) {
    QTemporaryDir dir;
    const QString root = canon(dir);
    writeFile(root + "/src/a.c", cFile("a"));
    writeFile(root + "/docs/x.md", QStringLiteral("# x\n"));
    writeFile(root + "/.ants/project.json",
              QStringLiteral("{\"source_roots\": [\"src\"]}\n"));

    const ProjectSettings::Suggestion s = ProjectSettings::detect(root);
    ASSERT_TRUE(s.present) << "fixture must take the configured short-circuit";
    ASSERT_FALSE(s.docsDir.has_value())
        << "the short-circuit proposes nothing — that is the trap being tested";

    const ProjectSettings::UndeclaredSplit split = ProjectSettings::splitUndeclared(
        root, s, QStringList{QStringLiteral("docs_dir"), QStringLiteral("roadmap")},
        QStringList{});
    EXPECT_TRUE(split.undeclared.contains(QStringLiteral("docs_dir")))
        << "docs/ is on disk, so declaring docs_dir is actionable";
    EXPECT_TRUE(split.unavailable.contains(QStringLiteral("roadmap")))
        << "no ROADMAP.md in this fixture";
}

// INV-22 (ANTS-4092) — a gitignored top-level dir is excluded from the walk
// rather than ranked as a source_root. Pre-fix, detect ignored .gitignore
// entirely while workspace_search defaults to respect_gitignore:true, so on a
// repo with no src/ — where the default walk indexes 0 files and detect is at
// its most confident — a state directory won.
TEST(ProjectSettingsVerb, Inv22GitignoredDirsExcluded) {
    QTemporaryDir dir;
    const QString root = canon(dir);
    writeFile(root + "/engine/a.c", cFile("a"));
    writeFile(root + "/engine/b.c", cFile("b"));
    writeFile(root + "/state/x.c", cFile("x"));
    writeFile(root + "/state/y.c", cFile("y"));
    writeFile(root + "/state/z.c", cFile("z"));
    writeFile(root + "/.gitignore", QStringLiteral("state/\n"));

    // Without a git repo the probe is a no-op, so `state` still ranks — which
    // is also the documented fallback when git is unavailable.
    const ProjectSettings::Suggestion before = ProjectSettings::detect(root);
    ASSERT_TRUE(before.sourceRoots.has_value());
    EXPECT_TRUE(before.sourceRoots->contains(QStringLiteral("state")))
        << "no repo → no ignore data → pre-4092 behaviour";

    QProcess git;
    git.setWorkingDirectory(root);
    git.start(QStringLiteral("git"), {QStringLiteral("init"), QStringLiteral("-q")});
    if (!git.waitForFinished(10000) || git.exitCode() != 0)
        GTEST_SKIP() << "git unavailable — the fallback leg above still ran";

    const ProjectSettings::Suggestion s = ProjectSettings::detect(root);
    ASSERT_TRUE(s.sourceRoots.has_value());
    EXPECT_FALSE(s.sourceRoots->contains(QStringLiteral("state")))
        << "a gitignored dir must not be suggested as a source_root";
    EXPECT_TRUE(s.sourceRoots->contains(QStringLiteral("engine")));
    EXPECT_TRUE(s.excluded.contains(QStringLiteral("state")))
        << "and the caller is told what was discounted";
    // The ignored files are not counted either — they were never walked.
    EXPECT_EQ(s.totalSourceCount, 2);
}

// INV-5 / INV-6 / INV-10 / INV-13 — verb-layer + registration wiring
// (source-grep; the verb glue isn't unit-testable without RemoteControl).
// INV-20 (ANTS-3705) — op:detect echoes the CURRENT declaration, so the six
// keys can be inspected without a native Read of .ants/project.json. It reads
// the stored file rather than ProjectSettings::load(), because load() drops an
// entry whose path no longer resolves — the very state `declared_missing`
// exists to name.
TEST(ProjectSettingsVerb, Inv20DetectEchoesDeclaration) {
    const std::string rc = ants_test::slurpRemoteControl();
    const std::string ci = ants_test::slurpFile(srcPath("src/claudeintegration.cpp"));

    EXPECT_TRUE(has(rc, "\"declared\""));
    EXPECT_TRUE(has(rc, "\"declared_missing\""));
    // Existence is judged with the same anchor check the loader uses, so the
    // echo and the drop cannot disagree.
    EXPECT_TRUE(has(rc, "PathValidation::isInsideProject(rootCanonical, abs)"));
    // Documented on the verb, else a caller never learns the field is there.
    EXPECT_TRUE(has(ci, "declared_missing"));

    // INV-21 (ANTS-4093) — `undeclared[]` names every recognised key with no
    // declaration, and the description says detect covers all six keys. A
    // reply that lists only what IS set is what got read as "nothing to do".
    EXPECT_TRUE(has(rc, "\"undeclared\""));
    EXPECT_TRUE(has(ci, "detect suggests ALL SIX recognised keys"));
    // INV-22 (ANTS-4092) — the gitignore probe, and its documentation.
    EXPECT_TRUE(has(ants_test::slurpFile(srcPath("src/projectsettings.cpp")),
                    "check-ignore"));
    EXPECT_TRUE(has(ci, "gitignored directories are excluded"));
}

TEST(ProjectSettingsVerb, VerbAndRegistrationWiring) {
    const std::string rc = ants_test::slurpRemoteControl();
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

// ANTS-4648 — the present:true path returns before the walk (INV-2), so the
// two counts were never taken. Reporting 0 there is a measurement of nothing,
// and a session reading it "fixes" a declaration that was already right.
TEST(ProjectSettingsVerb, Ants4648ConfiguredProjectMarksCountsUncomputed) {
    QTemporaryDir dir;
    const QString root = canon(dir);
    writeFile(root + "/engine/a.c", cFile("a"));
    writeFile(root + "/engine/b.c", cFile("b"));
    const QString settings = root + "/.ants/project.json";
    writeFile(settings, QStringLiteral("{\"source_roots\":[\".\"]}\n"));

    const ProjectSettings::Suggestion s = ProjectSettings::detect(root);
    ASSERT_TRUE(s.present);
    EXPECT_FALSE(s.countsComputed)
        << "no walk ran, so the counts carry no measurement";

    // The contrast is the same tree with the short-circuit removed: identical
    // files, and now the counts mean something.
    ASSERT_TRUE(QFile::remove(settings));
    const ProjectSettings::Suggestion w = ProjectSettings::detect(root);
    ASSERT_FALSE(w.present);
    EXPECT_TRUE(w.countsComputed);
    EXPECT_EQ(w.totalSourceCount, 2);
}

// ANTS-4648 (verb layer) — the envelope is where the zero was read. Absent
// reads as not-computed, which is what `compact:true` already does for empties;
// `counts_computed` is always present so absence is never ambiguous.
TEST(ProjectSettingsVerb, Ants4648DetectOmitsUncomputedCounts) {
    QTemporaryDir dir;
    const QString root = canon(dir);
    writeFile(root + "/engine/a.c", cFile("a"));
    writeFile(root + "/engine/b.c", cFile("b"));
    const QString settings = root + "/.ants/project.json";
    writeFile(settings, QStringLiteral("{\"source_roots\":[\".\"]}\n"));

    RemoteControl rc(nullptr, nullptr);
    QJsonObject req;
    req["caller_cwd"] = root;
    req["op"] = "detect";

    const QJsonObject skipped =
        rc.cmdProjectSettings(req).object().value("suggestion").toObject();
    ASSERT_FALSE(skipped.isEmpty());
    ASSERT_TRUE(skipped.contains("counts_computed"));
    EXPECT_FALSE(skipped.value("counts_computed").toBool());
    EXPECT_FALSE(skipped.contains("default_source_count"))
        << "a count nobody took must not be emitted as 0";
    EXPECT_FALSE(skipped.contains("total_source_count"));

    ASSERT_TRUE(QFile::remove(settings));
    const QJsonObject walked =
        rc.cmdProjectSettings(req).object().value("suggestion").toObject();
    EXPECT_TRUE(walked.value("counts_computed").toBool());
    EXPECT_EQ(walked.value("total_source_count").toInt(), 2);
}

// ---------------------------------------------------------------------------
// ANTS-4903 — op:"get", the read that is named like one.
//
// Reported by AI_Prompts. The verb took detect|init|set, and the standing
// project-layout instruction asks a session to answer "is .ants/project.json
// complete?" at orientation. The only route to that was op:"detect", whose
// name and documented purpose are to PROPOSE settings for a project that may
// have none — so every session paid a call it had to reason about before
// trusting, and one reading `detect` as write-shaped skips the check the
// instruction exists to force.
//
// The information was always reachable: detect echoes `declared`. This splits
// the read from the proposal, the way the other verbs do.

namespace {

QJsonObject settingsCall(const QString &root, const QString &op) {
    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("op")]         = op;
    return rc.cmdProjectSettings(req).object();
}

}  // namespace

// INV-1 — op:"get" answers the question, and carries no proposal.
TEST(ProjectSettingsVerb, Ants4903GetIsTheRead) {
    QTemporaryDir dir;
    const QString root = canon(dir);
    writeFile(root + "/src/a.c", cFile("a"));
    writeFile(root + "/CHANGELOG.md", QStringLiteral("# c\n"));
    writeFile(root + "/.ants/project.json",
              QStringLiteral("{\n  \"source_roots\": [\"src\"],\n"
                             "  \"changelog\": \"CHANGELOG.md\"\n}\n"));

    const QJsonObject env = settingsCall(root, QStringLiteral("get"));
    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(env).toJson().toStdString();

    const QJsonObject declared =
        env.value(QStringLiteral("declared")).toObject();
    EXPECT_TRUE(declared.contains(QStringLiteral("source_roots")));
    EXPECT_TRUE(declared.contains(QStringLiteral("changelog")));

    // The three fields a caller acts on ride the read unchanged.
    EXPECT_TRUE(env.contains(QStringLiteral("undeclared"))
                || env.contains(QStringLiteral("unavailable")))
        << "the read must still say what is NOT declared";

    // And the proposal does not: that is what makes this a read.
    EXPECT_FALSE(env.contains(QStringLiteral("suggestion")))
        << "op:get must not carry detect's proposal — the `suggestion` block "
           "on an already-declared project is the noise this op removes";
    EXPECT_EQ(env.value(QStringLiteral("op")).toString(),
              QStringLiteral("get"))
        << "the reply must name the op it answered";
}

// INV-2 — op:"get" writes nothing, on a project that has no settings file.
// The op it replaces is read-only too; what changes is that the NAME says so.
TEST(ProjectSettingsVerb, Ants4903GetCreatesNothing) {
    QTemporaryDir dir;
    const QString root = canon(dir);
    writeFile(root + "/src/a.c", cFile("a"));

    const QJsonObject env = settingsCall(root, QStringLiteral("get"));
    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(env.value(QStringLiteral("present")).toBool());
    EXPECT_FALSE(QFileInfo::exists(root + "/.ants/project.json"))
        << "a read must not create the file it reports on";
    // Nothing declared, so every recognised key is accounted for on one of
    // the two undeclared arms.
    EXPECT_FALSE(env.contains(QStringLiteral("declared")));
}

// INV-3 — the refusal names every op, so a caller that guessed one wrong is
// told what to reach for.
TEST(ProjectSettingsVerb, Ants4903RefusalNamesEveryOp) {
    QTemporaryDir dir;
    const QString root = canon(dir);
    writeFile(root + "/src/a.c", cFile("a"));

    const QJsonObject env = settingsCall(root, QStringLiteral("fetch"));
    EXPECT_FALSE(env.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(env.value(QStringLiteral("code")).toString(),
              QStringLiteral("bad_args"));
    EXPECT_TRUE(env.value(QStringLiteral("error")).toString()
                    .contains(QStringLiteral("get")))
        << "the refusal must name the read op: "
        << env.value(QStringLiteral("error")).toString().toStdString();
}
