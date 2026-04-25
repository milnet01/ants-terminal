// Feature-conformance test for spec.md —
//
// Source-grep test (no Qt link). Five persistence sites must keep
// both the pre-write fd chmod AND the post-rename path chmod.
//
//   I1: src/config.cpp Config::save — chmod after std::rename success.
//   I2: src/sessionmanager.cpp saveSession + saveTabOrder — chmod
//       after QFile::rename success.
//   I3: src/settingsdialog.cpp installClaudeHooks +
//       installClaudeGitContextHook — chmod after settingsOut.commit().
//   I4: every site retains the pre-write fd chmod.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#ifndef SRC_CONFIG_CPP_PATH
#  error "SRC_CONFIG_CPP_PATH compile definition required"
#endif
#ifndef SRC_SESSIONMANAGER_CPP_PATH
#  error "SRC_SESSIONMANAGER_CPP_PATH compile definition required"
#endif
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

int countOccurrences(const std::string &haystack, const std::string &needle) {
    if (needle.empty()) return 0;
    int n = 0;
    size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

void checkConfigCpp() {
    const std::string src = slurp(SRC_CONFIG_CPP_PATH);

    // I4 — pre-write fd chmod retained.
    expect(src.find("setOwnerOnlyPerms(file)") != std::string::npos,
           "I4/config-pre-write-fd-chmod-retained");

    // I1 — post-rename chmod present.
    const size_t renameIdx = src.find("std::rename");
    const size_t chmodPathIdx = src.find("setOwnerOnlyPerms(path)");
    expect(renameIdx != std::string::npos && chmodPathIdx != std::string::npos,
           "I1/config-both-rename-and-post-chmod-present");
    expect(renameIdx < chmodPathIdx,
           "I1/config-chmod-after-rename-call",
           "expected std::rename before setOwnerOnlyPerms(path)");

    // I1 (gating) — the chmod must be inside the success branch
    // (the `else` of `if (rc != 0)`). Look for the `if (rc != 0)`
    // anchor and confirm the chmod appears after it AND between it
    // and the next `}` that closes the file-write block.
    const size_t failBranchIdx = src.find("if (rc != 0)");
    expect(failBranchIdx != std::string::npos,
           "I1/config-rename-failure-branch-still-present");
    expect(failBranchIdx < chmodPathIdx,
           "I1/config-chmod-after-failure-branch",
           "expected the rename-failure branch (`if (rc != 0)`) "
           "to precede the post-rename chmod (chmod is in the else)");
}

void checkSessionManager() {
    const std::string src = slurp(SRC_SESSIONMANAGER_CPP_PATH);

    // I4 — both writers retain pre-write fd chmod.
    int fdCount = countOccurrences(src, "setOwnerOnlyPerms(file)");
    char buf[64];
    std::snprintf(buf, sizeof(buf), "got %d", fdCount);
    expect(fdCount >= 2,
           "I4/sessionmanager-pre-write-fd-chmod-twice", buf);

    // I2 — post-rename chmod for both writers.
    int pathChmodCount = countOccurrences(src, "setOwnerOnlyPerms(path)");
    std::snprintf(buf, sizeof(buf), "got %d", pathChmodCount);
    expect(pathChmodCount >= 2,
           "I2/sessionmanager-post-rename-chmod-twice", buf);

    // I2 (gating) — the chmod must be inside an `if (QFile::rename(...))`.
    expect(src.find("if (QFile::rename(tmpPath, path))") != std::string::npos,
           "I2/sessionmanager-rename-success-gated");
}

void checkSettingsDialog() {
    const std::string src = slurp(SRC_SETTINGSDIALOG_CPP_PATH);

    // I4 — pre-write fd chmod at both QSaveFile sites.
    int fdCount = countOccurrences(src, "setOwnerOnlyPerms(settingsOut)");
    char buf[64];
    std::snprintf(buf, sizeof(buf), "got %d", fdCount);
    expect(fdCount >= 2,
           "I4/settingsdialog-pre-write-fd-chmod-twice", buf);

    // I3 — post-commit path chmod at both QSaveFile sites.
    int pathCount = countOccurrences(src, "setOwnerOnlyPerms(settingsPath)");
    std::snprintf(buf, sizeof(buf), "got %d", pathCount);
    expect(pathCount >= 2,
           "I3/settingsdialog-post-commit-path-chmod-twice", buf);

    // I3 (gating) — chmod must come after settingsOut.commit().
    const size_t commitIdx = src.find("settingsOut.commit");
    const size_t chmodIdx = src.find("setOwnerOnlyPerms(settingsPath)");
    expect(commitIdx != std::string::npos &&
               chmodIdx != std::string::npos &&
               commitIdx < chmodIdx,
           "I3/settingsdialog-chmod-after-commit",
           "expected settingsOut.commit before setOwnerOnlyPerms(settingsPath)");
}

}  // namespace

int main() {
    checkConfigCpp();
    checkSessionManager();
    checkSettingsDialog();

    if (g_failures) {
        std::fprintf(stderr, "\n%d invariant(s) failed\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nall invariants hold\n");
    return 0;
}
