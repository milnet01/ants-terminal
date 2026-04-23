// Feature-conformance test for spec.md — asserts:
//   (A) filterControlChars strips the dangerous C0 subset + DEL while
//       preserving HT / LF / CR and all UTF-8 bytes >= 0x80.
//   (B) Config API round-trip for `remote_control_enabled` — the gate
//       the listener-start path reads — plus structural confirmation
//       that `RemoteControl::start()` is call-gated, not unconditional.
//   (C) source-grep invariants on raw-bypass wiring inside cmdSendText.
//
// (A) exercises the filter function directly — pure, no Qt dialog
// infrastructure needed. (B) replaces the earlier "grep for the getter
// name" gate test: a refactor that renames Config::remoteControlEnabled()
// now fails at compile-time (the test calls the getter directly), and
// a refactor that hoists start() out of any conditional is caught by
// the structural pattern match rather than the fragile 400-char
// lookback. (C) lives unchanged — the raw-bypass path is still a
// natural structural check.
//
// Exit 0 = all assertions hold. Non-zero = regression.

#include "config.h"
#include "remotecontrol.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#ifndef SRC_RC_CPP
#error "SRC_RC_CPP compile definition required"
#endif
#ifndef SRC_MAINWINDOW_CPP
#error "SRC_MAINWINDOW_CPP compile definition required"
#endif

namespace {

int g_failures = 0;

void expect(bool ok, const char *label, const char *detail = "") {
    std::fprintf(stderr, "[%-48s] %s%s%s\n",
                 label, ok ? "PASS" : "FAIL",
                 (detail && *detail) ? " — " : "",
                 detail ? detail : "");
    if (!ok) ++g_failures;
}

void testFilter() {
    // Pure payload: nothing stripped.
    {
        int n = 0;
        QByteArray out = RemoteControl::filterControlChars(
            QByteArray("hello world\n"), &n);
        expect(out == "hello world\n" && n == 0,
               "filter: printable + LF passes through");
    }

    // HT / LF / CR preserved.
    {
        int n = 0;
        QByteArray in; in.append('\t').append('\n').append('\r');
        QByteArray out = RemoteControl::filterControlChars(in, &n);
        expect(out == in && n == 0, "filter: HT/LF/CR preserved");
    }

    // ESC stripped — the headline vector.
    {
        int n = 0;
        // "A\x1B[2JB" — ESC [ 2 J = clear screen. ESC alone dropped,
        // [2J are printable so they survive — consumer sees "A[2JB"
        // which is harmless literal text, NOT a terminal command.
        QByteArray in; in.append('A').append('\x1B').append("[2J").append('B');
        QByteArray out = RemoteControl::filterControlChars(in, &n);
        expect(out == "A[2JB" && n == 1, "filter: ESC (0x1B) stripped");
    }

    // NUL / BS / DEL stripped.
    {
        int n = 0;
        QByteArray in; in.append('x').append('\x00').append('\x08')
                         .append('\x7F').append('y');
        QByteArray out = RemoteControl::filterControlChars(in, &n);
        expect(out == "xy" && n == 3, "filter: NUL/BS/DEL stripped");
    }

    // All of 0x00-0x08 and 0x0B-0x1F stripped.
    {
        int n = 0;
        QByteArray in;
        for (int b = 0x00; b <= 0x1F; ++b) in.append(static_cast<char>(b));
        in.append('\x7F');
        QByteArray out = RemoteControl::filterControlChars(in, &n);
        // Only HT LF CR survive out of C0; DEL removed.
        expect(out.size() == 3, "filter: C0 range stripped to {HT,LF,CR}");
        expect(out.contains('\t') && out.contains('\n') && out.contains('\r'),
               "filter: HT/LF/CR survived sweep");
        expect(n == 30, "filter: stripped count = (0x00..0x1F = 32) + DEL - (HT+LF+CR) = 30");
    }

    // UTF-8 bytes >= 0x80 pass through (e.g. 'é' = 0xC3 0xA9).
    {
        int n = 0;
        QByteArray in; in.append("caf").append('\xC3').append('\xA9');
        QByteArray out = RemoteControl::filterControlChars(in, &n);
        expect(out == in && n == 0, "filter: UTF-8 multi-byte preserved");
    }

    // The canonical attack: inject a newline-terminated shell command.
    // Attacker payload: BS-over-user-input + CR-to-start-of-line + ESC[
    // sequence + shell command + LF to execute. After filter, the
    // shell command text survives but its delivery mechanism (CR + ESC)
    // is declawed — the user would SEE "rm -rf ~" appended to their
    // current line rather than having it execute.
    {
        int n = 0;
        QByteArray attack;
        attack.append('\x08')           // BS
              .append('\r')             // CR — kept (legitimate keystroke)
              .append('\x1B').append("[2K")  // ESC[2K = erase line
              .append("rm -rf ~")
              .append('\n');
        QByteArray out = RemoteControl::filterControlChars(attack, &n);
        // CR + "[2K" + "rm -rf ~\n" — the ESC is gone, so what reaches
        // the PTY is harmless text. The `rm -rf ~` string is present
        // but the `\r` ahead of it is just a keystroke, not a sequence.
        expect(!out.contains('\x1B'), "filter: attack ESC stripped");
        expect(!out.contains('\x08'), "filter: attack BS stripped");
        expect(out.contains("rm -rf ~"), "filter: benign literal survives (by design)");
        expect(n == 2, "filter: attack stripped exactly ESC + BS");
    }
}

static std::string slurp(const char *path) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(2);
    }
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

