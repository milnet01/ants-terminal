// Feature-conformance test for spec.md — asserts SshBookmark::toSshCommand
// always emits a bare `--` separator between options and host, so a
// host value starting with `-` cannot be reparsed by OpenSSH as an
// option (CVE-2017-1000117 class).
//
// Exit 0 = all four scenarios hold. Non-zero = regression.

#include "sshdialog.h"

#include <QString>
#include <QStringList>
#include <cstdio>

namespace {

int fail(const char *scenario, const char *detail, const QString &cmd) {
    std::fprintf(stderr, "FAIL [%s]: %s\n  cmd: %s\n",
                 scenario, detail, qUtf8Printable(cmd));
    return 1;
}

// Returns true if a `--` token appears at an argv position strictly
// before the host substring. Tokens are split on whitespace; we don't
// need a full shell parser because the builder emits one space per
// argv boundary and never embeds spaces inside unquoted tokens.
bool dashDashBeforeHost(const QString &cmd, const QString &host) {
    QStringList tokens = cmd.split(' ', Qt::SkipEmptyParts);
    int dashIdx = tokens.indexOf("--");
    if (dashIdx < 0) return false;
    // Locate the first token whose quoted form matches the host. The host
    // may be single-quoted by shellQuote; we want to treat `'foo'` as the
    // same token as `foo` for this ordering check.
    for (int i = dashIdx + 1; i < tokens.size(); ++i) {
        QString t = tokens[i];
        if (t.startsWith('\'') && t.endsWith('\''))
            t = t.mid(1, t.size() - 2);
        if (t.contains(host)) return true;
    }
    return false;
}

bool check(const char *scenarioName, const SshBookmark &bm) {
    const QString cmd = bm.toSshCommand(false);
    if (!cmd.contains(" -- ")) {
        fail(scenarioName,
             "generated command is missing the ` -- ` argv terminator", cmd);
        return false;
    }
    if (!dashDashBeforeHost(cmd, bm.host)) {
        fail(scenarioName,
             "`--` must appear before the host token", cmd);
        return false;
    }
    // No token starting with `-oProxyCommand=` may appear before `--`.
    QStringList tokens = cmd.split(' ', Qt::SkipEmptyParts);
    int dashIdx = tokens.indexOf("--");
    for (int i = 0; i < dashIdx; ++i) {
        if (tokens[i].startsWith("-oProxyCommand=")) {
            fail(scenarioName,
                 "a `-oProxyCommand=` token appeared BEFORE `--` — this "
                 "would let OpenSSH interpret an attacker-controlled host "
                 "as an option", cmd);
            return false;
        }
    }
    return true;
}

}  // namespace

int main() {
    // Scenario 1 — plain host
    {
        SshBookmark bm;
        bm.host = "example.com";
        if (!check("plain host", bm)) return 1;
    }
    // Scenario 2 — user@host
    {
        SshBookmark bm;
        bm.host = "example.com";
        bm.user = "alice";
        if (!check("user@host", bm)) return 1;
    }
    // Scenario 3 — host + identity file + non-default port
    {
        SshBookmark bm;
        bm.host = "jump.example.com";
        bm.identityFile = "/home/alice/.ssh/id_ed25519";
        bm.port = 2222;
        if (!check("host + identity + port", bm)) return 1;
    }
    // Scenario 4 — host + extraArgs
    {
        SshBookmark bm;
        bm.host = "example.com";
        bm.extraArgs = "-C -o StrictHostKeyChecking=accept-new";
        if (!check("host + extraArgs", bm)) return 1;
    }
    // Scenario 5 — the CVE-class malicious host value. The builder must
    // still emit `--` and the dash-prefixed host must land AFTER the
    // separator so OpenSSH binds it to the host role, not ProxyCommand.
    {
        SshBookmark bm;
        bm.host = "-oProxyCommand=evil";
        const QString cmd = bm.toSshCommand(false);
        if (!cmd.contains(" -- ")) {
            fail("malicious host",
                 "generated command is missing the ` -- ` argv terminator", cmd);
            return 1;
        }
        QStringList tokens = cmd.split(' ', Qt::SkipEmptyParts);
        int dashIdx = tokens.indexOf("--");
        // Every -oProxyCommand occurrence must come AFTER the --.
        for (int i = 0; i < dashIdx; ++i) {
            if (tokens[i].contains("-oProxyCommand=")) {
                fail("malicious host",
                     "attacker-controlled `-oProxyCommand=` token "
                     "appeared before `--`", cmd);
                return 1;
            }
        }
    }

    std::printf("ssh_dash_host_rejected: 5 scenarios hold the `--` invariant\n");
    return 0;
}
