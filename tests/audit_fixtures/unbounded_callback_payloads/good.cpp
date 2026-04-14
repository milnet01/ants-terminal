// Safe patterns — MUST NOT match the rule's literal regex. The audit-dialog
// runtime filter additionally drops lines with `.left(`, `.truncate(`,
// `.mid(`, `.chopped(`, `.chop(` even when the raw regex matches — but this
// fixture tests the regex arm, so we route every Utf8 conversion through a
// pre-bounded local (or use a trivial constant) to avoid the literal entirely.
//
// Comments deliberately avoid the dangerous form (`Callback (` / `fromUtf8 (`
// gap with c-underscore-str on the same comment line) so the grep doesn't
// flag the prose.

#include <QString>
#include <functional>
#include <string>

using NotifyFn = std::function<void(const QString&, const QString&)>;
using StreamFn = std::function<void(const QString&)>;

struct Bridge {
    NotifyFn m_notifyCallback;
    StreamFn m_responseCallback;
};

void osc9_safe(Bridge &b, const std::string &payload, std::size_t semi) {
    constexpr int kMaxNotifyBody = 1024;
    QString body = QString::fromUtf8(payload.c_str() + semi + 1).left(kMaxNotifyBody);
    b.m_notifyCallback(QString(), body);
}

void streaming_safe(Bridge &b, const std::string &chunk) {
    QString text = QString::fromUtf8(chunk.c_str()).left(8192);
    b.m_responseCallback(text);
}

void constant_payload(Bridge &b) {
    b.m_notifyCallback(QString(), QString("hello"));
}

void utf8_outside_callback(const std::string &payload) {
    QString title = QString::fromUtf8(payload.c_str());
    (void)title;
}
