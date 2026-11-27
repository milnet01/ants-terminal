#include "globalshortcutsportal.h"

#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QMetaType>
#include <QUuid>

// Portal constants. Service + path are the shared xdg-desktop-portal
// endpoint; interface + request-interface names are per-method.
namespace {
constexpr const char *kService     = "org.freedesktop.portal.Desktop";
constexpr const char *kPath        = "/org/freedesktop/portal/desktop";
constexpr const char *kIface       = "org.freedesktop.portal.GlobalShortcuts";
constexpr const char *kReqIface    = "org.freedesktop.portal.Request";
// Session interface for the explicit Close call in the destructor.
constexpr const char *kSessionIface = "org.freedesktop.portal.Session";
}

// Custom D-Bus type: one entry in BindShortcuts' shortcuts array.
// Wire type is `(sa{sv})` — a string id followed by a dict of string
// keys to variant values (description, preferred_trigger).
namespace {
struct PortalShortcut {
    QString id;
    QVariantMap properties;
};
}  // namespace
Q_DECLARE_METATYPE(PortalShortcut)

static QDBusArgument &operator<<(QDBusArgument &arg, const PortalShortcut &s) {
    arg.beginStructure();
    arg << s.id << s.properties;
    arg.endStructure();
    return arg;
}

static const QDBusArgument &operator>>(const QDBusArgument &arg, PortalShortcut &s) {
    arg.beginStructure();
    arg >> s.id >> s.properties;
    arg.endStructure();
    return arg;
}

bool GlobalShortcutsPortal::isAvailable() {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) return false;
    auto *iface = bus.interface();
    if (!iface) return false;
    // isServiceRegistered returns a QDBusReply<bool>; .value() returns
    // false on D-Bus error which is the right default for "unavailable".
    const QString service = QString::fromLatin1(kService);
    if (iface->isServiceRegistered(service).value()) return true;
    // ANTS-5081 — an on-demand (D-Bus activatable) portal is not registered
    // until something calls it, so accept an activatable name too;
    // CreateSession then starts it.
    return iface->activatableServiceNames().value().contains(service);
}

GlobalShortcutsPortal::GlobalShortcutsPortal(QObject *parent)
    : QObject(parent), m_bus(QDBusConnection::sessionBus()) {
    // Register the custom meta types used by BindShortcuts. Both calls
    // are idempotent — safe across multiple instances and safe against
    // re-registration by other code in the same process.
    qDBusRegisterMetaType<PortalShortcut>();
    qDBusRegisterMetaType<QList<PortalShortcut>>();

    // ANTS-5081 — fail a CreateSession whose Response never arrives, the same
    // way a rejected one fails (ANTS-1152: permanent for the process).
    m_createSessionTimer.setSingleShot(true);
    m_createSessionTimer.setInterval(kCreateSessionTimeoutMs);
    connect(&m_createSessionTimer, &QTimer::timeout, this, [this] {
        if (!m_sessionPending) return;
        m_sessionPending = false;
        detachResponseSlots(m_createSessionReqPath);
        m_createSessionReqPath.clear();
        failPermanently(QStringLiteral(
            "CreateSession got no response within %1 s")
            .arg(kCreateSessionTimeoutMs / 1000));
    });
}

GlobalShortcutsPortal::~GlobalShortcutsPortal() {
    // Explicit Session.Close so xdg-desktop-portal releases the session
    // handle on a clean exit (xdg-desktop-portal-spec §
    // org.freedesktop.portal.Session). A crash or SIGKILL never reaches
    // this destructor.
    if (m_sessionHandle.isEmpty()) return;
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QString::fromLatin1(kService),
        m_sessionHandle,
        QString::fromLatin1(kSessionIface),
        QStringLiteral("Close"));
    // Fire-and-forget: we're in a destructor, can't wait for the
    // reply. The portal processes Close asynchronously.
    m_bus.asyncCall(msg);
}

