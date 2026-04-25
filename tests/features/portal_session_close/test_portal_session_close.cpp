// Feature-conformance test for spec.md —
//
// Source-grep test. GlobalShortcutsPortal must declare a destructor
// in the header AND define one in the cpp that issues an async
// Session.Close on a non-empty session handle.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#ifndef SRC_PORTAL_H_PATH
#  error "SRC_PORTAL_H_PATH compile definition required"
#endif
#ifndef SRC_PORTAL_CPP_PATH
#  error "SRC_PORTAL_CPP_PATH compile definition required"
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

// Slice from "GlobalShortcutsPortal::~GlobalShortcutsPortal()" up
// to the matched closing brace.
std::string extractDtor(const std::string &src) {
    const size_t at = src.find(
        "GlobalShortcutsPortal::~GlobalShortcutsPortal()");
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
    const std::string hdr = slurp(SRC_PORTAL_H_PATH);
    const std::string cpp = slurp(SRC_PORTAL_CPP_PATH);

    // I1 — header declares the destructor with override.
    expect(contains(hdr, "~GlobalShortcutsPortal() override;"),
           "I1/header-declares-virtual-dtor-with-override");

    // I3 — kSessionIface constant exists in the cpp anonymous
    // namespace.
    expect(contains(cpp,
               "kSessionIface = \"org.freedesktop.portal.Session\""),
           "I3/kSessionIface-constant-defined");

    // I2 + I4 — destructor body checks emptiness and dispatches the
    // asyncCall through the Session.Close member.
    const std::string dtor = extractDtor(cpp);
    expect(!dtor.empty(), "extract/dtor-body-found");

    expect(contains(dtor, "if (m_sessionHandle.isEmpty()) return;"),
           "I4/early-return-on-empty-session-handle");

    // The destructor MUST construct the QDBusMessage targeting
    // kSessionIface + "Close" (not kIface — that's GlobalShortcuts,
    // not Session).
    expect(contains(dtor, "QDBusMessage::createMethodCall("),
           "I2/dispatches-via-createMethodCall");
    expect(contains(dtor, "kSessionIface"),
           "I2/uses-kSessionIface-constant");
    expect(contains(dtor, "QStringLiteral(\"Close\")"),
           "I2/calls-Close-member");
    expect(contains(dtor, "m_bus.asyncCall(msg)"),
           "I2/uses-asyncCall-fire-and-forget");

    if (g_failures) {
        std::fprintf(stderr, "\n%d invariant(s) failed\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nall invariants hold\n");
    return 0;
}
