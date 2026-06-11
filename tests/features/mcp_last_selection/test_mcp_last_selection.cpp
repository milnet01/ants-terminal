// ANTS-1312 — feature-conformance test for last_selection MCP. Pure
// source-grep wiring contract: the live behaviour is delegated to
// TerminalWidget::selectedText() which has its own coverage. See spec.md.

#include "../../_support/expect.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#ifndef SRC_RC_HEADER
#error "SRC_RC_HEADER compile definition required"
#endif
#ifndef SRC_REMOTECONTROL_CPP_PATH
#error "SRC_REMOTECONTROL_CPP_PATH compile definition required"
#endif
#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif
#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {

bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

TEST(McpLastSelection, WiringContract) {
    expect_reset();

    const std::string rcHdr = ants_test::slurpFile(SRC_RC_HEADER);
    const std::string rcCpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    const std::string mwCpp = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    const std::string ciCpp = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);

    // INV-1 — declaration + definition.
    expect(contains(rcHdr, "cmdLastSelection"),
           "INV-1: declared in remotecontrol.h");
    expect(contains(rcCpp, "RemoteControl::cmdLastSelection"),
           "INV-1: defined in remotecontrol.cpp");

    // INV-2 — mainwindow registration with TabSpecific contract.
    expect(contains(mwCpp, "registerToolProvider(\"last_selection\""),
           "INV-2: last_selection registered in mainwindow.cpp");

    // INV-3 — claudeintegration descriptor.
    expect(contains(ciCpp, "t[\"name\"] = \"last_selection\""),
           "INV-3: tool descriptor present");

    // INV-4 — explicit `tab` override allowlist.
    expect(contains(ciCpp,
               "toolName == QLatin1String(\"last_selection\")"),
           "INV-4: tabSpecificAcceptsTabIndex membership");

    // INV-5 — token-cost entry.
    expect(contains(ciCpp, "\"last_selection\""),
           "INV-5: token-cost entry (name appears in cost map)");

    // INV-6 — kindForName terminal bucket.
    expect(contains(ciCpp,
               "name == QLatin1String(\"last_selection\")"),
           "INV-6: kindForName terminal-bucket membership");

    // INV-7 — callerCwdContractFor TabSpecific.
    expect(contains(ciCpp,
               "if (toolName == QStringLiteral(\"last_selection\"))    return C::TabSpecific;"),
           "INV-7: callerCwdContractFor TabSpecific branch");

    // INV-8 — delegates to TerminalWidget::selectedText (single source
    //         of truth, no reimplementation in cmdLastSelection).
    expect(contains(rcCpp, "->selectedText()"),
           "INV-8: cmdLastSelection calls selectedText() somewhere "
           "in remotecontrol.cpp");

    // INV-9 — has_selection / text fields in the envelope.
    expect(contains(rcCpp, "\"has_selection\""),
           "INV-9: response envelope carries has_selection field");
    expect(contains(rcCpp, "last_selection: no terminal"),
           "INV-10: refuses with no_window message when no terminal");

    EXPECT_EQ(0, expect_failures());
}
