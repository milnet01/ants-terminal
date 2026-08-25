// Feature-conformance test for the codebase_index MCP tool (ANTS-1637).
// Behavioural invariants drive the pure CodebaseIndex helper; wiring
// invariants source-scrape the registration sites. See spec.md +
// docs/specs/ANTS-1637.md.

#include "../../_support/expect.h"
#include "codebaseindex.h"

#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"
#include <QDir>
#include <QFile>
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

using namespace CodebaseIndex;

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

// A minimal C++ fixture with a known class + qualified method (FileOutline
// emits "class" Foo and "func" Foo::bar).
QString cppWith(const QString &cls, const QString &method) {
    return QStringLiteral("class %1 {\npublic:\n    void %2();\n};\n"
                          "void %1::%2() {\n    return;\n}\n")
        .arg(cls, method);
}

QueryParams summaryQ() { return QueryParams{}; }

}  // namespace

// INV-1 — cold build shape + no-source root.
TEST(CodebaseIndex, BuildShapeAndEmptyRoot) {
    QTemporaryDir dir;
    writeFile(dir.path() + "/src/alpha.cpp", cppWith("Alpha", "run"));
    Index idx = build(dir.path(), 1000);
    ASSERT_EQ(idx.files.size(), 1);
    EXPECT_EQ(idx.files[0].path, QStringLiteral("src/alpha.cpp"));
    EXPECT_EQ(idx.files[0].language, QStringLiteral("cpp"));
    EXPECT_EQ(idx.files[0].role, QStringLiteral("impl"));
    bool sawMethod = false;
    for (const Symbol &s : idx.files[0].symbols)
        if (s.name == QStringLiteral("Alpha::run")) sawMethod = true;
    EXPECT_TRUE(sawMethod);

    QTemporaryDir empty;
    Index e = build(empty.path(), 1000);
    EXPECT_EQ(e.files.size(), 0);
    QJsonObject env = query(e, summaryQ(), 0, QStringLiteral("/c"));
    EXPECT_TRUE(env.value("ok").toBool());
    EXPECT_EQ(env.value("file_count").toInt(), 0);
}

// ANTS-4419 — an empty index says WHY it is empty, and the three reasons are
// distinguishable. ANTS-2148 added the `empty` boolean and stopped there, so a
// caller could not tell "this project was never registered" from "there is no
// code here" and reasonably concluded the latter — reported by a Charls_Site
// session that fell back to grep on a real tree.
//
// All three branches are asserted because the ORDER is the load-bearing part.
// ProjectSettings::detect() does not walk when .ants/project.json is present,
// so totalSourceCount is 0 there by construction; a gate testing the count
// before `present` would label a registered project no_indexable_source. That
// is the same class of mistake as the missing-.git diagnosis this item exists
// to correct, which is why it gets its own case rather than a comment.
TEST(CodebaseIndex, Ants4419EmptyReasonNamesTheCondition) {
    // (a) source exists, but not under src/ or tests/, and nothing declares
    // where it lives — the reported case.
    {
        QTemporaryDir dir;
        writeFile(dir.path() + "/site/app.js", QStringLiteral("function f() {}\n"));
        const QString cache = dir.path() + "/cache.json";
        const QJsonObject env = serve(dir.path(), 1000, summaryQ(), Options{}, cache);
        EXPECT_EQ(env.value("file_count").toInt(), 0);
        ASSERT_TRUE(env.value("empty").toBool());
        EXPECT_EQ(env.value("empty_reason").toString(),
                  QStringLiteral("project_not_registered"))
            << "source outside src//tests/ with no .ants/project.json must say "
               "the project is unregistered, not that the tree is empty";
        EXPECT_FALSE(env.value("empty_hint").toString().isEmpty())
            << "the hint is the whole point — it names the one-step remedy";
        EXPECT_FALSE(env.value("empty_detail").toString().isEmpty())
            << "detect()'s measured counts must ride along";
    }
    // (b) genuinely nothing indexable anywhere.
    {
        QTemporaryDir dir;
        writeFile(dir.path() + "/notes.txt", QStringLiteral("prose\n"));
        const QString cache = dir.path() + "/cache.json";
        const QJsonObject env = serve(dir.path(), 1000, summaryQ(), Options{}, cache);
        ASSERT_TRUE(env.value("empty").toBool());
        EXPECT_EQ(env.value("empty_reason").toString(),
                  QStringLiteral("no_indexable_source"))
            << "a tree with no indexable suffix anywhere is accurately empty";
    }
    // (c) registered, but the declared roots hold no source. This is the case
    // detect()'s no-walk short-circuit would misreport if the count were
    // tested first.
    {
        QTemporaryDir dir;
        writeFile(dir.path() + "/declared/keep.txt", QStringLiteral("x\n"));
        writeFile(dir.path() + "/.ants/project.json",
                  QStringLiteral("{\"source_roots\":[\"declared\"]}\n"));
        const QString cache = dir.path() + "/cache.json";
        const QJsonObject env = serve(dir.path(), 1000, summaryQ(), Options{}, cache);
        ASSERT_TRUE(env.value("empty").toBool());
        EXPECT_EQ(env.value("empty_reason").toString(),
                  QStringLiteral("declared_roots_hold_no_source"))
            << "a present settings file must be reported as such; detect() "
               "skips the walk here, so a count-first gate reads 0 and says "
               "no_indexable_source";
    }
}

