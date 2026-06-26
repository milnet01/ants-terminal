// ANTS-1901 — master Ants-MCP on/off toggle. Conformance test.
// See spec.md (this dir) and docs/specs/ANTS-1901.md.
//
// INV-1            real Config round-trip (claude.mcp_enabled).
// INV-2..7 + reg.  source-greps locking the gating wiring (the
//                  flatpak_host_shell pattern — the runtime gates fire
//                  in the MainWindow ctor / live socket, not headlessly).

#include "config.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <QByteArray>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <string>

#ifndef SRC_MAINWINDOW_CPP
#error "SRC_MAINWINDOW_CPP compile definition required"
#endif
#ifndef SRC_CLAUDESTATUSWIDGETS_CPP_PATH
#error "SRC_CLAUDESTATUSWIDGETS_CPP_PATH compile definition required"
#endif
#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif
#ifndef SRC_SETTINGSDIALOG_CPP_PATH
#error "SRC_SETTINGSDIALOG_CPP_PATH compile definition required"
#endif
#ifndef MCP_ERROR_CODES_MD_PATH
#error "MCP_ERROR_CODES_MD_PATH compile definition required"
#endif

namespace {

// Sandbox XDG_CONFIG_HOME so Config reads/writes a temp dir, not the
// user's real config (mirrors config_tab_title_format's guard).
struct XdgConfigHomeGuard {
    QByteArray prior;
    bool hadPrior = false;
    XdgConfigHomeGuard() {
        hadPrior = qEnvironmentVariableIsSet("XDG_CONFIG_HOME");
        if (hadPrior) prior = qgetenv("XDG_CONFIG_HOME");
    }
    void set(const QByteArray &v) { qputenv("XDG_CONFIG_HOME", v); }
    ~XdgConfigHomeGuard() {
        if (hadPrior) qputenv("XDG_CONFIG_HOME", prior);
        else qunsetenv("XDG_CONFIG_HOME");
    }
};

std::string slurp(const char *path) { return ants_test::slurpFile(path); }

bool has(const std::string &s, const std::string &needle) {
    return s.find(needle) != std::string::npos;
}

size_t countOccurrences(const std::string &s, const std::string &needle) {
    size_t c = 0, p = 0;
    while ((p = s.find(needle, p)) != std::string::npos) {
        ++c;
        p += needle.size();
    }
    return c;
}

}  // namespace

// INV-1 — config round-trip: default true, persists a written value.
TEST(McpMasterToggle, INV1_ConfigRoundTrip) {
    XdgConfigHomeGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    guard.set(tmp.path().toUtf8());

    { Config a; EXPECT_TRUE(a.claudeMcpEnabled()); }       // default on
    { Config a; a.setClaudeMcpEnabled(false); }
    { Config b; EXPECT_FALSE(b.claudeMcpEnabled()); }      // persisted off
    { Config b; b.setClaudeMcpEnabled(true); }
    { Config c; EXPECT_TRUE(c.claudeMcpEnabled()); }       // persisted on
}

// INV-2 — startup gate: one bind + one export, both behind the gate.
TEST(McpMasterToggle, INV2_StartupGate) {
    const std::string mw = slurp(SRC_MAINWINDOW_CPP);
    EXPECT_EQ(countOccurrences(mw, "startMcpServer("), 1u);
    EXPECT_EQ(countOccurrences(mw, "qputenv(\"ANTS_MCP_SOCKET\""), 1u);
    const auto gate = mw.find("m_config.claudeMcpEnabled()");
    const auto bind = mw.find("startMcpServer(");
    ASSERT_NE(gate, std::string::npos);
    ASSERT_NE(bind, std::string::npos);
    EXPECT_LT(gate, bind) << "the claudeMcpEnabled() gate must precede the bind";
}

// INV-3 — orientation install gated on the master; master-off uninstalls.
TEST(McpMasterToggle, INV3_OrientationGatedOnMaster) {
    const std::string mw = slurp(SRC_MAINWINDOW_CPP);
    EXPECT_TRUE(has(mw, "mcpOn && m_config.claudeMcpOrientationEnabled()"))
        << "orientation install must require the master gate too";
    EXPECT_TRUE(has(mw, "ants::mcp_orientation::uninstall()"));
    // the live settingsChanged handler removes the hook when master is off
    EXPECT_TRUE(has(mw, "if (!m_config.claudeMcpEnabled())"));
}

