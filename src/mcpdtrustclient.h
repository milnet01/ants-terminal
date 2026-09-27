// ANTS-5464 — ants-mcpd's trust client: it asks the running terminal to show
// its trust prompt for an untrusted `.ants/verify.json`.
// Spec: docs/specs/ANTS-5464-mcpd-trust-prompt.md § 2.3.
//
// The terminal's word alone never trusts anything. A `trusted` reply counts
// only once this client's own re-read of the trust file finds the SHA or a
// matching repo pin. With no terminal, it answers Headless at once.

#pragma once

#include "verifytrust.h"

namespace mcpd {

// The longest ants-mcpd waits for the terminal's answer. The dialog stays
// open past it; a later click still saves the trust (ANTS-5411 carries it).
inline constexpr int kTrustPromptTimeoutMs = 120 * 1000;

class ForwardingTrustClient : public VerifyTrust::FilePersistedTrustClient {
public:
    // An empty `trustFilePath` is the default file. `timeoutMs` is for tests.
    explicit ForwardingTrustClient(const QString &trustFilePath = {},
                                   int timeoutMs = kTrustPromptTimeoutMs);

protected:
    VerifyTrust::Decision prompt(const QString &projectPath,
                                 const QString &shaHex,
                                 const QByteArray &configBytes) override;

private:
    int m_timeoutMs;
};

}  // namespace mcpd
