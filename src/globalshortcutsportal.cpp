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
// 0.7.33: Session interface for the explicit Close call in the
// destructor. The session handle returned by CreateSession lives
// for the process lifetime of the D-Bus client unless we call
// org.freedesktop.portal.Session.Close on its object path — without
// the close, xdg-desktop-portal accumulates one orphan session per
// Ants invocation that crashed or was killed (visible via
// `busctl --user introspect org.freedesktop.portal.Desktop ...` on
// the long-running portal service).
constexpr const char *kSessionIface = "org.freedesktop.portal.Session";
}

// Custom D-Bus type: one entry in BindShortcuts' shortcuts array.
// Wire type is `(sa{sv})` — a string id followed by a dict of string
// keys to variant values (description, preferred_trigger).
struct PortalShortcut {
    QString id;
    QVariantMap properties;
};
Q_DECLARE_METATYPE(PortalShortcut)

QDBusArgument &operator<<(QDBusArgument &arg, const PortalShortcut &s) {
    arg.beginStructure();
    arg << s.id << s.properties;
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, PortalShortcut &s) {
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
    return iface->isServiceRegistered(QString::fromLatin1(kService)).value();
}

GlobalShortcutsPortal::GlobalShortcutsPortal(QObject *parent)
    : QObject(parent), m_bus(QDBusConnection::sessionBus()) {
    // Register the custom meta types used by BindShortcuts. Both calls
    // are idempotent — safe across multiple instances and safe against
    // re-registration by other code in the same process.
    qDBusRegisterMetaType<PortalShortcut>();
    qDBusRegisterMetaType<QList<PortalShortcut>>();
}

GlobalShortcutsPortal::~GlobalShortcutsPortal() {
    // 0.7.33: explicit Session.Close so xdg-desktop-portal releases
    // the session handle when Ants exits. Without this, the portal
    // service tracks one orphan session per Ants invocation that
    // crashed / was SIGKILLed / closed before the process unwound;
    // they're released only when xdg-desktop-portal itself restarts.
    // Pre-fix this leak was bounded but cosmetic — sessions don't
    // hold compositor state beyond the binds, which we tear down
    // when the bus connection drops. Still: a tidy shutdown is the
    // documented contract (xdg-desktop-portal-spec §
    // org.freedesktop.portal.Session).
    if (m_sessionHandle.isEmpty()) return;
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QString::fromLatin1(kService),
        m_sessionHandle,
        QString::fromLatin1(kSessionIface),
        QStringLiteral("Close"));
    // Fire-and-forget: we're in a destructor, can't wait for the
    // reply. The portal processes Close asynchronously; if we exit
    // before it completes, the kernel-level connection drop has the
    // same effect.
    m_bus.asyncCall(msg);
}

void GlobalShortcutsPortal::bindShortcut(const QString &id,
                                         const QString &description,
                                         const QString &preferredTrigger) {
    // ANTS-1152 — early-return on permanent failure. Without
    // this guard, a re-bind path (config reload, plugin reload)
    // would re-enter createSession()/BindShortcuts and re-trip
    // the same backend failure that caused the original
    // sessionFailed emission. The 0.7.67 ANTS-1142 H-1 fix
    // cleared `m_pending` + `m_sessionHandle` to make
    // sessionFailed terminal-per-process; this latch enforces
    // that contract at the entry point.
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
        m_permanentlyFailed = true;  // ANTS-1152
        emit sessionFailed(QStringLiteral(
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
    auto *watcher = new QDBusPendingCallWatcher(reply, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
        [this](QDBusPendingCallWatcher *w) {
            QDBusPendingReply<QDBusObjectPath> r = *w;
            w->deleteLater();
            if (r.isError()) {
                m_sessionPending = false;
                detachResponseSlots(m_createSessionReqPath);
                m_createSessionReqPath.clear();
                m_permanentlyFailed = true;  // ANTS-1152
                emit sessionFailed(r.error().message());
            }
            // Success path: wait for onCreateSessionResponse.
        });
}

void GlobalShortcutsPortal::onCreateSessionResponse(uint response,
                                                    const QVariantMap &results) {
    detachResponseSlots(m_createSessionReqPath);
    m_createSessionReqPath.clear();
    m_sessionPending = false;

    if (response != 0) {
        m_permanentlyFailed = true;  // ANTS-1152
        emit sessionFailed(QStringLiteral(
            "CreateSession rejected (response code %1)").arg(response));
        return;
    }
    if (!results.contains(QStringLiteral("session_handle"))) {
        m_permanentlyFailed = true;  // ANTS-1152
        emit sessionFailed(QStringLiteral(
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

    emit sessionReady();
    flushPending();
}

void GlobalShortcutsPortal::flushPending() {
    if (m_sessionHandle.isEmpty() || m_pending.isEmpty()) return;

    const QString handleToken = QStringLiteral("ants_bind_%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-'));
    m_bindShortcutsReqPath = predictRequestPath(handleToken);

    m_bus.connect(QString::fromLatin1(kService),
                  m_bindShortcutsReqPath,
                  QString::fromLatin1(kReqIface),
                  QStringLiteral("Response"),
                  this,
                  SLOT(onBindShortcutsResponse(uint, QVariantMap)));

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
    m_bus.asyncCall(msg);
    // BindShortcuts response arrives at onBindShortcutsResponse.
}

void GlobalShortcutsPortal::onBindShortcutsResponse(uint response,
                                                    const QVariantMap &results) {
    Q_UNUSED(results);
    detachResponseSlots(m_bindShortcutsReqPath);
    m_bindShortcutsReqPath.clear();
    if (response != 0) {
        // ANTS-1142 — drain m_pending and clear m_sessionHandle
        // on BindShortcuts failure. Pre-fix code left both
        // populated, so a subsequent bindShortcut() call would
        // see a non-empty session handle and re-trigger
        // BindShortcuts → re-fail forever (the failure-loop
        // pathology indie-review L11 HIGH-1 flagged on GNOME).
        // sessionFailed is now terminal-per-process: callers
        // should treat it as a permanent state and not retry.
        m_pending.clear();
        m_sessionHandle.clear();
        m_permanentlyFailed = true;  // ANTS-1152
        emit sessionFailed(QStringLiteral(
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
// Changing to const QString & would silently break the connect
// at globalshortcutsportal.cpp:219. Cppcheck false-positive
// class; suppress in-line.
// cppcheck-suppress passedByValue
void GlobalShortcutsPortal::onActivatedSignal(QDBusObjectPath sessionHandle,
                                              QString shortcutId,
                                              qulonglong timestamp,
                                              QVariantMap options) {
    Q_UNUSED(sessionHandle);
    Q_UNUSED(timestamp);
    Q_UNUSED(options);
    emit activated(shortcutId);
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
                     SLOT(onBindShortcutsResponse(uint, QVariantMap)));
}
