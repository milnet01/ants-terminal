#pragma once

// Freedesktop Portal GlobalShortcuts client (0.6.39).
//
// Wraps the `org.freedesktop.portal.GlobalShortcuts` D-Bus interface so
// Ants can register a true out-of-focus global hotkey on Wayland
// (replacing the KGlobalAccel + GNOME-settings-daemon path the original
// roadmap called for with one compositor-agnostic portal API — the same
// approach the upstream Wayland ecosystem has converged on since 2023).
//
// Responsibility split:
//   - GlobalShortcutsPortal owns the D-Bus handshake: CreateSession →
//     session_handle → BindShortcuts → Activated signal pump.
//   - MainWindow owns policy: which shortcut id maps to which action,
//     debouncing against the in-app QShortcut when both fire.
//
// Availability matrix as of 2026-04:
//   - xdg-desktop-portal-kde    : ✓ (Plasma 6 routes to KGlobalAccel)
//   - xdg-desktop-portal-hyprland : ✓ (built-in GlobalShortcuts impl)
//   - xdg-desktop-portal-wlr    : ✓ (sway, river, ...)
//   - xdg-desktop-portal-gnome  : ✗ (not implemented yet; CreateSession
//     succeeds but BindShortcuts fails — we emit sessionFailed and
//     MainWindow falls back to the in-app QShortcut from 0.6.38)
//   - X11 sessions              : the portal service is typically still
//     running (xdg-desktop-portal is session-bus, not compositor-tied)
//     but global hotkey semantics on X11 are conventionally handled by
//     the desktop's own keybinding manager (KWin keyboard shortcuts,
//     sxhkd, etc.) pointing at `ants-terminal --dropdown`; users who
//     want an in-process path get the QShortcut fallback.

#include <QObject>
#include <QDBusConnection>
#include <QDBusObjectPath>
#include <QList>
#include <QString>
#include <QVariantMap>

class GlobalShortcutsPortal : public QObject {
    Q_OBJECT
public:
    explicit GlobalShortcutsPortal(QObject *parent = nullptr);
    ~GlobalShortcutsPortal() override;

    // Returns true iff the xdg-desktop-portal service is currently
    // registered on the session bus. Cheap synchronous query — safe to
    // call from a constructor. Does NOT verify the GlobalShortcuts
    // interface is actually implemented by the running portal backend;
    // that can only be detected by dispatching CreateSession and
    // waiting for the Response (see sessionReady / sessionFailed).
    static bool isAvailable();

    // Register (or replace) a single shortcut. Idempotent per id. The
    // actual BindShortcuts D-Bus call is deferred until the session
    // handshake completes; calls made before the session is ready are
    // queued and flushed from onCreateSessionResponse.
    //
    // preferredTrigger uses the freedesktop shortcut syntax
    // ("CTRL+ALT+T", "F12", "LOGO+grave", …). An empty string omits
    // the preferred_trigger property, in which case the compositor
    // presents a configuration UI on first bind (KDE) or accepts the
    // bind with no default (portal behaviour is backend-defined).
    void bindShortcut(const QString &id,
                      const QString &description,
                      const QString &preferredTrigger);

signals:
    // Fired whenever the portal reports a bound shortcut has been
    // triggered. shortcutId matches the id passed to bindShortcut().
    void activated(const QString &shortcutId);

    // Fired once after the CreateSession + BindShortcuts handshake
    // completes. MainWindow uses this to disable the in-app QShortcut
    // fallback (or to keep it for defence-in-depth and debounce —
    // current policy is the latter; see mainwindow.cpp).
    void sessionReady();

    // Fired if any step of the handshake fails. reason is the portal
    // error message (or a synthesised reason string for invariant
    // violations like missing session_handle in the response).
    void sessionFailed(const QString &reason);

private slots:
    // D-Bus signal handlers. Must be slots (not lambdas) because
    // QDBusConnection::connect() takes a SLOT()-string target.
    void onCreateSessionResponse(uint response, const QVariantMap &results);
    void onBindShortcutsResponse(uint response, const QVariantMap &results);
    void onActivatedSignal(QDBusObjectPath sessionHandle,
                           QString shortcutId,
                           qulonglong timestamp,
                           QVariantMap options);

private:
    struct PendingBind {
        QString id;
        QString description;
        QString trigger;
    };

    void createSession();
    void flushPending();
    QString predictRequestPath(const QString &handleToken) const;
    void detachResponseSlots(const QString &requestPath);

    QDBusConnection m_bus;
    QString m_sessionHandle;                // empty until the handshake completes
    QString m_createSessionReqPath;          // per-request, reset on completion
    QString m_bindShortcutsReqPath;
    bool m_sessionPending = false;           // CreateSession in flight
    QList<PendingBind> m_pending;
};
