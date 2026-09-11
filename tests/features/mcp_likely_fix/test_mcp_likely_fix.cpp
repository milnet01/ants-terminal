// ANTS-3374 — feature-conformance test for the likely_fix add_include hint.
// Pure BuildFixHint behaviour against a synthetic source tree + a
// source-grep wiring contract that both diagnostics verbs enrich. See spec.md.

#include "../../_support/expect.h"

#include "buildfixhint.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <cstdio>
#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#ifndef ANTS_RC_SOURCES
#error "ANTS_RC_SOURCES compile definition required"
#endif
#ifndef ANTS_SOURCE_DIR
#error "ANTS_SOURCE_DIR compile definition required"
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

// ANTS-5053 (INV-6) — resolveHeaders(root, symbols) must return, for every distinct
// input, exactly what resolveHeader(root, thatSymbol) returns: this is the
// guard that must stay true whether resolveHeaders walks the tree once per
// symbol (today's stub) or once for the whole batch (the fix). Reuses the
// same seeded tree as the ResolveHeader test above (header match, source-only
// sibling fallback, no-sibling source-only, unresolved, invalid) plus an
// explicit duplicate, so a real "many symbols" call is exercised.
TEST(McpLikelyFix, ResolveHeadersEquivalence) {
    expect_reset();
    using BuildFixHint::resolveHeader;
    using BuildFixHint::resolveHeaders;

    QTemporaryDir tmp;
    expect(tmp.isValid(), "setup: QTemporaryDir valid");
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();

    writeFile(root, QStringLiteral("src/devhotswap.h"),
              QStringLiteral("#pragma once\n"
                             "enum class DeviceHotSwapMode { Off, On };\n"));
    writeFile(root, QStringLiteral("src/helpers.h"),
              QStringLiteral("#pragma once\n"
                             "// (sibling header, no declaration of doStuff)\n"));
    writeFile(root, QStringLiteral("src/helpers.cpp"),
              QStringLiteral("#include \"helpers.h\"\n"
                             "void doStuff() {\n"
                             "    return;\n"
                             "}\n"));
    writeFile(root, QStringLiteral("src/orphan.cpp"),
              QStringLiteral("void orphanFn() {\n"
                             "    return;\n"
                             "}\n"));

    const QStringList symbols = {
        QStringLiteral("DeviceHotSwapMode"),  // header match
        QStringLiteral("doStuff"),            // sibling-header fallback
        QStringLiteral("orphanFn"),           // source-only, no sibling
        QStringLiteral("TotallyAbsentSymbol"),// unresolved
        QStringLiteral("not a symbol"),       // invalid symbol
        QStringLiteral("DeviceHotSwapMode"),  // duplicate of the first
    };

    const QHash<QString, QString> batch = resolveHeaders(root, symbols);

    // Every DISTINCT input, including the duplicate, must be present and
    // must equal the single-symbol call — the duplicate is not a second
    // check, it proves the batch doesn't choke on a repeated needle.
    QSet<QString> distinct;
    for (const QString &sym : symbols) distinct.insert(sym);
    expect(static_cast<qsizetype>(batch.size()) == distinct.size(),
           "batch has one entry per distinct symbol");

    for (const QString &sym : distinct) {
        const QString single = resolveHeader(root, sym);
        const auto it = batch.constFind(sym);
        const bool present = (it != batch.constEnd());
        expect(present,
               (QStringLiteral("batch has an entry for '") + sym +
                QLatin1Char('\''))
                   .toUtf8()
                   .constData());
        if (present) {
            const QString got = it.value();
            const bool eq = (got == single);
            expect(eq,
                   (QStringLiteral("resolveHeaders('") + sym +
                    QStringLiteral("') == resolveHeader('") + sym +
                    QStringLiteral("') — expected \"") + single +
                    QStringLiteral("\", got \"") + got + QLatin1Char('"'))
                       .toUtf8()
                       .constData());
        }
    }

    // Empty root → every value empty, same as resolveHeader(QString(), sym).
    const QHash<QString, QString> emptyRootBatch =
        resolveHeaders(QString(), symbols);
    bool allEmpty = true;
    for (auto it = emptyRootBatch.constBegin(); it != emptyRootBatch.constEnd();
         ++it) {
        if (!it.value().isEmpty()) allEmpty = false;
    }
    expect(allEmpty, "empty root → every resolveHeaders() value empty");

    EXPECT_EQ(0, expect_finish());
}

