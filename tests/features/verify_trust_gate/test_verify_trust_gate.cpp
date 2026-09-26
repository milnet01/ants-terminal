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
#include <QJsonObject>
#include <QJsonParseError>
#include <QString>
#include <QTemporaryDir>

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
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

    // TF-5 (ANTS-5082) — saveToDisk checks the write, flush, close and
    // permissions before renaming. Source-scrape: a full disk cannot be
    // simulated here.
    {
        const QString srcPath =
            QFileInfo(QString::fromUtf8(__FILE__)).absolutePath()
            + QStringLiteral("/../../../src/verifytrust.cpp");
        QFile sf(srcPath);
        ASSERT_TRUE(sf.open(QIODevice::ReadOnly | QIODevice::Text))
            << "cannot read " << srcPath.toStdString();
        const QString code = QString::fromUtf8(sf.readAll());
        const int s = code.indexOf(QStringLiteral("::saveToDisk("));
        ASSERT_GE(s, 0);
        const QString body = code.mid(s, 3000);
        expect(body.contains(QStringLiteral("f.write(bytes) == bytes.size()"))
                   && body.contains(QStringLiteral("f.flush()"))
                   && body.contains(QStringLiteral("!setOwnerOnlyPerms(tmp)")),
               "TF-5 save checks write, flush and permissions before rename");
    }

    // TF-6 (ANTS-5082) — a corrupt trust file is moved aside, not destroyed.
    {
        QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
        const QString path =
            tmp.path() + QStringLiteral("/verify-trust.json");
        QFile bad(path);
        ASSERT_TRUE(bad.open(QIODevice::WriteOnly));
        ASSERT_GT(bad.write("corrupt trust {{{"), 0);
        bad.close();

        VerifyTrust::FilePersistedTrustClient client(path);
        const QStringList aside = QDir(tmp.path()).entryList(
            { QStringLiteral("verify-trust.json.corrupt.*") }, QDir::Files);
        expect(aside.size() == 1, "TF-6 corrupt file moved aside");
        if (aside.size() == 1) {
            QFile kept(QDir(tmp.path()).filePath(aside.first()));
            ASSERT_TRUE(kept.open(QIODevice::ReadOnly));
            expect(kept.readAll() == QByteArray("corrupt trust {{{"),
                   "TF-6 moved-aside copy keeps the original bytes");
        }
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

        // TF-9 — a trust whose save failed is not honoured later in the
        // session. The in-memory map is what outcomeForConfig reads, so
        // an entry left behind by a failed save would read as Trusted.
        expect(!client.addTrustedSha(shaHex),
               "TF-9 addTrustedSha reports the failed save");
        expect(client.outcomeForConfig(QStringLiteral("/some/proj"), cfgBytes)
                   .outcome != VerifyTrust::Outcome::Trusted,
               "TF-9 a SHA whose save failed is not trusted afterwards");
        expect(!client.addTrustedRepo(QStringLiteral("/some/proj"), shaHex,
                                      /*untilShaChanges=*/false),
               "TF-9 addTrustedRepo reports the failed save");
        expect(client.outcomeForConfig(QStringLiteral("/some/proj"), cfgBytes)
                   .outcome != VerifyTrust::Outcome::Trusted,
               "TF-9 a repo whose save failed is not trusted afterwards");
    }

    // TF-7 (ANTS-5082) — first_trusted is the date an entry was first
    // trusted: a later save keeps it, and only a new entry gets a new date.
    {
        QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
        const QString path =
            tmp.path() + QStringLiteral("/verify-trust.json");
        const QString oldSha(64, QLatin1Char('a'));
        const QByteArray seeded =
            QByteArray("{\n  \"version\": 1,\n  \"trusted_shas\": {\"")
            + oldSha.toLatin1()
            + "\": {\"first_trusted\": \"2020-01-02T03:04:05Z\"}},\n"
              "  \"trusted_repos\": {\"/old/repo\": {\"first_trusted\": "
              "\"2021-01-02T03:04:05Z\", \"sha\": \""
            + oldSha.toLatin1() + "\", \"until_sha_changes\": true}}\n}\n";
        {
            QFile f(path);
            ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            ASSERT_GT(f.write(seeded), 0);
        }

        VerifyTrust::FilePersistedTrustClient client(path);
        const QString newSha(64, QLatin1Char('c'));
        ASSERT_TRUE(client.addTrustedSha(newSha));
        ASSERT_TRUE(client.addTrustedSha(oldSha, QStringLiteral("again")));

        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        const QJsonObject shas =
            root.value(QStringLiteral("trusted_shas")).toObject();
        const QString firstTrusted = QStringLiteral("first_trusted");
        expect(shas.value(oldSha).toObject().value(firstTrusted).toString()
                   == QStringLiteral("2020-01-02T03:04:05Z"),
               "TF-7 an existing SHA keeps its first_trusted date");
        expect(root.value(QStringLiteral("trusted_repos")).toObject()
                       .value(QStringLiteral("/old/repo")).toObject()
                       .value(firstTrusted).toString()
                   == QStringLiteral("2021-01-02T03:04:05Z"),
               "TF-7 an existing repo keeps its first_trusted date");
        expect(!shas.value(newSha).toObject().value(firstTrusted)
                    .toString().isEmpty(),
               "TF-7 a new SHA gets a first_trusted date");
    }

    // TF-8 (ANTS-5082) — ANTS-1337 § 6: a trust file other users can read
    // is warned about on load, and its entries are still honoured.
    {
        QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
        const QString path =
            tmp.path() + QStringLiteral("/verify-trust.json");
        const QByteArray cfgBytes = "{\"build\":{\"command\":\"echo tf8\"}}";
        const QString shaHex = QString::fromLatin1(
            QCryptographicHash::hash(cfgBytes,
                                     QCryptographicHash::Sha256).toHex());
        {
            QFile f(path);
            ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            ASSERT_GT(f.write(QByteArray("{\"version\":1,\"trusted_shas\":{\"")
                              + shaHex.toLatin1() + "\":{}}}"), 0);
        }
        ASSERT_EQ(0, ::chmod(path.toLocal8Bit().constData(), 0644));

        QFile cap(tmp.path() + QStringLiteral("/stderr.txt"));
        ASSERT_TRUE(cap.open(QIODevice::WriteOnly));
        std::fflush(stderr);
        const int savedErr = ::dup(2);
        ASSERT_GE(savedErr, 0);
        // No ASSERT until stderr is restored.
        ::dup2(cap.handle(), 2);
        bool trusted = false;
        {
            VerifyTrust::FilePersistedTrustClient client(path);
            trusted = client.outcomeForConfig(QStringLiteral("/tf8/proj"),
                                              cfgBytes).outcome
                      == VerifyTrust::Outcome::Trusted;
        }
        std::fflush(stderr);
        ::dup2(savedErr, 2);
        ::close(savedErr);
        cap.close();

        QFile logged(cap.fileName());
        ASSERT_TRUE(logged.open(QIODevice::ReadOnly));
        const QByteArray text = logged.readAll();
        expect(text.contains(path.toLocal8Bit()) && text.contains("other users"),
               "TF-8 a trust file other users can read is warned about");
        expect(trusted, "TF-8 a trust file other users can read is still honoured");
    }
}

