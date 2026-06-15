// ANTS-1897 — MCP discoverability via SessionStart hook installer.
//
// Covers the 14 invariants in docs/specs/ANTS-1897.md.
//   INV-1       content-parity rewrite: byte-identical → idempotent
//               skip; in-version stale body or version bump → rewrite
//               (ANTS-2038)
//   INV-2, 2b   user-owned preserved / first-launch fresh write
//   INV-3       settings.json merge idempotent (marker substring)
//   INV-4       settings.json merge preserves unrelated entries
//   INV-5       disable removes the Ants entry
//   INV-6       selection_hint coverage (source-grep)
//   INV-7       selection_hint shape (≤280 chars, "Use " prefix)
//   INV-9       script exits 0 even when socket absent
//   INV-10      script stdout ≤ 1400 chars (ANTS-2140 raised from 1200)
//   INV-11      settings.json owner-only perms
//   INV-12      script byte-equality with template after arg()
//   INV-13      bad settings.json — no clobber
//   INV-14      qputenv("ANTS_MCP_SOCKET", ...) after startMcpServer (grep)

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>

#include "mcporientation.h"

namespace MO = ants::mcp_orientation;

namespace {

QByteArray readFileBytes(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    return f.readAll();
}

bool writeFileBytes(const QString &path, const QByteArray &body) {
    QFileInfo fi(path);
    if (!fi.absolutePath().isEmpty()) QDir().mkpath(fi.absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(body) == body.size();
}

QString scriptPathIn(const QString &home) {
    return home + QLatin1String("/.config/ants-terminal/hooks/mcp-orientation.sh");
}

QString settingsPathIn(const QString &home) {
    return home + QLatin1String("/.claude/settings.json");
}

QJsonObject readSettings(const QString &home) {
    QFile f(settingsPathIn(home));
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray raw = f.readAll();
    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(raw, &pe);
    return doc.isObject() ? doc.object() : QJsonObject();
}

int countAntsEntries(const QJsonObject &settings) {
    const auto sessions = settings.value("hooks").toObject()
                                  .value("SessionStart").toArray();
    int count = 0;
    const QString marker = MO::settingsMarkerSubstring();
    for (const auto &outerV : sessions) {
        const auto inner = outerV.toObject().value("hooks").toArray();
        for (const auto &eV : inner) {
            if (eV.toObject().value("command").toString().contains(marker)) {
                ++count;
            }
        }
    }
    return count;
}

}  // namespace

// ---------------------------------------------------------------------
// INV-1, INV-2 (first-launch), INV-12 — fresh install writes a file
// byte-identical to the template after arg(ANTS_VERSION).
TEST(McpOrientation_Inv2, FirstLaunchWritesFresh) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    auto r = MO::installAt(tmp.path());
    EXPECT_TRUE(r.ok) << r.warning.toStdString();
    EXPECT_TRUE(QFile::exists(scriptPathIn(tmp.path())));

    const QByteArray got = readFileBytes(scriptPathIn(tmp.path()));
    const QByteArray want = MO::orientationScriptTemplate()
                                .arg(QStringLiteral(ANTS_VERSION)).toUtf8();
    EXPECT_EQ(got, want);
}

// INV-1 — idempotent skip on matching marker version.
TEST(McpOrientation_Inv1, IdempotentRewrite) {
    QTemporaryDir tmp;
    auto r1 = MO::installAt(tmp.path());
    ASSERT_TRUE(r1.ok);

    const QByteArray before = readFileBytes(scriptPathIn(tmp.path()));
    QFileInfo fi(scriptPathIn(tmp.path()));
    const auto mtimeBefore = fi.lastModified();

    // Brief sleep so a second write would have a distinguishable mtime.
    QThread::msleep(20);

    auto r2 = MO::installAt(tmp.path());
    ASSERT_TRUE(r2.ok);

    const QByteArray after = readFileBytes(scriptPathIn(tmp.path()));
    QFileInfo fi2(scriptPathIn(tmp.path()));
    EXPECT_EQ(before, after);
    // Marker version matched AND body byte-identical → no rewrite →
    // mtime unchanged.
    EXPECT_EQ(fi.lastModified(), fi2.lastModified());
}

