// ANTS-1677 mainwindow piece 6/8 — quake mode
#include "mainwindow.h"
#include "mainwindow_internal.h"
#include "terminalwidget.h"
#include "globalshortcutsportal.h"
#include <QDateTime>
#include <QScreen>
#include <QPropertyAnimation>
#include <QEasingCurve>
#include <QGuiApplication>
#include <QWindow>
#ifdef ANTS_WAYLAND_LAYER_SHELL
#include <LayerShellQt/Window>
#endif

using namespace mainwindowdetail;

// --- Quake mode ---

namespace mainwindowdetail {
// Translate Qt's QKeySequence string form ("Ctrl+Shift+F12", "F12",
// "Ctrl+Alt+`") to the freedesktop shortcut syntax accepted by the
// GlobalShortcuts portal ("CTRL+SHIFT+F12", "F12", "CTRL+ALT+grave").
// Modifier names uppercase (with Meta→LOGO); keys pass through with a
// handful of common punctuation → xkb-keysym translations. Unmapped
// keys pass through unchanged — at worst the portal rejects the
// preferred_trigger, in which case the binding still succeeds with no
// default and the user adjusts in System Settings. Kept deliberately
// minimal; full keysym coverage is xkbcommon's job, not ours.
QString qtKeySequenceToPortalTrigger(const QString &qtHotkey) {
    if (qtHotkey.isEmpty()) return {};
    const QStringList parts = qtHotkey.split(QLatin1Char('+'), Qt::SkipEmptyParts);
    QStringList out;
    out.reserve(parts.size());
    for (const QString &raw : parts) {
        const QString upper = raw.toUpper();
        if (upper == QLatin1String("CTRL") ||
            upper == QLatin1String("ALT") ||
            upper == QLatin1String("SHIFT")) {
            out << upper;
        } else if (upper == QLatin1String("META") ||
                   upper == QLatin1String("WIN") ||
                   upper == QLatin1String("SUPER")) {
            out << QStringLiteral("LOGO");
        } else if (raw == QLatin1String("`")) {
            out << QStringLiteral("grave");
        } else if (raw == QLatin1String("'")) {
            out << QStringLiteral("apostrophe");
        } else if (raw == QLatin1String(" ")) {
            out << QStringLiteral("space");
        } else {
            // F-keys and letters pass through as-is. F1..F24, single
            // letters A..Z, and digits 0..9 are accepted verbatim by
            // every portal backend we've tested.
            out << raw;
        }
    }
    return out.join(QLatin1Char('+'));
}
}  // namespace mainwindowdetail

