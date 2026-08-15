// ANTS-3745 — build_target_for engine conformance. Contract: spec.md beside
// this file.

#include "buildtargets.h"

#include <QFile>
#include <QString>
#include <QStringList>

#include <gtest/gtest.h>

#if !defined(ANTS_SOURCE_DIR)
#error "build_target_for test needs ANTS_SOURCE_DIR for the live CMake rows"
#endif

namespace {

QString slurp(const QString &abs) {
    QFile f(abs);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    return QString::fromUtf8(f.readAll());
}

const BuildTargets::Target *byName(const QList<BuildTargets::Target> &ts,
                                   const char *name) {
    for (const auto &t : ts)
        if (t.name == QLatin1String(name)) return &t;
    return nullptr;
}

QStringList ownerNames(const QList<BuildTargets::Target> &ts,
                       const char *relPath) {
    QStringList out;
    for (const auto &t :
         BuildTargets::ownersOf(ts, QString::fromLatin1(relPath)))
        out.append(t.name);
    return out;
}

}  // namespace

TEST(BuildTargetFor, ParsesTheThreeDeclaringCommands) {
    const QString cm = QStringLiteral(
        "add_library(mylib STATIC\n"
        "    src/a.cpp\n"
        "    src/b.cpp\n"
        ")\n"
        "add_executable(myexe src/main.cpp)\n"
        "function(ants_add_gui_bundle name)\n"
        "    add_executable(${name} tests/bundle_main_gui.cpp ${B_SOURCES})\n"
        "endfunction()\n"
        "ants_add_gui_bundle(test_thing\n"
        "    LIBS    ants_core_lib\n"
        "    SOURCES tests/features/x/test_x.cpp\n"
        ")\n");
    const auto ts = BuildTargets::parse(cm);

    // Three targets, not four: the wrapper's own `add_executable(${name} …)`
    // is the function BODY, and counting it would invent a target named
    // `${name}` owning every bundle's main.
    ASSERT_EQ(ts.size(), 3) << "a ${name} expansion must not become a target";

    ASSERT_NE(byName(ts, "mylib"), nullptr);
    EXPECT_EQ(byName(ts, "mylib")->kind, QStringLiteral("library"));
    EXPECT_EQ(byName(ts, "myexe")->kind, QStringLiteral("executable"));
    EXPECT_EQ(byName(ts, "test_thing")->kind, QStringLiteral("bundle"));
    EXPECT_EQ(byName(ts, "test_thing")->command,
              QStringLiteral("ants_add_gui_bundle"));

    // The declaring line is 1-based, so a caller can open it.
    EXPECT_EQ(byName(ts, "mylib")->line, 1);
    EXPECT_EQ(byName(ts, "myexe")->line, 5);

    EXPECT_EQ(ownerNames(ts, "src/b.cpp"), QStringList{"mylib"});
    EXPECT_EQ(ownerNames(ts, "tests/features/x/test_x.cpp"),
              QStringList{"test_thing"});
}

TEST(BuildTargetFor, SourcesAreCollectedByShapeNotByKeyword) {
    const QString cm = QStringLiteral(
        "ants_add_core_bundle(test_core\n"
        "    LIBS    ants_core_lib\n"
        "            ants_roadmapstore_lib   # ANTS-3756\n"
        "    SOURCES tests/features/a/test_a.cpp   # a comment\n"
        "            tests/features/b/test_b.cpp\n"
        ")\n"
        "add_library(tiny STATIC src/tiny.cpp)\n");
    const auto ts = BuildTargets::parse(cm);
    ASSERT_EQ(ts.size(), 2);

    const auto *bundle = byName(ts, "test_core");
    ASSERT_NE(bundle, nullptr);
    // A LIBS entry, the SOURCES keyword itself and the target name are all
    // absent — they carry no `/`, which is what makes a keyword state machine
    // unnecessary here.
    EXPECT_EQ(bundle->sources.size(), 2) << bundle->sources.join(',').toStdString();
    EXPECT_TRUE(bundle->sources.contains(
        QStringLiteral("tests/features/b/test_b.cpp")));
    for (const QString &s : bundle->sources)
        EXPECT_TRUE(s.contains(QLatin1Char('/'))) << s.toStdString();

    // A one-line block terminates at its own close paren rather than running
    // on and swallowing the next target's sources.
    EXPECT_EQ(byName(ts, "tiny")->sources, QStringList{"src/tiny.cpp"});
}