// INV-1 (ANTS-2038) — a marker-present file at the SAME version but
// with a stale body is rewritten to the canonical template. The prior
// version-only gate (markerVersion == runningVersion → skip) would
// have left it stale; content-parity rewrites on any byte difference.
TEST(McpOrientation_Inv1, InVersionBodyRefresh) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QByteArray canonical = MO::orientationScriptTemplate()
                                     .arg(QStringLiteral(ANTS_VERSION)).toUtf8();
    // Seed a marker-PRESENT file at the running version but with a
    // mutated body (an in-version prelude reword). Line 2 — the
    // `# ants-orientation-version:` marker — is preserved verbatim, so
    // the file classifies as Ants-managed, not user-owned (INV-2).
    QByteArray stale = canonical;
    stale.replace("session_orient", "STALE_PRELUDE_TOKEN");
    ASSERT_NE(stale, canonical)
        << "precondition: the mutation must change the body";
    ASSERT_TRUE(stale.contains("# ants-orientation-version:"))
        << "precondition: the marker line must survive the mutation";
    ASSERT_TRUE(writeFileBytes(scriptPathIn(tmp.path()), stale));

    auto r = MO::installAt(tmp.path());
    ASSERT_TRUE(r.ok) << r.warning.toStdString();

    const QByteArray after = readFileBytes(scriptPathIn(tmp.path()));
    EXPECT_EQ(after, canonical)
        << "ANTS-2038: a marker-present file with a stale body (same "
           "version) must be rewritten to the canonical template";
}

// INV-2 — pre-existing file without marker line is preserved (Ants
// does NOT overwrite a user-owned file).
TEST(McpOrientation_Inv2, UserOwnedPreserved) {
    QTemporaryDir tmp;
    const QString p = scriptPathIn(tmp.path());
    const QByteArray userBody =
        "#!/bin/sh\n"
        "echo 'user owns this file'\n";
    ASSERT_TRUE(writeFileBytes(p, userBody));

    auto r = MO::installAt(tmp.path());
    // Install reports a warning (user-owned), but settings merge still
    // succeeds — overall ok is still true.
    EXPECT_TRUE(r.ok);
    EXPECT_FALSE(r.warning.isEmpty()) << "expected user-owned warning";

    EXPECT_EQ(readFileBytes(p), userBody) << "user-owned script overwritten";
}

// INV-3 — settings merge is idempotent on the marker substring.
TEST(McpOrientation_Inv3, SettingsMergeIdempotent) {
    QTemporaryDir tmp;
    ASSERT_TRUE(MO::installAt(tmp.path()).ok);
    ASSERT_TRUE(MO::installAt(tmp.path()).ok);
    ASSERT_TRUE(MO::installAt(tmp.path()).ok);
    EXPECT_EQ(countAntsEntries(readSettings(tmp.path())), 1);
}

// ANTS-1901 — installAt must sweep ALL pre-existing marker-matching
// entries (not just update the first one), then append a single
// canonical entry. Mimics a settings.json that accumulated duplicates
// from buggy prior versions where the marker substring failed to match
// the production path "Ants Terminal/hooks/mcp-orientation.sh".
TEST(McpOrientation_Inv3, SweepsExistingDuplicates) {
    QTemporaryDir tmp;
    const QString sp = settingsPathIn(tmp.path());

    // Seed three entries that all reference a script at the marker
    // suffix path (the production form with capital + space, plus a
    // single-quoted variant — three different cosmetic shapes, same
    // marker suffix).
    const QString prodLike = "/somewhere/.config/Ants Terminal/hooks/mcp-orientation.sh";
    const QByteArray seed = QString(R"({
      "hooks": {
        "SessionStart": [
          {"hooks":[{"type":"command","command":"bash %1","timeout":3}]},
          {"hooks":[{"type":"command","command":"bash %1","timeout":3}]},
          {"hooks":[{"type":"command","command":"bash '%1'","timeout":3}]}
        ]
      }
    })").arg(prodLike).toUtf8();
    ASSERT_TRUE(writeFileBytes(sp, seed));

    auto r = MO::installAt(tmp.path());
    ASSERT_TRUE(r.ok) << r.warning.toStdString();
    const auto root = readSettings(tmp.path());
    const auto sessions = root.value("hooks").toObject()
                              .value("SessionStart").toArray();
    // Dump the file content for diagnostics.
    const QByteArray dump = readFileBytes(sp);
    EXPECT_EQ(sessions.size(), 1)
        << "stale duplicates not swept: " << sessions.size()
        << " SessionStart entries remain. File content:\n"
        << dump.toStdString();
    EXPECT_EQ(countAntsEntries(root), 1)
        << "installAt must sweep all marker-matching entries down to one";
}

