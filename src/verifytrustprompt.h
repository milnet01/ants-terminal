// ANTS-5464 — the terminal's answer to ants-mcpd asking it to prompt for
// trust in a project's `.ants/verify.json`.
// Spec: docs/specs/ANTS-5464-mcpd-trust-prompt.md § 2.1 / § 2.2.
//
// ants-mcpd has no window to prompt from, so it sends the JSON-RPC method
// below to the terminal's MCP socket. It is not a tool: no model can call it.
// Qt::Core only.

#pragma once

#include <QJsonObject>

namespace VerifyTrust {

class Client;

// The JSON-RPC method name, on the terminal's MCP socket.
inline constexpr char kPromptMethod[] = "ants/verifyTrustPrompt";

// The request's `result`: {"outcome": "trusted|denied|headless|no_config",
// "sha": "<sha256 hex, empty for no_config>"}.
//
// Everything that decides trust comes from the terminal's own read: only
// `params.root` is read, and the config is read under it through
// VerifyEngine::readAnchoredConfig. `client` makes the decision, prompting
// where it has a window. A null client answers `headless`.
QJsonObject answerPromptRequest(const QJsonObject &params, Client *client);

}  // namespace VerifyTrust
