#pragma once
// ANTS-5506 — `ants-mcpd --call <verb> [<json> | -] [--exit-code]`: run one
// project-scoped MCP verb from a shell and map its envelope to an exit code.
// Contract: docs/specs/ANTS-5506-mcpd-call.md.
//
// Free functions, so tests/features/mcpd_call/ can call the parsing, argument
// preparation, unwrapping and exit-code mapping without spawning a process.

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <cstdint>

class ClaudeIntegration;

namespace mcpd {

// § 2.2 — the four exit codes, the same meanings as ants-helper (ANTS-1116 INV-8).
enum CallExit : std::uint8_t {
    CallClean    = 0,
    CallFailed   = 1,
    CallUsage    = 2,
    CallFindings = 3,
};

// The command line after the program name, e.g. {"--call","spec_lint","{}"}.
struct CallRequest {
    bool isCall = false;      // `--call` was present
    QString verb;
    QJsonObject args;
    bool exitCode = false;    // `--exit-code`
    bool readStdin = false;   // the JSON argument was an explicit "-"
    QString usageError;       // non-empty → exit CallUsage
};

// § 2.1 — parses `--call <verb> [<json> | -] [--exit-code]`. No JSON argument
// is `{}`; only an explicit `-` asks for stdin.
CallRequest parseCallRequest(const QStringList &args);

// § 2.1 — the arguments the verb actually receives: the reply-shaping keys
// (compact, offload, fields, raw) removed, and `caller_cwd` filled from
// `processCwd` when absent.
QJsonObject prepareCallArguments(QJsonObject args, const QString &processCwd);

// § 2.1 — a tool result's text with the `ants_mcp_data` wrap removed where it
// is present; control-plane text comes back unchanged.
QString unwrapToolText(const QString &text);

// § 2.2 — the exit code for one reply envelope, the rules applied in order.
// `envelopeParsed` is false when the text was not a JSON object.
CallExit exitCodeFor(const QJsonObject &envelope, bool envelopeParsed,
                     bool exitCodeFlag);

// § 2.1 — runs one parsed request through `pipeline` and prints the unwrapped
// result to stdout. The caller has already set terse responses and offload off
// and built no forwarder and no usage snapshot. Returns the process exit code.
int runCall(const CallRequest &request, ClaudeIntegration &pipeline);

} // namespace mcpd
