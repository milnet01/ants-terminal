// ANTS-3374 — feature-conformance test for the likely_fix add_include hint.
// Pure BuildFixHint behaviour against a synthetic source tree + a
// source-grep wiring contract that both diagnostics verbs enrich. See spec.md.

#include "../../_support/expect.h"

#include "buildfixhint.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cstdio>
#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#ifndef SRC_REMOTECONTROL_CPP_PATH
#error "SRC_REMOTECONTROL_CPP_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {

void writeFile(const QString &root, const QString &rel, const QString &body) {
    const QString full = root + QLatin1Char('/') + rel;
    QDir().mkpath(QFileInfo(full).absolutePath());
    QFile f(full);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        std::fprintf(stderr, "setup-fail: cannot write %s\n",
                     full.toUtf8().constData());
        std::exit(2);
    }
    f.write(body.toUtf8());
    f.close();
}

}  // namespace

// INV-1 — symbol extraction across all four recognised forms + a negative.
TEST(McpLikelyFix, UndeclaredSymbolPatterns) {
    expect_reset();
    using BuildFixHint::undeclaredSymbol;

    expect(undeclaredSymbol(QStringLiteral(
               "error: 'DeviceHotSwapMode' has not been declared")) ==
               QStringLiteral("DeviceHotSwapMode"),
           "GCC 'has not been declared'");
    expect(undeclaredSymbol(QStringLiteral(
               "error: 'gWidget' was not declared in this scope")) ==
               QStringLiteral("gWidget"),
           "GCC 'was not declared in this scope'");
    expect(undeclaredSymbol(QStringLiteral(
               "error: unknown type name 'FooBar'")) ==
               QStringLiteral("FooBar"),
           "clang 'unknown type name'");
    expect(undeclaredSymbol(QStringLiteral(
               "error: use of undeclared identifier 'baz'")) ==
               QStringLiteral("baz"),
           "clang 'use of undeclared identifier'");
    // Negative: an unrelated diagnostic yields no symbol.
    expect(undeclaredSymbol(QStringLiteral(
               "error: redefinition of 'Widget'")).isEmpty(),
           "unrelated message → empty");
    expect(undeclaredSymbol(QString()).isEmpty(), "empty message → empty");

    EXPECT_EQ(0, expect_finish());
}

// INV-2/3/4 — header resolution against a seeded temp project.
TEST(McpLikelyFix, ResolveHeader) {
    expect_reset();
    using BuildFixHint::resolveHeader;

    QTemporaryDir tmp;
    expect(tmp.isValid(), "setup: QTemporaryDir valid");
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();

    // A type declared in a header (INV-2).
    writeFile(root, QStringLiteral("src/devhotswap.h"),
              QStringLiteral("#pragma once\n"
                             "enum class DeviceHotSwapMode { Off, On };\n"));
    // A free function defined only in a source file, with a sibling header
    // that does NOT declare it (INV-3 sibling fallback).
    writeFile(root, QStringLiteral("src/helpers.h"),
              QStringLiteral("#pragma once\n"
                             "// (sibling header, no declaration of doStuff)\n"));
    writeFile(root, QStringLiteral("src/helpers.cpp"),
              QStringLiteral("#include \"helpers.h\"\n"
                             "void doStuff() {\n"
                             "    return;\n"
                             "}\n"));
    // A function defined only in a source file with NO sibling header.
    writeFile(root, QStringLiteral("src/orphan.cpp"),
              QStringLiteral("void orphanFn() {\n"
                             "    return;\n"
                             "}\n"));

    // INV-2 — header-declared symbol resolves to the header.
    expect(resolveHeader(root, QStringLiteral("DeviceHotSwapMode")) ==
               QStringLiteral("src/devhotswap.h"),
           "header-declared type → header path");

    // INV-3 — source-only symbol with an existing sibling header.
    expect(resolveHeader(root, QStringLiteral("doStuff")) ==
               QStringLiteral("src/helpers.h"),
           "source-only symbol → sibling header");

    // INV-3 — source-only symbol with no sibling header → empty.
    expect(resolveHeader(root, QStringLiteral("orphanFn")).isEmpty(),
           "source-only symbol, no sibling header → empty");

    // INV-4 — self-gating: an unresolved symbol yields no suggestion.
    expect(resolveHeader(root, QStringLiteral("TotallyAbsentSymbol")).isEmpty(),
           "unresolved symbol → empty");
    // INV-4 — empty root / invalid symbol → empty.
    expect(resolveHeader(QString(), QStringLiteral("DeviceHotSwapMode"))
               .isEmpty(),
           "empty root → empty");
    expect(resolveHeader(root, QStringLiteral("not a symbol")).isEmpty(),
           "invalid symbol → empty");

    EXPECT_EQ(0, expect_finish());
}

// INV-5 — both diagnostics verbs wire the enrichment onto their errors array.
TEST(McpLikelyFix, VerbWiring) {
    expect_reset();
    const std::string rc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(!rc.empty(), "remotecontrol.cpp readable");

    const std::string recent =
        ants_test::slurpFunctionBody(rc, "RemoteControl::cmdRecentErrors");
    const std::string build =
        ants_test::slurpFunctionBody(rc, "RemoteControl::cmdBuildStatus");
    expect(!recent.empty(), "cmdRecentErrors body found");
    expect(!build.empty(), "cmdBuildStatus body found");

    expect(ants_test::countOccurrences(recent, "enrichLikelyFixes(") >= 1,
           "cmdRecentErrors calls enrichLikelyFixes");
    expect(ants_test::countOccurrences(build, "enrichLikelyFixes(") >= 1,
           "cmdBuildStatus calls enrichLikelyFixes");

    EXPECT_EQ(0, expect_finish());
}
