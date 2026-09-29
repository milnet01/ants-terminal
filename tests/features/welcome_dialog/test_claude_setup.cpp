// Feature-conformance tests for docs/specs/ANTS-5558-welcome-dialog.md —
// the ants::claude_setup module (src/claudesetup.{h,cpp}).
//
// INV-5 — the moved hook installers behave as before (7 events, idempotent,
//         a settings.json that does not parse is copied aside and not written).
// INV-6 — registerMcp's argv, mcpd::locateLaunch, the About query's source.
// INV-7 — the CLAUDE.md note lives between its markers.
// INV-8 — shell integration: two blocks, idempotent, key kept.
//
// Why this exists: the setup steps moved out of SettingsDialog into one
// module the welcome dialog shares; a move that changes what is written, a
// shell-string registration, a second CLAUDE.md block, or a regenerated
// OSC 133 key would each silently break a user's setup.
//
// Every test runs under a temporary HOME (welcome_support.h Sandbox); nothing
// reads or writes the real ~/.claude, ~/.claude.json or shell rc files.

#include "claudesetup.h"
#include "configpaths.h"
#include "mcpdversion.h"

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"
#include "welcome_support.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

ANTS_TEST_SCOPE();

using namespace ants::claude_setup;
using namespace welcome_test;

namespace {

const char *stateName(State s) {
    switch (s) {
    case State::Installed:   return "Installed";
    case State::Partial:     return "Partial";
    case State::Missing:     return "Missing";
    case State::Unavailable: return "Unavailable";
    }
    return "?";
}

int entriesWithCommand(const QJsonArray &arr, const QString &cmd) {
    int n = 0;
    for (const QJsonValue &v : arr)
        for (const QJsonValue &h : v.toObject().value("hooks").toArray())
            if (h.toObject().value("command").toString() == cmd) ++n;
    return n;
}

QStringList corruptSiblings(const QString &settingsPath) {
    const QFileInfo fi(settingsPath);
    return QDir(fi.absolutePath())
        .entryList({fi.fileName() + QStringLiteral(".corrupt-*")}, QDir::Files);
}

// A settings.json that does not parse is copied aside and left unwritten.
void expectRefusesCorrupt(const char *tag, Outcome (*install)()) {
    Sandbox sb;
    ASSERT_TRUE(sb.valid());
    const QString settings = sb.home + QStringLiteral("/.claude/settings.json");
    const QByteArray broken = "{ \"hooks\": [ this is not json\n";
    ASSERT_TRUE(writeFile(settings, broken));

    const Outcome o = install();
    expect(!o.ok, tag,
           QStringLiteral("a settings.json that does not parse must be refused "
                          "(Outcome.ok == false); got ok=true, message: %1")
               .arg(o.message));
    expect(readFile(settings) == broken, tag,
           QStringLiteral("the unparsable settings.json must be left byte-for-"
                          "byte unwritten; expected %1, actual %2")
               .arg(clip(broken), clip(readFile(settings))));
    const QStringList aside = corruptSiblings(settings);
    expect(!aside.isEmpty(), tag,
           QStringLiteral("rotateCorruptFileAside must leave a "
                          "settings.json.corrupt-* copy beside it; none found"));
    if (!aside.isEmpty())
        expect(readFile(QFileInfo(settings).absolutePath() + QLatin1Char('/') +
                        aside.first()) == broken,
               tag, QStringLiteral("the aside copy must hold the original bytes"));
}

// Executable ants-mcpd beside an app dir.
QString makeMcpdBeside(const QString &dir) {
    const QString p = dir + QStringLiteral("/ants-mcpd");
    makeExecutable(p);
    return p;
}

QStringList stripClaude(QStringList argv) {
    if (!argv.isEmpty() && (argv.first() == QLatin1String("claude") ||
                            argv.first().endsWith(QLatin1String("/claude"))))
        argv.removeFirst();
    return argv;
}

}  // namespace

// ---------------------------------------------------------------- INV-5 ----