// ---- MD-* trust prompt (ANTS-1337 § 4.3) ---------------------------
// Source scrapes: the prompt is a modal QMessageBox, which a unit test
// cannot drive.

void testModalPrompt() {
    const QString srcPath =
        QFileInfo(QString::fromUtf8(__FILE__)).absolutePath()
        + QStringLiteral("/../../../src/verifytrustmodal.cpp");
    QFile sf(srcPath);
    ASSERT_TRUE(sf.open(QIODevice::ReadOnly | QIODevice::Text))
        << "cannot read " << srcPath.toStdString();
    const QString code = QString::fromUtf8(sf.readAll());
    const int s = code.indexOf(QStringLiteral("Decision ModalClient::showPrompt("));
    ASSERT_GE(s, 0);
    const QString body = code.mid(s);

    // MD-1 — the prompt names the gates the config would run.
    expect(body.contains(QStringLiteral("<b>Gates:</b>")),
           "MD-1 the trust prompt shows a Gates line");
    // MD-2 — "Trust this repo" carries a re-prompt checkbox, on by default.
    expect(body.contains(QStringLiteral("setCheckBox("))
               && body.contains(QStringLiteral("setChecked(true)")),
           "MD-2 the trust prompt has a re-prompt checkbox, on by default");
    expect(body.contains(QStringLiteral("reprompt->isChecked()")),
           "MD-2 Trust this repo passes the checkbox's state");
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
    // CC-T2 (audit 2026-09-26) — `body` always holds the joining newline, so
    // testing it could never fail; each half is checked on its own.
    expect(!impl.empty(), "MC-1 cmdVerifyChangesImpl body slurped");
    if (!wrapper.empty() && !impl.empty()) {
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
    testModalPrompt();
    testMcpWiring();
    return expect_finish();
}

}  // namespace

TEST(VerifyTrustGate, Main) { ASSERT_EQ(0, runMain()); }
