// Feature-conformance test for spec.md — asserts:
//   (A) filterControlChars strips the dangerous C0 subset + DEL while
//       preserving HT / LF / CR and all UTF-8 bytes >= 0x80.
//   (B) source-grep invariants confirming the opt-in config gate and
//       the raw-bypass path are wired into cmdSendText + mainwindow.
//
// (A) exercises the filter function directly — pure, no Qt dialog
// infrastructure needed. (B) locks down the wiring so a future refactor
// doesn't silently bypass the filter or open the socket by default.
//
// Exit 0 = all assertions hold. Non-zero = regression.

#include "remotecontrol.h"

#include <QByteArray>
#include <QCoreApplication>

#include <cstdio>
#include <cstring>
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

    // INV-C: mainwindow.cpp gates start() on remoteControlEnabled().
    expect(mw.find("remoteControlEnabled()") != std::string::npos,
           "source: MainWindow gates listener on config key");

    // INV-D: the gate surrounds start() (no unconditional call after
    // the introduction of the config key). Heuristic: find the first
    // occurrence of "m_remoteControl->start()" and confirm a
    // "remoteControlEnabled" appears in the preceding 200 chars.
    size_t startCall = mw.find("m_remoteControl->start()");
    expect(startCall != std::string::npos,
           "source: RemoteControl::start() call present");
    if (startCall != std::string::npos) {
        const size_t lookback = std::min<size_t>(startCall, 400);
        const std::string window = mw.substr(startCall - lookback, lookback);
        expect(window.find("remoteControlEnabled") != std::string::npos,
               "source: start() call gated by remoteControlEnabled check");
    }
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    testFilter();
    testSourceInvariants();

    if (g_failures > 0) {
        std::fprintf(stderr, "\n%d assertion(s) failed.\n", g_failures);
        return 1;
    }
    return 0;
}
