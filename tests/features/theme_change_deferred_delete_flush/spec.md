# ANTS-2024 — DeferredDelete flush precedes the app-wide theme restyle

## Background

CRITICAL, reliably reproducible (two byte-identical core dumps,
2026-06-04): changing the colour theme could SIGSEGV. Stack:
`QAction::triggered` (theme menu) → `MainWindow::applyTheme` →
`qApp->setStyleSheet(...)` → Qt's app-wide widget walk → null `d_ptr`
deref on a **freed** widget. The freed widget was the Claude status-bar
permission-prompt group (`btnWidget` + Allow/Deny/Add buttons,
claudestatuswidgets.cpp): every teardown path tears the group down via
`btnWidget->deleteLater()`, and each button carries a per-widget
stylesheet that registers in Qt's pointer-keyed `QStyleSheetStyle`
caches.

Root cause: a `deleteLater()`-pending prompt widget that gets reaped
mid-flight (e.g. by the theme `QMenu`'s nested event loop) leaves the
app-wide `setStyleSheet` walk dereferencing freed memory. Because
`deleteLater` is the *only* teardown path for the prompt group, forcing
all pending `DeferredDelete` events to be processed immediately before
the restyle makes the widget set quiescent — any pending prompt widget
is fully destructed (and removed from every Qt widget list + style
cache) before Qt builds its walk.

Fix (mainwindow.cpp `applyTheme`):
`QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete)`
runs immediately **before** `qApp->setStyleSheet(...)`. The explicit
`sendPostedEvents(obj, eventType)` form force-reaps regardless of the
loop level at which `deleteLater` was posted, so it covers the
nested-menu-loop timing too.

This is a source-level ordering guard. The runtime use-after-free itself
is confirmed under the ASan debug build (`cmake --preset=debug`) by
opening a Claude permission prompt and then changing the theme — that
manual GUI repro is the closure gate; this test guards against the fix
being silently reordered or removed.

## Invariants

### INV-1 — applyTheme reaps DeferredDelete before the app-wide restyle

In `src/mainwindow.cpp`, within `MainWindow::applyTheme`, a
`QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete)`
call appears at a source offset **before** the first
`qApp->setStyleSheet(` call.

### INV-2 — both anchors are present

Both the `sendPostedEvents(..., QEvent::DeferredDelete)` flush and the
`qApp->setStyleSheet(` app-wide restyle are present in `applyTheme`
(guards against a refactor dropping either anchor and trivially passing
INV-1 on two missing substrings).

## Test plan

Source-scrape over `SRC_MAINWINDOW_CPP_PATH`: locate the
`MainWindow::applyTheme` body, then assert the byte offset of the
DeferredDelete flush is less than the offset of the first
`qApp->setStyleSheet(`. Deterministic; no GUI. The runtime confirmation
is the manual ASan repro described above.
