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

// INV-11 (ANTS-3433) — an out-of-line member definition whose return type is
// a two-word builtin (`unsigned int`, `long long`, `unsigned char`) or a
// `const T&` — with the ref glued to EITHER the type or the name — must be
// detected and emitted as the bare qualified `Class::method` (so read_region
// symbol-mode resolves it). The old single-token return-type regex dropped
// every one of these. Vestige feedback (2026-07-04).
TEST(FileOutlineCppScanner, TwoWordReturnTypeMemberDetected) {
    QTemporaryDir dir;
    const QString path = writeCpp(dir, QStringLiteral(
        "unsigned int Grid::cellCount(int row) const {\n"
        "    return row;\n"
        "}\n"
        "long long Grid::checksum() {\n"
        "    return 0;\n"
        "}\n"
        "unsigned char Grid::flags() const { return 0; }\n"
        "const std::string& Grid::name() const { return name_; }\n"
        "std::string &Grid::mutableName() { return name_; }\n"));
    const QStringList fns = funcNames(path);
    EXPECT_TRUE(fns.contains(QStringLiteral("Grid::cellCount")));
    EXPECT_TRUE(fns.contains(QStringLiteral("Grid::checksum")));
    EXPECT_TRUE(fns.contains(QStringLiteral("Grid::flags")));
    EXPECT_TRUE(fns.contains(QStringLiteral("Grid::name")));       // & glued to type
    EXPECT_TRUE(fns.contains(QStringLiteral("Grid::mutableName")));  // & glued to name
}

// INV-12 (ANTS-3735) — a DECLARATION whose ';' is followed by a trailing
// comment must not be read as a definition that opens a body. The terminator
// test ran on the raw line, so `int f(char* c);   // note` did not "end with
// ';'"; the scanner set funcOpenAtDepth awaiting a '{' and then adopted the
// next brace it met — an anonymous `namespace {` — as that function's body.
// Because clearing the latch requires the depth to first EXCEED the recorded
// depth and then return to it, a namespace brace never releases it: every
// func symbol is suppressed until the namespace closes. In DOOM's
// r_vulkan.cpp that swallowed 5,742 lines (~30 functions), and
// workspace_search enclosing_symbol then attributed every match in that span
// to the last emitted symbol, the struct VulkanState. DOOM feedback
// (2026-07-30).
TEST(FileOutlineCppScanner, TrailingCommentDeclDoesNotOpenBody) {
    QTemporaryDir dir;
    const QString path = writeCpp(dir, QStringLiteral(
        "extern \"C\" int M_CheckParm(const char* check);   // m_argv.c\n"
        "\n"
        "namespace {\n"
        "\n"
        "struct State { int a = 0; };\n"
        "\n"
        "void CreateInstance()\n"
        "{\n"
        "    int x = 0;\n"
        "}\n"
        "\n"
        "bool DeviceHasRT(int d) { return d > 0; }\n"
        "\n"
        "}  // namespace\n"
        "\n"
        "extern \"C\" void RB_Init(void) { CreateInstance(); }\n"));
    const QStringList fns = funcNames(path);
    // The declaration itself is still a symbol — it just must not open a body.
    EXPECT_TRUE(fns.contains(QStringLiteral("M_CheckParm")));
    // The functions INSIDE the anonymous namespace are the ones the latch ate.
    EXPECT_TRUE(fns.contains(QStringLiteral("CreateInstance")));
    EXPECT_TRUE(fns.contains(QStringLiteral("DeviceHasRT")));
    // And the scanner recovers past the namespace close.
    EXPECT_TRUE(fns.contains(QStringLiteral("RB_Init")));
    // The body-local must still be suppressed — the fix must not cost INV-1.
    EXPECT_FALSE(fns.contains(QStringLiteral("x")));
}

// INV-13 (ANTS-3735) — over-reach guard, non-discriminating by design. The
// terminator test must read CODE, not raw text: a ';' inside a string or
// char literal must not be mistaken for the line's terminator, or a genuine
// definition would be misread as a declaration and its body's locals would
// leak as file-scope funcs (the INV-1 failure mode, in reverse). Passes
// before and after the fix — it exists to prove the fix cost nothing.
TEST(FileOutlineCppScanner, TerminatorTestIsLiteralAware) {
    QTemporaryDir dir;
    const QString path = writeCpp(dir, QStringLiteral(
        "void emit(const char* s = \";\") {\n"
        "    int inner = 0;\n"
        "}\n"
        "\n"
        "void semi(char c = ';');\n"
        "\n"
        "void after() { }\n"));
    const QStringList fns = funcNames(path);
    EXPECT_TRUE(fns.contains(QStringLiteral("emit")));
    EXPECT_TRUE(fns.contains(QStringLiteral("semi")));
    EXPECT_TRUE(fns.contains(QStringLiteral("after")));
    // `emit` is a DEFINITION despite the ';' in its default argument, so its
    // body opens and the local is suppressed.
    EXPECT_FALSE(fns.contains(QStringLiteral("inner")));
}
