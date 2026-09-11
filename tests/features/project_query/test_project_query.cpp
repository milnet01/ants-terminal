// Feature-conformance test for ANTS-2093 — project_query: a sandboxed
// server-side read-only Lua query verb. The sandbox/marshaller/confinement
// invariants drive LuaEngine::runQuery / projectQueryVerb directly against
// QTemporaryDir fixtures; the verb-glue + registration invariants
// source-scrape the handler/wiring. See spec.md + docs/specs/ANTS-2093.md.

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"

#include <string>

#include <gtest/gtest.h>
#include <QString>

#ifdef ANTS_LUA_PLUGINS
#include "luaengine.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QTemporaryDir>
#endif

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
}  // namespace

// ===================================================================
// Wiring (always compiled — pure source-scrape, no Lua dependency).
// ===================================================================
TEST(ProjectQuery, RegistrationWiring) {
    const std::string mw = ants_test::slurpFile(srcPath("src/mainwindow.cpp"));
    const std::string ci = ants_test::slurpFile(srcPath("src/claudeintegration.cpp"));
    const std::string mp = ants_test::slurpFile(srcPath("src/mcpprojection.cpp"));
    const std::string cf = ants_test::slurpFile(srcPath("src/config.cpp"));

    // Provider registered (Required) in mainwindow, under the Lua guard.
    EXPECT_TRUE(has(mw, "registerToolProvider(\"project_query\""));
    // Required caller_cwd contract in claudeintegration.
    EXPECT_TRUE(has(ci, "if (toolName == QStringLiteral(\"project_query\"))"));
    EXPECT_TRUE(has(ci, "C::Required"));
    // Catalogue lists the verb (under #ifdef ANTS_LUA_PLUGINS).
    EXPECT_TRUE(has(ci, "pqTool[\"name\"] = \"project_query\""));
    EXPECT_TRUE(has(ci, "#ifdef ANTS_LUA_PLUGINS"));
    // INV-9 — added to the offload-eligible set.
    EXPECT_TRUE(has(mp, "QStringLiteral(\"project_query\")"));
    // Config gate, default true.
    EXPECT_TRUE(has(cf, "claude.mcp_project_query_enabled"));
}

#ifdef ANTS_LUA_PLUGINS

namespace {

LuaEngine::QueryResult run(const QString &code, const QString &root,
                           qint64 timeoutMs = 1500, int cap = 65536) {
    LuaEngine eng;
    return eng.runQuery(code, root, timeoutMs, cap);
}

// A throwaway root that exists (the snippet doesn't touch the FS).
QString anyRoot() { return QDir::tempPath(); }

}  // namespace

// ---- INV-6 — marshalling, one case per §2.4 row ----
TEST(ProjectQuery, MarshalScalars) {
    const QString r = anyRoot();
    EXPECT_TRUE(run("return nil", r).result.isNull());
    EXPECT_TRUE(run("return true", r).result.toBool());
    EXPECT_FALSE(run("return false", r).result.toBool(true));
    EXPECT_EQ(run("return 42", r).result.toInteger(), 42);
    EXPECT_DOUBLE_EQ(run("return 1.5", r).result.toDouble(), 1.5);
    EXPECT_EQ(run("return 'hi'", r).result.toString(), QStringLiteral("hi"));
    // no explicit return -> null (a clean no-return, NOT query_error)
    const auto noret = run("local x = 1", r);
    EXPECT_TRUE(noret.ok);
    EXPECT_TRUE(noret.result.isNull());
}

TEST(ProjectQuery, MarshalTables) {
    const QString r = anyRoot();
    const auto arr = run("return {10, 20, 30}", r);
    ASSERT_TRUE(arr.ok);
    ASSERT_TRUE(arr.result.isArray());
    EXPECT_EQ(arr.result.toArray().size(), 3);
    EXPECT_EQ(arr.result.toArray().at(1).toInteger(), 20);

    const auto obj = run("return {a = 1, b = 'two'}", r);
    ASSERT_TRUE(obj.ok);
    ASSERT_TRUE(obj.result.isObject());
    EXPECT_EQ(obj.result.toObject().value("b").toString(), QStringLiteral("two"));
}

TEST(ProjectQuery, MarshalRejections) {
    const QString r = anyRoot();
    EXPECT_EQ(run("return function() end", r).code, QStringLiteral("query_error"));
    // invalid UTF-8 (0xFF 0xFE are never valid leading bytes)
    EXPECT_EQ(run("return string.char(0xff, 0xfe)", r).code,
              QStringLiteral("query_error"));
    // non-string key
    EXPECT_EQ(run("local t = {} t[true] = 1 return t", r).code,
              QStringLiteral("query_error"));
}

