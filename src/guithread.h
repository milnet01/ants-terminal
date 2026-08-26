#pragma once

// ANTS-2132 — marshal a MainWindow read onto the GUI thread.
//
// Once MCP verbs dispatch off the GUI thread, a verb body that reaches into
// MainWindow is racing the widget it reads. Before ANTS-2132 that could not
// happen: rcDelegateWorker joined, so the GUI thread was parked in
// QThread::wait() for the whole call and could not touch the same widgets.
// Dropping the join drops that guarantee, so every such read is routed here.
//
// See docs/specs/ANTS-2132-async-mcp-dispatch.md § 2.5.

#include <QCoreApplication>
#include <QMetaObject>
#include <QObject>
#include <QThread>

#include <atomic>
#include <optional>
#include <type_traits>
#include <utility>

namespace ants {

// Set by ~ClaudeIntegration before it joins the dispatch worker. A worker
// parked in a BlockingQueuedConnection while the GUI thread waits in that join
// would deadlock, so teardown refuses new marshals first and only then joins.
// An inline variable rather than a .cpp symbol: the readers live in core_lib
// and the writer in the Claude layer, and one definition keeps that link
// direction from mattering.
inline std::atomic<bool> g_guiMarshalRefused{false};

inline void setGuiMarshalRefused(bool refused) {
    g_guiMarshalRefused.store(refused, std::memory_order_release);
}

inline bool guiMarshalRefused() {
    return g_guiMarshalRefused.load(std::memory_order_acquire);
}

// Runs `f` on the GUI thread and returns its result.
//
// std::nullopt means the read did NOT happen — the dispatcher is shutting
// down, or there is no application object. A caller must refuse with its own
// anchor-failure code on nullopt and must NOT fall back to a default-
// constructed value: an empty project root is a silently wrong answer, which
// is worse than a refusal the caller can see.
//
// Cannot deadlock during dispatch, because the GUI thread never waits on the
// worker while serving a request (spec § 2.1); the one join is at teardown and
// is guarded by the flag above.
template <class F>
auto onGuiThread(F &&f) -> std::optional<std::invoke_result_t<F>> {
    using R = std::invoke_result_t<F>;
    static_assert(!std::is_void_v<R>,
                  "onGuiThread needs a value to return; have the callable "
                  "return something observable so the caller can tell a "
                  "refused marshal from a completed one");

    QObject *app = QCoreApplication::instance();
    if (!app) return std::nullopt;

    // Already there: call directly. Keeps every non-MCP caller (the --remote
    // CLI path, the e2e harness) free of queued-invocation cost, and keeps
    // this usable from code that does not know which thread it is on.
    if (QThread::currentThread() == app->thread())
        return std::optional<R>(std::forward<F>(f)());

    if (guiMarshalRefused()) return std::nullopt;

    std::optional<R> out;
    QMetaObject::invokeMethod(
        app, [&out, &f]() { out.emplace(f()); }, Qt::BlockingQueuedConnection);
    return out;
}

}  // namespace ants
