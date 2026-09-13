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
#include <QMutex>
#include <QMutexLocker>
#include <QObject>
#include <QSet>
#include <QThread>

#include <optional>
#include <type_traits>
#include <utility>

namespace ants {

// The threads whose marshals are refused. ~ClaudeIntegration adds its own
// dispatch worker before joining it: a worker parked in a
// BlockingQueuedConnection while the GUI thread waits in that join would
// deadlock, so teardown refuses that worker's marshals first and only then
// joins. Keyed by thread, not one flag, so closing a second window refuses
// nothing for the first window's worker (ANTS-5142, ANTS-2132 spec INV-17).
// Inline variables rather than .cpp symbols: the readers live in core_lib and
// the writer in the Claude layer, and one definition keeps that link direction
// from mattering.
inline QMutex g_guiMarshalRefusedMutex;
inline QSet<const QThread *> g_guiMarshalRefusedThreads;

inline void setGuiMarshalRefused(const QThread *caller, bool refused) {
    const QMutexLocker lock(&g_guiMarshalRefusedMutex);
    if (refused)
        g_guiMarshalRefusedThreads.insert(caller);
    else
        g_guiMarshalRefusedThreads.remove(caller);
}

inline bool guiMarshalRefused(const QThread *caller) {
    const QMutexLocker lock(&g_guiMarshalRefusedMutex);
    return g_guiMarshalRefusedThreads.contains(caller);
}

// Runs `f` on the GUI thread and returns its result.
//
// std::nullopt means the read did NOT happen — the dispatcher is shutting
// down, or there is no application object. A caller must refuse with its own
// anchor-failure code on nullopt and must NOT fall back to a default-
// constructed value: an empty project root is a silently wrong answer, which
// is worse than a refusal the caller can see.
//
// Never call it from a thread the GUI thread is blocked joining: the queued
// call is never served and both threads hang (ANTS-5024). The ANTS-2132
// dispatch worker is safe while it serves requests, because the GUI thread
// never waits on it then (spec § 2.1). A verb that runs its own worker and
// joins it on the GUI thread is not covered by that argument. The dispatch
// worker's one join is at teardown, through joinRefusingMarshals below, which
// releases a marshal already parked when the flag was set (ANTS-5113).
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

    const QThread *const caller = QThread::currentThread();
    if (guiMarshalRefused(caller)) return std::nullopt;

    // ANTS-5113 — a marshal delivered after its thread is refused must not run
    // `f`: at teardown what it reads is being destroyed. `out` stays empty, so
    // the caller sees a refusal.
    std::optional<R> out;
    QMetaObject::invokeMethod(
        app,
        [&out, &f, caller]() {
            if (!guiMarshalRefused(caller)) out.emplace(f());
        },
        Qt::BlockingQueuedConnection);
    return out;
}

// ANTS-5113 — join `worker` from the GUI thread at teardown. A worker already
// parked in onGuiThread is waiting for this thread, so a bare wait() never
// returns. Between short waits, deliver the posted marshals: with `worker`
// refused, its own release it without running their callables. A marshal
// from any other thread is served normally. Only onGuiThread queues calls on
// the application object (src/ surveyed 2026-09-11); anything queued there
// later would run here too. Returns false when the worker has not exited
// within `timeoutMs`; negative waits forever.
inline bool joinRefusingMarshals(QThread *worker, int timeoutMs = -1) {
    constexpr int kSliceMs = 20;
    const QDeadlineTimer deadline(timeoutMs);
    QObject *app = QCoreApplication::instance();
    while (!worker->wait(QDeadlineTimer(kSliceMs))) {
        if (app) QCoreApplication::sendPostedEvents(app, QEvent::MetaCall);
        if (deadline.hasExpired()) return false;
    }
    return true;
}

}  // namespace ants
