// ANTS-1401 — Central `caller_cwd` resolution result type.
//
// Three call paths historically decoded the same `caller_cwd` MCP
// argument with subtly different rules:
//   - terminalForCaller(QString)         in mainwindow.cpp
//   - resolveRootCanonical(MW *, req)    in remotecontrol.cpp
//   - cmdSessionMemory read path         in remotecontrol.cpp
// All three now route through `resolveCallerCwdRoot()` (declared in
// remotecontrol.h, defined alongside the existing `resolveRootCanonical`
// overloads). This header carries only the value type they return —
// kept dependency-light so the regression test can include it without
// pulling Qt6::Widgets.
//
// See docs/specs/ANTS-1401.md for the full design.

#pragma once

#include <optional>

#include <QString>

class MainWindow;

namespace ants {

struct ResolvedRoot {
    enum class Source {
        // caller_cwd present, canonicalises, and matches an open tab.
        ExplicitMatch,
        // caller_cwd absent or empty — focused-tab fallback.
        EmptyFallback,
        // caller_cwd present and canonicalises, but no open tab's
        // shellCwd canonicalises to the same form.
        NoMatch,
        // caller_cwd present but QFileInfo::canonicalFilePath()
        // returned empty (path doesn't exist).
        Unresolvable,
    };

    // Canonical FS path. Empty on Source::Unresolvable; empty on
    // Source::EmptyFallback when no terminal is focused or its
    // shellCwd doesn't canonicalise.
    QString cwd;

    // Which branch fired. Always set; defaults to EmptyFallback so
    // a default-constructed ResolvedRoot behaves like "empty req,
    // no terminal" — same as the pre-helper QString-empty signal.
    Source source = Source::EmptyFallback;

    // Tab index of the matching terminal. Set when
    //   - Source == ExplicitMatch (the matched tab), or
    //   - Source == EmptyFallback AND the focused tab is identifiable.
    // nullopt for NoMatch / Unresolvable, and for EmptyFallback when
    // no tab is focused.
    std::optional<int> tabIndex;
};

// Single source of truth. Defined in remotecontrol.cpp next to the
// existing `resolveRootCanonical` overloads. Pass `callerCwd` as the
// raw arg (empty string for the absent case); the helper handles
// canonicalisation and tab matching internally. const-correct
// because the helper only queries MainWindow.
ResolvedRoot resolveCallerCwdRoot(const MainWindow *main,
                                  const QString &callerCwd);

// ANTS-1390 — global Claude config sentinel. Returns canonical
// `~/.claude/` when `callerCwd` is the `~global` or `~claude-config`
// magic value, else empty string. Path tools (workspace_search,
// file_outline) detect this before normal root resolution so callers
// editing global Claude config can search / outline without a project
// root. Skips the per-tool focused-tab fallback so the sentinel always
// means "global config tree", never accidentally "this project's
// .claude/ subdir".
QString expandGlobalConfigSentinel(const QString &callerCwd);

}  // namespace ants
