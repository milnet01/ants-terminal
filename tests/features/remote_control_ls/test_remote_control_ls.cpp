// Remote-control `ls` — source-grep regression test locking the
// first-slice protocol shape. See spec.md.

#include <cstdio>
#include <regex>
#include <string>


#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"
#ifndef SRC_RC_HEADER
#error "SRC_RC_HEADER compile definition required"
#endif
#ifndef ANTS_RC_SOURCES
#error "ANTS_RC_SOURCES compile definition required"
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


static int runMain() {
    const std::string h   = ants_test::slurpFile(SRC_RC_HEADER);
    const std::string rc  = ants_test::slurpRemoteControl();
    const std::string mwc = ants_test::slurpFile(SRC_MAINWINDOW_CPP);
    const std::string mwh = ants_test::slurpFile(SRC_MAINWINDOW_H);
    const std::string mc  = ants_test::slurpFile(SRC_MAIN_CPP);
    const std::string cm  = ants_test::slurpFile(SRC_CMAKELISTS);

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

    // INV-7: CMakeLists.txt wires the RemoteControl sources into the build.
    // ANTS-3833 — the implementation is eleven TUs, named once by the
    // ANTS_RC_SOURCES_REL list that ants_core_lib consumes. Asserting on the
    // list rather than on one filename is what the split made this mean, and
    // it is the stronger check: a TU added to add_library() but omitted from
    // the list would still satisfy a search for any single path.
    if (cm.find("ANTS_RC_SOURCES_REL") == std::string::npos) {
        fail("INV-7: CMakeLists.txt must declare ANTS_RC_SOURCES_REL and feed "
             "it to ants_core_lib");
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

TEST(RemoteControlLs, Main) {
    ASSERT_EQ(0, runMain());
}
