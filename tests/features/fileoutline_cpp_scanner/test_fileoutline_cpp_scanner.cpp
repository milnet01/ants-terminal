// Feature-conformance test for ANTS-2159 — the file_outline C++ scanner's
// scope-awareness + multi-line signature handling. Drives the pure
// FileOutline::compute against QTemporaryDir fixtures. See spec.md +
// docs/specs / ROADMAP ANTS-2159.
//
// Two coupled defects this locks against:
//   (a) FALSE POSITIVE — `Type name(expr);` locals and `case X: return f();`
//       statements (both inside a function body) tagged kind:func.
//   (b) FALSE NEGATIVE — id-Software / GNU style `void\nName(args)\n{` with
//       the return type on the previous line never matched.

#include "../../_support/expect.h"
#include "fileoutline.h"

#include <gtest/gtest.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

ANTS_TEST_SCOPE();

namespace {

QString writeCpp(const QTemporaryDir &dir, const QString &body) {
    const QString path = QFileInfo(dir.path()).canonicalFilePath() + "/f.cpp";
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(body.toUtf8());
    f.close();
    return path;
}

QStringList funcNames(const QString &path) {
    const QJsonObject out =
        FileOutline::compute(path, FileOutline::Mode::Cpp, false, 1000);
    QStringList names;
    for (const QJsonValue &v : out.value(QStringLiteral("symbols")).toArray()) {
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("kind")).toString() == QLatin1String("func"))
            names << o.value(QStringLiteral("name")).toString();
    }
    return names;
}

}  // namespace

// INV-1 — a most-vexing-parse local `Type name(arg);` inside a function body
// is NOT tagged as a function; the enclosing function still is.
TEST(FileOutlineCppScanner, LocalVarNotTaggedFunc) {
    QTemporaryDir dir;
    const QString path = writeCpp(dir, QStringLiteral(
        "int alpha(const QString &absPath) {\n"
        "    QFile f(absPath);\n"
        "    return 0;\n"
        "}\n"));
    const QStringList fns = funcNames(path);
    EXPECT_TRUE(fns.contains(QStringLiteral("alpha")));
    EXPECT_FALSE(fns.contains(QStringLiteral("f")));   // the local, not a func
}

// INV-2 — a statement line after a case-label (`case X: return helper(...)`)
// inside a function body is NOT tagged as a function.
TEST(FileOutlineCppScanner, CaseLabelStatementNotTaggedFunc) {
    QTemporaryDir dir;
    const QString path = writeCpp(dir, QStringLiteral(
        "const char *commentFor(int mode) {\n"
        "    switch (mode) {\n"
        "    case 1: return helper(mode);\n"
        "    }\n"
        "    return \"\";\n"
        "}\n"));
    const QStringList fns = funcNames(path);
    EXPECT_TRUE(fns.contains(QStringLiteral("commentFor")));
    EXPECT_FALSE(fns.contains(QStringLiteral("helper")));
}

// INV-3 — an old-style definition with the return type on the previous line
// (`void\nMyOldFunc(int a)\n{`) IS detected.
TEST(FileOutlineCppScanner, OldStyleReturnTypeOnPrevLineDetected) {
    QTemporaryDir dir;
    const QString path = writeCpp(dir, QStringLiteral(
        "void\n"
        "MyOldFunc(int a)\n"
        "{\n"
        "    (void)a;\n"
        "}\n"));
    EXPECT_TRUE(funcNames(path).contains(QStringLiteral("MyOldFunc")));
}

// INV-4 — brace-on-next-line definition (return type same line, body next)
// is detected; and a local inside it is not.
TEST(FileOutlineCppScanner, BraceOnNextLineDef) {
    QTemporaryDir dir;
    const QString path = writeCpp(dir, QStringLiteral(
        "int beta(int n)\n"
        "{\n"
        "    Helper h(n);\n"
        "    return h.v;\n"
        "}\n"));
    const QStringList fns = funcNames(path);
    EXPECT_TRUE(fns.contains(QStringLiteral("beta")));
    EXPECT_FALSE(fns.contains(QStringLiteral("h")));
}

// INV-5 — positive controls still work: a same-line free function, a
// qualified member definition, and a class-member declaration.
TEST(FileOutlineCppScanner, PositiveControlsStillDetected) {
    QTemporaryDir dir;
    const QString path = writeCpp(dir, QStringLiteral(
        "int alpha() { return 1; }\n"
        "void Foo::bar(int x) {\n"
        "    int y = x;\n"
        "    (void)y;\n"
        "}\n"
        "class Widget {\n"
        "    void doThing(int z);\n"
        "};\n"));
    const QStringList fns = funcNames(path);
    EXPECT_TRUE(fns.contains(QStringLiteral("alpha")));
    EXPECT_TRUE(fns.contains(QStringLiteral("Foo::bar")));
    EXPECT_TRUE(fns.contains(QStringLiteral("doThing")));   // member decl in class body
}

