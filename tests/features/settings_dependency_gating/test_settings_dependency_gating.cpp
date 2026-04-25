// Feature-conformance test for spec.md —
//
// Source-grep portion only. The runtime portion would require
// constructing a SettingsDialog (QDialog subclass), which pulls in
// most of the Qt6::Widgets stack. The dependency-gating logic is
// trivial QCheckBox::toggled wiring; the regression class we want
// to catch is "someone deletes the connect() or the syncXxx()
// call". Source-grep covers that.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#ifndef SRC_SETTINGSDIALOG_CPP_PATH
#  error "SRC_SETTINGSDIALOG_CPP_PATH compile definition required"
#endif

namespace {

int g_failures = 0;

void expect(bool cond, const char *label, const std::string &detail = {}) {
    if (cond) {
        std::fprintf(stderr, "[PASS] %s\n", label);
    } else {
        std::fprintf(stderr, "[FAIL] %s  %s\n", label, detail.c_str());
        ++g_failures;
    }
}

std::string slurp(const char *path) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(2);
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool contains(const std::string &h, const std::string &n) {
    return h.find(n) != std::string::npos;
}

}  // namespace

int main() {
    const std::string src = slurp(SRC_SETTINGSDIALOG_CPP_PATH);

    // I1 — AI tab: master `m_aiEnabled` toggled-wired to
    // setEnabled on the four AI fields, plus a one-shot sync.
    expect(contains(src, "auto syncAi = [this]()"),
           "I1/ai-sync-lambda-defined");
    expect(contains(src, "m_aiEndpoint->setEnabled(on)"),
           "I1/ai-endpoint-gated");
    expect(contains(src, "m_aiApiKey->setEnabled(on)"),
           "I1/ai-apikey-gated");
    expect(contains(src, "m_aiModel->setEnabled(on)"),
           "I1/ai-model-gated");
    expect(contains(src, "m_aiContextLines->setEnabled(on)"),
           "I1/ai-context-lines-gated");
    {
        std::regex aiConnect(
            R"(connect\(\s*m_aiEnabled\s*,\s*&QCheckBox::toggled\s*,)");
        expect(std::regex_search(src, aiConnect),
               "I1/ai-toggled-connect");
    }
    // I4 — one-shot sync call.
    expect(contains(src, "syncAi();"),
           "I4/ai-initial-sync-call");

    // I2 — Auto color scheme.
    expect(contains(src, "auto syncAutoColor = [this]()"),
           "I2/autocolor-sync-lambda-defined");
    expect(contains(src, "m_darkThemeCombo->setEnabled(on)"),
           "I2/dark-combo-gated");
    expect(contains(src, "m_lightThemeCombo->setEnabled(on)"),
           "I2/light-combo-gated");
    {
        std::regex acConnect(
            R"(connect\(\s*m_autoColorScheme\s*,\s*&QCheckBox::toggled\s*,)");
        expect(std::regex_search(src, acConnect),
               "I2/autocolor-toggled-connect");
    }
    expect(contains(src, "syncAutoColor();"),
           "I4/autocolor-initial-sync-call");

    // I3 — Quake mode.
    expect(contains(src, "auto syncQuake = [this, portalStatus]()"),
           "I3/quake-sync-lambda-defined");
    expect(contains(src, "m_quakeHotkey->setEnabled(on)"),
           "I3/quake-hotkey-gated");
    {
        std::regex qConnect(
            R"(connect\(\s*m_quakeMode\s*,\s*&QCheckBox::toggled\s*,)");
        expect(std::regex_search(src, qConnect),
               "I3/quake-toggled-connect");
    }
    expect(contains(src, "syncQuake();"),
           "I4/quake-initial-sync-call");

    if (g_failures) {
        std::fprintf(stderr, "\n%d invariant(s) failed\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nall invariants hold\n");
    return 0;
}