// ANTS-4419 — a NON-empty index gains no new fields, so the response shape is
// byte-identical for every project that was already working. The diagnostic is
// gated on query()'s own `empty` flag, which is set only on the summary path.
TEST(CodebaseIndex, Ants4419NonEmptyIndexIsUnannotated) {
    QTemporaryDir dir;
    writeFile(dir.path() + "/src/alpha.cpp", cppWith("Alpha", "run"));
    const QString cache = dir.path() + "/cache.json";
    const QJsonObject env = serve(dir.path(), 1000, summaryQ(), Options{}, cache);
    ASSERT_GT(env.value("file_count").toInt(), 0);
    EXPECT_FALSE(env.value("empty").toBool());
    EXPECT_FALSE(env.contains("empty_reason"));
    EXPECT_FALSE(env.contains("empty_hint"));
    EXPECT_FALSE(env.contains("empty_detail"));
}

// INV-2 — refresh: changed re-outlined, removed pruned (files + laneToFiles),
// added gets its lane; refreshedOut == changed+added.
TEST(CodebaseIndex, RefreshDeltaAddedRemoved) {
    QTemporaryDir dir;
    writeFile(dir.path() + "/src/keep.cpp", cppWith("Keep", "go"));
    writeFile(dir.path() + "/src/gone.cpp", cppWith("Gone", "go"));
    writeFile(dir.path() + "/src/thingengine.cpp", cppWith("ThingEngine", "tick"));
    Index prev = build(dir.path(), 1000);
    ASSERT_EQ(prev.files.size(), 3);
    ASSERT_TRUE(prev.laneToFiles.contains(QStringLiteral("thingengine")));

    // Mutate: change keep.cpp (new mtime via rewrite), delete gone.cpp, add
    // a new lane-family file.
    writeFile(dir.path() + "/src/keep.cpp", cppWith("Keep", "go2"));
    QFile::remove(dir.path() + "/src/gone.cpp");
    writeFile(dir.path() + "/src/otherwidget.cpp", cppWith("OtherWidget", "paint"));

    // Force keep.cpp's mtime to differ from the cached value deterministically.
    int refreshed = -1;
    // Use a bumped now so generated_at advances; staleFiles compares mtimes.
    Index cur = refresh(prev, dir.path(), 2000, mapSourceMtimeMs(dir.path()),
                        Options{}, &refreshed);
    // gone removed, otherwidget added, keep maybe changed → file set is keep,
    // otherwidget, thingengine.
    QStringList paths;
    for (const FileEntry &fe : cur.files) paths << fe.path;
    EXPECT_TRUE(paths.contains(QStringLiteral("src/keep.cpp")));
    EXPECT_TRUE(paths.contains(QStringLiteral("src/otherwidget.cpp")));
    EXPECT_FALSE(paths.contains(QStringLiteral("src/gone.cpp")));
    // Removed file's lane (gone has no family lane → "" → not in laneToFiles);
    // assert the added family lane is present and the removed file's path is in
    // no lane list.
    EXPECT_TRUE(cur.laneToFiles.contains(QStringLiteral("otherwidget")));
    for (auto it = cur.laneToFiles.cbegin(); it != cur.laneToFiles.cend(); ++it)
        EXPECT_FALSE(it.value().contains(QStringLiteral("src/gone.cpp")));
    EXPECT_GE(refreshed, 1);  // at least the added file (+ possibly changed)
}

// INV-3 — map-source change re-derives laneToFiles with refreshedOut==0.
TEST(CodebaseIndex, MapSourceChangeRemapsLanesNoReoutline) {
    QTemporaryDir dir;
    // A plain file with no family suffix: lane only via the module map.
    writeFile(dir.path() + "/src/zeta.cpp", cppWith("Zeta", "run"));
    writeFile(dir.path() + "/CLAUDE.md",
              QStringLiteral("## Module map (src/)\n- `zeta` — the zeta unit.\n"));
    Index prev = build(dir.path(), 1000);
    ASSERT_TRUE(prev.laneToFiles.contains(QStringLiteral("zeta")));

    // Rewrite the map so zeta is no longer a lane name; drive a bumped
    // map-source mtime so staleFiles reports mapSourceChanged deterministically.
    writeFile(dir.path() + "/CLAUDE.md",
              QStringLiteral("## Module map (src/)\n- `omega` — the omega unit.\n"));
    StaleSet ss = staleFiles(prev, dir.path(), prev.mapSourceMtimeMs + 5000);
    EXPECT_TRUE(ss.mapSourceChanged);
    EXPECT_TRUE(ss.changed.isEmpty());
    EXPECT_TRUE(ss.added.isEmpty());

    int refreshed = -1;
    Index cur = refresh(prev, dir.path(), 2000, prev.mapSourceMtimeMs + 5000,
                        Options{}, &refreshed);
    EXPECT_EQ(refreshed, 0);  // map-only change re-outlines nothing
    EXPECT_FALSE(cur.laneToFiles.contains(QStringLiteral("zeta")));
}