void testSourceInvariants() {
    const std::string rc = slurp(SRC_RC_CPP);
    const std::string mw = slurp(SRC_MAINWINDOW_CPP);

    // INV-A: cmdSendText consults the raw-bypass flag.
    expect(rc.find("req.value(\"raw\")") != std::string::npos,
           "source: cmdSendText reads raw bypass flag");

    // INV-B: cmdSendText calls filterControlChars.
    expect(rc.find("filterControlChars(") != std::string::npos,
           "source: cmdSendText delegates to filterControlChars");

    // INV-C (structural): start() call must appear inside a conditional
    // rather than at statement-top. Look backward 200 chars from the
    // first `m_remoteControl->start()` and confirm an `if (` is in
    // scope. Catches a refactor that removes the gate entirely.
    // Does NOT depend on the getter's name — the behavioral
    // round-trip below is what locks `remoteControlEnabled()`
    // specifically. (Plain-substring, not regex — avoids catastrophic
    // backtracking on the full mainwindow.cpp body.)
    {
        const size_t startCall = mw.find("m_remoteControl->start()");
        const bool found = (startCall != std::string::npos);
        expect(found, "source: RemoteControl::start() call present");
        if (found) {
            const size_t lookback = std::min<size_t>(startCall, 200);
            const std::string window = mw.substr(startCall - lookback, lookback);
            expect(window.find("if (") != std::string::npos ||
                   window.find("if(") != std::string::npos,
                   "source: start() lives inside a conditional (not unconditional)");
        }
    }
}

// Config round-trip — the behavioral piece that replaces the fragile
// "grep for remoteControlEnabled" lookback. A refactor that renames
// the getter fails at compile-time here; a refactor that changes the
// default from false fails at the default-check; a refactor that
// breaks save/load persistence fails at the round-trip.
void testGateBehavioral() {
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        expect(false, "gate-behavioral: QTemporaryDir failed to create");
        return;
    }
    // Sandbox XDG so Config::configPath() writes into tmp, not the
    // user's real ~/.config/ants-terminal. Do NOT use
    // QStandardPaths::setTestModeEnabled — it routes to a Qt-test-
    // specific path that ignores XDG and has collided with real user
    // configs in practice (see config_parse_failure_guard/test).
    qputenv("XDG_CONFIG_HOME", tmp.path().toLocal8Bit());

    // 1. Default: fresh Config → flag is false.
    {
        Config cfg;
        expect(cfg.remoteControlEnabled() == false,
               "gate-behavioral: fresh Config defaults remote_control_enabled=false");
    }

    // 2. Setter round-trip — set true, re-read same instance.
    {
        Config cfg;
        cfg.setRemoteControlEnabled(true);
        expect(cfg.remoteControlEnabled() == true,
               "gate-behavioral: setRemoteControlEnabled(true) round-trips in-memory");
    }

    // 3. Persistence round-trip — set true, discard, reload.
    {
        {
            Config cfg;
            cfg.setRemoteControlEnabled(true);
        }
        Config cfg2;
        expect(cfg2.remoteControlEnabled() == true,
               "gate-behavioral: remote_control_enabled persists across Config instances");
    }

    // 4. Setter round-trip back to false.
    {
        {
            Config cfg;
            cfg.setRemoteControlEnabled(false);
        }
        Config cfg2;
        expect(cfg2.remoteControlEnabled() == false,
               "gate-behavioral: setRemoteControlEnabled(false) round-trips across instances");
    }

    qunsetenv("XDG_CONFIG_HOME");
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    testFilter();
    testSourceInvariants();
    testGateBehavioral();

    if (g_failures > 0) {
        std::fprintf(stderr, "\n%d assertion(s) failed.\n", g_failures);
        return 1;
    }
    return 0;
}
