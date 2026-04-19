// Feature test: SSH ControlMaster auto-multiplexing on SshBookmark::toSshCommand.
// See spec.md for the contract + invariants.

#include "sshdialog.h"

#include <QDir>
#include <QString>

#include <cstdio>

static int failures = 0;

#define EXPECT(cond, msg) \
    do { \
        if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", msg); ++failures; } \
    } while (0)

int main() {
    // INV-1: default + explicit false produce zero Control* tokens.
    {
        SshBookmark bm;
        bm.host = "example.com";
        bm.user = "alice";

        QString defCmd = bm.toSshCommand();
        EXPECT(!defCmd.contains("ControlMaster"),
               "default toSshCommand() must not include ControlMaster");
        EXPECT(!defCmd.contains("ControlPath"),
               "default toSshCommand() must not include ControlPath");
        EXPECT(!defCmd.contains("ControlPersist"),
               "default toSshCommand() must not include ControlPersist");
        EXPECT(defCmd.contains("alice@example.com"),
               "default command must still include user@host");

        QString offCmd = bm.toSshCommand(false);
        EXPECT(offCmd == defCmd,
               "toSshCommand() and toSshCommand(false) must be byte-identical");
    }

    // INV-2: controlMaster=true emits all three -o options.
    {
        SshBookmark bm;
        bm.host = "example.com";
        bm.user = "alice";

        QString cmd = bm.toSshCommand(true);
        EXPECT(cmd.contains("-o ControlMaster=auto"),
               "toSshCommand(true) must include -o ControlMaster=auto");
        EXPECT(cmd.contains("ControlPath="),
               "toSshCommand(true) must include ControlPath=");
        EXPECT(cmd.contains("-o ControlPersist=10m"),
               "toSshCommand(true) must include -o ControlPersist=10m");
    }

    // INV-3: ControlPath resolves $HOME in-process — no literal ~/ prefix.
    {
        SshBookmark bm;
        bm.host = "example.com";

        QString cmd = bm.toSshCommand(true);
        const QString home = QDir::homePath();
        EXPECT(cmd.contains(home + "/.ssh/cm-"),
               "ControlPath must embed the resolved $HOME");
        EXPECT(!cmd.contains("ControlPath=~"),
               "ControlPath must not rely on shell tilde expansion");
    }

    // INV-4: %r@%h:%p tokens survive shell quoting intact.
    {
        SshBookmark bm;
        bm.host = "example.com";

        QString cmd = bm.toSshCommand(true);
        EXPECT(cmd.contains("cm-%r@%h:%p"),
               "ControlPath must carry the %r@%h:%p tokens unmolested");
    }

    // INV-5: ControlMaster options precede the [user@]host destination.
    {
        SshBookmark bm;
        bm.host = "example.com";
        bm.user = "alice";

        QString cmd = bm.toSshCommand(true);
        int cmIdx = cmd.indexOf("ControlMaster=auto");
        int userHostIdx = cmd.indexOf("alice@example.com");
        EXPECT(cmIdx >= 0, "ControlMaster token present");
        EXPECT(userHostIdx >= 0, "user@host token present");
        EXPECT(cmIdx >= 0 && userHostIdx >= 0 && cmIdx < userHostIdx,
               "ControlMaster options must come before the destination arg");
    }

    // INV-6: coexistence with port / identity / extraArgs.
    {
        SshBookmark bm;
        bm.host = "example.com";
        bm.user = "alice";
        bm.port = 2222;
        bm.identityFile = "/home/alice/.ssh/id_ed25519";
        bm.extraArgs = "-A -C";

        QString cmd = bm.toSshCommand(true);
        EXPECT(cmd.contains("-p 2222"),
               "custom port must be preserved alongside ControlMaster");
        EXPECT(cmd.contains("-i /home/alice/.ssh/id_ed25519"),
               "identity file must be preserved alongside ControlMaster");
        EXPECT(cmd.contains(" -A"),
               "extraArg -A must be preserved alongside ControlMaster");
        EXPECT(cmd.contains(" -C"),
               "extraArg -C must be preserved alongside ControlMaster");
        EXPECT(cmd.contains("ControlMaster=auto"),
               "ControlMaster still present when legacy flags are set");
    }

    if (failures) {
        std::fprintf(stderr, "ssh_control_master: %d assertion(s) failed\n", failures);
        return 1;
    }
    std::fprintf(stdout, "ssh_control_master: all invariants passed\n");
    return 0;
}