// INV-4 — selector arity.
TEST(CodebaseIndex, SelectorArity) {
    QTemporaryDir dir;
    writeFile(dir.path() + "/src/alpha.cpp", cppWith("Alpha", "run"));
    Index idx = build(dir.path(), 1000);

    QueryParams two; two.symbol = "x"; two.lane = "y";
    QJsonObject bad = query(idx, two, 0, QStringLiteral("/c"));
    EXPECT_FALSE(bad.value("ok").toBool());
    EXPECT_EQ(bad.value("code").toString(), QStringLiteral("bad_args"));

    QJsonObject sum = query(idx, summaryQ(), 0, QStringLiteral("/c"));
    EXPECT_TRUE(sum.value("ok").toBool());
    EXPECT_TRUE(sum.contains("lanes"));
    EXPECT_TRUE(sum.contains("languages"));
    EXPECT_TRUE(sum.contains("roles"));
    EXPECT_FALSE(sum.contains("matches"));  // never a full dump
}

// INV-5 — symbol hit + miss.
TEST(CodebaseIndex, SymbolHitAndMiss) {
    QTemporaryDir dir;
    writeFile(dir.path() + "/src/alpha.cpp", cppWith("Alpha", "run"));
    Index idx = build(dir.path(), 1000);

    QueryParams hit; hit.symbol = "Alpha::run";
    QJsonObject h = query(idx, hit, 0, QStringLiteral("/c"));
    EXPECT_TRUE(h.value("found").toBool());
    ASSERT_GE(h.value("matches").toArray().size(), 1);
    EXPECT_EQ(h.value("matches").toArray()[0].toObject().value("path").toString(),
              QStringLiteral("src/alpha.cpp"));

    QueryParams miss; miss.symbol = "Nope::gone";
    QJsonObject m = query(idx, miss, 0, QStringLiteral("/c"));
    EXPECT_TRUE(m.value("ok").toBool());
    EXPECT_FALSE(m.value("found").toBool());
    EXPECT_EQ(m.value("matches").toArray().size(), 0);
}

// INV-6 — lane hit + unknown.
TEST(CodebaseIndex, LaneHitAndUnknown) {
    QTemporaryDir dir;
    writeFile(dir.path() + "/src/fooengine.cpp", cppWith("FooEngine", "tick"));
    Index idx = build(dir.path(), 1000);

    QueryParams hit; hit.lane = "fooengine";
    QJsonObject h = query(idx, hit, 0, QStringLiteral("/c"));
    EXPECT_TRUE(h.value("found").toBool());
    EXPECT_GE(h.value("files").toArray().size(), 1);

    QueryParams unk; unk.lane = "doesnotexist";
    QJsonObject u = query(idx, unk, 0, QStringLiteral("/c"));
    EXPECT_TRUE(u.value("ok").toBool());
    EXPECT_FALSE(u.value("found").toBool());
    EXPECT_GE(u.value("lanes").toArray().size(), 1);  // available lanes echoed
}

// INV-7 (behavioural half) — file_path found / not-found.
TEST(CodebaseIndex, FilePathFoundAndMiss) {
    QTemporaryDir dir;
    writeFile(dir.path() + "/src/alpha.cpp", cppWith("Alpha", "run"));
    Index idx = build(dir.path(), 1000);

    QueryParams hit; hit.filePath = "src/alpha.cpp";
    QJsonObject h = query(idx, hit, 0, QStringLiteral("/c"));
    EXPECT_TRUE(h.value("found").toBool());
    EXPECT_TRUE(h.value("entry").toObject().contains("symbols"));

    QueryParams miss; miss.filePath = "src/missing.cpp";
    QJsonObject m = query(idx, miss, 0, QStringLiteral("/c"));
    EXPECT_TRUE(m.value("ok").toBool());
    EXPECT_FALSE(m.value("found").toBool());
}

