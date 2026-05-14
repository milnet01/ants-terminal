# 2026-05-14 — Resolution: 16-byte one-shot leak from MainWindow ctor

## Context

A prior debugging session (~2 days earlier) flagged a 16-byte one-shot
leak in `MainWindow::MainWindow(bool, QWidget*)` via LeakSanitizer.
The report was filed as a low-priority follow-up because (a) the leak
is allocated exactly once at startup and never grows, and (b) the
optimized-build LSan output didn't include a source line.

## Reproducer

Required: a `build-asan/` tree (configure with `cmake --preset=debug`).

Per-step hack used during diagnosis — a tiny env-gated auto-quit
shim in `src/main.cpp`:

```cpp
if (const int ms = qEnvironmentVariableIntValue("ANTS_DEBUG_QUIT_AFTER_INIT_MS"); ms > 0) {
    QTimer::singleShot(ms, qApp, &QCoreApplication::quit);
}
```

This is kept in tree under task #11's diagnostic-shim header so future
LSan hunts don't need to be invasive. With it, run:

```bash
QT_QPA_PLATFORM=offscreen ANTS_DEBUG_QUIT_AFTER_INIT_MS=500 \
  ASAN_OPTIONS='detect_leaks=1:fast_unwind_on_malloc=0:malloc_context_size=40' \
  build-asan/ants-terminal
```

The graceful quit lets LSan's atexit handler flush its report.

## Findings

LSan reported four leaks at process exit, totalling 19 bytes:

| Bytes | Origin | Chain |
|---:|---|---|
| 16 | `MainWindow::MainWindow` | `operator new(16)` → Qt6 slot object |
| 1  | `MainWindow::checkForUpdates` | `g_strdup` → libpxbackend |
| 1  | `MainWindow::checkForUpdates` | `g_strdup` → libpxbackend |
| 1  | `MainWindow::checkForUpdates` | `g_strdup` → libpxbackend |

### The 16-byte allocation

Verified via `objdump -d build-asan/ants-terminal` starting at the
caller offset that LSan reported. The relevant sequence is:

```
mov $0x10, %edi          ; size = 16
call operator new(unsigned long)@plt
mov %rax, %r12           ; save result
...
lea QtPrivate::QMetaTypeInterfaceWrapper<TitleBar>::metaType(%rip), %rax
call __ubsan_handle_type_mismatch_v1@plt
```

The 16-byte block is followed by an alignment check against
`QtPrivate::QMetaTypeInterfaceWrapper<TitleBar>::metaType` — Qt6's
internal slot-object construction during a `connect()` to a TitleBar
signal. Qt6's slot objects are reference-counted and freed when the
connection ends; the residue at LSan-check time reflects shutdown-
ordering between QApplication's atexit teardown and LSan's own
atexit hook, not a true leak.

### The libpxbackend allocations

Stack chain: `MainWindow::checkForUpdates(bool)` →
`QNetworkAccessManager::get(QNetworkRequest)` →
`QNetworkProxyFactory::systemProxyForQuery` → libproxy →
`px_proxy_factory_new` → GLib `g_strdup`. libproxy caches its
proxy-config strings for the life of the process via GObject; no
hook to free them.

## Resolution

Both classes of leak are one-shot, third-party, and not progressive.
Suppressed via `tests/lsan-suppressions.txt` rather than worked around
in our code, because:

- The Qt-slot leak is internal Qt bookkeeping that no MainWindow code
  change can address. Future Qt6 updates may fix the shutdown ordering.
- The libproxy leaks require an upstream patch.

`CMakePresets.json` was updated to flip the `debug` test preset from
`detect_leaks=0` to `detect_leaks=1`, with the suppression file
loaded via `LSAN_OPTIONS`. This means CI will now catch any *new*
leaks while keeping the documented known set silent.

## Re-audit triggers

Re-check `tests/lsan-suppressions.txt` whenever:

- Qt 6 minor/major version bumps (slot-object lifecycle may change).
- libproxy is replaced or removed from the chain.
- A new MainWindow ctor connect pattern is introduced (a different
  slot leak might appear under the same suppression).

Run the reproducer above; remove entries that no longer match.