// ANTS-1901 — the written hook command must be runnable when the
// install path contains a space. Production Qt sets the application
// name to "Ants Terminal" so QStandardPaths::AppConfigLocation
// resolves to ".../Ants Terminal/...", which bash splits at the space
// unless the path is quoted.
TEST(McpOrientation_Inv3, CommandRunnableWithSpacesInPath) {
    QTemporaryDir tmp;
    const QString homeWithSpace = tmp.path() + QStringLiteral("/home dir");
    ASSERT_TRUE(QDir().mkpath(homeWithSpace));

    auto r = MO::installAt(homeWithSpace);
    ASSERT_TRUE(r.ok) << r.warning.toStdString();

    const auto root = readSettings(homeWithSpace);
    const auto sessions = root.value("hooks").toObject()
                              .value("SessionStart").toArray();
    ASSERT_EQ(sessions.size(), 1);
    const QString cmd = sessions.at(0).toObject().value("hooks").toArray()
                                .at(0).toObject().value("command").toString();

    QProcess p;
    auto env = QProcessEnvironment::systemEnvironment();
    env.remove("ANTS_MCP_SOCKET");
    p.setProcessEnvironment(env);
    p.start("bash", {"-c", cmd});
    ASSERT_TRUE(p.waitForStarted(2000));
    ASSERT_TRUE(p.waitForFinished(5000));
    EXPECT_EQ(p.exitCode(), 0)
        << "command failed — likely an unquoted path with a space; cmd="
        << cmd.toStdString()
        << "; stderr=" << p.readAllStandardError().toStdString();
}

// INV-4 — merge preserves unrelated top-level keys + sibling
// SessionStart entries from other tools.
TEST(McpOrientation_Inv4, SettingsMergePreservesUnrelated) {
    QTemporaryDir tmp;
    const QString sp = settingsPathIn(tmp.path());
    // Pre-seed with a UserPromptSubmit hook (mimics the user's actual
    // settings) + a foreign SessionStart entry + an unrelated key.
    const QByteArray seed = R"({
      "theme": "custom",
      "hooks": {
        "UserPromptSubmit": [
          {"hooks": [{"type":"command",
                       "command":"bash /tmp/git-context.sh",
                       "timeout":5}]}
        ],
        "SessionStart": [
          {"hooks": [{"type":"command",
                       "command":"/usr/local/bin/some-other-tool",
                       "timeout":10}]}
        ]
      }
    })";
    ASSERT_TRUE(writeFileBytes(sp, seed));

    ASSERT_TRUE(MO::installAt(tmp.path()).ok);

    auto root = readSettings(tmp.path());
    EXPECT_EQ(root.value("theme").toString(), "custom");

    auto hooks = root.value("hooks").toObject();
    auto ups = hooks.value("UserPromptSubmit").toArray();
    ASSERT_EQ(ups.size(), 1);
    EXPECT_TRUE(ups.at(0).toObject().value("hooks").toArray()
                .at(0).toObject().value("command").toString()
                .contains("git-context.sh"));

    auto ss = hooks.value("SessionStart").toArray();
    // Foreign entry survives + Ants entry appended.
    ASSERT_EQ(ss.size(), 2);
    EXPECT_EQ(countAntsEntries(root), 1);
}

// INV-5 — disable removes the Ants entry (but not unrelated ones).
TEST(McpOrientation_Inv5, DisableRemovesEntry) {
    QTemporaryDir tmp;
    ASSERT_TRUE(MO::installAt(tmp.path()).ok);
    EXPECT_EQ(countAntsEntries(readSettings(tmp.path())), 1);

    ASSERT_TRUE(MO::uninstallAt(tmp.path()).ok);
    EXPECT_EQ(countAntsEntries(readSettings(tmp.path())), 0);
}