// ANTS-5053 wiring (INV-7) — resolveHeaders' body walks the tree once via
// SymbolQuery::findDefinitions, never per-symbol via resolveHeader/
// findDefinition (singular). Red against the ANTS-5053 stub, which loops
// resolveHeader per symbol.
TEST(McpLikelyFix, ResolveHeadersBatchWalksOnce) {
    expect_reset();

    const std::string src = ants_test::slurpFile(
        std::string(ANTS_SOURCE_DIR) + "/src/buildfixhint.cpp");
    expect(!src.empty(), "buildfixhint.cpp readable");

    const std::string body = ants_test::stripComments(
        ants_test::slurpFunctionBody(
            src, "QHash<QString, QString> resolveHeaders("));
    expect(!body.empty(), "resolveHeaders body found");

    const std::size_t batchCalls =
        ants_test::countOccurrences(body, "SymbolQuery::findDefinitions(");
    expect(batchCalls >= 1,
           "resolveHeaders calls SymbolQuery::findDefinitions() at least once");

    const std::size_t perSymbolResolveHeader =
        ants_test::countOccurrences(body, "resolveHeader(");
    expect(perSymbolResolveHeader == 0,
           ("resolveHeaders body must not call resolveHeader() per symbol — "
            "saw " + std::to_string(perSymbolResolveHeader) + " call(s)")
               .c_str());

    const std::size_t perSymbolFindDefinition =
        ants_test::countOccurrences(body, "findDefinition(");
    expect(perSymbolFindDefinition == 0,
           ("resolveHeaders body must not call the singular findDefinition() "
            "per symbol — saw " + std::to_string(perSymbolFindDefinition) +
            " call(s)")
               .c_str());

    EXPECT_EQ(0, expect_finish());
}

// ANTS-5053 wiring (INV-7) — enrichLikelyFixes must make exactly one
// BuildFixHint::resolveHeaders() call and no longer call the per-symbol
// BuildFixHint::resolveHeader(). Red against current code, which loops
// resolveHeader() inside the per-error loop.
TEST(McpLikelyFix, EnrichLikelyFixesUsesBatch) {
    expect_reset();
    const std::string rc = ants_test::slurpRemoteControl();
    expect(!rc.empty(), "remotecontrol.cpp readable");

    const std::string body = ants_test::stripComments(
        ants_test::slurpFunctionBody(
            rc, "void RemoteControl::enrichLikelyFixes("));
    expect(!body.empty(), "enrichLikelyFixes body found");

    const std::size_t batchCalls =
        ants_test::countOccurrences(body, "BuildFixHint::resolveHeaders(");
    expect(batchCalls == 1,
           ("enrichLikelyFixes must call BuildFixHint::resolveHeaders() "
            "exactly once — saw " + std::to_string(batchCalls) + " call(s)")
               .c_str());

    const std::size_t perErrorCalls =
        ants_test::countOccurrences(body, "BuildFixHint::resolveHeader(");
    expect(perErrorCalls == 0,
           ("enrichLikelyFixes must not call BuildFixHint::resolveHeader() "
            "per error anymore — saw " + std::to_string(perErrorCalls) +
            " call(s)")
               .c_str());

    EXPECT_EQ(0, expect_finish());
}

// INV-5 — both diagnostics verbs wire the enrichment onto their errors array.
TEST(McpLikelyFix, VerbWiring) {
    expect_reset();
    const std::string rc = ants_test::slurpRemoteControl();
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
