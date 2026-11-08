// ANTS-6708 — the welcome dialog and Settings install the ants-*.sh hook
// pack, and the status flags a hook whose script is missing.
// Contract: docs/specs/ANTS-5558-welcome-dialog.md INV-16 and INV-17.
//
// Why this exists: after a reinstall, ~/.claude/settings.json still named the
// pack's scripts but ~/.claude/hooks/ was empty, so every Claude Code session
// reported missing hooks, and the only fix was tools/install-hooks.sh from a
// source checkout. An AppImage user has none.
//
// Every test runs under a temporary HOME (welcome_support.h Sandbox).

#include "claudesetup.h"

#include "../../_support/expect.h"
#include "../welcome_dialog/welcome_support.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>

ANTS_TEST_SCOPE();

using namespace ants::claude_setup;
using namespace welcome_test;

namespace {

const QString kPackDir = QStringLiteral(SRC_HOOKS_DIR);

QStringList packScripts() {
    return QDir(kPackDir).entryList({QStringLiteral("ants-*.sh"),
                                     QStringLiteral("_common.sh")},
                                    QDir::Files, QDir::Name);
}

QString hooksDir(const Sandbox &sb) { return sb.home + QStringLiteral("/.claude/hooks"); }
QString settingsPath(const Sandbox &sb) {
    return sb.home + QStringLiteral("/.claude/settings.json");
}

}  // namespace

// INV-16 — every script is copied, executable; settings.json gets the same
// entries tools/install-hooks.sh writes; a second run changes nothing; a
// user's own hook on a shared event is kept.
TEST(HookPackInstall, Inv16CopiesPackAndMatchesInstallScript) {
    Sandbox sb;
    ASSERT_TRUE(sb.valid());
    ASSERT_GE(packScripts().size(), 7) << qPrintable(kPackDir);
    const QString user = QStringLiteral("/opt/mine/prompt-hook.sh");
    ASSERT_TRUE(writeFile(settingsPath(sb),
        QStringLiteral("{\"model\":\"opus\",\"hooks\":{\"SessionStart\":[{\"hooks\":"
                       "[{\"type\":\"command\",\"command\":\"%1\"}]}]}}")
            .arg(user).toUtf8()));

    const Outcome o = installHookPack({kPackDir});
    ASSERT_TRUE(o.ok) << qPrintable(o.message);
    for (const QString &name : packScripts()) {
        const QString dst = hooksDir(sb) + QLatin1Char('/') + name;
        EXPECT_EQ(readFile(dst), readFile(kPackDir + QLatin1Char('/') + name)) << qPrintable(name);
        EXPECT_TRUE(QFileInfo(dst).isExecutable()) << qPrintable(name);
    }
    const QJsonObject root = readJson(settingsPath(sb));
    EXPECT_EQ(root.value("model").toString(), QStringLiteral("opus"));
    EXPECT_TRUE(root.value("ants_hooks_pack_v1").toBool());
    EXPECT_TRUE(QJsonDocument(root.value("hooks").toObject().value("SessionStart").toArray())
                    .toJson().contains(user.toUtf8()))
        << "the user's own SessionStart hook was dropped";

    // Parity: install-hooks.sh, run against a copy of the same starting file
    // with the same hooks dir, writes the same hooks object.
    const QString jq = QStandardPaths::findExecutable(QStringLiteral("jq"));
    if (jq.isEmpty()) GTEST_SKIP() << "jq not installed; parity half not run";
    const QString other = sb.path(QStringLiteral("script/settings.json"));
    ASSERT_TRUE(writeFile(other,
        QStringLiteral("{\"model\":\"opus\",\"hooks\":{\"SessionStart\":[{\"hooks\":"
                       "[{\"type\":\"command\",\"command\":\"%1\"}]}]}}")
            .arg(user).toUtf8()));
    QProcess p;
    p.start(QStringLiteral("bash"),
            {QStringLiteral(SRC_INSTALL_HOOKS_SH), QStringLiteral("--target"), other,
             QStringLiteral("--hooks-dir"), hooksDir(sb)});
    ASSERT_TRUE(p.waitForFinished(30000));
    ASSERT_EQ(p.exitCode(), 0) << p.readAllStandardError().toStdString();
    EXPECT_EQ(readJson(other).value("hooks").toObject(), root.value("hooks").toObject())
        << "C++: " << qPrintable(compact(root.value("hooks").toObject()))
        << "\nscript: " << qPrintable(compact(readJson(other).value("hooks").toObject()));

    // A second run leaves settings.json byte-identical.
    const QByteArray before = readFile(settingsPath(sb));
    ASSERT_TRUE(installHookPack({kPackDir}).ok);
    EXPECT_EQ(readFile(settingsPath(sb)), before);
}

