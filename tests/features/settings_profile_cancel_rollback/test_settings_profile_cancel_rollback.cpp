// Feature-conformance test for spec.md —
//
// Source-grep test. The Profiles tab's Save/Delete/Load buttons
// must route mutations through the pending-state pair, and only
// applySettings may call m_config->setProfiles / setActiveProfile.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#ifndef SRC_SETTINGSDIALOG_CPP_PATH
#  error "SRC_SETTINGSDIALOG_CPP_PATH compile definition required"
#endif
#ifndef SRC_SETTINGSDIALOG_H_PATH
#  error "SRC_SETTINGSDIALOG_H_PATH compile definition required"
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

int countMatches(const std::string &h, const std::string &n) {
    if (n.empty()) return 0;
    int c = 0;
    size_t pos = 0;
    while ((pos = h.find(n, pos)) != std::string::npos) {
        ++c;
        pos += n.size();
    }
    return c;
}

// Extract the body of `setupProfilesTab` so we can assert what's
// inside that function specifically (vs. applySettings or
// loadSettings, which are allowed to call setProfiles).
std::string extractFnBody(const std::string &src, const std::string &fnName) {
    const size_t fnStart = src.find("void SettingsDialog::" + fnName);
    if (fnStart == std::string::npos) return {};
    const size_t openBrace = src.find('{', fnStart);
    if (openBrace == std::string::npos) return {};
    int depth = 1;
    size_t i = openBrace + 1;
    while (i < src.size() && depth > 0) {
        const char c = src[i];
        if (c == '"') {
            // Skip string literal.
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
    const std::string cpp = slurp(SRC_SETTINGSDIALOG_CPP_PATH);
    const std::string hdr = slurp(SRC_SETTINGSDIALOG_H_PATH);

    // I1+I2+I3 (declarations) — pending state declared in the
    // header.
    expect(contains(hdr, "QJsonObject m_pendingProfiles"),
           "decl/m_pendingProfiles-declared-in-header");
    expect(contains(hdr, "QString     m_pendingActiveProfile") ||
               contains(hdr, "QString m_pendingActiveProfile"),
           "decl/m_pendingActiveProfile-declared-in-header");

    // Extract the relevant function bodies.
    const std::string profilesBody  = extractFnBody(cpp, "setupProfilesTab");
    const std::string applyBody     = extractFnBody(cpp, "applySettings");
    const std::string loadBody      = extractFnBody(cpp, "loadSettings");

    expect(!profilesBody.empty(), "extract/setupProfilesTab-body-found");
    expect(!applyBody.empty(),    "extract/applySettings-body-found");
    expect(!loadBody.empty(),     "extract/loadSettings-body-found");

    // I1+I2+I3 — setupProfilesTab body must NOT contain
    // m_config->setProfiles( or m_config->setActiveProfile(.
    // Those are reserved for applySettings.
    expect(!contains(profilesBody, "m_config->setProfiles("),
           "I1+I2/save-delete-do-NOT-mutate-m_config-directly",
           "Save/Delete must mutate m_pendingProfiles only");
    expect(!contains(profilesBody, "m_config->setActiveProfile("),
           "I3/load-does-NOT-mutate-m_config-directly",
           "Load must mutate m_pendingActiveProfile only");

    // I1+I2+I3 — pending state IS mutated.
    expect(contains(profilesBody, "m_pendingProfiles"),
           "I1+I2/profiles-buttons-mutate-pending-profiles-dict");
    expect(contains(profilesBody, "m_pendingActiveProfile"),
           "I3/load-button-mutates-pending-active-profile");

    // I4 — applySettings commits both pending fields.
    expect(contains(applyBody, "setProfiles(m_pendingProfiles)"),
           "I4/applySettings-commits-pendingProfiles");
    expect(contains(applyBody, "setActiveProfile(m_pendingActiveProfile)"),
           "I4/applySettings-commits-pendingActiveProfile");

    // I5 — loadSettings reinitializes pending state from m_config.
    expect(contains(loadBody, "m_pendingProfiles = m_config->profiles()"),
           "I5/loadSettings-reinits-pendingProfiles");
    expect(contains(loadBody, "m_pendingActiveProfile = m_config->activeProfile()"),
           "I5/loadSettings-reinits-pendingActiveProfile");

    // Sanity — the only `m_config->setProfiles(` call across the
    // whole file appears inside applySettings. countMatches across
    // the cpp should be 1.
    int setProfilesCalls = countMatches(cpp, "m_config->setProfiles(");
    expect(setProfilesCalls == 1,
           "global/single-setProfiles-call-site",
           "expected 1, got " + std::to_string(setProfilesCalls));

    int setActiveProfileCalls = countMatches(cpp, "m_config->setActiveProfile(");
    expect(setActiveProfileCalls == 1,
           "global/single-setActiveProfile-call-site",
           "expected 1, got " + std::to_string(setActiveProfileCalls));

    if (g_failures) {
        std::fprintf(stderr, "\n%d invariant(s) failed\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nall invariants hold\n");
    return 0;
}
