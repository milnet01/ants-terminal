// Source-grep + drift-guard harness for ANTS-1307 — the
// `project_conventions` MCP tool. See spec.md and
// docs/specs/ANTS-1307.md.
//
// Wiring is locked by source-grep. INV-5 is a real behavioural drift
// guard: every `source` path baked into the curated table must exist on
// disk.
//
// ANTS-4460 — this file used to say the verb "is not GUI-free exercisable"
// because it resolves through resolveRootCanonical(m_main, req). That is
// stale: with a non-empty caller_cwd the two-arg form returns rr.cwd and
// never reaches the MainWindow, so the verb runs against a synthetic root
// with m_main null. The claim is what kept the answer itself untested,
// which is how a curated table came to be served to projects it does not
// describe.

#include "../../_support/expect.h"

#include <fstream>
#include <set>
#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include "remotecontrol.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif
#ifndef SRC_RC_HEADER
#error "SRC_RC_HEADER compile definition required"
#endif
#ifndef ANTS_RC_SOURCES
#error "ANTS_RC_SOURCES compile definition required"
#endif
#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif
#ifndef ANTS_SOURCE_DIR
#error "ANTS_SOURCE_DIR compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {


bool has(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

std::string fnBody(const std::string &src, const char *decl) {
    const auto pos = src.find(decl);
    if (pos == std::string::npos) return {};
    auto end = src.find("\nQJsonDocument RemoteControl::cmd",
                        pos + std::string(decl).size());
    if (end == std::string::npos) end = src.size();
    return src.substr(pos, end - pos);
}

bool fileExists(const std::string &path) {
    std::ifstream f(path);
    return f.good();
}

// Extract every curated-table `source` path: the QStringLiteral that
// immediately precedes a `})` close of a conv.append({...,...}) pair.
// Matching `")});` (string-close, QStringLiteral-close, list-close,
// append-close) only catches the source slot — not `.md` mentions
// inside rule text.
std::set<std::string> extractSources(const std::string &body) {
    std::set<std::string> out;
    const std::string closeTok = "\")});";
    const std::string openTok = "QStringLiteral(\"";
    size_t p = 0;
    while ((p = body.find(closeTok, p)) != std::string::npos) {
        // The string content ends at p (the `"` of `")});`). Find the
        // QStringLiteral(" that opens it — the last one before p.
        const size_t open = body.rfind(openTok, p);
        if (open != std::string::npos) {
            const size_t start = open + openTok.size();
            if (start <= p) out.insert(body.substr(start, p - start));
        }
        p += closeTok.size();
    }
    return out;
}

}  // namespace

TEST(McpProjectConventions, WiringAndDriftGuard) {
    expect_reset();

    const std::string rcHdr = ants_test::slurpFile(SRC_RC_HEADER);
    const std::string rcCpp = ants_test::slurpRemoteControl();
    const std::string ciCpp = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string mwCpp = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);

    const std::string body =
        fnBody(rcCpp, "QJsonDocument RemoteControl::cmdProjectConventions(");

    // INV-1 — declaration + definition + anchor.
    expect(has(rcHdr, "cmdProjectConventions(const QJsonObject &req)"),
           "INV-1", "decl missing from remotecontrol.h");
    expect(!body.empty(),
           "INV-1", "definition missing from remotecontrol.cpp");
    expect(has(rcCpp, "ANTS-1307"),
           "INV-1", "ANTS-1307 anchor comment missing");

    // INV-2 — task_type enum validation.
    expect(has(body, "feature") && has(body, "bugfix") &&
           has(body, "refactor") && has(body, "docs") &&
           has(body, "test"),
           "INV-2", "task_type enum incomplete");
    expect(has(body, "bad_args"),
           "INV-2", "bad_args refusal missing");

    // INV-3 — no_project refusal.
    expect(has(body, "no_project"),
           "INV-3", "no_project refusal missing");

    // INV-4 — common rows + per-type branches.
    expect(has(body, "docs/standards/commits.md") &&
           has(body, "docs/standards/coding.md"),
           "INV-4", "common rows (commits.md / coding.md) missing");
    expect(has(body, "taskType == QLatin1String(\"feature\")") &&
           has(body, "taskType == QLatin1String(\"bugfix\")") &&
           has(body, "taskType == QLatin1String(\"refactor\")") &&
           has(body, "taskType == QLatin1String(\"docs\")"),
           "INV-4", "per-task-type branches missing");

    // INV-5 — DRIFT GUARD: every curated source path exists on disk.
    const std::set<std::string> sources = extractSources(body);
    expect(sources.size() >= 6,
           "INV-5", "expected >=6 distinct curated source paths");
    const std::string root = ANTS_SOURCE_DIR;
    for (const std::string &s : sources) {
        const std::string full = root + "/" + s;
        std::string msg = std::string("INV-5 source must exist: ") + s;
        expect(fileExists(full), "INV-5", msg.c_str());
    }

    // INV-6 — sources[] existence check via QFileInfo.
    expect(has(body, "QFileInfo") && has(body, "exists"),
           "INV-6", "sources[] existence check missing");

    // INV-7 — envelope keys.
    for (const char *k : {"\"task_type\"", "\"conventions\"",
                          "\"conventions_count\"", "\"sources\"",
                          "\"sources_count\""}) {
        std::string msg = std::string("INV-7 envelope key ") + k;
        expect(has(body, k), "INV-7", msg.c_str());
    }

    // INV-8 — MainWindow registration, Required contract.
    expect(has(mwCpp, "registerToolProvider(\"project_conventions\""),
           "INV-8", "project_conventions not registered");
    {
        const auto pos =
            mwCpp.find("registerToolProvider(\"project_conventions\"");
        const std::string window =
            pos == std::string::npos ? std::string()
                                     : mwCpp.substr(pos, 180);
        expect(has(window, "CallerCwdContract::Required"),
               "INV-8", "registration not Required");
    }

    // INV-9 — callerCwdContractFor Required.
    expect(has(ciCpp, "project_conventions") && has(ciCpp, "C::Required"),
           "INV-9", "callerCwdContractFor missing Required branch");

    // INV-10 — kindForName "convention".
    expect(has(ciCpp, "project_conventions") && has(ciCpp, "\"convention\""),
           "INV-10", "kindForName missing project_conventions -> convention");

    // INV-11 — token-cost ledger.
    expect((has(ciCpp, "project_conventions") && has(ciCpp, "400") &&
            has(ciCpp, "1500")),
           "INV-11", "token-cost {400,1500} for project_conventions missing");

    // INV-12 — tools/list schema with enum + selection_hint.
    expect(has(ciCpp, "t[\"name\"] = \"project_conventions\""),
           "INV-12", "tools/list schema entry missing");
    {
        const auto pos = ciCpp.find("t[\"name\"] = \"project_conventions\"");
        const auto endp = ciCpp.find("tools.append(t);", pos);
        const std::string block =
            (pos == std::string::npos || endp == std::string::npos)
                ? std::string()
                : ciCpp.substr(pos, endp - pos);
        expect(has(block, "selection_hint"),
               "INV-12", "schema missing selection_hint");
        expect(has(block, "typeEnum") || has(block, "\"enum\""),
               "INV-12", "task_type enum missing from schema");
        expect(has(block, "req.append(\"task_type\")") &&
               has(block, "req.append(\"caller_cwd\")"),
               "INV-12", "required args missing");
    }

    EXPECT_EQ(0, expect_failures());
}

