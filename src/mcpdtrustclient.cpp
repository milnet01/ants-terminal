// ANTS-5464 — see mcpdtrustclient.h.

#include "mcpdtrustclient.h"

#include "mcpdsocket.h"
#include "verifytrustprompt.h"

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>

#include <algorithm>
#include <unistd.h>

namespace mcpd {

namespace {
// A reply is two short strings; anything longer is not one.
constexpr qsizetype kMaxReplyBytes = qsizetype{64} * 1024;
}  // namespace

ForwardingTrustClient::ForwardingTrustClient(const QString &trustFilePath,
                                             int timeoutMs)
    : FilePersistedTrustClient(trustFilePath), m_timeoutMs(timeoutMs) {}

// § 2.3 — runs on the calling thread (the Bulk worker when verify_changes
// asks), blocking it for at most m_timeoutMs. The main thread and the Shared
// worker keep serving meanwhile.
VerifyTrust::Decision ForwardingTrustClient::prompt(const QString &projectPath,
                                                    const QString &shaHex,
                                                    const QByteArray &) {
    using VerifyTrust::Outcome;
    VerifyTrust::Decision headless{Outcome::Headless, shaHex};

    // Step 1 — the socket and uid checks mcpd::Forwarder makes (ANTS-4932
    // § 2.5, INV-11). No acceptable terminal: Headless at once (INV-4).
    const uid_t me = ::getuid();
    const QString path = pickTerminalSocket(me);
    if (path.isEmpty()) return headless;

    QElapsedTimer clock;
    clock.start();
    const auto remaining = [&] {
        return static_cast<int>(std::max<qint64>(0, m_timeoutMs - clock.elapsed()));
    };

    QLocalSocket sock;
    sock.connectToServer(path);
    if (!sock.waitForConnected(remaining())) return headless;
    if (!peerUidIs(static_cast<int>(sock.socketDescriptor()), me)) return headless;

    // Step 2 — § 2.1's one line. Only the root: the terminal reads the config
    // itself (INV-1).
    const QJsonObject req{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), 1},
        {QStringLiteral("method"), QLatin1String(VerifyTrust::kPromptMethod)},
        {QStringLiteral("params"), QJsonObject{{QStringLiteral("root"), projectPath}}}};
    sock.write(QJsonDocument(req).toJson(QJsonDocument::Compact) + '\n');
    while (sock.bytesToWrite() > 0) {
        if (remaining() == 0 || !sock.waitForBytesWritten(remaining())) return headless;
    }

    QByteArray buf;
    qsizetype nl = -1;
    while ((nl = buf.indexOf('\n')) < 0) {
        if (buf.size() > kMaxReplyBytes) return headless;
        // INV-6 — a late or missing reply is Headless, and not cached.
        if (remaining() == 0) return headless;
        const bool ready = sock.waitForReadyRead(remaining());
        buf += sock.readAll();
        if (!ready && buf.indexOf('\n') < 0) return headless;
    }
    const QJsonObject result = QJsonDocument::fromJson(buf.left(nl)).object()
                                   .value(QStringLiteral("result")).toObject();
    const QString outcome = result.value(QStringLiteral("outcome")).toString();
    // A reply about another SHA is about another file: Headless.
    if (result.value(QStringLiteral("sha")).toString() != shaHex) return headless;

    // Step 3. INV-2 — the terminal's word alone never trusts anything: the
    // grant counts only once this process's own re-read of the trust file
    // finds it (ANTS-5411).
    if (outcome == QLatin1String("trusted"))
        return isTrustedNow(projectPath, shaHex)
            ? VerifyTrust::Decision{Outcome::Trusted, shaHex} : headless;
    // INV-5 — the base class caches a denial for this SHA.
    if (outcome == QLatin1String("denied"))
        return {Outcome::UntrustedFellBack, shaHex};
    return headless;
}

}  // namespace mcpd