TEST(ProjectQuery, DepthBoundary) {
    const QString r = anyRoot();
    // 32 nested tables succeed (the boundary)…
    EXPECT_TRUE(run("local t = 0 for i = 1, 32 do t = {t} end return t", r).ok);
    // …33 refuse.
    EXPECT_EQ(run("local t = 0 for i = 1, 33 do t = {t} end return t", r).code,
              QStringLiteral("query_error"));
    // circular table caught by the same bound (not a cycle check).
    EXPECT_EQ(run("local t = {} t.self = t return t", r).code,
              QStringLiteral("query_error"));
}

// ---- INV-1 — no write / side-effect surface ----
TEST(ProjectQuery, NoWriteSurface) {
    const QString r = anyRoot();
    EXPECT_EQ(run("return type(ants)", r).result.toString(), QStringLiteral("nil"));
    EXPECT_EQ(run("return type(os)", r).result.toString(), QStringLiteral("nil"));
    EXPECT_EQ(run("return type(io)", r).result.toString(), QStringLiteral("nil"));
    EXPECT_EQ(run("return type(require)", r).result.toString(), QStringLiteral("nil"));
    EXPECT_EQ(run("return type(load)", r).result.toString(), QStringLiteral("nil"));
    // calling into a nil'd global raises -> query_error
    EXPECT_EQ(run("os.execute('true') return 1", r).code, QStringLiteral("query_error"));
}

// ---- INV-5 — fresh VM per call, no state bleed ----
TEST(ProjectQuery, NoStateBleed) {
    const QString r = anyRoot();
    LuaEngine eng;
    const auto a = eng.runQuery("g_leak = 5 return g_leak", r, 1500, 65536);
    EXPECT_EQ(a.result.toInteger(), 5);
    const auto b = eng.runQuery("return g_leak", r, 1500, 65536);
    EXPECT_TRUE(b.ok);
    EXPECT_TRUE(b.result.isNull());  // fresh _G — the global is gone
}

// ---- INV-4 — budget kills ----
TEST(ProjectQuery, TimeoutAndOom) {
    const QString r = anyRoot();
    // short budget so the wall-clock hook trips quickly
    EXPECT_EQ(run("while true do end", r, /*timeoutMs=*/200).code,
              QStringLiteral("query_timeout"));
    // allocator-buster trips the 10 MiB cap (the refusal, not a nil result)
    EXPECT_EQ(run("local t = {} local i = 1 "
                  "while true do t[i] = string.rep('x', 4096) i = i + 1 end", r).code,
              QStringLiteral("query_oom"));
}

// ---- INV-7 — output cap (no partial) ----
TEST(ProjectQuery, OutputCap) {
    const QString r = anyRoot();
    const auto big = run("return string.rep('x', 100000)", r, 1500, /*cap=*/65536);
    EXPECT_EQ(big.code, QStringLiteral("result_too_large"));
    EXPECT_FALSE(big.ok);
    EXPECT_TRUE(big.result.isNull());  // no value emitted
}

// ---- INV-10 — marshal budget bounds visited-node count (ANTS-5069) ----
// Why this exists: the marshal used to visit every value reachable by PATH
// before ever checking resultCapBytes. A table that shares one sub-table
// down both of its branches has few distinct values but exponentially many
// paths, so a snippet a few tables deep inside the (10 MiB-capped) Lua VM
// could still make the C++ marshal allocate millions of JSON nodes before
// the byte-size check ran — the watchdog can't interrupt it because the
// query already returned; only the marshal step is unbounded. The fix must
// charge each value's approximate serialised size against resultCapBytes as
// it walks, refusing with result_too_large as soon as the budget is spent,
// so marshalNodes stays bounded by the budget regardless of sharing.
namespace {
// "A few KiB" — small enough that even a bounded marshal visits only a few
// thousand values.
constexpr int kBudgetCapBytes = 4096;
// Every visited value costs at least one serialised byte, so a bounded
// marshal cannot visit more values than the budget allows. This multiplier
// is deliberately generous — it exists so the guard isn't flaky against
// whatever per-node charging the fix uses, not to pin an exact scheme.
constexpr qint64 kBudgetSlackMultiplier = 8;
constexpr qint64 kBudgetSlackAdditive = 256;
qint64 budgetBound(int capBytes) {
    return static_cast<qint64>(capBytes) * kBudgetSlackMultiplier + kBudgetSlackAdditive;
}
}  // namespace