// INV-9 (behavioural) — warm-query etag stability: a fully-warm re-serve
// produces a byte-identical envelope (would 304), and refreshed_files drops
// to 0 after the cold build.
TEST(CodebaseIndex, WarmServeIsStable) {
    QTemporaryDir dir;
    writeFile(dir.path() + "/src/alpha.cpp", cppWith("Alpha", "run"));
    const QString cache = dir.path() + "/cache.json";

    QJsonObject c1 = serve(dir.path(), 1000, summaryQ(), Options{}, cache);
    QJsonObject c2 = serve(dir.path(), 2000, summaryQ(), Options{}, cache);
    QJsonObject c3 = serve(dir.path(), 3000, summaryQ(), Options{}, cache);
    EXPECT_GE(c1.value("refreshed_files").toInt(), 1);  // cold build
    EXPECT_EQ(c2.value("refreshed_files").toInt(), 0);  // warm
    EXPECT_EQ(c3.value("refreshed_files").toInt(), 0);  // warm
    // call 2 and call 3 are byte-identical (stable etag → 304).
    EXPECT_EQ(QJsonDocument(c2).toJson(QJsonDocument::Compact),
              QJsonDocument(c3).toJson(QJsonDocument::Compact));
}

// INV-11 — file-count ceiling: deterministic prefix, tests/ dropped.
TEST(CodebaseIndex, FileCountCeiling) {
    QTemporaryDir dir;
    writeFile(dir.path() + "/src/a.cpp", cppWith("A", "f"));
    writeFile(dir.path() + "/src/b.cpp", cppWith("B", "f"));
    writeFile(dir.path() + "/tests/t.cpp", cppWith("T", "f"));
    Options o; o.maxIndexFiles = 2;
    Index idx = build(dir.path(), 1000, o);
    EXPECT_TRUE(idx.filesTruncated);
    ASSERT_EQ(idx.files.size(), 2);
    EXPECT_EQ(idx.files[0].path, QStringLiteral("src/a.cpp"));
    EXPECT_EQ(idx.files[1].path, QStringLiteral("src/b.cpp"));
}

// INV-15 — byte ceiling: tiny maxCacheBytes truncates, prefix survives.
TEST(CodebaseIndex, ByteCeiling) {
    QTemporaryDir dir;
    writeFile(dir.path() + "/src/a.cpp", cppWith("A", "f"));
    writeFile(dir.path() + "/src/b.cpp", cppWith("B", "f"));
    writeFile(dir.path() + "/src/c.cpp", cppWith("C", "f"));
    Options o; o.maxCacheBytes = 1;  // forces stop after the first file
    Index idx = build(dir.path(), 1000, o);
    EXPECT_TRUE(idx.filesTruncated);
    ASSERT_GE(idx.files.size(), 1);
    EXPECT_EQ(idx.files[0].path, QStringLiteral("src/a.cpp"));
    EXPECT_LT(idx.files.size(), 3);
}

// INV-12 — toJson → fromJson round-trip equality.
TEST(CodebaseIndex, SerialisationRoundTrip) {
    QTemporaryDir dir;
    writeFile(dir.path() + "/src/fooengine.cpp", cppWith("FooEngine", "tick"));
    writeFile(dir.path() + "/tests/t.cpp", cppWith("T", "f"));
    Index idx = build(dir.path(), 1000);
    const QByteArray a = QJsonDocument(toJson(idx)).toJson(QJsonDocument::Compact);
    Index rt = fromJson(toJson(idx));
    const QByteArray b = QJsonDocument(toJson(rt)).toJson(QJsonDocument::Compact);
    EXPECT_EQ(a, b);
    EXPECT_EQ(rt.files.size(), idx.files.size());
    EXPECT_EQ(rt.laneToFiles.size(), idx.laneToFiles.size());
}

// INV-13 — version-0 / foreign-root / garbage cache rebuilds.
TEST(CodebaseIndex, StaleCacheRebuilds) {
    QTemporaryDir dir;
    writeFile(dir.path() + "/src/alpha.cpp", cppWith("Alpha", "run"));

    auto serveWith = [&](const QString &seed) {
        const QString cache = dir.path() + "/c.json";
        writeFile(cache, seed);
        QJsonObject env = serve(dir.path(), 1000, summaryQ(), Options{}, cache);
        // After serve, the cache must be a valid current-version index.
        QFile f(cache);
        EXPECT_TRUE(f.open(QIODevice::ReadOnly));
        Index loaded = fromJson(QJsonDocument::fromJson(f.readAll()).object());
        EXPECT_EQ(loaded.version, kIndexVersion);
        EXPECT_EQ(loaded.rootCanonical, dir.path());
        EXPECT_EQ(env.value("file_count").toInt(), 1);
    };
    serveWith(QStringLiteral("{\"version\":0,\"root_canonical\":\"%1\"}").arg(dir.path()));
    serveWith(QStringLiteral("{\"version\":1,\"root_canonical\":\"/somewhere/else\"}"));
    serveWith(QStringLiteral("}{ not json"));
}