TEST(ClaudeSetup, HookInstallersIdempotentAndRefuseCorrupt) {
    expect_reset();
    const QStringList events{
        QStringLiteral("SessionStart"),       QStringLiteral("PreToolUse"),
        QStringLiteral("PostToolUse"),        QStringLiteral("Stop"),
        QStringLiteral("PreCompact"),         QStringLiteral("PermissionRequest"),
        QStringLiteral("PostToolUseFailure")};

    // --- status-bar hooks: seven events, once, foreign content kept.
    {
        Sandbox sb;
        ASSERT_TRUE(sb.valid());
        const QString settings = sb.home + QStringLiteral("/.claude/settings.json");
        const QString fwd = ConfigPaths::antsClaudeForwardScript();
        ASSERT_TRUE(writeFile(
            settings,
            "{\"model\":\"opus\",\"hooks\":{\"UserPromptSubmit\":[{\"hooks\":"
            "[{\"type\":\"command\",\"command\":\"/x/foreign.sh\"}]}]}}"));

        const Outcome o1 = installStatusHooks();
        expect(o1.ok, "INV5a/status-first-install-ok",
               QStringLiteral("installStatusHooks() on a parsable settings.json "
                              "must succeed; expected ok=true, actual ok=false, "
                              "message: %1").arg(o1.message));
        const QJsonObject after1 = readJson(settings);
        const QJsonObject hooks1 = after1.value("hooks").toObject();
        int wired = 0;
        for (const QString &ev : events) {
            const int n = entriesWithCommand(hooks1.value(ev).toArray(), fwd);
            if (n == 1) ++wired;
            expect(n == 1, "INV5b/status-event-wired-once",
                   QStringLiteral("event %1: expected exactly 1 entry running %2, "
                                  "actual %3 (settings: %4)")
                       .arg(ev, fwd).arg(n).arg(compact(after1)));
        }
        expect(wired == 7, "INV5c/all-seven-events",
               QStringLiteral("expected all 7 events wired, actual %1").arg(wired));
        expect(after1.value("model").toString() == QLatin1String("opus"),
               "INV5d/status-keeps-foreign-key",
               QStringLiteral("a non-hooks key must survive; settings: %1")
                   .arg(compact(after1)));
        expect(entriesWithCommand(hooks1.value("UserPromptSubmit").toArray(),
                                  QStringLiteral("/x/foreign.sh")) == 1,
               "INV5e/status-keeps-foreign-hook",
               QStringLiteral("a hook the user already had must survive; "
                              "settings: %1").arg(compact(after1)));

        const Outcome o2 = installStatusHooks();
        const QJsonObject after2 = readJson(settings);
        expect(o2.ok, "INV5f/status-second-install-ok",
               QStringLiteral("second run must succeed; message: %1").arg(o2.message));
        expect(after2 == after1, "INV5g/status-second-run-adds-no-duplicate",
               QStringLiteral("a second run must change nothing; expected %1, "
                              "actual %2").arg(compact(after1), compact(after2)));
    }

    // --- git-context hook: one UserPromptSubmit entry, once.
    {
        Sandbox sb;
        ASSERT_TRUE(sb.valid());
        const QString settings = sb.home + QStringLiteral("/.claude/settings.json");
        const QString script = ConfigPaths::antsClaudeGitContextScript();
        ASSERT_TRUE(writeFile(
            settings,
            "{\"hooks\":{\"UserPromptSubmit\":[{\"hooks\":[{\"type\":\"command\","
            "\"command\":\"/x/foreign.sh\"}]}]}}"));

        const Outcome o1 = installGitContextHook();
        expect(o1.ok, "INV5h/git-first-install-ok",
               QStringLiteral("installGitContextHook() must succeed; expected "
                              "ok=true, actual ok=false, message: %1").arg(o1.message));
        const QJsonObject after1 = readJson(settings);
        const QJsonArray ups1 =
            after1.value("hooks").toObject().value("UserPromptSubmit").toArray();
        expect(entriesWithCommand(ups1, script) == 1, "INV5i/git-one-entry",
               QStringLiteral("expected exactly 1 UserPromptSubmit entry running "
                              "%1, actual %2 (settings: %3)")
                   .arg(script).arg(entriesWithCommand(ups1, script))
                   .arg(compact(after1)));
        expect(entriesWithCommand(ups1, QStringLiteral("/x/foreign.sh")) == 1,
               "INV5j/git-keeps-foreign-hook",
               QStringLiteral("the user's own UserPromptSubmit hook must survive; "
                              "settings: %1").arg(compact(after1)));

        (void)installGitContextHook();
        const QJsonObject after2 = readJson(settings);
        expect(after2 == after1, "INV5k/git-second-run-adds-no-duplicate",
               QStringLiteral("a second run must change nothing; expected %1, "
                              "actual %2").arg(compact(after1), compact(after2)));
    }

    // --- both refuse a settings.json that does not parse.
    expectRefusesCorrupt("INV5l/status-refuses-corrupt", &installStatusHooks);
    expectRefusesCorrupt("INV5m/git-refuses-corrupt", &installGitContextHook);

    ASSERT_EQ(0, expect_finish());
}

