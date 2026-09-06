// Feature-conformance test for ANTS-2220 — workspace_search
// `enclosing_symbol`. Exercises the pure RemoteControl::enclosingSymbolForLine
// heuristic directly (ES-1..ES-6, no Qt event loop) plus source-greps the
// handler + schema wiring (WI-1..WI-3 — the handler needs a live MainWindow).
// See spec.md.

#include "../../_support/expect.h"
#include "remotecontrol.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#ifndef SRC_RC_HEADER
#error "SRC_RC_HEADER compile definition required"
#endif
#ifndef ANTS_RC_SOURCES
#error "ANTS_RC_SOURCES compile definition required"
#endif
#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {

bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

// Build a file_outline-shaped symbols[] array from {line, name} pairs.
QJsonArray symbols(std::initializer_list<std::pair<int, const char *>> items) {
    QJsonArray a;
    for (const auto &it : items) {
        QJsonObject o;
        o["line"] = it.first;
        o["name"] = QString::fromLatin1(it.second);
        a.append(o);
    }
    return a;
}

}  // namespace

// ES-1 — a match inside a function resolves to that function.
// ES-4 — the exact start line is inclusive.
TEST(WorkspaceSearchEnclosing, InsideFunction) {
    const QJsonArray syms = symbols({{10, "alpha"}, {25, "beta"}, {40, "gamma"}});
    EXPECT_EQ(RemoteControl::enclosingSymbolForLine(syms, 15).toStdString(),
              "alpha");                                         // ES-1
    EXPECT_EQ(RemoteControl::enclosingSymbolForLine(syms, 30).toStdString(),
              "beta");
    EXPECT_EQ(RemoteControl::enclosingSymbolForLine(syms, 25).toStdString(),
              "beta") << "ES-4: start line must be inclusive";  // ES-4
    EXPECT_EQ(RemoteControl::enclosingSymbolForLine(syms, 999).toStdString(),
              "gamma") << "last symbol owns everything after its start";
}

// ES-2 — a line in the gap between two top-level symbols attributes to the
// preceding one (nearest-preceding heuristic; the flat outline carries start
// lines only).
TEST(WorkspaceSearchEnclosing, GapAttributesToPreceding) {
    // alpha at 10 (body ends ~18), beta at 25 — line 20 sits in the gap.
    const QJsonArray syms = symbols({{10, "alpha"}, {25, "beta"}});
    EXPECT_EQ(RemoteControl::enclosingSymbolForLine(syms, 20).toStdString(),
              "alpha")
        << "ES-2: gap line attributes to the preceding symbol";
}

// ES-3 — a match before the first symbol (e.g. in includes) has no enclosing.
TEST(WorkspaceSearchEnclosing, BeforeFirstSymbolEmpty) {
    const QJsonArray syms = symbols({{10, "alpha"}, {25, "beta"}});
    EXPECT_TRUE(RemoteControl::enclosingSymbolForLine(syms, 3).isEmpty())
        << "ES-3: a line before the first symbol must return empty";
}

// ES-5 — a qualified method name is returned verbatim (reads as Foo::bar).
TEST(WorkspaceSearchEnclosing, QualifiedMethodVerbatim) {
    const QJsonArray syms =
        symbols({{5, "Widget"}, {12, "Widget::compute"}, {30, "Widget::reset"}});
    EXPECT_EQ(RemoteControl::enclosingSymbolForLine(syms, 18).toStdString(),
              "Widget::compute") << "ES-5: qualified name returned verbatim";
}

// ES-6 — an empty symbols[] (file the outline could not scan) returns empty,
// no crash.
TEST(WorkspaceSearchEnclosing, EmptySymbolsEmpty) {
    EXPECT_TRUE(
        RemoteControl::enclosingSymbolForLine(QJsonArray{}, 42).isEmpty())
        << "ES-6: empty symbols must return empty string";
}

// WI-1..WI-3 — wiring contract (the handler needs a live MainWindow, so the
// per-file scan + cache loop is locked at source level; mirrors the
// mcp_workspace_search / mcp_file_outline source-grep tests).
TEST(WorkspaceSearchEnclosing, WiringContract) {
    expect_reset();
    const std::string rcCpp =
        ants_test::slurpRemoteControl();
    const std::string rcHdr = ants_test::slurpFile(SRC_RC_HEADER);
    const std::string ciCpp =
        ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);

    // WI-1 — handler reads the arg, scans via FileOutline::compute, resolves
    // through the helper, and writes the `enclosing` field.
    expect(contains(rcCpp, "QStringLiteral(\"enclosing_symbol\")") &&
           contains(rcCpp, "enclosingSymbolForLine(") &&
           contains(rcCpp, "FileOutline::compute(") &&
           contains(rcCpp, "m[\"enclosing\"]"),
           "WI-1",
           "cmdWorkspaceSearch does not wire enclosing_symbol → "
           "FileOutline::compute → enclosingSymbolForLine → m[\"enclosing\"]");
    // WI-2 — the helper is a static inline on RemoteControl (testable).
    expect(contains(rcHdr,
               "static inline QString enclosingSymbolForLine(const QJsonArray"),
           "WI-2",
           "remotecontrol.h missing the static inline "
           "enclosingSymbolForLine helper");
    // WI-3 — schema registers the enclosing_symbol property.
    expect(contains(ciCpp, "\"enclosing_symbol\""),
           "WI-3",
           "claudeintegration.cpp does not register the enclosing_symbol "
           "property on workspace_search");

    EXPECT_EQ(0, expect_failures())
        << expect_failures() << " ANTS-2220 wiring invariant(s) failed";
}