TEST(ProjectQuery, MarshalBudgetSharedTable) {
    const QString r = anyRoot();
    // t = {1}; 18 rounds of t = {t, t} — the same sub-table referenced by
    // both slots at every level. ~2^18 (262144) leaf values are reachable
    // by path (~786k total values counting internal nodes along every
    // path), but only ~2*18+1 distinct table objects ever exist inside the
    // VM (few tables in Lua, 2^18 JSON leaves in C++ memory — the defect's
    // own description). D=18 is chosen so the STUB's full (unbounded)
    // marshal still finishes in about a second and well under a GB on this
    // run, while landing orders of magnitude past a few-KiB cap.
    const auto qr = run("local t = {1} for i = 1, 18 do t = {t, t} end return t",
                        r, /*timeoutMs=*/1500, kBudgetCapBytes);
    EXPECT_EQ(qr.code, QStringLiteral("result_too_large"))
        << "code=" << qr.code.toStdString() << " error=" << qr.error.toStdString()
        << " marshalNodes=" << qr.marshalNodes;
    EXPECT_LE(qr.marshalNodes, budgetBound(kBudgetCapBytes))
        << "ANTS-5069: a doubly-shared table must not cost one visit per PATH — "
           "marshalNodes=" << qr.marshalNodes << " cap=" << kBudgetCapBytes
        << " bound=" << budgetBound(kBudgetCapBytes)
        << " (a bounded marshal should have stopped once the byte budget was spent)";
}

TEST(ProjectQuery, MarshalBudgetFlatArray) {
    const QString r = anyRoot();
    // No sharing at all: a plain flat array whose serialised size alone
    // exceeds the cap. Same bound applies — the marshal must stop early
    // rather than visit all 200000 elements before checking the byte size.
    const auto qr = run("local t = {} for i = 1, 200000 do t[i] = i end return t",
                        r, /*timeoutMs=*/1500, kBudgetCapBytes);
    EXPECT_EQ(qr.code, QStringLiteral("result_too_large"))
        << "code=" << qr.code.toStdString() << " error=" << qr.error.toStdString()
        << " marshalNodes=" << qr.marshalNodes;
    EXPECT_LE(qr.marshalNodes, budgetBound(kBudgetCapBytes))
        << "marshalNodes=" << qr.marshalNodes << " cap=" << kBudgetCapBytes
        << " bound=" << budgetBound(kBudgetCapBytes);
}

TEST(ProjectQuery, MarshalBudgetUnderCapNodeCount) {
    const QString r = anyRoot();
    // Guard: a small result well under the cap is unaffected by the budget
    // — it still succeeds, matches its expected JSON, and marshalNodes
    // still counts every value in it (root + a + b-table + 2 + 3 = 5).
    const auto qr = run("return {a = 1, b = {2, 3}}", r, 1500, 65536);
    ASSERT_TRUE(qr.ok) << "code=" << qr.code.toStdString()
                        << " error=" << qr.error.toStdString();
    ASSERT_TRUE(qr.result.isObject());
    const QJsonObject obj = qr.result.toObject();
    EXPECT_EQ(obj.value("a").toInteger(), 1);
    ASSERT_TRUE(obj.value("b").isArray());
    const QJsonArray b = obj.value("b").toArray();
    ASSERT_EQ(b.size(), 2);
    EXPECT_EQ(b.at(0).toInteger(), 2);
    EXPECT_EQ(b.at(1).toInteger(), 3);
    EXPECT_EQ(qr.marshalNodes, 5) << "marshalNodes=" << qr.marshalNodes;
}

TEST(ProjectQuery, MarshalBudgetCircularStillRefuses) {
    const QString r = anyRoot();
    // Guard: the size budget must not interfere with the existing depth
    // bound (INV-6) that catches a circular table — same refusal code,
    // whatever resultCapBytes is set to.
    const auto qr = run("local t = {} t.self = t return t", r, 1500, 65536);
    EXPECT_EQ(qr.code, QStringLiteral("query_error"))
        << "code=" << qr.code.toStdString() << " error=" << qr.error.toStdString();
}

