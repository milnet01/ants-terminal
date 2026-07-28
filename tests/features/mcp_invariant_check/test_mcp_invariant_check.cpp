// Source-grep harness for ANTS-1308 — locks the wiring contract for
// the `invariant_check` MCP tool. See spec.md.
//
// Exit 0 = all 8 invariants hold.

#include "../../_support/expect.h"
#include "remotecontrol.h"

#include <string>

#include <gtest/gtest.h>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>
#include "../../_support/srcgrep.h"

#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif
#ifndef SRC_RC_HEADER
#error "SRC_RC_HEADER compile definition required"
#endif
#ifndef SRC_REMOTECONTROL_CPP_PATH
#error "SRC_REMOTECONTROL_CPP_PATH compile definition required"
#endif
#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {


bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

std::string extractFunctionBody(const std::string &src,
                                const std::string &declarationStart) {
    const auto pos = src.find(declarationStart);
    if (pos == std::string::npos) return {};
    auto end = src.find("\nQJsonDocument RemoteControl::cmd",
                        pos + declarationStart.size());
    if (end == std::string::npos) end = src.size();
    return src.substr(pos, end - pos);
}

}  // namespace

TEST(McpInvariantCheck, WiringContract) {
    expect_reset();

    const std::string rcHdr = ants_test::slurpFile(SRC_RC_HEADER);
    const std::string rcCpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    const std::string ciCpp = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string mwCpp = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);

    // INV-1 — declaration on RemoteControl.
    expect(contains(rcHdr, "cmdInvariantCheck(const QJsonObject &req)"),
           "INV-1",
           "cmdInvariantCheck decl missing from src/remotecontrol.h");

    // INV-2 — definition + ANTS-1308 anchor.
    const std::string body =
        extractFunctionBody(rcCpp,
            "QJsonDocument RemoteControl::cmdInvariantCheck(");
    expect(!body.empty(),
           "INV-2a",
           "cmdInvariantCheck body missing from src/remotecontrol.cpp");
    expect(contains(rcCpp, "ANTS-1308"),
           "INV-2b",
           "cmdInvariantCheck section must carry an ANTS-1308 anchor");

    // INV-3 — bad_files refusal.
    expect(contains(body, "bad_files"),
           "INV-3",
           "cmdInvariantCheck must refuse missing/empty files with "
           "code:bad_files");

    // INV-4 — directory walk via QDir + ANTS-*.md filter.
    expect(contains(body, "QDir"),
           "INV-4a",
           "cmdInvariantCheck must iterate via QDir");
    expect(contains(body, "ANTS-*.md"),
           "INV-4b",
           "cmdInvariantCheck must filter directory entries by "
           "the ANTS-*.md glob");

    // INV-5 — shared parser delegation.
    expect(contains(body, "parseSpecBody"),
           "INV-5",
           "cmdInvariantCheck must delegate parsing to the shared "
           "parseSpecBody helper (single-source the parser)");

    // INV-6 — mainwindow registration.
    expect(contains(mwCpp, "registerToolProvider(\"invariant_check\""),
           "INV-6a",
           "MainWindow must register \"invariant_check\" via "
           "registerToolProvider");
    expect(contains(mwCpp, "cmdInvariantCheck"),
           "INV-6b",
           "MainWindow registration must delegate to "
           "m_remoteControl->cmdInvariantCheck");

    // INV-7 — tools/list schema entry.
    expect(contains(ciCpp, "t[\"name\"] = \"invariant_check\""),
           "INV-7a",
           "tools/list block must register an \"invariant_check\" "
           "entry");
    {
        // ANTS-3720 — self-sizing descriptor block. This was a fixed 3000-byte
        // window from the ANTS-1308 anchor, which ANTS-3699's `mode` property
        // pushed `req.append("files")` straight past: a scrape that measures
        // the descriptor's length, not the wiring it claims to lock.
        const std::string region =
            ants_test::mcpToolDescriptor(ciCpp, "invariant_check");
        ASSERT_FALSE(region.empty())
            << "invariant_check descriptor block not found in "
               "src/claudeintegration.cpp";
        expect(contains(region, "req.append(\"files\")"),
               "INV-7b",
               "invariant_check schema must mark \"files\" as required");
        expect(contains(region, "req.append(\"caller_cwd\")"),
               "INV-7c",
               "invariant_check schema must mark \"caller_cwd\" as "
               "required");
        expect(contains(region, "minItems"),
               "INV-7d",
               "invariant_check schema's files array must declare a "
               "minItems constraint (non-empty)");
    }

    // INV-8 — Required contract.
    {
        const auto pos = ciCpp.find(
            "callerCwdContractFor(const QString &toolName)");
        ASSERT_NE(pos, std::string::npos);
        const auto end = ciCpp.find("\n}\n", pos);
        ASSERT_NE(end, std::string::npos);
        const std::string fn = ciCpp.substr(pos, end - pos);
        const auto branch = fn.find("\"invariant_check\"");
        ASSERT_NE(branch, std::string::npos)
            << "invariant_check must have an explicit branch in "
               "callerCwdContractFor";
        const auto eol = fn.find('\n', branch);
        ASSERT_NE(eol, std::string::npos);
        const std::string line = fn.substr(branch, eol - branch);
        expect(line.find("C::Required;") != std::string::npos,
               "INV-8",
               "invariant_check must be classified C::Required");
    }
}