// INV-7 — braces inside a raw-string literal (a regex, as this codebase
// uses heavily) must not stick the brace counter: a function defined AFTER
// such a body is still found.
TEST(FileOutlineCppScanner, RawStringBracesDoNotStickScope) {
    QTemporaryDir dir;
    const QString path = writeCpp(dir, QStringLiteral(
        "void withRawRegex() {\n"
        "    const char *p = R\"(^(a|b){1,6}\\s*[{}])\";\n"
        "    (void)p;\n"
        "}\n"
        "int afterRaw() { return 0; }\n"));
    const QStringList fns = funcNames(path);
    EXPECT_TRUE(fns.contains(QStringLiteral("withRawRegex")));
    EXPECT_TRUE(fns.contains(QStringLiteral("afterRaw")));   // not lost to brace drift
}

// INV-6 — a function declaration (prototype, ends in ';') at file scope is
// detected AND does not open a body (so a following local-looking line at
// file scope is still scanned, not suppressed).
TEST(FileOutlineCppScanner, PrototypeDoesNotOpenBody) {
    QTemporaryDir dir;
    const QString path = writeCpp(dir, QStringLiteral(
        "int proto(int a);\n"
        "int gamma() { return 0; }\n"));
    const QStringList fns = funcNames(path);
    EXPECT_TRUE(fns.contains(QStringLiteral("proto")));
    EXPECT_TRUE(fns.contains(QStringLiteral("gamma")));
}

// INV-8 (ANTS-3351) — an `extern "C"` linkage-specifier prefix on a function
// definition must not hide it: the `"C"` string literal was breaking the
// return-type match, so the whole function went undetected and its interior
// most-vexing-parse locals leaked as file-scope funcs. Reproduces DOOM's
// r_vulkan.cpp:108/141 (`RB_VulkanProbe` + `devs`/`exts`).
TEST(FileOutlineCppScanner, ExternCLinkageFunctionDetected) {
    QTemporaryDir dir;
    const QString path = writeCpp(dir, QStringLiteral(
        "extern \"C\" int RB_VulkanProbe(void)\n"
        "{\n"
        "    uint32_t ndev = 0;\n"
        "    std::vector<VkPhysicalDevice> devs(ndev);\n"
        "    (void)devs;\n"
        "    return 0;\n"
        "}\n"
        "extern \"C\" void RB_Vulkan_Init(void)\n"
        "{\n"
        "    std::vector<int> exts(4);\n"
        "    (void)exts;\n"
        "}\n"));
    const QStringList fns = funcNames(path);
    EXPECT_TRUE(fns.contains(QStringLiteral("RB_VulkanProbe")));
    EXPECT_TRUE(fns.contains(QStringLiteral("RB_Vulkan_Init")));
    EXPECT_FALSE(fns.contains(QStringLiteral("devs")));   // MVP local, not a func
    EXPECT_FALSE(fns.contains(QStringLiteral("exts")));   // MVP local, not a func
}

// INV-9 (ANTS-3412 defect a) — a method whose parameter type carries an
// EMPTY inner paren pair (`std::function<void()>`) must still be detected.
// The `\([^)]*\)` arg matcher closed on the FIRST ')' — the inner `void()`
// ended the arg list early, then the trailing `>` broke the `[{;]`/`$`
// tail, so the whole line failed to match. A populated inner list
// (`std::function<void(uint32_t,uint32_t)>`) hit the same truncation.
// Vestige's job_system.h: submit / runOnMainThread went missing.
TEST(FileOutlineCppScanner, FunctionalParamEmptyInnerParens) {
    QTemporaryDir dir;
    const QString path = writeCpp(dir, QStringLiteral(
        "void submit(std::function<void()> job) {\n"
        "    job();\n"
        "}\n"
        "void runOnMainThread(std::function<void(uint32_t,uint32_t)> cb);\n"
        "int plain(int n) { return n; }\n"));
    const QStringList fns = funcNames(path);
    EXPECT_TRUE(fns.contains(QStringLiteral("submit")));
    EXPECT_TRUE(fns.contains(QStringLiteral("runOnMainThread")));
    EXPECT_TRUE(fns.contains(QStringLiteral("plain")));   // positive control
}

// INV-10 (ANTS-3412 defect b) — a one-line inline accessor with a trailing
// cv/ref/noexcept qualifier between ')' and '{' (`T f() const { ... }`)
// must be detected. The `\)\s*[{;]` tail forbade any qualifier, so const
// accessors were silently dropped. Vestige's workerCount / isSynchronous.
TEST(FileOutlineCppScanner, InlineConstAccessorDetected) {
    QTemporaryDir dir;
    const QString path = writeCpp(dir, QStringLiteral(
        "int workerCount() const { return n_; }\n"
        "bool isSynchronous() const noexcept { return sync_; }\n"
        "int mutableOne() { return 1; }\n"));   // positive control (no qualifier)
    const QStringList fns = funcNames(path);
    EXPECT_TRUE(fns.contains(QStringLiteral("workerCount")));
    EXPECT_TRUE(fns.contains(QStringLiteral("isSynchronous")));
    EXPECT_TRUE(fns.contains(QStringLiteral("mutableOne")));
}