// ---- INV-2 — filesystem confinement + INV-7 list determinism ----
TEST(ProjectQuery, ConfinementAndList) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    auto write = [&](const QString &rel, const QString &body) {
        QDir().mkpath(QFileInfo(root + "/" + rel).absolutePath());
        QFile f(root + "/" + rel);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(body.toUtf8());
    };
    write("a.txt", "hello");
    write("sub/c.txt", "x");

    // in-root read succeeds
    const auto okRead = run("return project.read('a.txt')", root);
    ASSERT_TRUE(okRead.ok);
    EXPECT_EQ(okRead.result.toString(), QStringLiteral("hello"));

    // .. traversal, absolute escape, and an in-root symlink-out all raise
    EXPECT_EQ(run("return project.read('../../etc/passwd')", root).code,
              QStringLiteral("query_error"));
    EXPECT_EQ(run("return project.read('/etc/passwd')", root).code,
              QStringLiteral("query_error"));
    QFile::link("/etc", root + "/escape");  // symlink whose target is outside root
    EXPECT_EQ(run("return project.read('escape/passwd')", root).code,
              QStringLiteral("query_error"));
    // ANTS-3847 — list's refusal raises too; under LeakSanitizer this also
    // proves the refusal leaks nothing.
    EXPECT_EQ(run("return project.list('../..')", root).code,
              QStringLiteral("query_error"));

    // list is deterministic (byte-identical across calls) and finds the files
    const auto l1 = run("return project.list()", root);
    const auto l2 = run("return project.list()", root);
    ASSERT_TRUE(l1.ok);
    ASSERT_TRUE(l1.result.isArray());
    EXPECT_EQ(l1.result, l2.result);
    EXPECT_GE(l1.result.toArray().size(), 2);  // a.txt, sub/c.txt (≥; not .git)

    // ANTS-2203 — a symlink whose target escapes the root must not be disclosed
    // even by name (project.read already refuses its contents); an in-root
    // symlink stays listed, keeping list ⊆ read-acceptable.
    QTemporaryDir outside;
    ASSERT_TRUE(outside.isValid());
    {
        QFile f(outside.path() + "/secret.txt");
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("nope");
    }
    QFile::link(QFileInfo(outside.path() + "/secret.txt").canonicalFilePath(),
                root + "/leak.txt");                       // target outside root
    write("real.txt", "y");
    QFile::link(QFileInfo(root + "/real.txt").canonicalFilePath(),
                root + "/inlink.txt");                     // target inside root
    const auto escList = run("return project.list()", root).result.toArray();
    bool hasLeak = false, hasInlink = false;
    for (const auto &v : escList) {
        const QString s = v.toString();
        if (s == QStringLiteral("leak.txt"))   hasLeak = true;
        if (s == QStringLiteral("inlink.txt")) hasInlink = true;
    }
    EXPECT_FALSE(hasLeak)
        << "ANTS-2203: a symlink escaping the root must not be listed by name";
    EXPECT_TRUE(hasInlink) << "an in-root symlink must stay listed";
}

// ---- INV-8 — verb-layer gating + envelope (projectQueryVerb) ----
TEST(ProjectQuery, VerbEnvelopeAndGate) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();

    QJsonObject req;
    req["caller_cwd"] = root;
    req["code"]       = QStringLiteral("return 1 + 1");

    // feature gate FIRST — off → query_disabled regardless of args
    QJsonObject offReq;  // even with empty args, gate wins
    EXPECT_EQ(LuaEngine::projectQueryVerb(offReq, /*enabled=*/false, 1500, 65536)
                  .value("code").toString(),
              QStringLiteral("query_disabled"));

    // happy path
    const QJsonObject ok = LuaEngine::projectQueryVerb(req, true, 1500, 65536);
    EXPECT_TRUE(ok.value("ok").toBool());
    EXPECT_EQ(ok.value("result").toInteger(), 2);
    EXPECT_TRUE(ok.contains("elapsed_ms"));

    // missing code → missing_field (after the gate, after caller_cwd)
    QJsonObject noCode;
    noCode["caller_cwd"] = root;
    EXPECT_EQ(LuaEngine::projectQueryVerb(noCode, true, 1500, 65536)
                  .value("code").toString(),
              QStringLiteral("missing_field"));

    // over-cap via the verb → result_too_large, no result field
    QJsonObject bigReq;
    bigReq["caller_cwd"] = root;
    bigReq["code"]       = QStringLiteral("return string.rep('x', 100000)");
    const QJsonObject big = LuaEngine::projectQueryVerb(bigReq, true, 1500, 65536);
    EXPECT_EQ(big.value("code").toString(), QStringLiteral("result_too_large"));
    EXPECT_FALSE(big.contains("result"));
}

#endif  // ANTS_LUA_PLUGINS