void MainWindow::wireQuakeHotkey() {
    // Two-path activation: an in-app QShortcut that fires when Ants
    // has focus, plus a Freedesktop Portal GlobalShortcuts binding
    // that fires whether or not Ants has focus (0.6.39). The in-app
    // path from 0.6.38 stays as the always-on fallback because the
    // portal is only implemented by some backends (KDE Plasma 6,
    // xdg-desktop-portal-hyprland, -wlr) — GNOME Shell and the
    // X11-on-legacy-portal cases fall back to the in-app binding.
    //
    // Double-fire debounce: on focused systems where both paths
    // deliver the same key press, we'd hide-then-show (visible
    // flicker). The in-app lambda and the portal lambda both stamp
    // m_lastQuakeToggleMs and reject if the previous stamp is less
    // than 500 ms old. QShortcut is in-process and fires first; the
    // portal's D-Bus round-trip makes its event arrive second, so
    // the debounce drops the portal's duplicate.
    //
    // Idempotent: the !m_gsPortal guard means a second call (e.g. the
    // Settings toggle after the constructor already wired it) won't
    // double-bind the portal. The constructor and the toggle paths are
    // mutually exclusive in practice (the toggle's !m_quakeMode guard),
    // but the helper stays safe if a future caller breaks that.
    QString hotkeyStr = m_config.quakeHotkey();
    if (!hotkeyStr.isEmpty()) {
        QKeySequence hotkey(hotkeyStr);
        if (!hotkey.isEmpty()) {
            auto *sc = new QShortcut(hotkey, this);
            sc->setContext(Qt::ApplicationShortcut);
            connect(sc, &QShortcut::activated, this, [this]() {
                const qint64 now = QDateTime::currentMSecsSinceEpoch();
                if (now - m_lastQuakeToggleMs < 500) return;
                m_lastQuakeToggleMs = now;
                toggleQuakeVisibility();
            });
        }
    }

    // Portal binding (only when xdg-desktop-portal is on the bus).
    // Request the same hotkey the user configured — the portal's
    // preferred_trigger is advisory, and on first bind KDE's
    // backend shows a system-settings prompt that takes our
    // suggestion as the default. Translation from Qt's
    // "Ctrl+Shift+`" to the portal's "CTRL+SHIFT+grave" is
    // best-effort; unrecognised keys pass through unchanged and
    // the user adjusts in System Settings if needed.
    if (!hotkeyStr.isEmpty() && !m_gsPortal && GlobalShortcutsPortal::isAvailable()) {
        m_gsPortal = new GlobalShortcutsPortal(this);
        connect(m_gsPortal, &GlobalShortcutsPortal::activated, this,
                [this](const QString &id) {
                    if (id != QStringLiteral("toggle-quake")) return;
                    const qint64 now = QDateTime::currentMSecsSinceEpoch();
                    if (now - m_lastQuakeToggleMs < 500) return;
                    m_lastQuakeToggleMs = now;
                    toggleQuakeVisibility();
                });
        // ANTS-1142 — listen to sessionFailed with a status-bar
        // notification fallback. Pre-fix code emitted
        // sessionFailed to no listener — on GNOME (where
        // CreateSession succeeds but BindShortcuts fails) the
        // user just saw "F12 doesn't work" with no diagnostic.
        // Surface it once, then move on (sessionFailed is
        // terminal-per-process per the post-ANTS-1142 contract).
        connect(m_gsPortal, &GlobalShortcutsPortal::sessionFailed,
                this, [this](const QString &reason) {
            qWarning("GlobalShortcutsPortal::sessionFailed: %s",
                     qUtf8Printable(reason));
            showStatusMessage(
                tr("Global hotkey unavailable — %1").arg(reason),
                6000);
        });
        m_gsPortal->bindShortcut(
            QStringLiteral("toggle-quake"),
            tr("Toggle Ants Terminal drop-down"),
            qtKeySequenceToPortalTrigger(hotkeyStr));
    }
}

void MainWindow::setupQuakeMode() {
    m_quakeMode = true;
    m_quakeVisible = true;

    // Platform-dispatch:
    //   X11:  Qt::WindowStaysOnTopHint + Qt::Tool + move() — standard
    //         _NET_WM_STATE_ABOVE path; the compositor honours client-side
    //         positioning.
    //   Wayland: the compositor owns the stacking order and positioning for
    //         regular toplevel surfaces — move() is ignored and there's no
    //         equivalent of _NET_WM_STATE_ABOVE. With LayerShellQt available
    //         at build time, we promote the window to a wlr-layer-shell-v1
    //         top-layer surface anchored to the top edge of the active
    //         screen. Without it, the Wayland path falls back to the Qt
    //         toplevel and lives with whatever the compositor decides.
    const bool isWayland = QGuiApplication::platformName().startsWith(
        QStringLiteral("wayland"), Qt::CaseInsensitive);

#ifdef ANTS_WAYLAND_LAYER_SHELL
    if (isWayland) {
        // Ensure the QWindow exists before configuring layer-shell properties,
        // which must be set BEFORE show() so the xdg_surface role upgrade to
        // zwlr_layer_surface_v1 happens at the right point in the Wayland
        // handshake. winId() on a QWidget forces a native window backing.
        create();
        if (QWindow *qw = windowHandle()) {
            auto *layer = LayerShellQt::Window::get(qw);
            layer->setLayer(LayerShellQt::Window::LayerTop);
            LayerShellQt::Window::Anchors anchors =
                LayerShellQt::Window::AnchorTop;
            anchors |= LayerShellQt::Window::AnchorLeft;
            anchors |= LayerShellQt::Window::AnchorRight;
            layer->setAnchors(anchors);
            layer->setExclusiveZone(0);  // don't push neighbours; we overlay
            layer->setKeyboardInteractivity(
                LayerShellQt::Window::KeyboardInteractivityOnDemand);
            layer->setScope(QStringLiteral("ants-terminal-quake"));
            layer->setCloseOnDismissed(false);
        }
    }
#endif

    if (!isWayland) {
        // X11 path — unchanged from pre-0.6.38 behaviour.
        setWindowFlags(windowFlags() | Qt::WindowStaysOnTopHint | Qt::Tool);
    }

    if (QScreen *screen = this->screen()) {
        QRect geo = screen->availableGeometry();
        int h = geo.height() / 3;
        resize(geo.width(), h);
        if (!isWayland) {
            // On Wayland the compositor + layer-shell anchors do the
            // positioning; a move() there is silently ignored and muddies
            // the trace logs.
            move(geo.x(), geo.y());
        }
    }
    show();
}

