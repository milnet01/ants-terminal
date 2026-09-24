// ANTS-4932 § 2.3 — the one registration list for the project-scoped MCP
// verbs, consumed by both hosts.
//
// The terminal calls registerProjectScopedVerbs() from
// MainWindow::setupClaudeMcpProviders() and registers the terminal-scoped
// verbs itself; ants-mcpd calls it and registers a forwarder for each name in
// terminalScopedVerbNames(). A new project-scoped verb is registered HERE,
// never in mainwindow.cpp (INV-2).

#pragma once

#include "mcptoolsink.h"
#include "resolvedroot.h"

#include <QJsonDocument>
#include <QString>
#include <QVector>

#include <functional>
#include <optional>

class ClaudeIntegration;
class RemoteControl;
namespace ants { class RootProvider; }

namespace mcp {

// Read lazily on every call: the terminal registers before it has built its
// RemoteControl.
using RemoteControlGetter = std::function<RemoteControl *()>;

// project_query's three Config reads (ANTS-2093).
struct ProjectQueryConfig {
    bool enabled = false;
    int timeoutMs = 0;
    int capBytes = 0;
};

// What the inline handlers need from the host besides the sink. Every field
// may be null in a recording test; a handler touches it only when called.
struct RegistryHost {
    ClaudeIntegration *ci = nullptr;
    const ants::RootProvider *roots = nullptr;
    // nullopt refuses the call (gui_read_refused), never defaults it.
    std::function<std::optional<ProjectQueryConfig>()> projectQueryConfig;
};

// Registers every verb that does not require a terminal, against any host.
// The ONLY enumeration of the project-scoped verb set.
void registerProjectScopedVerbs(ToolSink &sink, RemoteControlGetter rc,
                                RegistryHost host = {});

// The verbs that DO require a terminal: the ones the terminal registers
// itself, plus get_session_info, which its pipeline dispatches inline.
const QVector<QString> &terminalScopedVerbNames();

// ANTS-1782 — the RC-delegate factory. Most verbs are byte-identical shims
// that differ only in which RemoteControl cmd*() they forward `args` to; this
// builds that handler so the null-guard and serialise body live in one place.
// ANTS-2132 — returns the MARKED handler type, so registration can tell a
// forward-to-cmd*() body from a hand-written lambda and decide which thread
// may run it.
RcHandler rcDelegate(RemoteControlGetter rc,
                     QJsonDocument (RemoteControl::*fn)(const QJsonObject &),
                     DispatchLane lane = DispatchLane::Shared);

// ANTS-1400 — `ants::ResolvedRoot::Source` as caller_cwd_info reports it.
// PascalCase mirrors the enum identifiers; -Wswitch catches a new value.
QString sourceToString(ants::ResolvedRoot::Source s);

}  // namespace mcp