// ---------------------------------------------------------------- INV-6 ----

TEST(ClaudeSetup, McpRegistrationArgv) {
    expect_reset();
    Sandbox sb;
    ASSERT_TRUE(sb.valid());
    // No stray ants-mcpd or claude on PATH: every arm decides for itself.
    const QString emptyPath = sb.path(QStringLiteral("emptybin"));
    QDir().mkpath(emptyPath);
    sb.env.setEnv("PATH", emptyPath.toUtf8());

    const QString appDir = sb.path(QStringLiteral("app"));
    QDir().mkpath(appDir);
    const QString sibling = makeMcpdBeside(appDir);

    // --- the command to register.
    {
        const QString img = QStringLiteral("/opt/My Apps/Ants.AppImage");
        const QStringList l = mcpLaunchCommand(img, appDir);
        expect(l == QStringList({img, QStringLiteral("--mcpd")}),
               "INV6a/appimage-launch-is-appimage-mcpd",
               QStringLiteral("with APPIMAGE set the command is `$APPIMAGE "
                              "--mcpd` even when an ants-mcpd sits beside the "
                              "binary; expected [%1, --mcpd], actual [%2]")
                   .arg(img, l.join(QStringLiteral(", "))));
    }
    {
        const QStringList l = mcpLaunchCommand(QString(), appDir);
        expect(l.size() == 1 &&
                   QFileInfo(l.first()).absoluteFilePath() ==
                       QFileInfo(sibling).absoluteFilePath(),
               "INV6b/no-appimage-launch-is-sibling",
               QStringLiteral("without APPIMAGE the command is the ants-mcpd "
                              "beside the binary; expected [%1], actual [%2]")
                   .arg(sibling, l.join(QStringLiteral(", "))));
    }
    {
        const QString bare = sb.path(QStringLiteral("bare"));
        QDir().mkpath(bare);
        const QStringList l = mcpLaunchCommand(QString(), bare);
        expect(l.isEmpty(), "INV6c/none-found-is-empty",
               QStringLiteral("with no AppImage, no sibling and none on PATH the "
                              "command must be empty; actual [%1]")
                   .arg(l.join(QStringLiteral(", "))));
    }

    // --- the claude argv lists, as an argv (never a shell string).
    {
        const QStringList launch{QStringLiteral("/opt/My Apps/Ants.AppImage"),
                                 QStringLiteral("--mcpd")};
        QStringList wantAdd{
            QStringLiteral("mcp"), QStringLiteral("add"), QStringLiteral("--scope"),
            QStringLiteral("user"), QStringLiteral("ants"), QStringLiteral("--")};
        wantAdd += launch;
        const QStringList wantRemove{
            QStringLiteral("mcp"), QStringLiteral("remove"),
            QStringLiteral("--scope"), QStringLiteral("user"),
            QStringLiteral("ants")};

        const QList<QStringList> fresh = mcpRegistrationCommands(launch, false);
        expect(fresh.size() == 1, "INV6d/fresh-is-one-command",
               QStringLiteral("no registration yet: expected exactly the `add` "
                              "command, actual %1 commands").arg(fresh.size()));
        if (!fresh.isEmpty())
            expect(stripClaude(fresh.first()) == wantAdd, "INV6e/add-argv",
                   QStringLiteral("expected argv [%1], actual [%2] (the path "
                                  "with a space must stay ONE element: an argv, "
                                  "not a shell string)")
                       .arg(wantAdd.join(QStringLiteral(", ")),
                            stripClaude(fresh.first()).join(QStringLiteral(", "))));

        const QList<QStringList> again = mcpRegistrationCommands(launch, true);
        expect(again.size() == 2, "INV6f/registered-is-remove-then-add",
               QStringLiteral("a registration exists: expected `remove` then "
                              "`add`, actual %1 commands").arg(again.size()));
        if (again.size() == 2) {
            expect(stripClaude(again[0]) == wantRemove, "INV6g/remove-argv-first",
                   QStringLiteral("expected first argv [%1], actual [%2]")
                       .arg(wantRemove.join(QStringLiteral(", ")),
                            stripClaude(again[0]).join(QStringLiteral(", "))));
            expect(stripClaude(again[1]) == wantAdd, "INV6h/add-argv-second",
                   QStringLiteral("expected second argv [%1], actual [%2]")
                       .arg(wantAdd.join(QStringLiteral(", ")),
                            stripClaude(again[1]).join(QStringLiteral(", "))));
        }
    }

    // --- mcpd::locateLaunch keeps the registration's args only when the
    //     registered command is the program it returns.
    {
        const QString exe = sb.path(QStringLiteral("reg/Ants.AppImage"));
        ASSERT_TRUE(makeExecutable(exe));
        const QString json = sb.path(QStringLiteral("claude-a.json"));
        QJsonObject ants{{"type", "stdio"},
                         {"command", exe},
                         {"args", QJsonArray{QStringLiteral("--mcpd")}}};
        QJsonObject root{{"mcpServers", QJsonObject{{"ants", ants}}}};
        ASSERT_TRUE(writeFile(json, QJsonDocument(root).toJson()));

        const mcpd::Launch l = mcpd::locateLaunch(json, appDir);
        expect(l.program == exe, "INV6i/registered-program-kept",
               QStringLiteral("an executable `$APPIMAGE --mcpd` registration: "
                              "expected program %1, actual %2").arg(exe, l.program));
        expect(l.args == QStringList{QStringLiteral("--mcpd")},
               "INV6j/registered-args-kept",
               QStringLiteral("the registration's args must ride with its "
                              "command; expected [--mcpd], actual [%1]")
                   .arg(l.args.join(QStringLiteral(", "))));
    }
    {
        const QString json = sb.path(QStringLiteral("claude-b.json"));
        QJsonObject ants{{"type", "stdio"},
                         {"command", sb.path(QStringLiteral("gone/Ants.AppImage"))},
                         {"args", QJsonArray{QStringLiteral("--mcpd")}}};
        QJsonObject root{{"mcpServers", QJsonObject{{"ants", ants}}}};
        ASSERT_TRUE(writeFile(json, QJsonDocument(root).toJson()));

        const mcpd::Launch l = mcpd::locateLaunch(json, appDir);
        expect(QFileInfo(l.program).absoluteFilePath() ==
                   QFileInfo(sibling).absoluteFilePath(),
               "INV6k/missing-registration-falls-back-to-sibling",
               QStringLiteral("a registration whose command is missing: expected "
                              "the sibling %1, actual %2").arg(sibling, l.program));
        expect(l.args.isEmpty(), "INV6l/fallback-carries-no-args",
               QStringLiteral("a fallback program must carry none of the "
                              "registration's args; expected [], actual [%1]")
                   .arg(l.args.join(QStringLiteral(", "))));
    }

    // --- the About query runs <program> <args...> --version (source check:
    //     the About dialog must ask locateLaunch, not the args-less locateBinary).
    {
        const QString about = QString::fromStdString(
            ants_test::slurpFile(std::string(SRC_ABOUTDIALOGS_PATH)));
        expect(!about.isEmpty(), "INV6m/about-source-readable",
               QStringLiteral("could not read %1")
                   .arg(QString::fromUtf8(SRC_ABOUTDIALOGS_PATH)));
        expect(about.contains(QStringLiteral("locateLaunch")),
               "INV6n/about-uses-locateLaunch",
               QStringLiteral("aboutdialogs.cpp must find the ants-mcpd through "
                              "mcpd::locateLaunch so an AppImage registration is "
                              "queried `$APPIMAGE --mcpd --version`; the text "
                              "does not mention locateLaunch"));
    }

    ASSERT_EQ(0, expect_finish());
}