// INV-9 — running the installed script without a socket exits 0
// silently (no stdout, no stderr, exit 0).
TEST(McpOrientation_Inv9, ScriptExitZero) {
    QTemporaryDir tmp;
    ASSERT_TRUE(MO::installAt(tmp.path()).ok);

    QProcess p;
    p.setProcessEnvironment(QProcessEnvironment::systemEnvironment());
    // Explicitly clear the env var so the guard exits silently.
    auto env = QProcessEnvironment::systemEnvironment();
    env.remove("ANTS_MCP_SOCKET");
    p.setProcessEnvironment(env);
    p.start("bash", {scriptPathIn(tmp.path())});
    ASSERT_TRUE(p.waitForStarted(2000));
    ASSERT_TRUE(p.waitForFinished(5000));
    EXPECT_EQ(p.exitCode(), 0);
    EXPECT_EQ(p.readAllStandardOutput().size(), 0);
}

// INV-10 — running with a present socket prints ≤ 1400 chars.
TEST(McpOrientation_Inv10, ScriptOutputByteCap) {
    QTemporaryDir tmp;
    ASSERT_TRUE(MO::installAt(tmp.path()).ok);

    // Create a real socket file under the tmp dir.
    const QString fakeSocket = tmp.path() + QStringLiteral("/ants-mcp.sock");
    // `[ -S file ]` requires the file to be a real socket — bash's test
    // builtin will return false for a regular file. Use socat-like
    // creation? Cheapest: use python to bind a unix socket if available.
    // For this fast test we skip if no quick way; replace the script's
    // guard via an `ANTS_MCP_SOCKET_TEST_FORCE` override.
    QProcess maker;
    maker.start("python3", {"-c",
        QStringLiteral("import socket,os,sys;"
                       "s=socket.socket(socket.AF_UNIX);"
                       "s.bind(sys.argv[1]);"
                       "print('ok')").toUtf8(),
        fakeSocket});
    if (!maker.waitForStarted(1000) || !maker.waitForFinished(2000)) {
        GTEST_SKIP() << "python3 unavailable for socket fixture";
    }

    QProcess p;
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert("ANTS_MCP_SOCKET", fakeSocket);
    p.setProcessEnvironment(env);
    p.start("bash", {scriptPathIn(tmp.path())});
    ASSERT_TRUE(p.waitForStarted(2000));
    ASSERT_TRUE(p.waitForFinished(5000));
    EXPECT_EQ(p.exitCode(), 0);
    const QByteArray out = p.readAllStandardOutput();
    EXPECT_LE(out.size(), 1400) << "stdout=" << out.size() << " bytes";
    // ANTS-2140 — the prelude advertises codebase_index (layer-3: query
    // the index instead of grep) so a session reaches for the verb.
    EXPECT_TRUE(out.contains("codebase_index"))
        << "ANTS-2140: prelude must advertise the codebase_index verb";
    EXPECT_GT(out.size(), 100) << "expected the prelude to print";

    QFile::remove(fakeSocket);  // best-effort cleanup
}

// ANTS-1985 INV-14 — the prelude's "Full catalog:" line is repointed
// at `tool_info {catalog:true}`. (The ≤1400 B cap itself is INV-10
// above; this locks the content of the repoint so the prelude actually
// names the new verb.)
TEST(McpOrientation_Inv14, FullCatalogNamesToolInfoCatalog) {
    const QString tmpl = MO::orientationScriptTemplate();
    const int pos = tmpl.indexOf(QStringLiteral("Full catalog"));
    ASSERT_NE(pos, -1) << "INV-14: 'Full catalog' line missing";
    // Take the rest of that line + the continuation line.
    const QString tail = tmpl.mid(pos, 160);
    EXPECT_TRUE(tail.contains(QStringLiteral("tool_info")))
        << "INV-14: Full-catalog line must name tool_info";
    EXPECT_TRUE(tail.contains(QStringLiteral("catalog")))
        << "INV-14: Full-catalog line must name the catalog arg";
}

