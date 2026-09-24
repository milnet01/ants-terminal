// ANTS-4932 § 2.3 — the types a tool registration names, and the sink it
// registers against.
//
// Moved out of ClaudeIntegration so the registration list
// (mcptoolregistry.cpp) can run against any host, and against a recording
// sink in a test. ClaudeIntegration keeps an alias for each, so existing
// `ClaudeIntegration::CallerCwdContract::Required` call sites compile
// unchanged.

#pragma once

#include <QJsonObject>
#include <QString>

#include <functional>

namespace mcp {

// ANTS-1253 — a handler receives the JSON-RPC `arguments` object and
// returns the tool's response as a JSON string.
using ToolHandler = std::function<QString(const QJsonObject &args)>;

// ANTS-1404 — per-tool caller_cwd contract. Recorded once per tool at
// registration time and consulted by the dispatcher before the handler
// runs. See docs/specs/ANTS-1404.md.
enum class CallerCwdContract {
    // Anchorable + leaks if absent — refuse with
    // {ok:false, code:"caller_cwd_required"} when caller_cwd is empty.
    Required,
    // Anchor when caller_cwd present, host fallback when absent.
    // Survey-from-outside legitimate.
    Optional,
    // Per-tab reads that route on `tab` index *or* caller_cwd. Refused
    // with no routing key (ANTS-1415 Phase 3b).
    TabSpecific,
    // No per-tab / per-project state; caller_cwd accepted-and-ignored.
    ProcessGlobal,
};

// ANTS-5086 — which worker an off-thread handler runs on (ANTS-2132
// § 2.10). Bulk is a second worker for a verb that holds its thread for
// seconds, so the shared worker's other verbs do not queue behind it.
enum class DispatchLane { Shared, Bulk };

// ANTS-2132 — a handler whose body is known to touch no widget, so the
// dispatcher may run it off the GUI thread. Produced by rcDelegate, whose
// body is nothing but a forward to a RemoteControl cmd*(). Deliberately NOT
// implicitly constructible from a ToolHandler: that would make every inline
// registration ambiguous between the two overloads.
struct RcHandler {
    ToolHandler fn;
    bool offThreadEligible = true;
    DispatchLane lane = DispatchLane::Shared;
};

// ANTS-2132 § 2.8 — a verb that replies later. Runs on the GUI thread and
// must return promptly; `reply` is called exactly once, on that thread.
using DeferredToolHandler =
    std::function<void(const QJsonObject &args,
                       std::function<void(QString)> reply)>;

// What a registration list registers against. ClaudeIntegration is the
// production sink in both hosts.
class ToolSink {
public:
    virtual ~ToolSink() = default;
    virtual void registerToolProvider(const QString &name,
                                      CallerCwdContract contract,
                                      ToolHandler handler) = 0;
    virtual void registerToolProvider(const QString &name,
                                      CallerCwdContract contract,
                                      RcHandler handler) = 0;
    virtual void registerToolProvider(const QString &name,
                                      CallerCwdContract contract,
                                      DeferredToolHandler handler) = 0;
};

}  // namespace mcp