// ---------------------------------------------------------------- INV-7 ----

TEST(ClaudeSetup, ClaudeMdNoteBlockIsReplacedNotAppended) {
    expect_reset();
    Sandbox sb;
    ASSERT_TRUE(sb.valid());
    const QByteArray kBegin = "<!-- ants-terminal:begin -->";
    const QByteArray kEnd = "<!-- ants-terminal:end -->";

    // outside-the-block bytes: everything before the begin marker and after
    // the end marker.
    auto outside = [&](const QByteArray &t, QByteArray *before, QByteArray *after) {
        const qsizetype b = t.indexOf(kBegin);
        const qsizetype e = b < 0 ? -1 : t.indexOf(kEnd, b);
        if (b < 0 || e < 0) return false;
        *before = t.left(b);
        *after = t.mid(e + kEnd.size());
        return true;
    };

    // --- created when absent.
    {
        const QString fresh = sb.path(QStringLiteral("fresh/CLAUDE.md"));
        QDir().mkpath(QFileInfo(fresh).absolutePath());
        const Outcome o = installClaudeMdNote(fresh);
        const QByteArray t = readFile(fresh);
        expect(o.ok && countOf(t, kBegin) == 1 && countOf(t, kEnd) == 1,
               "INV7a/created-when-absent",
               QStringLiteral("installing into an absent file must create it "
                              "holding one marked block; ok=%1, actual: %2")
                   .arg(o.ok).arg(clip(t)));
    }

    const QString path = sb.path(QStringLiteral("claude/CLAUDE.md"));
    const QByteArray top = "# My rules\nkeep this line\n";
    ASSERT_TRUE(writeFile(path, top));

    const Outcome o1 = installClaudeMdNote(path);
    const QByteArray t1 = readFile(path);
    expect(o1.ok, "INV7b/first-install-ok",
           QStringLiteral("expected ok=true, message: %1").arg(o1.message));
    expect(countOf(t1, kBegin) == 1 && countOf(t1, kEnd) == 1,
           "INV7c/one-block-after-install",
           QStringLiteral("expected one begin and one end marker; actual: %1")
               .arg(clip(t1)));
    expect(t1.startsWith(top), "INV7d/user-text-above-kept",
           QStringLiteral("bytes before the block must be unchanged; expected "
                          "prefix %1, actual %2").arg(clip(top), clip(t1)));
    const QByteArray note = claudeMdNoteText().toUtf8();
    expect(!note.isEmpty() && t1.contains(note), "INV7e/note-text-inside-block",
           QStringLiteral("the block must hold claudeMdNoteText(); the text is "
                          "%1, file: %2")
               .arg(note.isEmpty() ? QStringLiteral("empty")
                                   : QStringLiteral("absent"))
               .arg(clip(t1)));

    // The user adds text below the block.
    const QByteArray tail = "\n## Below the block\nuser tail line\n";
    ASSERT_TRUE(writeFile(path, t1 + tail));
    const QByteArray t1b = readFile(path);
    QByteArray beforeA, afterA;
    const bool haveA = outside(t1b, &beforeA, &afterA);
    expect(haveA, "INV7f/markers-locatable",
           QStringLiteral("could not locate the marker pair in %1").arg(clip(t1b)));

    const Outcome o2 = installClaudeMdNote(path);
    const QByteArray t2 = readFile(path);
    QByteArray beforeB, afterB;
    const bool haveB = outside(t2, &beforeB, &afterB);
    expect(o2.ok, "INV7g/second-install-ok",
           QStringLiteral("expected ok=true, message: %1").arg(o2.message));
    expect(countOf(t2, kBegin) == 1 && countOf(t2, kEnd) == 1,
           "INV7h/second-install-leaves-one-block",
           QStringLiteral("a second install must replace the block in place, not "
                          "append; expected 1 begin marker, actual %1 (file: %2)")
               .arg(countOf(t2, kBegin)).arg(clip(t2)));
    expect(haveA && haveB && beforeA == beforeB && afterA == afterB,
           "INV7i/bytes-outside-unchanged-by-reinstall",
           QStringLiteral("bytes outside the block must be identical after a "
                          "reinstall; before-block expected %1 actual %2; "
                          "after-block expected %3 actual %4")
               .arg(clip(beforeA), clip(beforeB), clip(afterA), clip(afterB)));
    expect(countOf(t2, "user tail line") == 1, "INV7j/tail-kept-once",
           QStringLiteral("the user's line below the block must appear exactly "
                          "once; actual %1").arg(countOf(t2, "user tail line")));

    // Remove: no marker left, and every non-blank line outside survives as is.
    const Outcome o3 = removeClaudeMdNote(path);
    const QByteArray t3 = readFile(path);
    expect(o3.ok, "INV7k/remove-ok",
           QStringLiteral("expected ok=true, message: %1").arg(o3.message));
    expect(countOf(t3, "ants-terminal:begin") == 0 &&
               countOf(t3, "ants-terminal:end") == 0,
           "INV7l/remove-leaves-no-marker",
           QStringLiteral("no marker may remain after remove; actual: %1")
               .arg(clip(t3)));
    expect(!note.isEmpty() && !t3.contains(note), "INV7m/remove-drops-note",
           QStringLiteral("the note text must be gone after remove; actual: %1")
               .arg(clip(t3)));
    auto lines = [](const QByteArray &b) {
        QList<QByteArray> out;
        for (const QByteArray &l : b.split('\n'))
            if (!l.trimmed().isEmpty()) out.append(l);
        return out;
    };
    expect(haveA && lines(t3) == lines(beforeA + afterA),
           "INV7n/remove-keeps-outside-bytes",
           QStringLiteral("after remove the file must hold exactly the lines "
                          "that were outside the block; expected %1, actual %2")
               .arg(clip(beforeA + afterA), clip(t3)));

    ASSERT_EQ(0, expect_finish());
}