// ANTS-1971 — the feedback-tool hint is surfaced CONDITIONALLY: it
// prints only when the project CC launched in keeps a
// *_Ants_MCP_Feedback.md. Absent the file, the always-on prelude must
// NOT mention feedback_query / feedback_log (kept out to honour the
// INV-10 cap, ANTS-1970); present the file, the hint appears.
TEST(McpOrientation_Ants1971, ConditionalFeedbackHint) {
    QTemporaryDir tmp;
    ASSERT_TRUE(MO::installAt(tmp.path()).ok);

    const QString fakeSocket = tmp.path() + QStringLiteral("/ants-mcp.sock");
    QProcess maker;
    maker.start("python3", {"-c",
        QStringLiteral("import socket,os,sys;"
                       "s=socket.socket(socket.AF_UNIX);"
                       "s.bind(sys.argv[1]);"
                       "print('ok')").toUtf8(),
        fakeSocket});
    if (!maker.waitForStarted(1000) || !maker.waitForFinished(2000)) {
        GTEST_SKIP() << "python3 unavailable for socket fixture";
    }

    // A project dir WITHOUT a feedback file → no hint.
    QTemporaryDir noFeedback;
    auto runWithProjectDir = [&](const QString &projectDir) -> QByteArray {
        QProcess p;
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("ANTS_MCP_SOCKET", fakeSocket);
        env.insert("CLAUDE_PROJECT_DIR", projectDir);
        p.setProcessEnvironment(env);
        p.start("bash", {scriptPathIn(tmp.path())});
        EXPECT_TRUE(p.waitForStarted(2000));
        EXPECT_TRUE(p.waitForFinished(5000));
        EXPECT_EQ(p.exitCode(), 0);
        return p.readAllStandardOutput();
    };

    const QByteArray without = runWithProjectDir(noFeedback.path());
    EXPECT_FALSE(without.contains("feedback_query"))
        << "no feedback file present — hint must stay silent";

    // A project dir WITH a feedback file → hint present.
    QTemporaryDir withFeedback;
    ASSERT_TRUE(writeFileBytes(
        withFeedback.path() + QStringLiteral("/Demo_Ants_MCP_Feedback.md"),
        QByteArray("# placeholder\n")));
    const QByteArray with = runWithProjectDir(withFeedback.path());
    EXPECT_TRUE(with.contains("feedback_query"))
        << "feedback file present — hint must point at feedback_query/log";
    EXPECT_TRUE(with.contains("feedback_log"));

    QFile::remove(fakeSocket);  // best-effort cleanup
}

// INV-11 — settings.json after merge is mode 0600.
TEST(McpOrientation_Inv11, SettingsPermsOwnerOnly) {
    QTemporaryDir tmp;
    ASSERT_TRUE(MO::installAt(tmp.path()).ok);
    QFileInfo fi(settingsPathIn(tmp.path()));
    ASSERT_TRUE(fi.exists());
    QFile::Permissions p = fi.permissions();
    // Owner read+write must be on; group + other must be off.
    EXPECT_TRUE(p.testFlag(QFile::ReadOwner));
    EXPECT_TRUE(p.testFlag(QFile::WriteOwner));
    EXPECT_FALSE(p.testFlag(QFile::ReadGroup));
    EXPECT_FALSE(p.testFlag(QFile::ReadOther));
}

// INV-13 — bad JSON refuses + warns + leaves file alone.
TEST(McpOrientation_Inv13, BadJsonNoClobber) {
    QTemporaryDir tmp;
    const QString sp = settingsPathIn(tmp.path());
    const QByteArray garbage = "this is not { valid JSON";
    ASSERT_TRUE(writeFileBytes(sp, garbage));

    auto r = MO::installAt(tmp.path());
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.warning.isEmpty());
    // File still on disk + byte-identical to garbage.
    EXPECT_EQ(readFileBytes(sp), garbage);
}

// INV-6 / INV-7 / INV-14 — source-grep contracts.
//   INV-6: every "*Tool[\"name\"] =" site has a sibling "*Tool[\"selection_hint\"] =".
//   INV-7: every selection_hint is ≤ 280 chars + starts with "Use ".
//   INV-14: qputenv("ANTS_MCP_SOCKET", ...) is called right after
//           m_claudeIntegration->startMcpServer(mcpSocket) in mainwindow.cpp.

// Helper: read whole file as QString.
namespace {
QString slurp(const char *path) {
    QFile f(QString::fromUtf8(path));
    if (!f.open(QIODevice::ReadOnly)) return QString();
    return QString::fromUtf8(f.readAll());
}
}  // namespace

// Project source paths are baked in via CMake -D defines. test_core
// uses `SRC_MAINWINDOW_CPP` (no `_PATH` suffix) per the existing
// convention; provide a fallback so the file compiles outside the
// bundle for development convenience.
#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#  define SRC_CLAUDE_INTEGRATION_CPP_PATH "src/claudeintegration.cpp"
#endif
#ifndef SRC_MAINWINDOW_CPP
#  define SRC_MAINWINDOW_CPP "src/mainwindow.cpp"
#endif