void GlobalShortcutsPortal::bindShortcut(const QString &id,
                                         const QString &description,
                                         const QString &preferredTrigger) {
    // ANTS-1152 — early-return on permanent failure. Without
    // this guard, a re-bind path (config reload, plugin reload)
    // would re-enter createSession()/BindShortcuts and re-trip
    // the same backend failure that caused the original
    // sessionFailed emission. failPermanently() sets the latch;
    // this check enforces it at the entry point.
    if (m_permanentlyFailed) {
        emit sessionFailed(QStringLiteral(
            "GlobalShortcutsPortal session permanently failed earlier "
            "this process — re-binds suppressed"));
        return;
    }
    // Replace any existing pending entry with the same id — the public
    // contract is idempotent-per-id.
    for (int i = 0; i < m_pending.size(); ++i) {
        if (m_pending[i].id == id) {
            m_pending.removeAt(i);
            break;
        }
    }
    m_pending.append({id, description, preferredTrigger});

    if (!isAvailable()) {
        failPermanently(QStringLiteral(
            "xdg-desktop-portal service is not registered on the session bus"));
        return;
    }
    if (!m_sessionHandle.isEmpty()) {
        flushPending();
    } else if (!m_sessionPending) {
        createSession();
    }
    // else: CreateSession is in flight — flushPending() will fire from
    //       onCreateSessionResponse once the session handle arrives.
}

QString GlobalShortcutsPortal::predictRequestPath(const QString &handleToken) const {
    // xdg-desktop-portal uses a deterministic request path so clients
    // can install the Response match rule BEFORE dispatching the
    // method call — without this, the portal may emit Response before
    // Qt has routed the match to our slot, and the event is lost.
    //
    // Format per spec:
    //   /org/freedesktop/portal/desktop/request/SENDER_SAN/HANDLE_TOKEN
    // SENDER_SAN = unique name with leading ':' removed and every '.'
    // replaced by '_'. Example: ":1.42" -> "1_42".
    QString sender = m_bus.baseService();
    if (sender.startsWith(QLatin1Char(':'))) sender.remove(0, 1);
    sender.replace(QLatin1Char('.'), QLatin1Char('_'));
    return QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2")
        .arg(sender, handleToken);
}

