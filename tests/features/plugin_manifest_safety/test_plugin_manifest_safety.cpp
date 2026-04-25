// Feature-conformance test for spec.md —
//
// Source-grep test. PluginManager::scanAndLoad must:
//   I1: cap manifest reads at 1 MiB via kMaxManifestBytes.
//   I2: pass QDir::NoSymLinks to entryList.
//   I3: anchor on canonicalFilePath of m_pluginDir.
//   I4: per-entry canonical-containment reject.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#ifndef SRC_PLUGINMANAGER_CPP_PATH
#  error "SRC_PLUGINMANAGER_CPP_PATH compile definition required"
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

std::string extractFn(const std::string &src, const std::string &fnSig) {
    const size_t at = src.find(fnSig);
    if (at == std::string::npos) return {};
    const size_t openBrace = src.find('{', at);
    if (openBrace == std::string::npos) return {};
    int depth = 1;
    size_t i = openBrace + 1;
    while (i < src.size() && depth > 0) {
        char c = src[i];
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
    const std::string src = slurp(SRC_PLUGINMANAGER_CPP_PATH);
    const std::string body = extractFn(src,
        "void PluginManager::scanAndLoad(const QStringList &enabledList)");
    expect(!body.empty(), "extract/scanAndLoad-body-found");

    // I1 — manifest cap.
    expect(contains(body, "kMaxManifestBytes"),
           "I1/kMaxManifestBytes-named-constant");
    expect(contains(body, "1024 * 1024"),
           "I1/cap-value-1MiB");
    expect(contains(body, "f.read(kMaxManifestBytes)"),
           "I1/uses-bounded-read-not-readAll");
    expect(!contains(body, "f.readAll(), &err"),
           "I1/no-unbounded-readAll-feeding-fromJson");

    // I2 — NoSymLinks flag.
    expect(contains(body, "QDir::NoSymLinks"),
           "I2/entryList-passes-NoSymLinks");

    // I3 — canonical root anchor.
    expect(contains(body,
               "QFileInfo(m_pluginDir).canonicalFilePath()"),
           "I3/canonicalFilePath-on-m_pluginDir");
    expect(contains(body, "canonicalRoot"),
           "I3/canonicalRoot-named");

    // I4 — per-entry canonical-containment.
    expect(contains(body, "QFileInfo(pluginPath).canonicalFilePath()"),
           "I4/per-entry-canonical-resolution");
    expect(contains(body, "canonicalRootPrefix"),
           "I4/canonical-root-prefix-named");
    expect(contains(body, "startsWith(canonicalRootPrefix)"),
           "I4/canonical-startsWith-containment-check");
    // The qWarning format string spans two adjacent C-string
    // literals at the line wrap, so "resolves outside the plugin"
    // is the contiguous substring we can grep for. The full
    // sentence "resolves outside the plugin root" only exists
    // post-preprocessor concatenation.
    expect(contains(body, "resolves outside the plugin"),
           "I4/reject-warning-message");

    if (g_failures) {
        std::fprintf(stderr, "\n%d invariant(s) failed\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nall invariants hold\n");
    return 0;
}