namespace {

// Seed <root>/docs/specs/<id>.md with one INV bullet whose body is long
// enough that its presence or absence is unmistakable in the envelope.
void seedSpec(const QString &root, const QString &id, const QString &mentions) {
    QDir(root).mkpath(QStringLiteral("docs/specs"));
    QFile f(root + QStringLiteral("/docs/specs/") + id + QStringLiteral(".md"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.write((QStringLiteral("# ") + id + QStringLiteral(" — seeded\n\n"
            "**Status:** accepted.\n\n## 2. Surface\n\nTouches `") + mentions +
            QStringLiteral("` in anger.\n\n## 3. Invariants\n\n"
            "- **INV-1** — ") + QString(400, QLatin1Char('x')) +
            QStringLiteral(". *Test:* T1.\n"
            "- **INV-2** — ") + QString(400, QLatin1Char('y')) +
            QStringLiteral(". *Test:* T2.\n")).toUtf8());
}

QJsonObject runCheck(const QString &root, const QString &file,
                     const QString &mode) {
    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    QJsonArray files;
    files.append(file);
    req[QStringLiteral("files")] = files;
    if (!mode.isEmpty()) req[QStringLiteral("mode")] = mode;
    return rc.cmdInvariantCheck(req).object();
}

}  // namespace

// INV-9 (ANTS-3699) — summary is the DEFAULT shape: the match list survives,
// the invariant BODIES do not, and the envelope says so. The whole point is
// that a caller who knows nothing about `mode` gets the cheap answer, so the
// default is what this pins.
TEST(McpInvariantCheck, Ants3699SummaryOmitsBodiesByDefault) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    seedSpec(dir.path(), QStringLiteral("ANTS-9001"),
             QStringLiteral("src/widget.cpp"));

    const QJsonObject env =
        runCheck(dir.path(), QStringLiteral("src/widget.cpp"), QString());
    ASSERT_TRUE(env.value("ok").toBool());
    ASSERT_EQ(env.value("matched_count").toInt(), 1);
    EXPECT_EQ(env.value("mode").toString(), "summary");
    EXPECT_FALSE(env.value("invariants_included").toBool());

    const QJsonObject spec =
        env.value("matched_specs").toArray().at(0).toObject();
    EXPECT_EQ(spec.value("id").toString(), "ANTS-9001");
    EXPECT_EQ(spec.value("path").toString(), "docs/specs/ANTS-9001.md");
    EXPECT_EQ(spec.value("matched_terms").toArray().size(), 1);
    // The count is the real one even though the bodies are gone — that is what
    // makes the summary a usable answer rather than a truncation.
    EXPECT_EQ(spec.value("invariants_count").toInt(), 2);
    EXPECT_FALSE(spec.contains("invariants"))
        << "summary mode must OMIT invariant bodies, not shorten them";
    EXPECT_TRUE(env.contains("hint"))
        << "a summary with matches must say how to get the bodies";
}

// INV-10 (ANTS-3699) — mode:"full" restores the bodies verbatim, and the
// envelope's shape flags flip with it.
TEST(McpInvariantCheck, Ants3699FullRestoresBodies) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    seedSpec(dir.path(), QStringLiteral("ANTS-9002"),
             QStringLiteral("src/widget.cpp"));

    const QJsonObject env = runCheck(dir.path(),
                                     QStringLiteral("src/widget.cpp"),
                                     QStringLiteral("full"));
    ASSERT_TRUE(env.value("ok").toBool());
    ASSERT_EQ(env.value("matched_count").toInt(), 1);
    EXPECT_EQ(env.value("mode").toString(), "full");
    EXPECT_TRUE(env.value("invariants_included").toBool());
    EXPECT_FALSE(env.contains("hint"));

    const QJsonObject spec =
        env.value("matched_specs").toArray().at(0).toObject();
    const QJsonArray invs = spec.value("invariants").toArray();
    ASSERT_EQ(invs.size(), 2);
    EXPECT_EQ(spec.value("invariants_count").toInt(), invs.size());
    EXPECT_TRUE(invs.at(0).toObject().value("body").toString().contains(
        QString(400, QLatin1Char('x'))))
        << "full mode must carry the invariant body verbatim";
}

// INV-11 (ANTS-3699) — an unknown mode refuses rather than silently picking
// one. A typo'd "brief" that quietly returned summary would look identical to
// a spec with no invariants.
TEST(McpInvariantCheck, Ants3699UnknownModeRefuses) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    seedSpec(dir.path(), QStringLiteral("ANTS-9003"),
             QStringLiteral("src/widget.cpp"));

    const QJsonObject env = runCheck(dir.path(),
                                     QStringLiteral("src/widget.cpp"),
                                     QStringLiteral("brief"));
    EXPECT_FALSE(env.value("ok").toBool());
    EXPECT_EQ(env.value("code").toString(), "bad_mode");
}
