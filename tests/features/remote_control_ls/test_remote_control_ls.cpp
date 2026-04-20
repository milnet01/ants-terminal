// Remote-control `ls` — source-grep regression test locking the
// first-slice protocol shape. See spec.md.

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#ifndef SRC_RC_HEADER
#error "SRC_RC_HEADER compile definition required"
#endif
#ifndef SRC_RC_CPP
#error "SRC_RC_CPP compile definition required"
#endif
#ifndef SRC_MAINWINDOW_CPP
#error "SRC_MAINWINDOW_CPP compile definition required"
#endif
#ifndef SRC_MAINWINDOW_H
#error "SRC_MAINWINDOW_H compile definition required"
#endif
#ifndef SRC_MAIN_CPP
#error "SRC_MAIN_CPP compile definition required"
#endif
#ifndef SRC_CMAKELISTS
#error "SRC_CMAKELISTS compile definition required"
#endif

static std::string slurp(const char *path) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(2);
    }
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

int main() {
    const std::string h   = slurp(SRC_RC_HEADER);
    const std::string rc  = slurp(SRC_RC_CPP);
    const std::string mwc = slurp(SRC_MAINWINDOW_CPP);
    const std::string mwh = slurp(SRC_MAINWINDOW_H);
    const std::string mc  = slurp(SRC_MAIN_CPP);
    const std::string cm  = slurp(SRC_CMAKELISTS);

    int failures = 0;
    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };

    // INV-1: class RemoteControl : public QObject in the header.
    std::regex classDecl(R"(class\s+RemoteControl\s*:\s*public\s+QObject)");
    if (!std::regex_search(h, classDecl)) {
        fail("INV-1: class RemoteControl : public QObject not declared in remotecontrol.h");
    }

    // INV-2: defaultSocketPath consults ANTS_REMOTE_SOCKET via qgetenv.
    std::regex envOverride(R"(qgetenv\s*\(\s*"ANTS_REMOTE_SOCKET"\s*\))");
    if (!std::regex_search(rc, envOverride)) {
        fail("INV-2: ANTS_REMOTE_SOCKET override via qgetenv not found — "
             "multi-instance scripting requires this escape hatch");
    }

    // INV-3: dispatch() recognises the `ls` command and has an
    // `unknown command` fallthrough. The regex pair below is
    // order-independent (the literal strings must both appear in the
    // dispatch function body — any reasonable refactor preserves both).
    if (rc.find("\"ls\"") == std::string::npos) {
        fail("INV-3a: RemoteControl::dispatch must handle the literal \"ls\" command name");
    }
    if (rc.find("unknown command") == std::string::npos) {
        fail("INV-3b: dispatch() must return an \"unknown command:\" error envelope "
             "for unrecognised commands");
    }

    // INV-4: ok + tabs field names appear in cmdLs (stable contract).
    if (rc.find("out[\"ok\"]") == std::string::npos ||
        rc.find("out[\"tabs\"]") == std::string::npos) {
        fail("INV-4: cmdLs() must set `ok` and `tabs` fields on the response — "
             "those names are the stable rc_protocol contract");
    }

    // INV-5: tabListForRemote on MainWindow is public, emits the
    // four field names we've documented as stable.
    if (mwh.find("QJsonArray tabListForRemote() const") == std::string::npos) {
        fail("INV-5a: MainWindow::tabListForRemote() const declaration missing from mainwindow.h");
    }
    // Locate the function body — a heuristic, not a full parser.
    size_t bodyStart = mwc.find("MainWindow::tabListForRemote");
    if (bodyStart == std::string::npos) {
        fail("INV-5b: MainWindow::tabListForRemote definition missing from mainwindow.cpp");
    } else {
        std::string body = mwc.substr(bodyStart, 1400);
        for (const char *field : {"\"index\"", "\"title\"", "\"cwd\"", "\"active\""}) {
            if (body.find(field) == std::string::npos) {
                std::string msg = "INV-5c: tabListForRemote must emit field ";
                msg += field;
                std::fprintf(stderr, "FAIL: %s\n", msg.c_str());
                ++failures;
            }
        }
    }

    // INV-6: main.cpp registers --remote and delegates to runClient
    // *before* MainWindow construction.
    std::regex remoteOptRegex(R"(QCommandLineOption\s+\w+\s*\(\s*"remote")");
    if (!std::regex_search(mc, remoteOptRegex)) {
        fail("INV-6a: main.cpp must register a --remote QCommandLineOption");
    }
    if (mc.find("RemoteControl::runClient") == std::string::npos) {
        fail("INV-6b: main.cpp must call RemoteControl::runClient for --remote invocations");
    }
    // Ordering check — runClient must return (exit) before MainWindow
    // is constructed. Trivial check: the `return RemoteControl::runClient`
    // line appears before the `MainWindow window(` line.
    const size_t clientPos = mc.find("RemoteControl::runClient");
    const size_t mwPos     = mc.find("MainWindow window(");
    if (clientPos == std::string::npos || mwPos == std::string::npos ||
        clientPos >= mwPos) {
        fail("INV-6c: --remote branch must run before MainWindow construction — "
             "otherwise the client would boot a second GUI");
    }

    // INV-7: CMakeLists.txt lists remotecontrol.cpp in the executable.
    if (cm.find("src/remotecontrol.cpp") == std::string::npos) {
        fail("INV-7: CMakeLists.txt must list src/remotecontrol.cpp "
             "under the ants-terminal executable sources");
    }

    // INV-8 (negative): remotecontrol.cpp must not import Widgets UI headers.
    for (const char *banned : {
            "#include <QMessageBox>",
            "#include <QMenu>",
            "#include <QDialog>"}) {
        if (rc.find(banned) != std::string::npos) {
            std::string msg = "INV-8 (neg): remotecontrol.cpp must not include ";
            msg += banned;
            msg += " — the remote-control layer has no UI surface";
            std::fprintf(stderr, "FAIL: %s\n", msg.c_str());
            ++failures;
        }
    }

    if (failures > 0) {
        std::fprintf(stderr, "\n%d invariant(s) failed — see spec.md for context\n", failures);
        return 1;
    }
    std::printf("OK: remote-control `ls` invariants present\n");
    return 0;
}
