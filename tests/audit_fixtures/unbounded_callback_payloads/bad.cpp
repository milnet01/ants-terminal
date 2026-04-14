// Fixture for unbounded_callback_payloads. Pattern: a `*Callback(...)` call
// that forwards `QString::fromUtf8(<expr>.c_str()…)` directly without a
// `.left()` / `.truncate()` cap. Each @expect marker is one such call. The
// good-side shows the safe shape (length-cap before forward, or use of an
// already-bounded local).

#include <QString>
#include <functional>
#include <string>

using NotifyFn = std::function<void(const QString&, const QString&)>;
using StreamFn = std::function<void(const QString&)>;

struct Bridge {
    NotifyFn m_notifyCallback;
    StreamFn m_responseCallback;
    StreamFn errorCallback;
};

void osc9_unsafe(Bridge &b, const std::string &payload, std::size_t semi) {
    // Whole OSC payload after the first `;` shovelled into the desktop
    // notifier — no bound, attacker-controlled length.
    b.m_notifyCallback(QString(), QString::fromUtf8(payload.c_str() + semi + 1));  // @expect unbounded_callback_payloads
}

void streaming_unsafe(Bridge &b, const std::string &chunk) {
    b.m_responseCallback(QString::fromUtf8(chunk.c_str()));  // @expect unbounded_callback_payloads
}

void error_unsafe(Bridge &b, const std::string &errBody) {
    b.errorCallback(QString::fromUtf8(errBody.c_str() + 4));  // @expect unbounded_callback_payloads
}