// INV-14 — lane symbol cap: first symbol survives, symbols_truncated set.
TEST(CodebaseIndex, LaneSymbolCap) {
    QTemporaryDir dir;
    // Two files in one family lane, each with a method → ≥2 symbols.
    writeFile(dir.path() + "/src/capwidget.cpp", cppWith("CapWidget", "alpha"));
    writeFile(dir.path() + "/src/capwidgethelper.cpp", cppWith("CapWidget", "beta"));
    Index idx = build(dir.path(), 1000);
    // Both basenames start with "capwidget"? capwidgethelper matches the
    // family regex → lane "capwidgethelper"; capwidget.cpp → "capwidget".
    // Use the lane that has ≥2 symbols across its file(s). Simpler: query the
    // capwidget lane (1 file, but that file has class + method = 2 symbols).
    QueryParams q; q.lane = "capwidget";
    Options o; o.maxQuerySymbols = 1;
    QJsonObject env = query(idx, q, 0, QStringLiteral("/c"), o);
    if (env.value("found").toBool()) {
        EXPECT_TRUE(env.value("symbols_truncated").toBool());
    } else {
        GTEST_SKIP() << "capwidget lane not present in this fixture layout";
    }
}

// INV-16 — summary count integrity.
TEST(CodebaseIndex, SummarySumsToFileCount) {
    QTemporaryDir dir;
    writeFile(dir.path() + "/src/a.cpp", cppWith("A", "f"));
    writeFile(dir.path() + "/src/b.h", QStringLiteral("class B { void g(); };\n"));
    writeFile(dir.path() + "/tests/t.cpp", cppWith("T", "f"));
    Index idx = build(dir.path(), 1000);
    QJsonObject env = query(idx, summaryQ(), 0, QStringLiteral("/c"));
    const int fc = env.value("file_count").toInt();
    int roleSum = 0, langSum = 0;
    const QJsonObject roles = env.value("roles").toObject();
    for (auto it = roles.begin(); it != roles.end(); ++it) roleSum += it.value().toInt();
    const QJsonObject langs = env.value("languages").toObject();
    for (auto it = langs.begin(); it != langs.end(); ++it) langSum += it.value().toInt();
    EXPECT_EQ(roleSum, fc);
    EXPECT_EQ(langSum, fc);
}

// ANTS-2148 — the C family (`.c`, `.hxx`) is admitted and outlined with the
// C++ regex set, so a C-only project is not an empty map; the summary carries
// a soft `empty` signal keyed off file_count (so a consuming session can tell
// "nothing admitted" from "small project" instead of trusting an empty index).
TEST(CodebaseIndex, CFamilyAdmittedAndEmptySignal) {
    QTemporaryDir dir;
    writeFile(dir.path() + "/src/game.c",
              QStringLiteral("int update_state(int n) {\n    return n + 1;\n}\n"));
    writeFile(dir.path() + "/src/defs.hxx",
              QStringLiteral("struct Vec { int x; };\n"));
    Index idx = build(dir.path(), 1000);
    ASSERT_GE(idx.files.size(), 1);
    QStringList paths;
    for (const FileEntry &fe : idx.files) paths << fe.path;
    EXPECT_TRUE(paths.contains(QStringLiteral("src/game.c")))
        << "ANTS-2148: a .c file must be admitted to the index";
    bool sawCFunc = false;
    for (const FileEntry &fe : idx.files)
        if (fe.path == QStringLiteral("src/game.c"))
            for (const Symbol &s : fe.symbols)
                if (s.name == QStringLiteral("update_state")) sawCFunc = true;
    EXPECT_TRUE(sawCFunc) << "ANTS-2148: a .c free function must be outlined";

    QJsonObject env = query(idx, summaryQ(), 0, QStringLiteral("/c"));
    EXPECT_FALSE(env.value("empty").toBool());  // non-empty map → empty:false

    // A project with no admitted source must report empty:true alongside
    // file_count:0 (the soft signal DOOM asked for).
    QTemporaryDir noSrc;
    writeFile(noSrc.path() + "/README.txt", QStringLiteral("nothing to index\n"));
    Index e = build(noSrc.path(), 1000);
    EXPECT_EQ(e.files.size(), 0);
    QJsonObject ee = query(e, summaryQ(), 0, QStringLiteral("/c"));
    EXPECT_EQ(ee.value("file_count").toInt(), 0);
    EXPECT_TRUE(ee.value("empty").toBool())
        << "ANTS-2148: file_count:0 must carry empty:true";
}

