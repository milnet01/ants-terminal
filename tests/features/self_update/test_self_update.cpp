// Feature-conformance tests for docs/specs/ANTS-5560-appimage-self-update.md
// (§ 3 INV-1..INV-9; the spec is the contract, § 6 names each test).
//
// INV-1 ManifestVerification          INV-6 RelaunchCommand
// INV-2 DownloadMismatchLeavesOriginal INV-7 SkipAndStartupSetting
// INV-3 SwapInPlace                   INV-8 PipelineSignatureRoundTrip
// INV-4 InstallKind                   INV-9 ReleaseWorkflowSigns
// INV-5 HttpsOnly
//
// Why this exists: an updater that installs an unsigned, downgraded, torn or
// mis-sourced AppImage, or relaunches into the dead mount of the old
// process, replaces the user's terminal with a broken or hostile one.
//
// Key pairs and signatures are made in the test with the `openssl` CLI
// (libsodium is not linked into the tests); an arm skips with a message
// when openssl is absent or lacks -rawin. Nothing here reads HOME: every
// file lives under a QTemporaryDir.
//
// A build without libsodium (a distro package) compiles the verifier out:
// nothing verifies and installKind() reports NotAppImage. The tests that
// need a verified manifest skip there, and InstallKind asserts that answer.

#include "selfupdate.h"

#include "../../_support/expect.h"

#include <gtest/gtest.h>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>

ANTS_TEST_SCOPE();

#ifdef ANTS_HAVE_LIBSODIUM
#define REQUIRE_VERIFIER() (void)0
#else
#define REQUIRE_VERIFIER() GTEST_SKIP() << "built without libsodium: nothing can verify a manifest"
#endif