TEST(McpOrientation_Inv7, SelectionHintFormat) {
    const QString src = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(src.isEmpty())
        << "could not read " SRC_CLAUDE_INTEGRATION_CPP_PATH;

    // Find every selection_hint string literal: starts with
    // `["selection_hint"] = QStringLiteral(` and continues with
    // concatenated string literals until a `);`.
    QRegularExpression rx(
        R"(\[\"selection_hint\"\]\s*=\s*QStringLiteral\(\s*((?:\"(?:[^\"\\]|\\.)*\"\s*)+)\);)");
    auto it = rx.globalMatch(src);
    int total = 0;
    int badPrefix = 0;
    int tooLong = 0;
    while (it.hasNext()) {
        ++total;
        auto m = it.next();
        const QString blob = m.captured(1);
        // Concatenate the literal pieces — strip the surrounding " and
        // unescape \" / \\ minimally for our length+prefix check.
        QString joined;
        QRegularExpression piece(R"(\"((?:[^\"\\]|\\.)*)\")");
        auto pi = piece.globalMatch(blob);
        while (pi.hasNext()) {
            auto pm = pi.next();
            joined += pm.captured(1)
                          .replace(QStringLiteral("\\\""), QStringLiteral("\""))
                          .replace(QStringLiteral("\\\\"), QStringLiteral("\\"));
        }
        if (joined.size() > 280) ++tooLong;
        if (!joined.startsWith(QStringLiteral("Use "), Qt::CaseInsensitive))
            ++badPrefix;
    }
    EXPECT_GE(total, 60) << "expected ≥60 selection_hint sites";
    EXPECT_EQ(badPrefix, 0)
        << "selection_hint(s) not starting with 'Use ' — see ANTS-1897 INV-7";
    EXPECT_EQ(tooLong, 0)
        << "selection_hint(s) > 280 chars — see ANTS-1897 INV-7";
}

TEST(McpOrientation_Inv6, SelectionHintCoverage) {
    // Source-grep over claudeintegration.cpp: every `*Tool["name"] =`
    // site that registers a tool (i.e. excluding serverInfo / props /
    // env-row sites) must have a sibling `*Tool["selection_hint"] =`
    // assignment within the next ~60 lines (the typical descriptor
    // block size). Cheap approximation of the runtime tools/list test.
    const QString src = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(src.isEmpty());

    // Net counts: 70 name hits − 3 exclusions = 67 tools;
    // 68 selection_hint hits − 1 exclusion = 67 hints.
    QRegularExpression nameRx(R"(\[\"name\"\]\s*=\s*)");
    QRegularExpression hintRx(R"(\[\"selection_hint\"\]\s*=\s*)");
    int nameTotal = 0, hintTotal = 0;
    {
        auto it = nameRx.globalMatch(src);
        while (it.hasNext()) { it.next(); ++nameTotal; }
    }
    {
        auto it = hintRx.globalMatch(src);
        while (it.hasNext()) { it.next(); ++hintTotal; }
    }
    EXPECT_GE(nameTotal, 60);
    EXPECT_GE(hintTotal, 60);
    // ≤ 4 difference accounts for non-tool [name] sites
    // (serverInfo + props + env-row) and the matching env-row
    // selection_hint site (1).
    EXPECT_LE(std::abs(nameTotal - hintTotal), 4)
        << "selection_hint coverage drift — name=" << nameTotal
        << " hint=" << hintTotal;
}

TEST(McpOrientation_Inv14, MainWindowExportsSocket) {
    const QString src = slurp(SRC_MAINWINDOW_CPP);
    ASSERT_FALSE(src.isEmpty());

    // Find startMcpServer call site.
    QRegularExpression startRx(
        R"(m_claudeIntegration->startMcpServer\(mcpSocket\))");
    auto m = startRx.match(src);
    ASSERT_TRUE(m.hasMatch())
        << "startMcpServer(mcpSocket) not found in mainwindow.cpp";

    const int startEnd = m.capturedEnd();
    // Look ahead at most ~2000 chars for the qputenv contract.
    const QString tail = src.mid(startEnd, 2000);
    EXPECT_TRUE(tail.contains(QStringLiteral("qputenv(\"ANTS_MCP_SOCKET\"")))
        << "qputenv(\"ANTS_MCP_SOCKET\", ...) not found within ~2000 chars "
           "after startMcpServer(mcpSocket) — see ANTS-1897 INV-14";
}
