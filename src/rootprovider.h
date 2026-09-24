// ANTS-4932 § 2.4 — the root provider seam.
//
// Every read a project-scoped verb makes of the host it runs in goes through
// this interface, so the verb bodies name no MainWindow. Two hosts implement
// it: the terminal answers from its focused tab (MainWindowRootProvider in
// mainwindow.cpp), and ants-mcpd answers from its own process cwd
// (Source::ServerCwd). A null provider means "no host": the verbs that used
// to refuse on a null MainWindow refuse on it instead.
//
// Implementations must be callable from any thread. The GUI host marshals
// its widget reads itself; the verb bodies do not.

#pragma once

#include "resolvedroot.h"

#include <QString>

#include <optional>

namespace ants {

class RootProvider {
public:
    virtual ~RootProvider() = default;
    // The directory a verb anchors on when no caller_cwd was supplied.
    // Raw, not canonicalised; empty when the host has no answer.
    virtual QString fallbackRoot() const = 0;
    // The roadmap file for that same fallback; empty when there is none.
    virtual QString fallbackRoadmapPath() const = 0;
    // The tab behind fallbackRoot(), when the host has tabs.
    virtual std::optional<int> fallbackTab() const = 0;
    // What resolveCallerCwdRoot reports for the no-caller_cwd case.
    virtual ResolvedRoot::Source fallbackSource() const = 0;
    // The lowest-index open tab whose cwd canonicalises to `canonical`.
    virtual std::optional<int> tabForCwd(const QString &canonical) const = 0;
};

// ants-mcpd's provider: no tabs, and the fallback root is this process's own
// cwd, which the client set to its working directory when it launched the
// server (spec § 2.1). Defined beside resolveCallerCwdRoot.
class ServerCwdRootProvider final : public RootProvider {
public:
    QString fallbackRoot() const override;
    QString fallbackRoadmapPath() const override;
    std::optional<int> fallbackTab() const override { return std::nullopt; }
    ResolvedRoot::Source fallbackSource() const override {
        return ResolvedRoot::Source::ServerCwd;
    }
    std::optional<int> tabForCwd(const QString &) const override {
        return std::nullopt;
    }
};

}  // namespace ants