// --------------------------------------------------------------- ANTS-4460 --
//
// The curated table reads nothing from the caller's project and its rows cite
// Ants-specific paths. Served to another project it is THIS project's
// conventions wearing that project's root, and the only signal was a per-row
// `exists:false` — a false bool, which compact:true drops by design.
//
// A reported run got five confident rules with every cited file absent, one of
// them prescribing a tests/features/<name>/ conformance test to a project that
// puts unit tests elsewhere and labels them differently.

TEST(McpProjectConventions, Ants4460DropsConventionsWhoseSourceIsAbsent) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();

    QJsonObject req;
    req[QStringLiteral("task_type")]  = QStringLiteral("feature");
    req[QStringLiteral("caller_cwd")] = root;

    RemoteControl rc(nullptr);
    const QJsonObject r = rc.cmdProjectConventions(req).object();
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(r).toJson().toStdString();

    // Nothing this table cites exists in an empty directory, so nothing in it
    // is this project's rule.
    EXPECT_EQ(r.value(QStringLiteral("conventions_count")).toInt(), 0)
        << "stated conventions read from documents that are not there: "
        << QJsonDocument(r).toJson().toStdString();
    EXPECT_EQ(r.value(QStringLiteral("sources_resolved")).toInt(), 0);
    EXPECT_TRUE(r.value(QStringLiteral("conventions_are_defaults")).toBool());

    // The hedge is a non-empty string on purpose: compact:true drops a false
    // bool, which is how exists:false became invisible.
    const QString warning = r.value(QStringLiteral("warning")).toString();
    EXPECT_FALSE(warning.isEmpty()) << "no hedge on an answer read from nothing";

    // Dropped, not silently omitted.
    const QJsonArray dropped =
        r.value(QStringLiteral("conventions_dropped")).toArray();
    EXPECT_GT(dropped.size(), 0);
    bool sawFeaturesRule = false;
    for (const auto &v : dropped) {
        if (v.toObject().value(QStringLiteral("source")).toString()
                == QStringLiteral("tests/features/README.md"))
            sawFeaturesRule = true;
    }
    EXPECT_TRUE(sawFeaturesRule)
        << "the tests/features conformance rule was still stated to a project "
           "that has no such directory";
}

TEST(McpProjectConventions, Ants4460KeepsConventionsThisProjectDoesDocument) {
    // The guard must not empty the answer where the documents ARE there: this
    // repository is the project the table was curated from.
    QJsonObject req;
    req[QStringLiteral("task_type")]  = QStringLiteral("feature");
    // Derived from a definition this file already requires, rather than a new
    // one: <root>/src/mainwindow.cpp up two levels is the repository root.
    const QString repoRoot =
        QFileInfo(QStringLiteral(SRC_MAINWINDOW_CPP_PATH)).dir().absolutePath()
        + QStringLiteral("/..");
    req[QStringLiteral("caller_cwd")] =
        QFileInfo(repoRoot).canonicalFilePath();

    RemoteControl rc(nullptr);
    const QJsonObject r = rc.cmdProjectConventions(req).object();
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(r).toJson().toStdString();

    EXPECT_GT(r.value(QStringLiteral("conventions_count")).toInt(), 0);
    EXPECT_GT(r.value(QStringLiteral("sources_resolved")).toInt(), 0);
    EXPECT_FALSE(r.contains(QStringLiteral("conventions_are_defaults")));
    EXPECT_FALSE(r.contains(QStringLiteral("warning")))
        << "hedged an answer whose every source is present: "
        << QJsonDocument(r).toJson().toStdString();
}
