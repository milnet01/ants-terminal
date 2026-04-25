// Feature-conformance test for spec.md —
//
// Source-grep test (no Qt). Mirrors the Config parse-failure guard
// coverage to claudeallowlist.cpp + settingsdialog.cpp:
//
//   I1: claudeallowlist.cpp refuse-to-save on parse failure.
//   I2: settingsdialog.cpp two installers each refuse-to-write on
//       parse failure.
//   I3: read-OPEN failure also refuses (not only parse failure).

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#ifndef SRC_CLAUDEALLOWLIST_CPP_PATH
#  error "SRC_CLAUDEALLOWLIST_CPP_PATH compile definition required"
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

}  // namespace

int main() {
    const std::string allowlist = slurp(SRC_CLAUDEALLOWLIST_CPP_PATH);
    const std::string settings  = slurp(SRC_SETTINGSDIALOG_CPP_PATH);

    // I1 — claudeallowlist.cpp refuses on parse failure.
    expect(contains(allowlist, "rotateCorruptFileAside(m_settingsPath)"),
           "I1/allowlist-rotates-corrupt-file");
    // The rotation must be followed (within ~500 chars) by an early
    // `return false;` so the write path is unreachable on the
    // parse-failure branch.
    {
        std::regex rotateThenReturnFalse(
            R"(rotateCorruptFileAside\(m_settingsPath\)[\s\S]{0,1500}?return\s+false\s*;)");
        expect(std::regex_search(allowlist, rotateThenReturnFalse),
               "I1/allowlist-returns-false-after-rotation");
    }

    // I3 (allowlist) — open-failure branch also refuses. Look for a
    // QFile::open(QIODevice::ReadOnly) on m_settingsPath that, when it
    // fails, returns false.
    expect(contains(allowlist,
               "exists but cannot be") ||
               contains(allowlist, "exists but could not"),
           "I3/allowlist-handles-open-failure-distinct-from-parse",
           "expected an early-exit branch for the open-failure case");
    expect(contains(allowlist,
               "(would risk clobbering"),
           "I3/allowlist-comment-explains-clobber-risk");

    // I2 — settingsdialog.cpp: TWO installers each call
    // rotateCorruptFileAside(settingsPath) — one per installer.
    int rotationCount = countMatches(
        settings, "rotateCorruptFileAside(settingsPath)");
    expect(rotationCount >= 2,
           "I2/settingsdialog-two-installers-rotate-corrupt",
           "got " + std::to_string(rotationCount));

    // I2 (continued) — each rotation must be followed by an early
    // `return;` (void context, not `return false`). The dialog
    // installers are void.
    {
        std::regex rotateThenReturn(
            R"(rotateCorruptFileAside\(settingsPath\)[\s\S]{0,2500}?return\s*;)");
        // We need TWO matches — one per installer. The simplest way
        // is to count how many rotations are followed by a `return;`
        // within the window, by sliding the search forward.
        int matched = 0;
        auto begin = std::sregex_iterator(settings.begin(), settings.end(),
                                          rotateThenReturn);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) ++matched;
        expect(matched >= 2,
               "I2/settingsdialog-each-installer-returns-after-rotation",
               "got " + std::to_string(matched));
    }

    // I3 (settingsdialog) — open-failure branch must exist (refuses
    // when the file exists but can't be opened).
    expect(contains(settings, "Could not read") ||
               contains(settings, "exists but"),
           "I3/settingsdialog-handles-open-failure");
    expect(contains(settings, "refusing to overwrite"),
           "I3/settingsdialog-comment-explains-refuse");

    if (g_failures) {
        std::fprintf(stderr, "\n%d invariant(s) failed\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nall invariants hold\n");
    return 0;
}
