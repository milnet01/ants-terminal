// ANTS-5734 — see traceverb.h.

#include "trace/traceverb.h"

#include "resolvedroot.h"
#include "runtraceverb.h"

#include <QJsonDocument>

namespace mcp {

void registerMcpdOnlyVerbs(ToolSink &sink, const RegistryHost &host, trace::Env env) {
    // § 2.1 — a cold parse holds its thread for seconds, so the Bulk lane:
    // the shared worker's other verbs do not queue behind it.
    RcHandler handler;
    handler.lane = DispatchLane::Bulk;
    handler.fn = [roots = host.roots, env = std::move(env)](const QJsonObject &args) -> QString {
        // An empty caller_cwd is the dispatcher's refusal (Required).
        const QString callerRaw = args.value(QStringLiteral("caller_cwd")).toString();
        const ants::ResolvedRoot rr = ants::resolveCallerCwdRoot(roots, callerRaw);
        QJsonObject reply;
        if (rr.source == ants::ResolvedRoot::Source::Unresolvable || rr.cwd.isEmpty()) {
            reply = {{QStringLiteral("ok"), false},
                     {QStringLiteral("code"), QStringLiteral("no_project")},
                     {QStringLiteral("error"),
                      QStringLiteral("trace: caller_cwd \"%1\" does not resolve to a directory")
                          .arg(callerRaw)}};
        } else {
            reply = trace::serve(args, RunTraceVerb::projectRootFor(rr.cwd), env);
        }
        return QString::fromUtf8(QJsonDocument(reply).toJson(QJsonDocument::Compact));
    };
    // A plain literal, as mcptoolregistry.cpp writes its names:
    // mcp_provider_registry INV-8 greps for registerToolProvider("trace".
    sink.registerToolProvider("trace", CallerCwdContract::Required, std::move(handler));
}

}  // namespace mcp