// INV-16 — a settings.json that does not parse is refused and left alone.
TEST(HookPackInstall, Inv16RefusesCorruptSettings) {
    Sandbox sb;
    ASSERT_TRUE(sb.valid());
    const QByteArray broken = "{ \"hooks\": [ not json\n";
    ASSERT_TRUE(writeFile(settingsPath(sb), broken));
    EXPECT_FALSE(installHookPack({kPackDir}).ok);
    EXPECT_EQ(readFile(settingsPath(sb)), broken);
}

// INV-17 — the status: Missing before, Installed after, Partial naming a
// deleted pack script, Partial naming any other hook whose script is gone,
// Unavailable when this install carries no pack and nothing is missing.
TEST(HookPackInstall, Inv17StatusFlagsMissingScripts) {
    Sandbox sb;
    ASSERT_TRUE(sb.valid());
    EXPECT_EQ(hookPackStatus({kPackDir}).state, State::Missing);
    EXPECT_EQ(hookPackStatus({sb.path(QStringLiteral("nowhere"))}).state, State::Unavailable);

    ASSERT_TRUE(installHookPack({kPackDir}).ok);
    EXPECT_EQ(hookPackStatus({kPackDir}).state, State::Installed)
        << qPrintable(hookPackStatus({kPackDir}).detail);

    ASSERT_TRUE(QFile::remove(hooksDir(sb) + QStringLiteral("/ants-bash-veto.sh")));
    Status s = hookPackStatus({kPackDir});
    EXPECT_EQ(s.state, State::Partial);
    EXPECT_TRUE(s.detail.contains(QStringLiteral("ants-bash-veto.sh"))) << qPrintable(s.detail);
    // With no pack to restore from, the missing script is still reported.
    s = hookPackStatus({sb.path(QStringLiteral("nowhere"))});
    EXPECT_EQ(s.state, State::Partial);
    EXPECT_TRUE(s.detail.contains(QStringLiteral("ants-bash-veto.sh"))) << qPrintable(s.detail);

    ASSERT_TRUE(installHookPack({kPackDir}).ok);
    QJsonObject root = readJson(settingsPath(sb));
    QJsonObject hooks = root.value("hooks").toObject();
    QJsonArray stop = hooks.value("Stop").toArray();
    stop.append(QJsonObject{{"hooks", QJsonArray{QJsonObject{
        {"type", "command"}, {"command", sb.path(QStringLiteral("gone/other.sh"))}}}}});
    hooks["Stop"] = stop;
    root["hooks"] = hooks;
    ASSERT_TRUE(writeFile(settingsPath(sb), QJsonDocument(root).toJson()));
    s = hookPackStatus({kPackDir});
    EXPECT_EQ(s.state, State::Partial);
    EXPECT_TRUE(s.detail.contains(QStringLiteral("other.sh"))) << qPrintable(s.detail);
}

// INV-16 — the pack ships in every package, and both surfaces offer it.
TEST(HookPackInstall, Inv16PackShipsAndBothSurfacesOfferIt) {
    const auto read = [](const char *p) { return readFile(QString::fromUtf8(p)); };
    EXPECT_TRUE(read(SRC_CMAKELISTS_PATH).contains("DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/ants-terminal/claude-hooks"));
    EXPECT_TRUE(read(SRC_OPENSUSE_SPEC_PATH).contains("%{_datadir}/%{name}/claude-hooks"));
    EXPECT_TRUE(read(SRC_WELCOMEDIALOG_PATH).contains("cs::installHookPack"));
    EXPECT_TRUE(read(SRC_SETTINGSDIALOG_PATH).contains("claude_setup::installHookPack"));
}