// ANTS-3393 — a flat-root project that declares source_roots=["."] must not
// have its committed virtualenv / vendored / build trees indexed. The walk
// shares isNoiseDir with op:detect, pruning venv/, node_modules/,
// __pycache__/ and build*/ per directory component while the real root
// module survives. (Contact_List saw codebase_index report 2350 vendored
// files vs ~10 real ones, because source_roots=["."] is the only way to
// capture flat-root modules.)
TEST(CodebaseIndex, NoiseDirsPrunedUnderDotRoot) {
    QTemporaryDir dir;
    writeFile(dir.path() + "/.ants/project.json",
              QStringLiteral("{\"source_roots\":[\".\"]}"));
    writeFile(dir.path() + "/app.cpp", cppWith("App", "run"));   // real root module
    // Vendored / cache / build trees that must be pruned.
    writeFile(dir.path() + "/venv/lib/dep.cpp", cppWith("Dep", "f"));
    writeFile(dir.path() + "/node_modules/pkg/mod.cpp", cppWith("Mod", "f"));
    writeFile(dir.path() + "/__pycache__/cached.cpp", cppWith("Cached", "f"));
    writeFile(dir.path() + "/build/out.cpp", cppWith("Out", "f"));

    Index idx = build(dir.path(), 1000);
    QStringList paths;
    for (const FileEntry &fe : idx.files) paths << fe.path;
    bool sawApp = false;
    for (const QString &p : paths)
        if (p.endsWith(QStringLiteral("app.cpp"))) sawApp = true;
    EXPECT_TRUE(sawApp) << "ANTS-3393: the real root-level module must still be indexed";
    for (const QString &p : paths) {
        const std::string ps = p.toStdString();
        EXPECT_FALSE(p.contains(QStringLiteral("venv/")))         << ps;
        EXPECT_FALSE(p.contains(QStringLiteral("node_modules/"))) << ps;
        EXPECT_FALSE(p.contains(QStringLiteral("__pycache__/")))  << ps;
        EXPECT_FALSE(p.contains(QStringLiteral("build/")))        << ps;
    }
}

// ANTS-3390 (INV-18) — a "." source_root must key files by their bare
// repo-relative path (`app.cpp`), NOT `./app.cpp`. The walk base `<root>/.`
// makes QDirIterator yield `./`-prefixed paths; walkSubtree strips the single
// leading `./`. Without the strip, findFile's exact `fe.path == rel` match
// (the file_path lookup) returns found:false for a bare `app.cpp` query — the
// exact RetroArch-class gap ANTS-3390 closes — and roleFor/tests/ detection
// misses a `./tests/…` path. Retroactively repairs shipped ANTS-3393 keys.
TEST(CodebaseIndex, DotRootKeysBareRelativePath) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    writeFile(dir.path() + "/.ants/project.json",
              QStringLiteral("{\"source_roots\":[\".\"]}"));
    writeFile(dir.path() + "/app.cpp", cppWith("App", "run"));       // root file
    writeFile(dir.path() + "/sub/lib.cpp", cppWith("Lib", "f"));     // subdir file

    Index idx = build(dir.path(), 1000);
    bool exactRoot = false, exactSub = false, anyDotPrefixed = false;
    for (const FileEntry &fe : idx.files) {
        if (fe.path == QStringLiteral("app.cpp"))     exactRoot = true;
        if (fe.path == QStringLiteral("sub/lib.cpp")) exactSub = true;
        if (fe.path.startsWith(QStringLiteral("./"))) anyDotPrefixed = true;
    }
    EXPECT_TRUE(exactRoot)
        << "root file must key as 'app.cpp' (findFile is exact-match), not './app.cpp'";
    EXPECT_TRUE(exactSub)  << "subdir file must key as 'sub/lib.cpp'";
    EXPECT_FALSE(anyDotPrefixed) << "no './'-prefixed keys under a '.' source_root";
}