namespace {

using SelfUpdate::VerifyError;

QByteArray readAll(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

bool writeAll(const QString &path, const QByteArray &b) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(b) == b.size();
}

QString show(const QByteArray &b) {
    return QString::fromUtf8(b.left(200)).replace(QLatin1Char('\n'), QStringLiteral("\\n"));
}

QString errName(VerifyError e) {
    switch (e) {
    case VerifyError::None: return QStringLiteral("None");
    case VerifyError::BadKey: return QStringLiteral("BadKey");
    case VerifyError::BadSignature: return QStringLiteral("BadSignature");
    case VerifyError::BadManifest: return QStringLiteral("BadManifest");
    case VerifyError::VersionNotTag: return QStringLiteral("VersionNotTag");
    case VerifyError::VersionNotNewer: return QStringLiteral("VersionNotNewer");
    case VerifyError::AssetName: return QStringLiteral("AssetName");
    case VerifyError::AssetMissing: return QStringLiteral("AssetMissing");
    }
    return QStringLiteral("?");
}

unsigned modeOf(const QString &path) {
    struct stat st{};
    if (::stat(path.toLocal8Bit().constData(), &st) != 0) return 0xFFFFFFFFu;
    return static_cast<unsigned>(st.st_mode & 07777);
}

QStringList listDir(const QString &dir) {
    return QDir(dir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                               QDir::Name);
}

// Restores a directory's mode on scope exit so QTemporaryDir can delete it.
struct ModeGuard {
    QString path;
    ~ModeGuard() {
        if (!path.isEmpty()) ::chmod(path.toLocal8Bit().constData(), 0755);
    }
};

// ---- openssl key pair + signing ---------------------------------------

struct ProcResult {
    int exitCode = -1;
    QByteArray out, err;
};

ProcResult runProcess(const QString &program, const QStringList &args,
               const QProcessEnvironment *env = nullptr) {
    QProcess p;
    if (env) p.setProcessEnvironment(*env);
    p.start(program, args);
    ProcResult r;
    if (!p.waitForStarted(5000)) return r;
    p.closeWriteChannel();
    p.waitForFinished(30000);
    r.exitCode = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
    r.out = p.readAllStandardOutput();
    r.err = p.readAllStandardError();
    return r;
}

struct KeyPair {
    bool ok = false;
    QString why;
    QString privPath, pubPath;
    QByteArray privPem, pubPem;
};

KeyPair makeKeyPair(const QString &dir, const QString &tag) {
    KeyPair k;
    k.privPath = dir + QLatin1Char('/') + tag + QStringLiteral(".priv.pem");
    k.pubPath = dir + QLatin1Char('/') + tag + QStringLiteral(".pub.pem");
    ProcResult g = runProcess(QStringLiteral("openssl"),
                       {QStringLiteral("genpkey"), QStringLiteral("-algorithm"),
                        QStringLiteral("ed25519"), QStringLiteral("-out"), k.privPath});
    if (g.exitCode != 0) { k.why = QStringLiteral("openssl genpkey ed25519 failed: ") + show(g.err); return k; }
    ProcResult p = runProcess(QStringLiteral("openssl"),
                       {QStringLiteral("pkey"), QStringLiteral("-in"), k.privPath,
                        QStringLiteral("-pubout"), QStringLiteral("-out"), k.pubPath});
    if (p.exitCode != 0) { k.why = QStringLiteral("openssl pkey -pubout failed: ") + show(p.err); return k; }
    k.privPem = readAll(k.privPath);
    k.pubPem = readAll(k.pubPath);
    k.ok = !k.privPem.isEmpty() && !k.pubPem.isEmpty();
    if (!k.ok) k.why = QStringLiteral("key files empty");
    return k;
}

// Raw 64-byte Ed25519 signature; empty on failure (no -rawin).
QByteArray signBytes(const KeyPair &k, const QString &dir, const QByteArray &data, QString *why) {
    const QString in = dir + QStringLiteral("/sign.in");
    const QString out = dir + QStringLiteral("/sign.out");
    QFile::remove(out);
    writeAll(in, data);
    ProcResult r = runProcess(QStringLiteral("openssl"),
                       {QStringLiteral("pkeyutl"), QStringLiteral("-sign"), QStringLiteral("-rawin"),
                        QStringLiteral("-inkey"), k.privPath, QStringLiteral("-in"), in,
                        QStringLiteral("-out"), out});
    if (r.exitCode != 0) {
        if (why) *why = QStringLiteral("openssl pkeyutl -sign -rawin failed: ") + show(r.err);
        return {};
    }
    return readAll(out);
}

QByteArray manifestJson(const QString &version, const QString &asset, qint64 size,
                        const QByteArray &sha256Hex) {
    return QStringLiteral("{\"format\":1,\"version\":\"%1\",\"asset\":\"%2\",\"size\":%3,\"sha256\":\"%4\"}")
        .arg(version, asset)
        .arg(size)
        .arg(QString::fromLatin1(sha256Hex))
        .toUtf8();
}

QString versionedAsset(const QString &v) {
    return QStringLiteral("Ants_Terminal-%1-x86_64.AppImage").arg(v);
}

QByteArray sha256Hex(const QByteArray &b) {
    return QCryptographicHash::hash(b, QCryptographicHash::Sha256).toHex();
}

QByteArray payload(int seed, int n) {
    QByteArray b;
    b.reserve(n);
    for (int i = 0; i < n; ++i) b.append(static_cast<char>((i * 31 + seed) & 0xFF));
    return b;
}

// Skips the calling TEST when the openssl CLI cannot make/sign Ed25519.
#define REQUIRE_OPENSSL(kp, dir, sigOut)                                            \
    KeyPair kp = makeKeyPair((dir), QStringLiteral(#kp));                           \
    if (!kp.ok) { GTEST_SKIP() << "openssl unavailable: " << kp.why.toStdString(); } \
    QString sigWhy_##kp;                                                            \
    QByteArray sigOut = signBytes(kp, (dir), QByteArrayLiteral("probe"), &sigWhy_##kp); \
    (void)sigOut;                                                                   \
    if (sigOut.size() != 64) { GTEST_SKIP() << "openssl lacks -rawin: " << sigWhy_##kp.toStdString(); }

// ---- INV-1 -------------------------------------------------------------

void checkManifestVerification(const QString &dir, const KeyPair &kp, const KeyPair &other) {
    const QString tag = QStringLiteral("v0.8.0");
    const QString running = QStringLiteral("0.7.5");
    const QString asset = versionedAsset(QStringLiteral("0.8.0"));
    const QStringList assets = {asset, asset + QStringLiteral(".manifest"),
                                asset + QStringLiteral(".manifest.sig"),
                                QStringLiteral("Ants_Terminal-x86_64.AppImage")};
    const QByteArray sha = sha256Hex(payload(1, 300));
    QString why;

    auto verifyWith = [&](const QByteArray &manifest, const KeyPair &signer,
                          const QString &relTag, const QString &run, const QStringList &list,
                          const QByteArray &pub) {
        const QByteArray sig = signBytes(signer, dir, manifest, &why);
        return SelfUpdate::verifyManifest(manifest, sig, pub, relTag, run, list);
    };
    auto expectErr = [&](const char *label, const SelfUpdate::VerifyResult &r, VerifyError want) {
        expect(r.error == want, label,
               QStringLiteral("expected %1, got %2 (%3)").arg(errName(want), errName(r.error), r.message));
    };

    const QByteArray good = manifestJson(QStringLiteral("0.8.0"), asset, 300, sha);
    const auto ok = verifyWith(good, kp, tag, running, assets, kp.pubPem);
    expectErr("INV-1 a correct manifest is accepted", ok, VerifyError::None);
    expect(ok.manifest.version == QStringLiteral("0.8.0"), "INV-1 parsed version",
           QStringLiteral("expected 0.8.0, got '%1'").arg(ok.manifest.version));
    expect(ok.manifest.asset == asset, "INV-1 parsed asset",
           QStringLiteral("expected %1, got '%2'").arg(asset, ok.manifest.asset));
    expect(ok.manifest.size == 300, "INV-1 parsed size",
           QStringLiteral("expected 300, got %1").arg(ok.manifest.size));
    expect(ok.manifest.sha256Hex == sha, "INV-1 parsed sha256",
           QStringLiteral("expected %1, got %2").arg(QString::fromLatin1(sha), QString::fromLatin1(ok.manifest.sha256Hex)));
    expect(ok.manifest.format == 1, "INV-1 parsed format",
           QStringLiteral("expected 1, got %1").arg(ok.manifest.format));

    // A one-byte change to the manifest.
    {
        QByteArray sig = signBytes(kp, dir, good, &why);
        QByteArray bad = good;
        bad[bad.size() / 2] = static_cast<char>(bad[bad.size() / 2] ^ 0x01);
        expectErr("INV-1 one flipped manifest byte -> BadSignature",
                  SelfUpdate::verifyManifest(bad, sig, kp.pubPem, tag, running, assets),
                  VerifyError::BadSignature);
        // A one-byte change to the signature.
        QByteArray badSig = sig;
        badSig[0] = static_cast<char>(badSig[0] ^ 0x01);
        expectErr("INV-1 one flipped signature byte -> BadSignature",
                  SelfUpdate::verifyManifest(good, badSig, kp.pubPem, tag, running, assets),
                  VerifyError::BadSignature);
    }
    // Signed by a different key.
    expectErr("INV-1 a signature by another key -> BadSignature",
              verifyWith(good, other, tag, running, assets, kp.pubPem), VerifyError::BadSignature);
    // Version differs from the tag (manifest is otherwise self-consistent).
    expectErr("INV-1 manifest version != tag -> VersionNotTag",
              verifyWith(manifestJson(QStringLiteral("0.8.1"), versionedAsset(QStringLiteral("0.8.1")), 300, sha),
                         kp, tag, running, {versionedAsset(QStringLiteral("0.8.1"))}, kp.pubPem),
              VerifyError::VersionNotTag);
    // Not newer: equal, then older (tag follows the manifest so only this check can fire).
    for (const QString &v : {QStringLiteral("0.7.5"), QStringLiteral("0.7.0")}) {
        expectErr(v == QStringLiteral("0.7.5") ? "INV-1 equal version -> VersionNotNewer"
                                               : "INV-1 older version -> VersionNotNewer",
                  verifyWith(manifestJson(v, versionedAsset(v), 300, sha), kp,
                             QStringLiteral("v") + v, running, {versionedAsset(v)}, kp.pubPem),
                  VerifyError::VersionNotNewer);
    }
    // Asset name other than the versioned AppImage (even though listed).
    {
        const QString alias = QStringLiteral("Ants_Terminal-x86_64.AppImage");
        expectErr("INV-1 asset is the version-less alias -> AssetName",
                  verifyWith(manifestJson(QStringLiteral("0.8.0"), alias, 300, sha), kp, tag, running,
                             assets, kp.pubPem),
                  VerifyError::AssetName);
    }
    // Asset missing from the release's list.
    expectErr("INV-1 asset absent from the release list -> AssetMissing",
              verifyWith(good, kp, tag, running,
                         {asset + QStringLiteral(".manifest"), QStringLiteral("Ants_Terminal-x86_64.AppImage")},
                         kp.pubPem),
              VerifyError::AssetMissing);
}

// ---- Session fixture (INV-2, INV-3) -------------------------------------

struct Fixture {
    QTemporaryDir tmp;
    QString dir, target;
    QByteArray oldBytes = payload(7, 2048);
    QByteArray newBytes = payload(9, 5000);
    KeyPair kp;
    bool ready = false;
};

// Returns false (and sets skip text) when openssl cannot sign.
bool makeFixture(Fixture &f, std::string *skip) {
    if (!f.tmp.isValid()) { *skip = "no temp dir"; return false; }
    f.dir = f.tmp.path() + QStringLiteral("/app");
    QDir().mkpath(f.dir);
    f.target = f.dir + QStringLiteral("/My Ants.AppImage");
    writeAll(f.target, f.oldBytes);
    ::chmod(f.target.toLocal8Bit().constData(), 0700);
    // An old mtime, so "unchanged" cannot be satisfied by a same-second rewrite.
    QFile tf(f.target);
    if (tf.open(QIODevice::ReadWrite))
        tf.setFileTime(QDateTime::fromSecsSinceEpoch(1'000'000'000), QFileDevice::FileModificationTime);
    tf.close();
    const QString keys = f.tmp.path() + QStringLiteral("/keys");
    QDir().mkpath(keys);
    f.kp = makeKeyPair(keys, QStringLiteral("k"));
    if (!f.kp.ok) { *skip = "openssl unavailable: " + f.kp.why.toStdString(); return false; }
    QString why;
    if (signBytes(f.kp, keys, QByteArrayLiteral("probe"), &why).size() != 64) {
        *skip = "openssl lacks -rawin: " + why.toStdString();
        return false;
    }
    f.ready = true;
    return true;
}

// Accepts a manifest describing `declared` bytes; returns the session's accept result.
SelfUpdate::VerifyResult accept(SelfUpdate::Session &s, Fixture &f, const QByteArray &declared) {
    const QString keys = f.tmp.path() + QStringLiteral("/keys");
    const QString asset = versionedAsset(QStringLiteral("0.8.0"));
    const QByteArray m = manifestJson(QStringLiteral("0.8.0"), asset, declared.size(), sha256Hex(declared));
    QString why;
    const QByteArray sig = signBytes(f.kp, keys, m, &why);
    return s.acceptManifest(m, sig, QStringLiteral("v0.8.0"), QStringLiteral("0.7.5"), {asset});
}

void checkMismatchLeavesOriginal(Fixture &f, const char *label, const QByteArray &declared,
                                 const QByteArray &delivered) {
    const QByteArray before = readAll(f.target);
    const QDateTime mtime = QFileInfo(f.target).lastModified();
    const unsigned mode = modeOf(f.target);
    const QStringList listing = listDir(f.dir);

    QString tempPath;
    bool finished = true;
    {
        SelfUpdate::Session s(f.kp.pubPem, f.target);
        const auto r = accept(s, f, declared);
        expect(r.ok(), label, QStringLiteral("manifest not accepted: %1 (%2)").arg(errName(r.error), r.message));
        const bool began = s.begin();
        expect(began, label, QStringLiteral("begin() failed: %1").arg(s.error()));
        tempPath = s.temporaryPath();
        // Deliver in two chunks, as a network would.
        s.write(delivered.left(delivered.size() / 2));
        s.write(delivered.mid(delivered.size() / 2));
        finished = s.finish();
        expect(!finished, label, QStringLiteral("finish() succeeded on a mismatching download (%1)").arg(label));
        expect(!s.error().isEmpty(), label, QStringLiteral("no error reported for a refused download"));
    }
    expect(readAll(f.target) == before, label, QStringLiteral("target bytes changed (%1)").arg(label));
    expect(QFileInfo(f.target).lastModified() == mtime, label,
           QStringLiteral("target mtime changed: %1 -> %2").arg(mtime.toString(), QFileInfo(f.target).lastModified().toString()));
    expect(modeOf(f.target) == mode, label,
           QStringLiteral("target mode changed: %1 -> %2").arg(mode, 0, 8).arg(modeOf(f.target), 0, 8));
    expect(listDir(f.dir) == listing, label,
           QStringLiteral("directory changed: before [%1] after [%2]").arg(listing.join(','), listDir(f.dir).join(',')));
    expect(tempPath.isEmpty() ? false : !QFileInfo::exists(tempPath), label,
           QStringLiteral("temporary file still present or never reported: '%1'").arg(tempPath));
}

// ---- INV-9 helpers -------------------------------------------------------

QString stripYamlComments(const QString &s) {
    QStringList keep;
    for (const QString &line : s.split(QLatin1Char('\n')))
        if (!line.trimmed().startsWith(QLatin1Char('#'))) keep << line;
    return keep.join(QLatin1Char('\n'));
}

QString stripCppComments(const QString &s) {
    QString out;
    const int n = s.size();
    for (int i = 0; i < n; ++i) {
        const QChar c = s.at(i);
        if (c == QLatin1Char('"')) {
            out += c;
            for (++i; i < n && s.at(i) != QLatin1Char('"'); ++i) {
                out += s.at(i);
                if (s.at(i) == QLatin1Char('\\') && i + 1 < n) out += s.at(++i);
            }
            if (i < n) out += s.at(i);
        } else if (c == QLatin1Char('/') && i + 1 < n && s.at(i + 1) == QLatin1Char('/')) {
            while (i < n && s.at(i) != QLatin1Char('\n')) ++i;
            out += QLatin1Char('\n');
        } else if (c == QLatin1Char('/') && i + 1 < n && s.at(i + 1) == QLatin1Char('*')) {
            i += 2;
            while (i + 1 < n && !(s.at(i) == QLatin1Char('*') && s.at(i + 1) == QLatin1Char('/'))) ++i;
            ++i;
        } else {
            out += c;
        }
    }
    return out;
}

struct Step { QString name, body; };

QList<Step> splitSteps(const QString &yaml) {
    static const QRegularExpression re(QStringLiteral("^\\s*-\\s+name:\\s*(.*)$"),
                                       QRegularExpression::MultilineOption);
    QList<Step> steps;
    QList<qsizetype> starts;
    QStringList names;
    auto it = re.globalMatch(yaml);
    while (it.hasNext()) {
        auto m = it.next();
        starts << m.capturedStart();
        names << m.captured(1).trimmed();
    }
    for (int i = 0; i < starts.size(); ++i) {
        const qsizetype end = i + 1 < starts.size() ? starts[i + 1] : yaml.size();
        steps.append({names[i], yaml.mid(starts[i], end - starts[i])});
    }
    return steps;
}

int stepIndex(const QList<Step> &steps, const QString &prefix) {
    for (int i = 0; i < steps.size(); ++i)
        if (steps[i].name.startsWith(prefix)) return i;
    return -1;
}

bool listsBothManifestFiles(const QString &body) {
    static const QRegularExpression man(QStringLiteral("\\.manifest(?!\\.sig)"));
    return man.match(body).hasMatch() && body.contains(QStringLiteral(".manifest.sig"));
}

}  // namespace

// ---------------------------------------------------------------------------

TEST(SelfUpdate, ManifestVerification) {
    REQUIRE_VERIFIER();
    expect_reset();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    REQUIRE_OPENSSL(kp, tmp.path(), probeSig);
    REQUIRE_OPENSSL(other, tmp.path(), probeSig2);
    checkManifestVerification(tmp.path(), kp, other);
    ASSERT_EQ(0, expect_finish());
}

TEST(SelfUpdate, DownloadMismatchLeavesOriginal) {
    REQUIRE_VERIFIER();
    expect_reset();
    // Flipped byte: right size, wrong hash.
    {
        Fixture f;
        std::string skip;
        if (!makeFixture(f, &skip)) GTEST_SKIP() << skip;
        QByteArray flipped = f.newBytes;
        flipped[flipped.size() / 3] = static_cast<char>(flipped[flipped.size() / 3] ^ 0x01);
        checkMismatchLeavesOriginal(f, "INV-2 flipped byte refused, original intact", f.newBytes, flipped);
    }
    // Short body: right prefix, wrong size.
    {
        Fixture f;
        std::string skip;
        if (!makeFixture(f, &skip)) GTEST_SKIP() << skip;
        checkMismatchLeavesOriginal(f, "INV-2 short body refused, original intact", f.newBytes,
                                    f.newBytes.left(f.newBytes.size() - 100));
    }
    ASSERT_EQ(0, expect_finish());
}

TEST(SelfUpdate, SwapInPlace) {
    REQUIRE_VERIFIER();
    expect_reset();
    Fixture f;
    std::string skip;
    if (!makeFixture(f, &skip)) GTEST_SKIP() << skip;
    QString tempPath;
    {
        SelfUpdate::Session s(f.kp.pubPem, f.target);
        const auto r = accept(s, f, f.newBytes);
        expect(r.ok(), "INV-3 manifest accepted",
               QStringLiteral("%1 (%2)").arg(errName(r.error), r.message));
        expect(s.begin(), "INV-3 begin()", QStringLiteral("failed: %1").arg(s.error()));
        tempPath = s.temporaryPath();
        expect(!tempPath.isEmpty(), "INV-3 temporaryPath reported", QStringLiteral("empty"));
        expect(QFileInfo(tempPath).absolutePath() == QFileInfo(f.target).absolutePath(),
               "INV-3 temporary file sits in the target's directory",
               QStringLiteral("temp '%1' target '%2'").arg(tempPath, f.target));
        expect(QFileInfo(tempPath).absoluteFilePath() != QFileInfo(f.target).absoluteFilePath(),
               "INV-3 temporary path differs from the target", tempPath);
        s.write(f.newBytes.left(1000));
        s.write(f.newBytes.mid(1000));
        expect(s.finish(), "INV-3 finish()", QStringLiteral("failed: %1").arg(s.error()));
    }
    const QByteArray now = readAll(f.target);
    expect(now == f.newBytes, "INV-3 target holds the new bytes",
           QStringLiteral("expected %1 bytes, got %2 (sha %3)")
               .arg(f.newBytes.size()).arg(now.size()).arg(QString::fromLatin1(sha256Hex(now))));
    expect(modeOf(f.target) == 0755, "INV-3 target mode is 0755",
           QStringLiteral("expected 755, got %1").arg(modeOf(f.target), 0, 8));
    expect(!tempPath.isEmpty() && !QFileInfo::exists(tempPath), "INV-3 no temporary file remains", tempPath);
    expect(listDir(f.dir) == QStringList{QStringLiteral("My Ants.AppImage")},
           "INV-3 directory holds only the target",
           QStringLiteral("got [%1]").arg(listDir(f.dir).join(',')));
    ASSERT_EQ(0, expect_finish());
}

TEST(SelfUpdate, InstallKind) {
    expect_reset();
    using SelfUpdate::InstallKind;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString rw = tmp.path() + QStringLiteral("/rw");
    const QString ro = tmp.path() + QStringLiteral("/ro");
    QDir().mkpath(rw);
    QDir().mkpath(ro);
    const QString rwFile = rw + QStringLiteral("/a.AppImage");
    const QString roFile = ro + QStringLiteral("/b.AppImage");
    writeAll(rwFile, "x");
    writeAll(roFile, "x");
    ModeGuard g{ro};
    ::chmod(ro.toLocal8Bit().constData(), 0555);

    auto kindOf = [](const QString *appImage) {
        QProcessEnvironment e;
        e.insert(QStringLiteral("PATH"), QStringLiteral("/usr/bin"));
        if (appImage) e.insert(QStringLiteral("APPIMAGE"), *appImage);
        return SelfUpdate::installKind(e);
    };
    auto expectKind = [&](const char *label, InstallKind got, InstallKind want) {
        expect(got == want, label,
               QStringLiteral("expected %1, got %2").arg(static_cast<int>(want)).arg(static_cast<int>(got)));
    };
    const QString missing = tmp.path() + QStringLiteral("/none.AppImage");
    expectKind("INV-4 APPIMAGE unset -> NotAppImage", kindOf(nullptr), InstallKind::NotAppImage);
    expectKind("INV-4 APPIMAGE names a missing file -> NotAppImage", kindOf(&missing), InstallKind::NotAppImage);
    expectKind("INV-4 APPIMAGE names a directory -> NotAppImage", kindOf(&rw), InstallKind::NotAppImage);
#ifndef ANTS_HAVE_LIBSODIUM
    expectKind("INV-4 no libsodium, writable directory -> NotAppImage", kindOf(&rwFile), InstallKind::NotAppImage);
    expectKind("INV-4 no libsodium, read-only directory -> NotAppImage", kindOf(&roFile), InstallKind::NotAppImage);
#else
    expectKind("INV-4 file in a writable directory -> Updatable", kindOf(&rwFile), InstallKind::Updatable);
    if (::geteuid() == 0 || QFileInfo(ro).isWritable()) {
        std::fprintf(stderr, "[SKIP-ARM] INV-4 ReadOnlyDir: this user can write a 0555 directory (root?)\n");
    } else {
        expectKind("INV-4 file in a read-only directory -> ReadOnlyDir", kindOf(&roFile), InstallKind::ReadOnlyDir);
    }
#endif
    ASSERT_EQ(0, expect_finish());
}

TEST(SelfUpdate, HttpsOnly) {
    expect_reset();
    const QUrl https(QStringLiteral("https://github.com/milnet01/ants-terminal/releases/download/v0.8.0/a.AppImage"));
    expect(SelfUpdate::isAllowedUrl(https), "INV-5 https is allowed", https.toString());
    QUrl http = https;
    http.setScheme(QStringLiteral("http"));
    expect(!SelfUpdate::isAllowedUrl(http), "INV-5 http is refused", http.toString());
    expect(!SelfUpdate::isAllowedUrl(QUrl(QStringLiteral("file:///etc/passwd"))), "INV-5 file is refused");
    expect(!SelfUpdate::isAllowedUrl(QUrl(QStringLiteral("ftp://example.com/a"))), "INV-5 ftp is refused");
    expect(!SelfUpdate::isAllowedUrl(QUrl()), "INV-5 an empty URL is refused");

    const QNetworkRequest req = SelfUpdate::makeRequest(https);
    expect(req.url() == https, "INV-5 the request targets the given URL", req.url().toString());
    const QVariant policy = req.attribute(QNetworkRequest::RedirectPolicyAttribute);
    expect(policy.isValid() && policy.toInt() == static_cast<int>(QNetworkRequest::NoLessSafeRedirectPolicy),
           "INV-5 redirects use NoLessSafeRedirectPolicy",
           QStringLiteral("expected %1, attribute is %2 (valid=%3)")
               .arg(static_cast<int>(QNetworkRequest::NoLessSafeRedirectPolicy))
               .arg(policy.toInt()).arg(int(policy.isValid())));

    // Source check: the updater's own code names the policy outside comments.
    const QString src = stripCppComments(QString::fromUtf8(readAll(QString::fromUtf8(SRC_SELFUPDATE_CPP_PATH))));
    expect(!src.isEmpty(), "INV-5 source readable", QString::fromUtf8(SRC_SELFUPDATE_CPP_PATH));
    expect(src.contains(QStringLiteral("NoLessSafeRedirectPolicy")) &&
               src.contains(QStringLiteral("RedirectPolicyAttribute")),
           "INV-5 source sets RedirectPolicyAttribute to NoLessSafeRedirectPolicy",
           QStringLiteral("selfupdate.cpp (comments stripped) lacks one of the two names"));
    expect(!src.contains(QStringLiteral("UserVerifiedRedirectPolicy")) &&
               !src.contains(QStringLiteral("ManualRedirectPolicy")) &&
               !src.contains(QStringLiteral("SameOriginRedirectPolicy")),
           "INV-5 source names no other redirect policy", QStringLiteral("another policy is mentioned"));
    ASSERT_EQ(0, expect_finish());
}

TEST(SelfUpdate, RelaunchCommand) {
    expect_reset();
    const QString appdir = QStringLiteral("/tmp/.mount_AntsX1y2Z3");
    const QString path = QStringLiteral("/home/u/My Apps/it's ants.AppImage");
    QProcessEnvironment env;
    env.insert(QStringLiteral("APPDIR"), appdir);
    env.insert(QStringLiteral("APPIMAGE"), path);
    env.insert(QStringLiteral("ARGV0"), QStringLiteral("./ants"));
    env.insert(QStringLiteral("OWD"), QStringLiteral("/home/u"));
    env.insert(QStringLiteral("PATH"), appdir + QStringLiteral("/usr/bin:/usr/bin"));
    env.insert(QStringLiteral("XDG_DATA_DIRS"), appdir + QStringLiteral("/usr/share:/usr/share"));
    env.insert(QStringLiteral("XDG_CONFIG_DIRS"), appdir + QStringLiteral("/etc/xdg:/etc/xdg"));
    env.insert(QStringLiteral("LD_LIBRARY_PATH"), appdir + QStringLiteral("/usr/lib:/opt/other/lib"));
    env.insert(QStringLiteral("QT_PLUGIN_PATH"), appdir + QStringLiteral("/usr/plugins:/opt/qt/plugins"));
    env.insert(QStringLiteral("GIO_EXTRA_MODULES"), appdir + QStringLiteral("/usr/lib/gio"));
    env.insert(QStringLiteral("KEEP_ME"), QStringLiteral("hello"));

    const SelfUpdate::Relaunch r = SelfUpdate::relaunchCommand(4242, path, env);
    expect(r.program == QStringLiteral("/bin/sh"), "INV-6 program is /bin/sh", r.program);
    const QStringList &a = r.arguments;
    expect(a.size() >= 5 && a[0] == QStringLiteral("-c"), "INV-6 argv starts with -c <script>",
           a.join(QStringLiteral(" | ")));
    const QString script = a.size() > 1 ? a[1] : QString();
    expect(script.contains(QStringLiteral("kill -0")), "INV-6 the script waits for the pid to exit", script);
    expect(!script.contains(path) && !script.contains(QStringLiteral("it's")) &&
               !script.contains(QStringLiteral("4242")),
           "INV-6 neither the path nor the pid is spliced into the script", script);
    expect(a.size() >= 2 && a[a.size() - 2] == QStringLiteral("4242") && a.last() == path,
           "INV-6 pid and path are separate trailing arguments",
           QStringLiteral("argv: %1").arg(a.join(QStringLiteral(" | "))));

    const QProcessEnvironment &o = r.environment;
    for (const char *k : {"APPDIR", "APPIMAGE", "ARGV0", "OWD"})
        expect(!o.contains(QString::fromLatin1(k)), "INV-6 dropped variable",
               QStringLiteral("%1 still set to '%2'").arg(QString::fromLatin1(k), o.value(QString::fromLatin1(k))));
    expect(o.value(QStringLiteral("PATH")) == QStringLiteral("/usr/bin"), "INV-6 PATH is /usr/bin alone",
           QStringLiteral("got '%1'").arg(o.value(QStringLiteral("PATH"))));
    expect(o.value(QStringLiteral("XDG_DATA_DIRS")) == QStringLiteral("/usr/share"),
           "INV-6 XDG_DATA_DIRS is /usr/share alone", QStringLiteral("got '%1'").arg(o.value(QStringLiteral("XDG_DATA_DIRS"))));
    expect(o.value(QStringLiteral("XDG_CONFIG_DIRS")) == QStringLiteral("/etc/xdg"),
           "INV-6 XDG_CONFIG_DIRS is /etc/xdg alone", QStringLiteral("got '%1'").arg(o.value(QStringLiteral("XDG_CONFIG_DIRS"))));
    expect(o.value(QStringLiteral("LD_LIBRARY_PATH")) == QStringLiteral("/opt/other/lib"),
           "INV-6 LD_LIBRARY_PATH keeps only entries outside the mount",
           QStringLiteral("got '%1'").arg(o.value(QStringLiteral("LD_LIBRARY_PATH"))));
    expect(o.value(QStringLiteral("QT_PLUGIN_PATH")) == QStringLiteral("/opt/qt/plugins"),
           "INV-6 QT_PLUGIN_PATH keeps only entries outside the mount",
           QStringLiteral("got '%1'").arg(o.value(QStringLiteral("QT_PLUGIN_PATH"))));
    expect(!o.contains(QStringLiteral("GIO_EXTRA_MODULES")), "INV-6 another variable naming the appdir is dropped whole",
           o.value(QStringLiteral("GIO_EXTRA_MODULES")));
    expect(o.value(QStringLiteral("KEEP_ME")) == QStringLiteral("hello"),
           "INV-6 an unrelated variable is kept", QStringLiteral("got '%1'").arg(o.value(QStringLiteral("KEEP_ME"))));
    for (const QString &k : o.keys())
        expect(!o.value(k).contains(appdir), "INV-6 no value names the old appdir",
               QStringLiteral("%1=%2").arg(k, o.value(k)));
    ASSERT_EQ(0, expect_finish());
}

TEST(SelfUpdate, SkipAndStartupSetting) {
    expect_reset();
    using SelfUpdate::CheckTrigger;
    const QString latest = QStringLiteral("0.9.0");
    const QString running = QStringLiteral("0.8.0");
    const QStringList skips = {QString(), QStringLiteral("0.9.0"), QStringLiteral("0.8.5")};
    for (CheckTrigger t : {CheckTrigger::Startup, CheckTrigger::Manual})
        for (const QString &skipped : skips)
            for (bool startup : {true, false}) {
                const bool manual = t == CheckTrigger::Manual;
                const bool want = manual ? true : (startup && skipped != latest);
                const bool got = SelfUpdate::shouldReport(t, latest, running, skipped, startup);
                expect(got == want, "INV-7 newer version",
                       QStringLiteral("trigger=%1 skipped='%2' checkOnStartup=%3: expected %4, got %5")
                           .arg(manual ? QStringLiteral("Manual") : QStringLiteral("Startup"), skipped).arg(int(startup)).arg(int(want)).arg(int(got)));
            }
    for (const QString &notNewer : {QStringLiteral("0.8.0"), QStringLiteral("0.7.9")})
        for (CheckTrigger t : {CheckTrigger::Startup, CheckTrigger::Manual})
            for (const QString &skipped : skips)
                for (bool startup : {true, false}) {
                    const bool got = SelfUpdate::shouldReport(t, notNewer, running, skipped, startup);
                    expect(!got, "INV-7 a version not newer is never reported",
                           QStringLiteral("latest=%1 trigger=%2 skipped='%3' checkOnStartup=%4: got true")
                               .arg(notNewer, t == CheckTrigger::Manual ? QStringLiteral("Manual") : QStringLiteral("Startup"), skipped).arg(int(startup)));
                }
    ASSERT_EQ(0, expect_finish());
}

TEST(SelfUpdate, PipelineSignatureRoundTrip) {
    expect_reset();
    // Arm 2 first: the embedded key is the committed file, and non-empty.
    {
        const QByteArray committed = readAll(QString::fromUtf8(PUBKEY_PEM_PATH));
        const QByteArray embedded = SelfUpdate::embeddedPublicKeyPem();
        expect(!committed.isEmpty(), "INV-8 committed key readable", QString::fromUtf8(PUBKEY_PEM_PATH));
        expect(!embedded.isEmpty(), "INV-8 embedded key is non-empty", QStringLiteral("embeddedPublicKeyPem() is empty"));
        expect(embedded == committed, "INV-8 embedded key equals the committed file",
               QStringLiteral("committed %1 bytes, embedded %2 bytes").arg(committed.size()).arg(embedded.size()));
    }
#ifndef ANTS_HAVE_LIBSODIUM
    std::fprintf(stderr, "[SKIP-ARM] INV-8 script round trip: built without libsodium\n");
#else
    // Arm 1: the script's output verifies under the C++ verifier.
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    KeyPair kp = makeKeyPair(tmp.path(), QStringLiteral("pipe"));
    QString why;
    if (!kp.ok || signBytes(kp, tmp.path(), QByteArrayLiteral("probe"), &why).size() != 64) {
        std::fprintf(stderr, "[SKIP-ARM] INV-8 script round trip: openssl unusable (%s%s)\n",
                     kp.why.toLocal8Bit().constData(), why.toLocal8Bit().constData());
        ASSERT_EQ(0, expect_finish());
        return;
    }
    const QString version = QStringLiteral("0.8.0");
    const QString image = tmp.path() + QLatin1Char('/') + versionedAsset(version);
    const QByteArray bytes = payload(3, 4096);
    writeAll(image, bytes);

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("ANTS_UPDATE_SIGNING_KEY"), QString::fromUtf8(kp.privPem));
    const ProcResult run = runProcess(QStringLiteral("bash"),
                               {QString::fromUtf8(SIGN_SCRIPT_PATH), image, version, kp.pubPath}, &env);
    expect(run.exitCode == 0, "INV-8 the script exits 0",
           QStringLiteral("exit %1; stderr: %2").arg(run.exitCode).arg(show(run.err)));
    const QByteArray manifest = readAll(image + QStringLiteral(".manifest"));
    const QByteArray sig = readAll(image + QStringLiteral(".manifest.sig"));
    expect(!manifest.isEmpty(), "INV-8 .manifest written beside the AppImage", image + QStringLiteral(".manifest"));
    expect(sig.size() == 64, "INV-8 .manifest.sig is the raw 64-byte signature",
           QStringLiteral("got %1 bytes").arg(sig.size()));
    const auto r = SelfUpdate::verifyManifest(manifest, sig, kp.pubPem, QStringLiteral("v") + version,
                                              QStringLiteral("0.7.0"), {versionedAsset(version)});
    expect(r.ok(), "INV-8 the C++ verifier accepts the script's output",
           QStringLiteral("%1 (%2); manifest: %3").arg(errName(r.error), r.message, show(manifest)));
    expect(r.manifest.asset == versionedAsset(version), "INV-8 manifest asset is the file name",
           QStringLiteral("got '%1'").arg(r.manifest.asset));
    expect(r.manifest.size == bytes.size(), "INV-8 manifest size is the AppImage size",
           QStringLiteral("expected %1, got %2").arg(bytes.size()).arg(r.manifest.size));
    expect(r.manifest.sha256Hex == sha256Hex(bytes), "INV-8 manifest sha256 is the AppImage's",
           QStringLiteral("expected %1, got %2").arg(QString::fromLatin1(sha256Hex(bytes)), QString::fromLatin1(r.manifest.sha256Hex)));

    // Without the secret the script refuses, names the variable and writes nothing (§ 2.3).
    const QString image2 = tmp.path() + QStringLiteral("/nosecret/") + versionedAsset(version);
    QDir().mkpath(QFileInfo(image2).absolutePath());
    writeAll(image2, bytes);
    QProcessEnvironment bare = QProcessEnvironment::systemEnvironment();
    bare.remove(QStringLiteral("ANTS_UPDATE_SIGNING_KEY"));
    const ProcResult refused = runProcess(QStringLiteral("bash"),
                                   {QString::fromUtf8(SIGN_SCRIPT_PATH), image2, version, kp.pubPath}, &bare);
    expect(refused.exitCode != 0, "INV-8 no secret -> non-zero exit", QStringLiteral("exit %1").arg(refused.exitCode));
    expect(QString::fromUtf8(refused.err + refused.out).contains(QStringLiteral("ANTS_UPDATE_SIGNING_KEY")),
           "INV-8 no secret -> the message names ANTS_UPDATE_SIGNING_KEY", show(refused.err + refused.out));
    expect(!QFileInfo::exists(image2 + QStringLiteral(".manifest")) &&
               !QFileInfo::exists(image2 + QStringLiteral(".manifest.sig")),
           "INV-8 no secret -> no manifest or signature written", QStringLiteral("files exist"));
#endif
    ASSERT_EQ(0, expect_finish());
}

TEST(SelfUpdate, ReleaseWorkflowSigns) {
    expect_reset();
    const QString yaml = stripYamlComments(QString::fromUtf8(readAll(QString::fromUtf8(RELEASE_YML_PATH))));
    ASSERT_FALSE(yaml.isEmpty()) << RELEASE_YML_PATH;
    const QList<Step> steps = splitSteps(yaml);
    const int smoke = stepIndex(steps, QStringLiteral("Smoke-test AppImage"));
    const int upload = stepIndex(steps, QStringLiteral("Upload to release"));
    const int verify = stepIndex(steps, QStringLiteral("Verify the release carries"));
    expect(smoke >= 0 && upload > smoke && verify > upload, "INV-9 smoke < upload < verify steps found",
           QStringLiteral("smoke=%1 upload=%2 verify=%3").arg(smoke).arg(upload).arg(verify));

    static const QRegularExpression secretRef(
        QStringLiteral("ANTS_UPDATE_SIGNING_KEY:\\s*\\$\\{\\{\\s*secrets\\.ANTS_UPDATE_SIGNING_KEY\\s*\\}\\}"));
    int signing = -1;
    for (int i = 0; i < steps.size(); ++i)
        if (steps[i].body.contains(QStringLiteral("tools/sign-update-manifest.sh")) &&
            secretRef.match(steps[i].body).hasMatch())
            signing = i;
    expect(signing >= 0, "INV-9 a step runs tools/sign-update-manifest.sh with the secret in its env",
           QStringLiteral("no step (comments ignored) carries both the script and `ANTS_UPDATE_SIGNING_KEY: ${{ secrets.ANTS_UPDATE_SIGNING_KEY }}`"));
    expect(signing > smoke && signing < upload && smoke >= 0, "INV-9 signing sits between the smoke test and the upload",
           QStringLiteral("smoke=%1 signing=%2 upload=%3").arg(smoke).arg(signing).arg(upload));
    if (signing >= 0) {
        const QString &b = steps[signing].body;
        expect(!b.contains(QStringLiteral("continue-on-error")) && !b.contains(QStringLiteral("|| true")) &&
                   !QRegularExpression(QStringLiteral("^\\s+if:"), QRegularExpression::MultilineOption).match(b).hasMatch(),
               "INV-9 the signing step cannot be skipped or soften its failure",
               QStringLiteral("step body has continue-on-error, `|| true` or an `if:` guard"));
    }
    if (upload >= 0)
        expect(listsBothManifestFiles(steps[upload].body), "INV-9 the upload names .manifest and .manifest.sig",
               QStringLiteral("upload step lacks one of them"));
    if (verify >= 0)
        expect(listsBothManifestFiles(steps[verify].body), "INV-9 the artefact check names .manifest and .manifest.sig",
               QStringLiteral("verify step lacks one of them"));
    ASSERT_EQ(0, expect_finish());
}
