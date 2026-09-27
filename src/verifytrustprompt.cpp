// ANTS-5464 — see verifytrustprompt.h.

#include "verifytrustprompt.h"

#include "verifyengine.h"
#include "verifytrust.h"

#include <QFileInfo>

namespace VerifyTrust {

QJsonObject answerPromptRequest(const QJsonObject &params, Client *client) {
    const auto answer = [](const QString &outcome, const QString &sha) {
        return QJsonObject{{QStringLiteral("outcome"), outcome},
                           {QStringLiteral("sha"), sha}};
    };

    // INV-1 — only `root` is read; the config comes from the terminal's own
    // anchored read under it, never from the request.
    const QString rootCanon = QFileInfo(
        params.value(QStringLiteral("root")).toString()).canonicalFilePath();
    QByteArray raw;
    if (rootCanon.isEmpty() || !VerifyEngine::readAnchoredConfig(rootCanon, &raw))
        return answer(QStringLiteral("no_config"), {});
    bool parseError = false;
    if (VerifyEngine::parseGateConfig(raw, &parseError).isEmpty() || parseError)
        return answer(QStringLiteral("no_config"), {});

    if (!client) return answer(QStringLiteral("headless"), {});
    const Decision d = client->outcomeForConfig(rootCanon, raw);
    switch (d.outcome) {
    case Outcome::Trusted:           return answer(QStringLiteral("trusted"), d.shaHex);
    case Outcome::UntrustedFellBack: return answer(QStringLiteral("denied"), d.shaHex);
    case Outcome::Headless:          break;
    }
    return answer(QStringLiteral("headless"), d.shaHex);
}

}  // namespace VerifyTrust