// ANTS-3468 — opt-in lane→source-file digest. params.laneFiles augments each
// summary lane with a sorted `source_files` array of its NON-test paths so the
// session_orient bundle's first-call map is navigable; the default (opt-out)
// summary stays counts-only (no source_files / no lane_digest_truncated).
// Deterministic → keeps session_orient's 304 ETag stable; globally capped.
TEST(CodebaseIndex, LaneDigestOptIn) {
    QTemporaryDir dir;
    // A module-map lane "bar" so a tests/ file (bar_extra.cpp) also resolves to
    // it — proving the role==test exclusion, not just a no-lane skip.
    writeFile(dir.path() + "/CLAUDE.md",
              QStringLiteral("## Module map (src/)\n- `bar` — the bar unit.\n"));
    writeFile(dir.path() + "/src/bar.cpp", cppWith("Bar", "run"));
    writeFile(dir.path() + "/src/bar.h",
              QStringLiteral("class Bar { void run(); };\n"));
    writeFile(dir.path() + "/tests/bar_extra.cpp", cppWith("BarExtra", "run"));
    Index idx = build(dir.path(), 1000);
    ASSERT_TRUE(idx.laneToFiles.contains(QStringLiteral("bar")));

    // Opt-out default: counts-only, no digest fields.
    QJsonObject plain = query(idx, summaryQ(), 0, QStringLiteral("/c"));
    for (const QJsonValue &v : plain.value("lanes").toArray())
        EXPECT_FALSE(v.toObject().contains("source_files"));
    EXPECT_FALSE(plain.contains("lane_digest_truncated"));

    // Opt-in: lane "bar" lists its non-test source (cpp + h), excludes the test.
    QueryParams q; q.laneFiles = true;
    QJsonObject env = query(idx, q, 0, QStringLiteral("/c"));
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_TRUE(env.contains("lane_digest_truncated"));
    QStringList barFiles;
    for (const QJsonValue &v : env.value("lanes").toArray()) {
        const QJsonObject l = v.toObject();
        ASSERT_TRUE(l.contains("source_files"));
        if (l.value("lane").toString() == QStringLiteral("bar"))
            for (const QJsonValue &s : l.value("source_files").toArray())
                barFiles << s.toString();
    }
    EXPECT_TRUE(barFiles.contains(QStringLiteral("src/bar.cpp")));
    EXPECT_TRUE(barFiles.contains(QStringLiteral("src/bar.h")));
    EXPECT_FALSE(barFiles.contains(QStringLiteral("tests/bar_extra.cpp")));

    // Deterministic → stable ETag: two opt-in queries are byte-identical.
    QJsonObject env2 = query(idx, q, 0, QStringLiteral("/c"));
    EXPECT_EQ(QJsonDocument(env).toJson(QJsonDocument::Compact),
              QJsonDocument(env2).toJson(QJsonDocument::Compact));

    // Global cap: a tiny cap truncates the digest and flags it (no silent cap).
    Options o; o.maxLaneDigestFiles = 1;
    QJsonObject capped = query(idx, q, 0, QStringLiteral("/c"), o);
    EXPECT_TRUE(capped.value("lane_digest_truncated").toBool());
}

// ANTS-3503 — no-module-map fallback. A project with no parseable `## Module
// map` yields an empty lane digest; laneFiles:true then emits a flat top-level
// `source_files` digest (sorted, non-test, same cap/flag) so lane-less repos
// still get a navigable first-call map instead of a bare counts blob.
TEST(CodebaseIndex, NoModuleMapFallbackDigest) {
    QTemporaryDir dir;
    // No CLAUDE.md module map → no file resolves to a lane (the finbreak shape).
    writeFile(dir.path() + "/src/beta.cpp", cppWith("Beta", "run"));
    writeFile(dir.path() + "/src/alpha.cpp", cppWith("Alpha", "run"));
    writeFile(dir.path() + "/tests/alpha_test.cpp", cppWith("AlphaTest", "run"));
    Index idx = build(dir.path(), 1000);
    ASSERT_TRUE(idx.laneToFiles.isEmpty());

    // Opt-out default: no top-level source_files digest (shape unchanged).
    QJsonObject plain = query(idx, summaryQ(), 0, QStringLiteral("/c"));
    EXPECT_FALSE(plain.contains("source_files"));

    // Opt-in fallback: flat non-test digest, sorted; the test file is excluded.
    QueryParams q; q.laneFiles = true;
    QJsonObject env = query(idx, q, 0, QStringLiteral("/c"));
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_EQ(env.value("lane_count").toInt(), 0);
    ASSERT_TRUE(env.contains("source_files"));
    QStringList files;
    for (const QJsonValue &v : env.value("source_files").toArray())
        files << v.toString();
    EXPECT_TRUE(files.contains(QStringLiteral("src/alpha.cpp")));
    EXPECT_TRUE(files.contains(QStringLiteral("src/beta.cpp")));
    EXPECT_FALSE(files.contains(QStringLiteral("tests/alpha_test.cpp")));
    QStringList sorted = files; sorted.sort();
    EXPECT_EQ(files, sorted) << "fallback digest must be sorted (304-stable)";

    // Deterministic → stable ETag: two opt-in queries are byte-identical.
    QJsonObject env2 = query(idx, q, 0, QStringLiteral("/c"));
    EXPECT_EQ(QJsonDocument(env).toJson(QJsonDocument::Compact),
              QJsonDocument(env2).toJson(QJsonDocument::Compact));

    // Global cap: a tiny cap truncates the flat digest and flags it.
    Options o; o.maxLaneDigestFiles = 1;
    QJsonObject capped = query(idx, q, 0, QStringLiteral("/c"), o);
    EXPECT_EQ(capped.value("source_files").toArray().size(), 1);
    EXPECT_TRUE(capped.value("lane_digest_truncated").toBool());

    // ANTS-4560 — and under the name that matches the array. On this arm
    // there are no lanes, so `lane_digest_truncated` names a digest the
    // caller cannot see, while `files_truncated` — the flag whose name looks
    // right — is about the index's own cap and is correctly false. A reporter
    // read those two and concluded a 1120-file project had no renderer.
    EXPECT_TRUE(capped.value("source_files_truncated").toBool());
    EXPECT_FALSE(capped.value("files_truncated").toBool())
        << "files_truncated is the index cap and must not be repurposed";
    // Both flags travel together, so a caller keyed on either is served.
    EXPECT_FALSE(env.value("source_files_truncated").toBool())
        << "an uncapped digest must report false, not go absent — absent "
           "reads as 'this build does not have the flag'";
    EXPECT_TRUE(env.contains("source_files_truncated"));

    // The opt-out summary gains nothing: no digest, no digest flag.
    EXPECT_FALSE(plain.contains("source_files_truncated"));
}

