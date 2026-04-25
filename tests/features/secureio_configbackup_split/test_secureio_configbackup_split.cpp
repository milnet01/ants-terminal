// Feature-conformance test for spec.md —
//
// Source-grep test (no Qt). Verifies the secureio.h / configbackup.h
// split:
//   I1: secureio.h is perms-only.
//   I2: configbackup.h owns rotation + lock and ConfigWriteLock is
//       non-copyable.
//   I3: every rotateCorruptFileAside caller includes configbackup.h.
//   I4: setOwnerOnlyPerms callers continue to include secureio.h.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#ifndef SRC_SECUREIO_H_PATH
#  error "SRC_SECUREIO_H_PATH compile definition required"
#endif
#ifndef SRC_CONFIGBACKUP_H_PATH
#  error "SRC_CONFIGBACKUP_H_PATH compile definition required"
#endif
#ifndef SRC_CONFIG_CPP_PATH
#  error "SRC_CONFIG_CPP_PATH compile definition required"
#endif
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

}  // namespace

int main() {
    const std::string secureio  = slurp(SRC_SECUREIO_H_PATH);
    const std::string backup    = slurp(SRC_CONFIGBACKUP_H_PATH);
    const std::string config    = slurp(SRC_CONFIG_CPP_PATH);
    const std::string allowlist = slurp(SRC_CLAUDEALLOWLIST_CPP_PATH);
    const std::string settings  = slurp(SRC_SETTINGSDIALOG_CPP_PATH);

    // I1 — secureio.h has perms helpers but NOT rotation / lock.
    expect(contains(secureio, "setOwnerOnlyPerms(QFileDevice &f)"),
           "I1/secureio-defines-setOwnerOnlyPerms-fd-overload");
    expect(contains(secureio, "setOwnerOnlyPerms(const QString &path)"),
           "I1/secureio-defines-setOwnerOnlyPerms-path-overload");
    expect(!contains(secureio, "rotateCorruptFileAside"),
           "I1/secureio-does-NOT-define-rotation",
           "rotateCorruptFileAside leaked back into secureio.h");
    expect(!contains(secureio, "class ConfigWriteLock"),
           "I1/secureio-does-NOT-define-ConfigWriteLock",
           "ConfigWriteLock leaked back into secureio.h");

    // I2 — configbackup.h owns rotation + lock + non-copyable lock.
    expect(contains(backup, "rotateCorruptFileAside"),
           "I2/configbackup-defines-rotation");
    expect(contains(backup, "class ConfigWriteLock"),
           "I2/configbackup-defines-ConfigWriteLock-class");
    expect(contains(backup,
               "ConfigWriteLock(const ConfigWriteLock &) = delete"),
           "I2/ConfigWriteLock-non-copyable-ctor");
    expect(contains(backup,
               "ConfigWriteLock &operator=(const ConfigWriteLock &) = delete"),
           "I2/ConfigWriteLock-non-copyable-assign");
    expect(contains(backup, "::flock("),
           "I2/ConfigWriteLock-uses-POSIX-flock");

    // I3 — every rotation caller includes configbackup.h.
    expect(contains(config, "#include \"configbackup.h\""),
           "I3/config.cpp-includes-configbackup");
    expect(contains(allowlist, "#include \"configbackup.h\""),
           "I3/claudeallowlist.cpp-includes-configbackup");
    expect(contains(settings, "#include \"configbackup.h\""),
           "I3/settingsdialog.cpp-includes-configbackup");

    // I4 — perms-helper callers retain the secureio.h include. Each
    // of these files calls setOwnerOnlyPerms (config.cpp + allowlist
    // do; settingsdialog also does after the 0.7.31 chmod hardening).
    expect(contains(config, "#include \"secureio.h\""),
           "I4/config.cpp-still-includes-secureio");
    expect(contains(allowlist, "#include \"secureio.h\""),
           "I4/claudeallowlist.cpp-still-includes-secureio");
    expect(contains(settings, "#include \"secureio.h\""),
           "I4/settingsdialog.cpp-still-includes-secureio");

    if (g_failures) {
        std::fprintf(stderr, "\n%d invariant(s) failed\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nall invariants hold\n");
    return 0;
}