// ---------------------------------------------------------------- INV-8 ----

namespace {

const QByteArray kShBegin = "# >>> ants-terminal shell integration >>>";
const QByteArray kShEnd = "# <<< ants-terminal shell integration <<<";

QByteArray keyOf(const QByteArray &profile) {
    static const QRegularExpression re(
        QStringLiteral("export ANTS_OSC133_KEY=\"([0-9A-Fa-f]{64})\""));
    const auto m = re.match(QString::fromUtf8(profile));
    return m.hasMatch() ? m.captured(1).toUtf8() : QByteArray();
}

// `[ -f X ] && source Y` inside the rc file; false when absent.
bool sourceLine(const QByteArray &rc, QString *fileArg, QString *srcArg) {
    static const QRegularExpression re(QStringLiteral(
        "\\[ -f [\"']?([^\"'\\s]+)[\"']? \\] && source [\"']?([^\"'\\s]+)[\"']?"));
    const auto m = re.match(QString::fromUtf8(rc));
    if (!m.hasMatch()) return false;
    *fileArg = m.captured(1);
    *srcArg = m.captured(2);
    return true;
}

bool outsideBlock(const QByteArray &t, QByteArray *before, QByteArray *after) {
    const qsizetype b = t.indexOf(kShBegin);
    const qsizetype e = b < 0 ? -1 : t.indexOf(kShEnd, b);
    if (b < 0 || e < 0) return false;
    *before = t.left(b);
    *after = t.mid(e + kShEnd.size());
    return true;
}

}  // namespace