void MainWindow::toggleQuakeVisibility() {
    if (!m_quakeMode) return;

    QScreen *screen = this->screen();
    if (!screen) return;
    QRect geo = screen->availableGeometry();
    int h = height();

    // On Wayland, client-side move() is silently ignored by the compositor
    // (true both with and without layer-shell — layer-shell anchors the
    // surface to a screen edge; without layer-shell the compositor picks
    // the position). The slide-up/down animation uses pos() as its Qt
    // property which is a no-op under Wayland, so the XCB-only animation
    // path degenerates to a plain show/hide toggle. Prefer the plain
    // toggle there rather than ship a broken animation that visibly snaps.
    const bool isWayland = QGuiApplication::platformName().startsWith(
        QStringLiteral("wayland"), Qt::CaseInsensitive);

    if (isWayland) {
        if (m_quakeVisible) {
            hide();
            m_quakeVisible = false;
        } else {
            show();
            raise();
            activateWindow();
            m_quakeVisible = true;
            if (auto *t = focusedTerminal()) t->setFocus();
        }
        return;
    }

    // m_quakeAnim is reused across hide/show toggles. Previously we
    // connected finished→hide() in the hide branch with UniqueConnection;
    // Qt can't dedupe lambdas, and even if it could, the same slot is
    // needed on both branches' end-states (animation end = whatever the
    // current "done" action is). On the show branch the stale
    // finished→hide() connection from a prior hide fired right after the
    // slide-down animation completed — the window would appear for 200 ms
    // and then vanish. Fix: disconnect all finished() slots before every
    // start(), and only add the hide() slot on the hide branch.
    if (!m_quakeAnim) {
        m_quakeAnim = new QPropertyAnimation(this, "pos", this);
        m_quakeAnim->setDuration(200);
    }
    m_quakeAnim->stop();
    QObject::disconnect(m_quakeAnim, &QPropertyAnimation::finished, this, nullptr);

    if (m_quakeVisible) {
        // Slide up (hide)
        m_quakeAnim->setEasingCurve(QEasingCurve::InQuad);
        m_quakeAnim->setStartValue(pos());
        m_quakeAnim->setEndValue(QPoint(geo.x(), geo.y() - h));
        connect(m_quakeAnim, &QPropertyAnimation::finished, this, [this]() {
            hide();
        });
        m_quakeAnim->start();
        m_quakeVisible = false;
    } else {
        // Slide down (show) — no finished() slot needed.
        move(geo.x(), geo.y() - h);
        show();
        raise();
        activateWindow();
        m_quakeAnim->setEasingCurve(QEasingCurve::OutQuad);
        m_quakeAnim->setStartValue(QPoint(geo.x(), geo.y() - h));
        m_quakeAnim->setEndValue(QPoint(geo.x(), geo.y()));
        m_quakeAnim->start();
        m_quakeVisible = true;
        if (auto *t = focusedTerminal()) t->setFocus();
    }
}