TEST(BuildTargetFor, UnresolvableSourcesAreLeftUnowned) {
    const QString cm = QStringLiteral(
        "add_library(varlib STATIC\n"
        "    ${SOME_LIST}\n"
        "    src/${MODULE}/thing.cpp\n"
        "    src/plain.cpp\n"
        ")\n");
    const auto ts = BuildTargets::parse(cm);
    ASSERT_EQ(ts.size(), 1);
    // Only the literal path. A variable-driven one is reported unowned rather
    // than attributed — the verb's `found:false` is the honest answer, and a
    // wrong target name is worse than none because the caller acts on it.
    EXPECT_EQ(ts.at(0).sources, QStringList{"src/plain.cpp"});
    EXPECT_TRUE(ownerNames(ts, "src/whatever/thing.cpp").isEmpty());
}

TEST(BuildTargetFor, GeneratorExpressionWrappedPathsResolve) {
    // This project's real lists use the wrapped form for optional sources, so
    // dropping the whole token would leave live sources unowned.
    const QString cm = QStringLiteral(
        "add_executable(app\n"
        "    src/main.cpp\n"
        "    $<$<BOOL:${ANTS_ENABLE_HELPER_CLI}>:src/helper.cpp>\n"
        ")\n");
    const auto ts = BuildTargets::parse(cm);
    ASSERT_EQ(ts.size(), 1);
    EXPECT_EQ(ownerNames(ts, "src/helper.cpp"), QStringList{"app"});
}

TEST(BuildTargetFor, GtestSuitesAreDeduplicatedInOrder) {
    const QString src = QStringLiteral(
        "// TEST(NotASuite, prose) — a mention in a comment\n"
        "TEST(Alpha, One) {}\n"
        "TEST_F(Beta, Two) {}\n"
        "TEST(Alpha, Three) {}\n"
        "TEST_P(Gamma, Four) {}\n"
        "    TEST(Delta, Indented) {}\n"
        "int x = 0; TEST(MidLine, No) {}\n");
    const QStringList got = BuildTargets::gtestSuites(src);
    // First-seen order, deduplicated. `NotASuite` is behind a comment marker
    // and `MidLine` is not at line start, so neither counts — an over-eager
    // scan here sends `ctest -R` at a suite that does not exist and reports
    // "no tests matched", which reads as a build problem.
    EXPECT_EQ(got, (QStringList{"Alpha", "Beta", "Gamma", "Delta"}));
}

// The two rows that matter most: the real file, and the two mappings ANTS-3745
// names as proof that recall does not substitute for a lookup.
TEST(BuildTargetFor, LiveCmakeMapsTheTwoNonObviousBundles) {
    const QString cm =
        slurp(QStringLiteral(ANTS_SOURCE_DIR) + QStringLiteral("/CMakeLists.txt"));
    ASSERT_FALSE(cm.isEmpty()) << "the project's own CMakeLists.txt must read";
    const auto ts = BuildTargets::parse(cm);
    EXPECT_GE(ts.size(), 10) << "the real file declares many targets";

    EXPECT_EQ(ownerNames(ts, "tests/features/cold_eyes_engine/"
                             "test_cold_eyes_engine.cpp"),
              QStringList{"test_audit"})
        << "the path says cold_eyes and the bundle is test_audit — the "
           "mapping ANTS-3745 was filed about";
    EXPECT_EQ(ownerNames(ts, "tests/features/spec_conformance/"
                             "test_spec_conformance.cpp"),
              QStringList{"test_claude"})
        << "…and this one is not test_core either";
}

TEST(BuildTargetFor, LiveCmakeOwnsALibrarySource) {
    const QString cm =
        slurp(QStringLiteral(ANTS_SOURCE_DIR) + QStringLiteral("/CMakeLists.txt"));
    ASSERT_FALSE(cm.isEmpty());
    const auto ts = BuildTargets::parse(cm);
    // A library source, so the answer is not test-only. This is the lookup
    // that tells you which archive to link a throwaway probe against.
    EXPECT_EQ(ownerNames(ts, "src/buildtargets.cpp"),
              QStringList{"ants_core_lib"});
}