TEST(ClaudeSetup, ShellIntegrationBlocksAndKey) {
    expect_reset();

    // --- bash: install, reinstall, key kept, outside bytes kept.
    {
        Sandbox sb;
        ASSERT_TRUE(sb.valid());
        const QString fx = sb.path(QStringLiteral("scripts"));
        const QString script = fx + QStringLiteral("/ants-osc133.bash");
        ASSERT_TRUE(writeFile(script, "# fixture bash hook\n"));
        const QStringList dirs{fx};
        const QString profile = sb.home + QStringLiteral("/.profile");
        const QString bashrc = sb.home + QStringLiteral("/.bashrc");
        const QByteArray profSeed = "export FOO=1\n";
        const QByteArray rcSeed = "alias a=b\n";
        ASSERT_TRUE(writeFile(profile, profSeed));
        ASSERT_TRUE(writeFile(bashrc, rcSeed));

        const Status s0 = shellIntegrationStatus(dirs);
        expect(s0.state == State::Missing, "INV8a/status-missing-before",
               QStringLiteral("nothing installed, script present: expected "
                              "Missing, actual %1 (%2)")
                   .arg(QLatin1String(stateName(s0.state)), s0.detail));

        const Outcome o1 = installShellIntegration(dirs);
        expect(o1.ok, "INV8b/first-install-ok",
               QStringLiteral("expected ok=true, message: %1").arg(o1.message));
        const QByteArray p1 = readFile(profile);
        const QByteArray r1 = readFile(bashrc);
        expect(countOf(p1, kShBegin) == 1 && countOf(p1, kShEnd) == 1,
               "INV8c/profile-one-block",
               QStringLiteral("~/.profile must hold one marked block; actual: %1")
                   .arg(clip(p1)));
        expect(countOf(r1, kShBegin) == 1 && countOf(r1, kShEnd) == 1,
               "INV8d/bashrc-one-block",
               QStringLiteral("~/.bashrc must hold one marked block; actual: %1")
                   .arg(clip(r1)));
        expect(p1.startsWith(profSeed) && r1.startsWith(rcSeed),
               "INV8e/user-text-above-kept",
               QStringLiteral("bytes before the blocks must be unchanged; "
                              "profile %1 / bashrc %2")
                   .arg(clip(p1), clip(r1)));
        const QByteArray key1 = keyOf(p1);
        expect(!key1.isEmpty(), "INV8f/profile-exports-64-hex-key",
               QStringLiteral("~/.profile must export ANTS_OSC133_KEY=\"<64 hex "
                              "digits>\"; actual: %1").arg(clip(p1)));
        QString fileArg, srcArg;
        const bool haveSrc = sourceLine(r1, &fileArg, &srcArg);
        expect(haveSrc && fileArg == script && srcArg == script,
               "INV8g/bashrc-sources-found-script",
               QStringLiteral("expected `[ -f %1 ] && source %1`; actual: %2")
                   .arg(script, clip(r1)));
        const Status s1 = shellIntegrationStatus(dirs);
        expect(s1.state == State::Installed, "INV8h/status-installed-after",
               QStringLiteral("both blocks present and the script exists: "
                              "expected Installed, actual %1 (%2)")
                   .arg(QLatin1String(stateName(s1.state)), s1.detail));

        // The user edits below each block; then reinstall.
        ASSERT_TRUE(writeFile(profile, p1 + "# user tail P\n"));
        ASSERT_TRUE(writeFile(bashrc, r1 + "# user tail R\n"));
        const QByteArray p1b = readFile(profile), r1b = readFile(bashrc);
        QByteArray pbA, paA, rbA, raA;
        const bool haveOut = outsideBlock(p1b, &pbA, &paA) &&
                             outsideBlock(r1b, &rbA, &raA);

        const Outcome o2 = installShellIntegration(dirs);
        const QByteArray p2 = readFile(profile), r2 = readFile(bashrc);
        expect(o2.ok, "INV8i/reinstall-ok",
               QStringLiteral("expected ok=true, message: %1").arg(o2.message));
        expect(countOf(p2, kShBegin) == 1 && countOf(r2, kShBegin) == 1,
               "INV8j/reinstall-leaves-one-block-per-file",
               QStringLiteral("a reinstall must replace, not append; begin "
                              "markers profile=%1 bashrc=%2 (expected 1 each)")
                   .arg(countOf(p2, kShBegin)).arg(countOf(r2, kShBegin)));
        expect(!key1.isEmpty() && keyOf(p2) == key1, "INV8k/reinstall-keeps-key",
               QStringLiteral("the key must not be regenerated; expected %1, "
                              "actual %2").arg(QString::fromUtf8(key1),
                                               QString::fromUtf8(keyOf(p2))));
        QByteArray pbB, paB, rbB, raB;
        const bool haveOut2 = outsideBlock(p2, &pbB, &paB) &&
                              outsideBlock(r2, &rbB, &raB);
        expect(haveOut && haveOut2 && pbA == pbB && paA == paB && rbA == rbB &&
                   raA == raB,
               "INV8l/bytes-outside-blocks-unchanged",
               QStringLiteral("bytes outside the blocks must survive a "
                              "reinstall; profile before/after expected %1 / %2, "
                              "actual %3 / %4")
                   .arg(clip(pbA), clip(paA), clip(pbB), clip(paB)));
        expect(countOf(p2, "# user tail P") == 1 && countOf(r2, "# user tail R") == 1,
               "INV8m/tail-kept-once",
               QStringLiteral("the user's trailing lines must appear exactly once"));
    }

    // --- an unsupported shell: Unavailable, nothing written.
    {
        Sandbox sb;
        ASSERT_TRUE(sb.valid());
        sb.env.setEnv("SHELL", QByteArrayLiteral("/usr/bin/fish"));
        const QString fx = sb.path(QStringLiteral("scripts"));
        ASSERT_TRUE(writeFile(fx + QStringLiteral("/ants-osc133.bash"), "# fixture\n"));
        const QString profile = sb.home + QStringLiteral("/.profile");
        const QString bashrc = sb.home + QStringLiteral("/.bashrc");
        ASSERT_TRUE(writeFile(profile, "keep P\n"));
        ASSERT_TRUE(writeFile(bashrc, "keep R\n"));

        const Status s = shellIntegrationStatus({fx});
        expect(s.state == State::Unavailable, "INV8n/fish-is-unavailable",
               QStringLiteral("SHELL=fish: expected Unavailable, actual %1 (%2)")
                   .arg(QLatin1String(stateName(s.state)), s.detail));
        const Outcome o = installShellIntegration({fx});
        expect(!o.ok, "INV8o/fish-install-refused",
               QStringLiteral("SHELL=fish: install must not succeed; expected "
                              "ok=false, actual ok=true (%1)").arg(o.message));
        expect(readFile(profile) == "keep P\n" && readFile(bashrc) == "keep R\n" &&
                   !QFileInfo::exists(sb.home + QStringLiteral("/.zshenv")) &&
                   !QFileInfo::exists(sb.home + QStringLiteral("/.zshrc")),
               "INV8p/fish-writes-nothing",
               QStringLiteral("an unsupported shell must write nothing; "
                              "profile=%1 bashrc=%2")
                   .arg(clip(readFile(profile)), clip(readFile(bashrc))));
    }

    // --- a missing script: Unavailable, nothing written.
    {
        Sandbox sb;
        ASSERT_TRUE(sb.valid());
        const QString emptyDir = sb.path(QStringLiteral("no-scripts"));
        QDir().mkpath(emptyDir);
        const QString profile = sb.home + QStringLiteral("/.profile");
        const QString bashrc = sb.home + QStringLiteral("/.bashrc");
        ASSERT_TRUE(writeFile(profile, "keep P\n"));
        ASSERT_TRUE(writeFile(bashrc, "keep R\n"));

        const Status s = shellIntegrationStatus({emptyDir});
        expect(s.state == State::Unavailable, "INV8q/no-script-is-unavailable",
               QStringLiteral("no ants-osc133.bash in the search list: expected "
                              "Unavailable, actual %1 (%2)")
                   .arg(QLatin1String(stateName(s.state)), s.detail));
        const Outcome o = installShellIntegration({emptyDir});
        expect(!o.ok, "INV8r/no-script-install-refused",
               QStringLiteral("missing script: install must not succeed; expected "
                              "ok=false, actual ok=true (%1)").arg(o.message));
        expect(readFile(profile) == "keep P\n" && readFile(bashrc) == "keep R\n",
               "INV8s/no-script-writes-nothing",
               QStringLiteral("a missing script must write nothing; profile=%1 "
                              "bashrc=%2")
                   .arg(clip(readFile(profile)), clip(readFile(bashrc))));
    }

    // --- under an AppImage the sourced path is the per-user copy.
    {
        Sandbox sb;
        ASSERT_TRUE(sb.valid());
        sb.env.setEnv("APPIMAGE", sb.path(QStringLiteral("mnt/Ants.AppImage")).toUtf8());
        const QString fx = sb.path(QStringLiteral("mnt/squash/share/shell-integration"));
        const QString script = fx + QStringLiteral("/ants-osc133.bash");
        const QByteArray body = "# fixture inside the mount\n";
        ASSERT_TRUE(writeFile(script, body));
        const QString bashrc = sb.home + QStringLiteral("/.bashrc");
        const QString copy = sb.home +
            QStringLiteral("/.local/share/ants-terminal/shell-integration/ants-osc133.bash");

        const Outcome o = installShellIntegration({fx});
        expect(o.ok, "INV8t/appimage-install-ok",
               QStringLiteral("expected ok=true, message: %1").arg(o.message));
        const QByteArray rc = readFile(bashrc);
        QString fileArg, srcArg;
        const bool haveSrc = sourceLine(rc, &fileArg, &srcArg);
        expect(haveSrc && fileArg == copy && srcArg == copy,
               "INV8u/appimage-sources-per-user-copy",
               QStringLiteral("with APPIMAGE set the sourced path must be the "
                              "per-user copy %1; actual: %2").arg(copy, clip(rc)));
        expect(readFile(copy) == body, "INV8v/per-user-copy-holds-the-script",
               QStringLiteral("the copy must hold the script's bytes; expected "
                              "%1, actual %2").arg(clip(body), clip(readFile(copy))));
        expect(!rc.contains(fx.toUtf8()), "INV8w/no-mount-path-in-rc",
               QStringLiteral("the AppImage mount path changes each launch and "
                              "must not be written to ~/.bashrc; actual: %1")
                   .arg(clip(rc)));
    }

    ASSERT_EQ(0, expect_finish());
}
