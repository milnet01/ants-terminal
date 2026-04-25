// Feature-conformance test for spec.md —
//
// Source-grep test. Every primary tab must declare a Restore
// Defaults QPushButton with a stable objectName, and the reset
// slot must mutate widgets only (no m_config-> calls inside the
// reset lambda body).

#include <cstdio>
#include <cstdlib>
#include <fstream>
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

// Locate the restore-defaults lambda body for a given objectName.
// We anchor on `setObjectName(QStringLiteral("<name>"))` and walk
// to the next `connect(...)`'s lambda body.
std::string extractResetLambda(const std::string &src,
                               const std::string &objectName) {
    const std::string anchor = "setObjectName(QStringLiteral(\""
                             + objectName + "\"))";
    const size_t a = src.find(anchor);
    if (a == std::string::npos) return {};
    // Find next "[this]() {" after the anchor, which is the reset
    // lambda body.
    const size_t lambda = src.find("[this]() {", a);
    if (lambda == std::string::npos) return {};
    const size_t openBrace = src.find('{', lambda);
    if (openBrace == std::string::npos) return {};
    int depth = 1;
    size_t i = openBrace + 1;
    while (i < src.size() && depth > 0) {
        const char c = src[i];
        if (c == '"') {
            ++i;
            while (i < src.size() && src[i] != '"') {
                if (src[i] == '\\') ++i;
                if (i < src.size()) ++i;
            }
            if (i < src.size()) ++i;
            continue;
        }
        if (c == '{') ++depth;
        else if (c == '}') --depth;
        ++i;
    }
    return src.substr(openBrace, i - openBrace);
}

}  // namespace

int main() {
    const std::string src = slurp(SRC_SETTINGSDIALOG_CPP_PATH);

    // I1 — every primary tab declares its Restore Defaults button.
    expect(contains(src,
               "setObjectName(QStringLiteral(\"restoreDefaultsGeneral\"))"),
           "I1/general-restore-defaults-button");
    expect(contains(src,
               "setObjectName(QStringLiteral(\"restoreDefaultsAppearance\"))"),
           "I1/appearance-restore-defaults-button");
    expect(contains(src,
               "setObjectName(QStringLiteral(\"restoreDefaultsTerminal\"))"),
           "I1/terminal-restore-defaults-button");
    expect(contains(src,
               "setObjectName(QStringLiteral(\"restoreDefaultsAi\"))"),
           "I1/ai-restore-defaults-button");

    // I2 — each tab's reset slot resets the documented controls.
    {
        const std::string body = extractResetLambda(src, "restoreDefaultsGeneral");
        expect(!body.empty(), "extract/general-reset-body");
        expect(contains(body, "m_sessionPersistence->setChecked(true)"),
               "I2/general-resets-sessionPersistence");
        expect(contains(body, "m_autoCopy->setChecked(true)"),
               "I2/general-resets-autoCopy");
        expect(contains(body, "m_notificationTimeout->setValue(5)"),
               "I2/general-resets-notificationTimeout");
        // I3 — no m_config-> in the reset body.
        expect(!contains(body, "m_config->"),
               "I3/general-reset-no-direct-config-write");
    }
    {
        const std::string body = extractResetLambda(src, "restoreDefaultsAppearance");
        expect(!body.empty(), "extract/appearance-reset-body");
        expect(contains(body, "m_fontSize->setValue(11)"),
               "I2/appearance-resets-fontSize");
        expect(contains(body, "m_themeCombo->setCurrentText(QStringLiteral(\"Dark\"))"),
               "I2/appearance-resets-theme");
        expect(contains(body, "m_opacitySlider->setValue(100)"),
               "I2/appearance-resets-opacity");
        expect(contains(body, "m_paddingSpinner->setValue(4)"),
               "I2/appearance-resets-padding");
        expect(!contains(body, "m_config->"),
               "I3/appearance-reset-no-direct-config-write");
    }
    {
        const std::string body = extractResetLambda(src, "restoreDefaultsTerminal");
        expect(!body.empty(), "extract/terminal-reset-body");
        expect(contains(body, "m_scrollbackLines->setValue(50000)"),
               "I2/terminal-resets-scrollback");
        expect(contains(body, "m_showCommandMarks->setChecked(true)"),
               "I2/terminal-resets-showCommandMarks");
        expect(contains(body, "m_quakeMode->setChecked(false)"),
               "I2/terminal-resets-quakeMode");
        expect(contains(body, "m_quakeHotkey->setText(QStringLiteral(\"F12\"))"),
               "I2/terminal-resets-quakeHotkey");
        expect(!contains(body, "m_config->"),
               "I3/terminal-reset-no-direct-config-write");
    }
    {
        const std::string body = extractResetLambda(src, "restoreDefaultsAi");
        expect(!body.empty(), "extract/ai-reset-body");
        expect(contains(body, "m_aiEnabled->setChecked(false)"),
               "I2/ai-resets-aiEnabled");
        expect(contains(body, "m_aiModel->setText(QStringLiteral(\"llama3\"))"),
               "I2/ai-resets-aiModel");
        expect(contains(body, "m_aiContextLines->setValue(50)"),
               "I2/ai-resets-aiContextLines");
        expect(!contains(body, "m_config->"),
               "I3/ai-reset-no-direct-config-write");
    }

    if (g_failures) {
        std::fprintf(stderr, "\n%d invariant(s) failed\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nall invariants hold\n");
    return 0;
}