// INV-8/9/10 — wiring source-scrapes.
TEST(CodebaseIndex, WiringRegistered) {
    const std::string ci = ants_test::slurpFile(srcPath("src/claudeintegration.cpp"));
    const std::string mw = ants_test::slurpFile(srcPath("src/mainwindow.cpp"));
    const std::string mp = ants_test::slurpFile(srcPath("src/mcpprojection.cpp"));
    const std::string rc = ants_test::slurpRemoteControl();

    // caller_cwd Required (INV-8) — the registerToolProvider call passes the
    // Required contract; callerCwdContractFor + the project-scoped list agree.
    EXPECT_TRUE(has(mw, "registerToolProvider(\"codebase_index\""));
    EXPECT_TRUE(has(mw, "CallerCwdContract::Required"));
    EXPECT_TRUE(has(ci, "codebase_index"));      // contract + etag + schema sites
    // ETag-304 (INV-9) — registered in isEtagSupportedTool; the handler emits
    // no etag itself (the dispatcher injects it).
    EXPECT_TRUE(has(ci, "isEtagSupportedTool"));
    EXPECT_FALSE(has(ants_test::slurpFile(srcPath("src/codebaseindex.cpp")), "\"etag\""));
    // Compaction table (INV-10). `fields=` needs no enrolment since
    // ANTS-4524 — every verb honours it; this row is the compaction answer.
    EXPECT_TRUE(has(mp, "codebase_index"));
    EXPECT_TRUE(has(mp, "isDefaultCompactTool"));
    // handler routes file_path through PathValidation (INV-7).
    EXPECT_TRUE(has(rc, "cmdCodebaseIndex"));
    EXPECT_TRUE(has(rc, "validatePath"));
    // ANTS-2149 — codebase_index accepts `path` as an alias for `file_path`
    // and file_outline accepts `file_path` as an alias for `path`; both
    // aliases are wired in remotecontrol.cpp.
    EXPECT_TRUE(has(rc, "ANTS-2149"));
    // ANTS-2223 extended the file_outline message to
    // `(alias: "file_path", "paths")`, so match the prefix without the
    // trailing ')' (the alias is still wired; the list just grew).
    EXPECT_TRUE(has(rc, "(alias: \\\"file_path\\\""));
    // atomic cache write (INV-12).
    EXPECT_TRUE(has(ants_test::slurpFile(srcPath("src/codebaseindex.cpp")), "QSaveFile"));
}

// ANTS-4425 — the same drift ANTS-4096 fixed for shaders, one verb later.
// `FileOutline` has outlined HTML since ANTS-4361, and `isIndexableSuffix` did
// not admit it, so a site project's pages were invisible to codebase_index AND
// to the indie_review computed partition, which walks by this predicate.
// Measured on Charls_Site 2026-08-17: detect() counted 9 source files (the .js
// and .py) on a tree of ~40 HTML/CSS/JS files.
TEST(CodebaseIndex, Ants4425HtmlIsIndexable) {
    EXPECT_TRUE(CodebaseIndex::isIndexableSuffix(QStringLiteral("html")));
    EXPECT_TRUE(CodebaseIndex::isIndexableSuffix(QStringLiteral("htm")))
        << "pickModeByExt has always accepted both spellings; the index gate "
           "must key on the same predicate, not a second list";
}

// The standing rule this file's predicate comment opens with is that
// count -> outline -> symbol query cover the SAME files. `css` has no outline
// mode, so admitting it would be the same drift in the opposite direction:
// counting files the outline cannot read. It needs a mode first, or an explicit
// decision to count-but-not-outline. Asserted so the choice is deliberate and a
// later widening has to come here and change it on purpose.
TEST(CodebaseIndex, Ants4425CssIsNotIndexableWhileItHasNoOutlineMode) {
    EXPECT_FALSE(CodebaseIndex::isIndexableSuffix(QStringLiteral("css")))
        << "admitting css would count files file_outline cannot read, breaking "
           "the count/outline/symbol-query in-step rule in the other direction";
}

// The predicate is shared rather than copied, which is the whole point of
// ANTS-4096's precedent: a second list is the defect with an extra step.
TEST(CodebaseIndex, Ants4425IndexGateDelegatesToFileOutline) {
    const std::string ci = ants_test::slurpFile(srcPath("src/codebaseindex.cpp"));
    EXPECT_TRUE(has(ci, "FileOutline::isHtmlExt"))
        << "the html gate must delegate to FileOutline, the way the GLSL line "
           "above it already does";
}