// INV-4 — auto-switcher early return precedes the /model injection.
TEST(McpMasterToggle, INV4_AutoSwitcherEarlyReturn) {
    const std::string cw = slurp(SRC_CLAUDESTATUSWIDGETS_CPP_PATH);
    // Anchor inside refreshAutoModelSwitch — the chip-click handler also
    // injects QStringLiteral("/model ") and appears earlier in the file.
    const auto fn = cw.find("ClaudeStatusBarController::refreshAutoModelSwitch");
    ASSERT_NE(fn, std::string::npos);
    const auto gate   = cw.find("!cfg.claudeMcpEnabled()", fn);
    const auto inject = cw.find("QStringLiteral(\"/model \")", fn);
    ASSERT_NE(gate, std::string::npos);
    ASSERT_NE(inject, std::string::npos);
    EXPECT_LT(gate, inject)
        << "the master early return must precede the /model injection";
}

// ANTS-2195 — the parked-feature code guard must short-circuit the actuator
// BEFORE the /model keystroke injection, so a config-migration bug that flips
// claude.auto_model_switch on cannot silently re-arm keystroke injection.
TEST(McpMasterToggle, Ants2195ParkedGuardPrecedesInjection) {
    const std::string cw = slurp(SRC_CLAUDESTATUSWIDGETS_CPP_PATH);
    const auto fn = cw.find("ClaudeStatusBarController::refreshAutoModelSwitch");
    ASSERT_NE(fn, std::string::npos);
    const auto guard  = cw.find("kAutoSwitchActuatorParked", fn);
    const auto inject = cw.find("QStringLiteral(\"/model \")", fn);
    ASSERT_NE(guard, std::string::npos)
        << "ANTS-2195 parked guard missing from refreshAutoModelSwitch";
    ASSERT_NE(inject, std::string::npos);
    EXPECT_LT(guard, inject)
        << "the parked guard must precede the /model injection";
}

// INV-5 — dispatcher refuses with mcp_disabled before the caller_cwd gate.
TEST(McpMasterToggle, INV5_DispatcherRefusal) {
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const auto branch = ci.find("method == \"tools/call\"");
    ASSERT_NE(branch, std::string::npos);
    // Anchor every search at/after the tools/call branch so an earlier
    // in-file mention (schema text, contract tables) can't skew ordering.
    const auto guard  = ci.find("!m_mcpEnabled", branch);
    const auto code   = ci.find("\"mcp_disabled\"", branch);
    const auto cwReq  = ci.find("QStringLiteral(\"caller_cwd_required\")", branch);
    ASSERT_NE(guard, std::string::npos);
    ASSERT_NE(code, std::string::npos);
    ASSERT_NE(cwReq, std::string::npos);
    EXPECT_LT(branch, guard) << "guard must be inside the tools/call branch";
    EXPECT_LT(guard, code)   << "the !m_mcpEnabled guard emits mcp_disabled";
    EXPECT_LT(guard, cwReq)  << "guard must precede the caller_cwd_required emit";
}

// INV-6 — Settings master checkbox on the General tab + cascade.
TEST(McpMasterToggle, INV6_SettingsMasterCheckbox) {
    const std::string sd = slurp(SRC_SETTINGSDIALOG_CPP_PATH);
    EXPECT_TRUE(has(sd, "m_claudeMcpEnabled = new QCheckBox"));
    EXPECT_TRUE(has(sd, "setClaudeMcpEnabled(m_claudeMcpEnabled->isChecked())"));
    EXPECT_TRUE(has(sd, "mirrorMcpMaster"));
    const auto general = sd.find("void SettingsDialog::setupGeneralTab");
    const auto created = sd.find("m_claudeMcpEnabled = new QCheckBox");
    const auto nextTab = sd.find("void SettingsDialog::setupAppearanceTab");
    ASSERT_NE(general, std::string::npos);
    ASSERT_NE(created, std::string::npos);
    ASSERT_NE(nextTab, std::string::npos);
    EXPECT_LT(general, created) << "checkbox must be created in setupGeneralTab";
    EXPECT_LT(created, nextTab) << "checkbox must be created in setupGeneralTab";
}

// INV-7 — settingsChanged propagates the value at runtime.
TEST(McpMasterToggle, INV7_RuntimePropagation) {
    const std::string mw = slurp(SRC_MAINWINDOW_CPP);
    EXPECT_TRUE(has(mw,
        "m_claudeIntegration->setMcpEnabled(m_config.claudeMcpEnabled())"));
}

// Registration — mcp_disabled lives under category 5 of the taxonomy.
TEST(McpMasterToggle, Registration_McpDisabledInTaxonomy) {
    const std::string ec = slurp(MCP_ERROR_CODES_MD_PATH);
    const auto cat5 = ec.find("Dispatcher / registry");
    const auto row  = ec.find("| `mcp_disabled` |");
    const auto next = ec.find("## Adding a new code");
    ASSERT_NE(cat5, std::string::npos);
    ASSERT_NE(row, std::string::npos);
    ASSERT_NE(next, std::string::npos);
    EXPECT_LT(cat5, row) << "row must be under the category-5 heading";
    EXPECT_LT(row, next) << "row must be before the next section";
}
