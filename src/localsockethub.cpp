#include "localsockethub.h"

#include "secureio.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>

#include <algorithm>

namespace ants {

namespace {

QPointer<LocalSocketHub> &hubSlot() {
    static QPointer<LocalSocketHub> hub;
    return hub;
}

}  // namespace

LocalSocketHub::LocalSocketHub(QObject *parent) : QObject(parent) {}

LocalSocketHub &LocalSocketHub::instance() {
    QPointer<LocalSocketHub> &hub = hubSlot();
    if (!hub) hub = new LocalSocketHub(QCoreApplication::instance());
    return *hub;
}

LocalSocketHub *LocalSocketHub::existing() {
    return hubSlot().data();
}

QLocalServer *LocalSocketHub::acquire(const QString &path) {
    const auto held = m_paths.constFind(path);
    if (held != m_paths.cend() && held->server) return held->server;

    // § 2.2 — settle what holds the path BEFORE listen. With a socket option
    // set, QLocalServer::listen binds in a private directory and renames the
    // socket over `path`, replacing whatever is there, a live server's socket
    // included; so listen never fails on a held path.
    struct stat st{};
    if (::lstat(QFile::encodeName(path).constData(), &st) == 0) {
        // ANTS-1132 — never unlink anything but a socket owned by this user.
        if (!safeToUnlinkLocalSocket(path)) return nullptr;
        // A socket that accepts a connection belongs to a live server, so
        // leave it alone. 200 ms is the author's setting, not measured: a
        // listening local socket accepts at once.
        QLocalSocket probe;
        probe.connectToServer(path);
        if (probe.waitForConnected(200)) {
            probe.disconnectFromServer();
            return nullptr;
        }
        QLocalServer::removeServer(path);
    }

    auto *server = new QLocalServer(this);
    // ANTS-1132 — UserAccessOption applies owner-only perms at the socket
    // layer before bind, closing the window before setOwnerOnlyPerms below.
    server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!server->listen(path)) {
        delete server;
        return nullptr;
    }
    setOwnerOnlyPerms(path);

    connect(server, &QLocalServer::newConnection, this,
            [this, path] { onNewConnection(path); });
    m_paths[path].server = server;
    return server;
}

void LocalSocketHub::attach(const QString &path, QObject *owner,
                            std::function<bool()> visible,
                            std::function<void()> serve,
                            std::function<bool(const QString &)> ownsSession,
                            std::function<void(const QJsonObject &)> onHookEvent) {
    if (!owner) return;
    detach(path, owner);
    Owner o;
    o.obj = owner;
    o.visible = std::move(visible);
    o.serve = std::move(serve);
    o.ownsSession = std::move(ownsSession);
    o.onHookEvent = std::move(onHookEvent);
    o.activatedAt = ++m_activations;
    o.onDestroyed = connect(owner, &QObject::destroyed, this,
                            [this, path, owner] { detach(path, owner); });
    m_paths[path].owners.append(std::move(o));
}

void LocalSocketHub::detach(const QString &path, QObject *owner) {
    const auto it = m_paths.find(path);
    if (it == m_paths.end()) return;
    // A destroyed owner's pointer already reads null, so drop those too.
    it->owners.removeIf([owner](const Owner &o) {
        const bool drop = o.obj.isNull() || o.obj == owner;
        if (drop) QObject::disconnect(o.onDestroyed);
        return drop;
    });
}

void LocalSocketHub::deliverHookEvent(const QString &path,
                                      const QJsonObject &event) {
    const auto it = m_paths.constFind(path);
    if (it == m_paths.cend()) return;
    const QString sessionId =
        event.value(QStringLiteral("session_id")).toString();
    const Owner *chosen = nullptr;
    if (!sessionId.isEmpty()) {
        for (const Owner &o : it->owners) {
            if (o.obj.isNull() || !o.ownsSession || !o.ownsSession(sessionId))
                continue;
            if (!chosen || o.activatedAt > chosen->activatedAt) chosen = &o;
        }
    }
    if (!chosen) chosen = target(*it);
    if (!chosen || !chosen->onHookEvent) return;
    // Copied: processing the event may re-enter the hub.
    const auto onHookEvent = chosen->onHookEvent;
    onHookEvent(event);
}

void LocalSocketHub::noteActivated(QObject *owner) {
    if (!owner) return;
    const quint64 now = ++m_activations;
    for (Path &p : m_paths)
        for (Owner &o : p.owners)
            if (o.obj == owner) o.activatedAt = now;
}

QList<QObject *> LocalSocketHub::owners(const QString &path) const {
    QList<const Owner *> attached;
    const auto it = m_paths.constFind(path);
    if (it != m_paths.cend())
        for (const Owner &o : it->owners)
            if (!o.obj.isNull()) attached.append(&o);
    std::sort(attached.begin(), attached.end(),
              [](const Owner *a, const Owner *b) {
                  return a->activatedAt > b->activatedAt;
              });
    QList<QObject *> out;
    for (const Owner *o : attached) out.append(o->obj.data());
    return out;
}

void LocalSocketHub::onNewConnection(const QString &path) {
    const auto it = m_paths.constFind(path);
    if (it == m_paths.cend()) return;
    const Owner *owner = target(*it);
    if (!owner) {
        closeUnowned(it->server);
        return;
    }
    // Copied: serving may re-enter the hub.
    const auto serve = owner->serve;
    serve();
}

const LocalSocketHub::Owner *LocalSocketHub::target(const Path &p) {
    const Owner *newest = nullptr;
    const Owner *newestVisible = nullptr;
    for (const Owner &o : p.owners) {
        if (o.obj.isNull()) continue;
        if (!newest || o.activatedAt > newest->activatedAt) newest = &o;
        const bool visible = !o.visible || o.visible();
        if (visible && (!newestVisible || o.activatedAt > newestVisible->activatedAt))
            newestVisible = &o;
    }
    return newestVisible ? newestVisible : newest;
}

void LocalSocketHub::closeUnowned(QLocalServer *server) {
    while (server->hasPendingConnections()) {
        QLocalSocket *socket = server->nextPendingConnection();
        socket->disconnectFromServer();
        socket->deleteLater();
    }
}

}  // namespace ants
