// Feature-conformance test for spec.md (canonical:
// docs/specs/ANTS-1337.md). Phase 1: infrastructure + engine API.
//
// VT-* tests exercise VerifyEngine::loadGateConfig with the three
// trust-client states (nullptr / AlwaysTrust / AlwaysDeny /
// FilePersistedTrustClient with explicit pre-loaded trust).
// TF-* tests pin the trust-file format + mode 0600.
//
// Modal + MCP envelope tests live in Phase 2 (test_chrome bundle).

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"
#include "verifyengine.h"
#include "verifytrust.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QString>
#include <QTemporaryDir>

#include <sys/stat.h>

#include <string>

#include <gtest/gtest.h>

ANTS_TEST_SCOPE();

namespace {

// Build a minimal valid .ants/verify.json inside a freshly-created
// project root. Returns the canonical project path. The fixture
// also writes a CMakeLists.txt so the auto-detect fallback has
// something to return (Build gate via `cmake --build build`).
QString makeProject(QTemporaryDir &tmp, const QByteArray &verifyJson) {
    const QString root = tmp.path();
    QDir().mkpath(root + QStringLiteral("/.ants"));
    QFile cfg(root + QStringLiteral("/.ants/verify.json"));
    if (!cfg.open(QIODevice::WriteOnly | QIODevice::Truncate)) return root;
    if (cfg.write(verifyJson) < 0) return root;
    cfg.close();

    // Auto-detect fallback needs SOMETHING to detect. CMakeLists.txt
    // is the simplest trigger — engine emits a "cmake --build build"
    // Build gate when it sees one.
    QFile cmake(root + QStringLiteral("/CMakeLists.txt"));
    if (!cmake.open(QIODevice::WriteOnly | QIODevice::Truncate)) return root;
    if (cmake.write("project(stub)\n") < 0) return root;
    cmake.close();

    return root;
}

// Schema uses top-level "build" / "tests" / "lint" keys per
// docs/specs/ANTS-1289.md § 2.4; each maps to a {command, format}
// object.
const QByteArray kSampleConfig =
    "{\n"
    "  \"build\": {\"command\": \"echo bespoke-build\"}\n"
    "}\n";

// ---- VT-* engine tests --------------------------------------------

void testEngine() {
    // VT-1 trusted SHA → bespoke gates honoured; verifyUntrusted=false.
    {
        QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
        const QString root = makeProject(tmp, kSampleConfig);

        VerifyTrust::AlwaysTrustClient client;
        QString source;
        bool untrusted = false;
        const auto gates = VerifyEngine::loadGateConfig(
            root, &source, &client, &untrusted);

        expect(gates.size() == 1, "VT-1 one bespoke gate returned");
        expect(source == QStringLiteral(".ants/verify.json"),
               (std::string("VT-1 configSource bespoke, got ")
                + source.toStdString()).c_str());
        expect(!untrusted, "VT-1 verifyUntrusted=false");
        if (!gates.isEmpty()) {
            expect(gates.first().command.contains(
                       QStringLiteral("bespoke-build")),
                   "VT-1 command is the bespoke one");
        }
    }

    // VT-2 untrusted SHA → auto-detect fallback;
    //      verifyUntrusted=true; configSource=untrusted-bespoke.
    {
        QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
        const QString root = makeProject(tmp, kSampleConfig);

        VerifyTrust::AlwaysDenyClient client;
        QString source;
        bool untrusted = false;
        const auto gates = VerifyEngine::loadGateConfig(
            root, &source, &client, &untrusted);

        expect(untrusted, "VT-2 verifyUntrusted=true");
        expect(source == QStringLiteral("auto (untrusted-bespoke)"),
               (std::string("VT-2 configSource flagged, got ")
                + source.toStdString()).c_str());
        // Bespoke command must NOT appear in the returned gates.
        bool bespokeLeaked = false;
        for (const auto &g : gates) {
            if (g.command.contains(QStringLiteral("bespoke-build"))) {
                bespokeLeaked = true; break;
            }
        }
        expect(!bespokeLeaked, "VT-2 bespoke command did not leak");
    }

    // VT-6 nullptr trust client → bespoke honoured (Phase-1 back-compat).
    {
        QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
        const QString root = makeProject(tmp, kSampleConfig);

        QString source;
        bool untrusted = false;
        const auto gates = VerifyEngine::loadGateConfig(
            root, &source, /*trustClient*/ nullptr, &untrusted);

        expect(gates.size() == 1, "VT-6 one bespoke gate returned");
        expect(source == QStringLiteral(".ants/verify.json"),
               "VT-6 configSource bespoke (back-compat)");
        expect(!untrusted, "VT-6 verifyUntrusted=false (back-compat)");
    }

    // VT-3 trusted repo with SHA-pin matching → bespoke honoured.
    // Uses FilePersistedTrustClient with explicit-path ctor to avoid
    // touching the real ~/.config dir.
    {
        QTemporaryDir trustHome; ASSERT_TRUE(trustHome.isValid());
        QTemporaryDir projTmp; ASSERT_TRUE(projTmp.isValid());
        const QString root = makeProject(projTmp, kSampleConfig);
        const QString rootCanon = QFileInfo(root).canonicalFilePath();

        const QString trustPath =
            trustHome.path() + QStringLiteral("/verify-trust.json");
        VerifyTrust::FilePersistedTrustClient client(trustPath);
        // Pre-trust the repo with the current SHA.
        const QByteArray cfgBytes = kSampleConfig;
        // Compute SHA via the same helper the engine will use.
        // We trust the SHA directly here (simpler than computing in
        // the test).
        QFile cfg(root + QStringLiteral("/.ants/verify.json"));
        ASSERT_TRUE(cfg.open(QIODevice::ReadOnly));
        const QByteArray actualBytes = cfg.readAll();
        cfg.close();
        const QByteArray shaBin =
            QCryptographicHash::hash(actualBytes,
                                     QCryptographicHash::Sha256);
        const QString shaHex = QString::fromLatin1(shaBin.toHex());
        ASSERT_TRUE(client.addTrustedRepo(rootCanon, shaHex,
                                          /*untilShaChanges*/ true));

        QString source; bool untrusted = false;
        const auto gates = VerifyEngine::loadGateConfig(
            root, &source, &client, &untrusted);
        expect(gates.size() == 1, "VT-3 repo-trust honours bespoke");
        expect(!untrusted, "VT-3 verifyUntrusted=false");
        (void)cfgBytes;
    }
}

// ---- TF-* trust-file format tests ---------------------------------

void testTrustFile() {
    // TF-1 trust file written with mode 0600.
    {
        QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
        const QString path =
            tmp.path() + QStringLiteral("/verify-trust.json");
        VerifyTrust::FilePersistedTrustClient client(path);
        ASSERT_TRUE(client.addTrustedSha(
            QStringLiteral("0000000000000000000000000000000000000000"
                           "000000000000000000000000")));

        struct stat st{};
        ASSERT_EQ(0, ::lstat(path.toLocal8Bit().constData(), &st));
        const int mode = st.st_mode & 0777;
        expect(mode == 0600,
               (std::string("TF-1 mode 0600 expected, got 0")
                + std::to_string(mode)).c_str());
    }

    // TF-2 atomic — the .tmp sibling doesn't linger after success.
    // (We can't easily SIGKILL mid-write in a unit test; the
    // post-rename check is the best proxy.)
    {
        QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
        const QString path =
            tmp.path() + QStringLiteral("/verify-trust.json");
        VerifyTrust::FilePersistedTrustClient client(path);
        ASSERT_TRUE(client.addTrustedSha(
            QStringLiteral("abcd1234567890123456789012345678901234567"
                           "89012345678901234567890")));  // 64 hex (ANTS-1614)

        expect(QFile::exists(path), "TF-2 final file present");
        expect(!QFile::exists(path + QStringLiteral(".tmp")),
               "TF-2 .tmp sibling cleaned");
    }

    // TF-3 corrupt JSON → empty trust set; subsequent write replaces.
    {
        QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
        const QString path =
            tmp.path() + QStringLiteral("/verify-trust.json");
        // Pre-place garbage.
        QFile bad(path);
        ASSERT_TRUE(bad.open(QIODevice::WriteOnly));
        ASSERT_GT(bad.write("not valid json {{{"), 0);
        bad.close();

        VerifyTrust::FilePersistedTrustClient client(path);
        // Internal state is empty — outcomeForConfig won't find any
        // SHA. We exercise this via addTrustedSha: the next save
        // overwrites the corrupt file.
        ASSERT_TRUE(client.addTrustedSha(
            QStringLiteral("deadbeefcafef00d12345678901234567890123456"
                           "7890123456789012345678")));  // 64 hex (ANTS-1614)

        // Read back, confirm it parses now.
        QFile good(path);
        ASSERT_TRUE(good.open(QIODevice::ReadOnly));
        const QByteArray bytes = good.readAll();
        good.close();
        QJsonParseError perr{};
        QJsonDocument::fromJson(bytes, &perr);
        expect(perr.error == QJsonParseError::NoError,
               "TF-3 corrupt file replaced by valid JSON");
    }

    // TF-4 (ANTS-1825) — a trust file stamped with a FUTURE schema
    // version is refused: the v1 reader honours none of its entries
    // (fail-closed) and a subsequent write no-ops, leaving the newer
    // file byte-identical (no downgrade-clobber).
    {
        QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
        const QString path =
            tmp.path() + QStringLiteral("/verify-trust.json");

        // A real SHA so we can prove the future file's trust entry is
        // NOT honoured via outcomeForConfig.
        const QByteArray cfgBytes =
            "{\"build\":{\"command\":\"echo hi\"}}";
        const QString shaHex = QString::fromLatin1(
            QCryptographicHash::hash(cfgBytes,
                                     QCryptographicHash::Sha256).toHex());

        const QByteArray futureFile =
            QByteArray("{\n  \"version\": 999,\n  "
                       "\"trusted_shas\": {\n    \"")
            + shaHex.toLatin1()
            + "\": {\"first_trusted\": \"x\"}\n  }\n}\n";
        {
            QFile f(path);
            ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            ASSERT_GT(f.write(futureFile), 0);
            f.close();
        }

        VerifyTrust::FilePersistedTrustClient client(path);

        // Fail-closed: the future file's trusted SHA must NOT be
        // honoured (base client has no modal → Headless, never
        // Trusted). Without the gate, the v999 entry would load and
        // this would return Trusted.
        const auto dec =
            client.outcomeForConfig(QStringLiteral("/some/proj"), cfgBytes);
        expect(dec.outcome != VerifyTrust::Outcome::Trusted,
               "TF-4 future-schema trust entry not honoured "
               "(fail-closed)");

        // Refuse-to-clobber: a write attempt no-ops (returns false)
        // and leaves the newer file untouched.
        const bool saved =
            client.addTrustedSha(QString(64, QLatin1Char('b')));
        expect(!saved,
               "TF-4 addTrustedSha no-ops under a future-schema file");

        QFile after(path);
        ASSERT_TRUE(after.open(QIODevice::ReadOnly));
        const QByteArray onDiskAfter = after.readAll();
        after.close();
        expect(onDiskAfter == futureFile,
               "TF-4 future-schema file left byte-identical "
               "(no downgrade-clobber)");
    }
}

// ---- MCP envelope wiring (Phase 2) --------------------------------

void testMcpWiring() {
#ifdef ANTS_RC_SOURCES
    const std::string rc = ants_test::slurpRemoteControl();
    // ANTS-1359 refactored cmdVerifyChanges into a thin RcGate wrapper
    // + a cmdVerifyChangesImpl that holds the trust-client wiring and
    // the new build-cache. Slurp both bodies and search the combined
    // text so the trust-gate invariants (MC-2..MC-4) keep firing
    // against whichever function carries them.
    const std::string wrapper =
        ants_test::slurpFunctionBody(rc, "RemoteControl::cmdVerifyChanges");
    const std::string impl =
        ants_test::slurpFunctionBody(rc, "RemoteControl::cmdVerifyChangesImpl");
    const std::string body = wrapper + "\n" + impl;
    expect(!wrapper.empty(), "MC-1 cmdVerifyChanges body slurped");
    if (!body.empty()) {
        // MC-2: response envelope carries the new field.
        expect(body.find("\"verify_untrusted\"") != std::string::npos,
               "MC-2 cmdVerifyChanges emits verify_untrusted field");
        // MC-3: trust client wired into VerifyOptions.
        expect(body.find("opts.trustClient") != std::string::npos,
               "MC-3 cmdVerifyChanges sets opts.trustClient");
        // MC-4: ANTS_VERIFY_TRUST_AUTOTRUST env-var bypass present
        // (transition window).
        expect(body.find("ANTS_VERIFY_TRUST_AUTOTRUST") != std::string::npos,
               "MC-4 ANTS_VERIFY_TRUST_AUTOTRUST env-var bypass present");
    }
#endif
}

int runMain() {
    expect_reset();
    testEngine();
    testTrustFile();
    testMcpWiring();
    return expect_finish();
}

}  // namespace

TEST(VerifyTrustGate, Main) { ASSERT_EQ(0, runMain()); }
