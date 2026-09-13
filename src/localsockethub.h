#pragma once

// ANTS-5144 — one QLocalServer per socket path for the whole process, shared
// by every window's ClaudeIntegration and RemoteControl. An owner attaches to
// a path; the hub picks which attached owner serves each connection, so no
// window holds the listener and closing one never takes it from the others.
// See docs/specs/ANTS-5144-shared-socket-listener.md.

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>

class QJsonObject;
class QLocalServer;

namespace ants {

class LocalSocketHub : public QObject {
public:
    // The process's hub, created on first use as a child of QCoreApplication.
    static LocalSocketHub &instance();
    // The hub if one exists, else nullptr — for teardown paths that must not
    // create one.
    static LocalSocketHub *existing();

    // The server listening on `path`, binding it on first use (§ 2.2).
    // nullptr when a server outside this process holds the path, or when
    // listen fails.
    QLocalServer *acquire(const QString &path);

    // Register `owner` as a target for connections on `path`. `serve` pulls
    // and serves the pending connections; `visible` says whether the owner's
    // window is on screen. On the hook path, `ownsSession` says whether the
    // owner's window tracks a session and `onHookEvent` processes an event
    // (§ 2.4); elsewhere both stay empty. Counts as an activation. Detaches
    // automatically when `owner` is destroyed.
    void attach(const QString &path, QObject *owner,
                std::function<bool()> visible, std::function<void()> serve,
                std::function<bool(const QString &)> ownsSession = {},
                std::function<void(const QJsonObject &)> onHookEvent = {});
    // Stop serving `owner` on `path`. The server stays: other owners may
    // still be attached.
    void detach(const QString &path, QObject *owner);

    // Run `onHookEvent` for exactly one owner on `path` (§ 2.4).
    void deliverHookEvent(const QString &path, const QJsonObject &event);

    // `owner`'s window became active (§ 2.3).
    void noteActivated(QObject *owner);

    // Every owner attached to `path`, in activation order, newest first.
    QList<QObject *> owners(const QString &path) const;

private:
    explicit LocalSocketHub(QObject *parent);

    struct Owner {
        QPointer<QObject> obj;
        std::function<bool()> visible;
        std::function<void()> serve;
        std::function<bool(const QString &)> ownsSession;
        std::function<void(const QJsonObject &)> onHookEvent;
        quint64 activatedAt = 0;
        QMetaObject::Connection onDestroyed;
    };
    struct Path {
        QLocalServer *server = nullptr;
        QList<Owner> owners;
    };

    void onNewConnection(const QString &path);
    // § 2.3 — the most recently activated visible owner, else the most
    // recently activated one; nullptr when none is attached.
    static const Owner *target(const Path &p);
    // § 2.5 — no owner: accept each pending connection and close it unanswered.
    static void closeUnowned(QLocalServer *server);

    QHash<QString, Path> m_paths;
    quint64 m_activations = 0;
};

}  // namespace ants