// ---------------------------------------------------------------------------
// ANTS-4901 — an annotation that could not run must say so.
//
// Reported by UT_MonsterHunt against a tree with no outline for its language:
// every row came back with no `enclosing`, and the envelope carried no signal
// at all. The ambiguity is the defect rather than the absence — ES-3 above
// makes "a match above the first symbol carries no enclosing" a legitimate
// empty case, so a reader cannot tell that from "this language is not outlined
// at all", which was true of every row. Learning which cost a separate
// file_outline call: the round-trip the annotation exists to remove.
//
// Behavioural, unlike WI-1..WI-3 above: cmdWorkspaceSearch runs without a
// MainWindow.

#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QTemporaryDir>

namespace {

bool writeSrc(const QString &path, const QByteArray &body) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool ok = (f.write(body) == body.size());
    f.close();
    return ok;
}

// One outlined file and one whose language file_outline has no mode for.
// `.lua` is the honest choice: SymbolQuery knows it, file_outline does not,
// so it is exactly the reporter's situation and not a contrived extension.
bool makeMixedTree(const QTemporaryDir &tmp) {
    return writeSrc(QDir(tmp.path()).filePath(QStringLiteral("engine.cpp")),
                    "#include <cstdio>\n"
                    "\n"
                    "void spawnWave() {\n"
                    "    // marker_token here\n"
                    "}\n")
        && writeSrc(QDir(tmp.path()).filePath(QStringLiteral("hooks.lua")),
                    "function on_load()\n"
                    "  -- marker_token here\n"
                    "end\n");
}

QJsonObject searchTree(const QTemporaryDir &tmp, bool enclosing) {
    RemoteControl rc(nullptr);
    QJsonObject r;
    r[QStringLiteral("caller_cwd")] = tmp.path();
    r[QStringLiteral("pattern")]    = QStringLiteral("marker_token");
    if (enclosing) r[QStringLiteral("enclosing_symbol")] = true;
    return rc.cmdWorkspaceSearch(r).object();
}

}  // namespace

// ANTS-4901 INV-1 — the envelope states how many rows were annotated, and
// names the files whose language yielded no outline. A zero that is stated is
// actionable; a zero that is absent is not.
TEST(WorkspaceSearchEnclosing, Ants4901UnavailableIsReported) {
    if (QStandardPaths::findExecutable(QStringLiteral("rg")).isEmpty())
        GTEST_SKIP() << "ripgrep not installed";
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(makeMixedTree(tmp));

    const QJsonObject env = searchTree(tmp, /*enclosing=*/true);
    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(env).toJson().toStdString();
    const QJsonArray matches = env.value(QStringLiteral("matches")).toArray();
    ASSERT_EQ(matches.size(), 2)
        << "both files carry the marker: "
        << QJsonDocument(env).toJson().toStdString();

    EXPECT_EQ(env.value(QStringLiteral("enclosing_annotated")).toInt(-1), 1)
        << "exactly one of the two rows is inside an outlined symbol";

    const QJsonArray unavailable =
        env.value(QStringLiteral("enclosing_symbol_unavailable")).toArray();
    ASSERT_EQ(unavailable.size(), 1)
        << "the un-outlined file must be named: "
        << QJsonDocument(env).toJson().toStdString();
    EXPECT_EQ(unavailable.at(0).toString(), QStringLiteral("hooks.lua"));
}

// ANTS-4901 INV-2 — an outlined file is never listed as unavailable, even
// when a particular match in it legitimately carries no `enclosing`. That is
// ES-3's case and the two must stay distinguishable.
TEST(WorkspaceSearchEnclosing, Ants4901OutlinedFileNotListed) {
    if (QStandardPaths::findExecutable(QStringLiteral("rg")).isEmpty())
        GTEST_SKIP() << "ripgrep not installed";
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    // The marker sits ABOVE the first symbol — outlined language, no
    // enclosing symbol for this row.
    ASSERT_TRUE(writeSrc(QDir(tmp.path()).filePath(QStringLiteral("top.cpp")),
                         "// marker_token in the header comment\n"
                         "#include <cstdio>\n"
                         "\n"
                         "void later() {}\n"));

    const QJsonObject env = searchTree(tmp, /*enclosing=*/true);
    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(env.value(QStringLiteral("enclosing_annotated")).toInt(-1), 0);
    EXPECT_TRUE(env.value(QStringLiteral("enclosing_symbol_unavailable"))
                    .toArray()
                    .isEmpty())
        << "an outlined file with an unattributable match is ES-3, not an "
           "unavailable language";
}

// ANTS-4901 INV-3 — the new keys ride the opt-in only. A search that did not
// ask for the annotation returns exactly what it returned before.
TEST(WorkspaceSearchEnclosing, Ants4901KeysAbsentWhenNotRequested) {
    if (QStandardPaths::findExecutable(QStringLiteral("rg")).isEmpty())
        GTEST_SKIP() << "ripgrep not installed";
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(makeMixedTree(tmp));

    const QJsonObject env = searchTree(tmp, /*enclosing=*/false);
    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(env.contains(QStringLiteral("enclosing_annotated")));
    EXPECT_FALSE(env.contains(QStringLiteral("enclosing_symbol_unavailable")));
}