void GlobalShortcutsPortal::createSession() {
    m_sessionPending = true;

    // Tokens are opaque client-chosen strings. They must be valid D-Bus
    // object-path components (alphanumeric + underscore), so strip the
    // braces/dashes from a QUuid.
    const QString handleToken = QStringLiteral("ants_req_%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-'));
    const QString sessionHandleToken = QStringLiteral("ants_sess_%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-'));

    m_createSessionReqPath = predictRequestPath(handleToken);

    // Install the Response match rule BEFORE dispatching the call. See
    // predictRequestPath() for why. Path + interface + member make the
    // match unique enough that the two Response slots (CreateSession,
    // BindShortcuts) don't cross-fire.
    m_bus.connect(QString::fromLatin1(kService),
                  m_createSessionReqPath,
                  QString::fromLatin1(kReqIface),
                  QStringLiteral("Response"),
                  this,
                  SLOT(onCreateSessionResponse(uint, QVariantMap)));

    QVariantMap opts;
    opts[QStringLiteral("handle_token")] = handleToken;
    opts[QStringLiteral("session_handle_token")] = sessionHandleToken;

    QDBusMessage msg = QDBusMessage::createMethodCall(
        QString::fromLatin1(kService),
        QString::fromLatin1(kPath),
        QString::fromLatin1(kIface),
        QStringLiteral("CreateSession"));
    msg << opts;

    QDBusPendingReply<QDBusObjectPath> reply = m_bus.asyncCall(msg);
    m_createSessionTimer.start();
    auto *watcher = new QDBusPendingCallWatcher(reply, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
        [this](QDBusPendingCallWatcher *w) {
            QDBusPendingReply<QDBusObjectPath> r = *w;
            w->deleteLater();
            // After a timeout the session already failed; don't fail it twice.
            if (r.isError() && m_sessionPending) {
                m_createSessionTimer.stop();
                m_sessionPending = false;
                detachResponseSlots(m_createSessionReqPath);
                m_createSessionReqPath.clear();
                failPermanently(r.error().message());
            }
            // Success path: wait for onCreateSessionResponse.
        });
}

void GlobalShortcutsPortal::onCreateSessionResponse(uint response,
                                                    const QVariantMap &results) {
    m_createSessionTimer.stop();
    detachResponseSlots(m_createSessionReqPath);
    m_createSessionReqPath.clear();
    m_sessionPending = false;

    if (response != 0) {
        failPermanently(QStringLiteral(
            "CreateSession rejected (response code %1)").arg(response));
        return;
    }
    if (!results.contains(QStringLiteral("session_handle"))) {
        failPermanently(QStringLiteral(
            "CreateSession response missing session_handle"));
        return;
    }
    m_sessionHandle = results.value(QStringLiteral("session_handle")).toString();

    // Install the Activated match on the session object path. The
    // portal's contract is that Activated / Deactivated fire on the
    // session path (not the portal path) for the lifetime of the
    // session. Session lives until the client exits or explicitly
    // calls org.freedesktop.portal.Session::Close.
    m_bus.connect(QString::fromLatin1(kService),
                  m_sessionHandle,
                  QString::fromLatin1(kIface),
                  QStringLiteral("Activated"),
                  this,
                  SLOT(onActivatedSignal(QDBusObjectPath, QString,
                                         qulonglong, QVariantMap)));

    // ANTS-5081 — send BindShortcuts before announcing the session is ready,
    // so a listener never sees "ready" with nothing bound yet.
    flushPending();
    emit sessionReady();
}

void GlobalShortcutsPortal::flushPending() {
    if (m_sessionHandle.isEmpty() || m_pending.isEmpty()) return;

    const QString handleToken = QStringLiteral("ants_bind_%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-'));
    // ANTS-5081 — a local, not a member: binds can overlap, and each
    // Response detaches its own path from the message.
    const QString reqPath = predictRequestPath(handleToken);

    m_bus.connect(QString::fromLatin1(kService),
                  reqPath,
                  QString::fromLatin1(kReqIface),
                  QStringLiteral("Response"),
                  this,
                  SLOT(onBindShortcutsResponse(uint, QVariantMap, QDBusMessage)));
    m_bindsInFlight.insert(reqPath);

    QList<PortalShortcut> shortcuts;
    shortcuts.reserve(m_pending.size());
    for (const PendingBind &pb : m_pending) {
        PortalShortcut s;
        s.id = pb.id;
        s.properties[QStringLiteral("description")] = pb.description;
        if (!pb.trigger.isEmpty()) {
            s.properties[QStringLiteral("preferred_trigger")] = pb.trigger;
        }
        shortcuts.append(s);
    }
    m_pending.clear();

    QVariantMap opts;
    opts[QStringLiteral("handle_token")] = handleToken;

    QDBusMessage msg = QDBusMessage::createMethodCall(
        QString::fromLatin1(kService),
        QString::fromLatin1(kPath),
        QString::fromLatin1(kIface),
        QStringLiteral("BindShortcuts"));
    msg << QVariant::fromValue(QDBusObjectPath(m_sessionHandle))
        << QVariant::fromValue(shortcuts)
        << QString()   // parent_window — portal prompt has no parent window
        << opts;
    // BindShortcuts response arrives at onBindShortcutsResponse. ANTS-5081 —
    // an error reply never reaches it, so watch the call the way
    // CreateSession's is watched and fail the same way.
    QDBusPendingReply<QDBusObjectPath> reply = m_bus.asyncCall(msg);
    auto *watcher = new QDBusPendingCallWatcher(reply, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
        [this, reqPath](QDBusPendingCallWatcher *w) {
            QDBusPendingReply<QDBusObjectPath> r = *w;
            w->deleteLater();
            if (!r.isError()) return;  // success: wait for onBindShortcutsResponse
            // After a timeout the bind already failed; don't fail it twice.
            if (!m_bindsInFlight.remove(reqPath)) return;
            detachResponseSlots(reqPath);
            failPermanently(r.error().message());
        });
    // A Response that never arrives fails the bind, as CreateSession's does.
    QTimer::singleShot(kBindShortcutsTimeoutMs, this, [this, reqPath] {
        if (!m_bindsInFlight.remove(reqPath)) return;
        detachResponseSlots(reqPath);
        failPermanently(QStringLiteral("BindShortcuts got no response within %1 s")
                            .arg(kBindShortcutsTimeoutMs / 1000));
    });
}

void GlobalShortcutsPortal::onBindShortcutsResponse(uint response,
                                                    const QVariantMap &results,
                                                    const QDBusMessage &message) {
    Q_UNUSED(results);
    m_bindsInFlight.remove(message.path());
    detachResponseSlots(message.path());
    if (response != 0) {
        failPermanently(QStringLiteral(
            "BindShortcuts rejected (response code %1)").arg(response));
        return;
    }
    // Success: Activated signals start flowing whenever any bound
    // shortcut fires. Nothing more to do here.
}

// ANTS-1142 audit-fold-in: cppcheck flagged shortcutId as
// passedByValue. Reverted — QString is copy-on-write (atomic
// refcount bump on copy), and Qt's old-style SLOT() macro
// signature-matches by exact type string at connect time.
// Changing to const QString & would silently break the Activated
// connect in onCreateSessionResponse. Cppcheck false-positive
// class; suppress in-line. The suppression must SPAN the signature
// (ANTS-3850): an inline `cppcheck-suppress` covers only the next line, so
// the single-line form silenced the first parameter and left shortcutId --
// the one this comment is actually about -- still reported.
// cppcheck-suppress-begin passedByValue
void GlobalShortcutsPortal::onActivatedSignal(const QDBusObjectPath &sessionHandle,
                                              const QString &shortcutId,
                                              qulonglong timestamp,
                                              const QVariantMap &options) {
    // cppcheck-suppress-end passedByValue
    Q_UNUSED(sessionHandle);
    Q_UNUSED(timestamp);
    Q_UNUSED(options);
    emit activated(shortcutId);
}

// ANTS-6190 — every failure gives up for the rest of the process (ANTS-1152):
// the latch stops a later bindShortcut() re-entering the failing handshake
// (ANTS-1142's loop), and the queued binds are dropped since none will be
// sent. The session handle is kept, so the destructor still closes a session
// that was created. A caller with a request in flight detaches its path first.
void GlobalShortcutsPortal::failPermanently(const QString &reason) {
    m_pending.clear();
    m_permanentlyFailed = true;  // ANTS-1152
    emit sessionFailed(reason);
}

void GlobalShortcutsPortal::detachResponseSlots(const QString &requestPath) {
    if (requestPath.isEmpty()) return;
    // We don't know which slot (CreateSession vs BindShortcuts) was
    // routed to this path; disconnect both. Misses are silent no-ops.
    m_bus.disconnect(QString::fromLatin1(kService),
                     requestPath,
                     QString::fromLatin1(kReqIface),
                     QStringLiteral("Response"),
                     this,
                     SLOT(onCreateSessionResponse(uint, QVariantMap)));
    m_bus.disconnect(QString::fromLatin1(kService),
                     requestPath,
                     QString::fromLatin1(kReqIface),
                     QStringLiteral("Response"),
                     this,
                     SLOT(onBindShortcutsResponse(uint, QVariantMap, QDBusMessage)));
}
