// Why this exists: locks in the 0.6.34 fix that flipped
// Config::sessionPersistence()'s default from false → true. Prior to
// the fix, fresh users (no `session_persistence` key in config.json)
// silently had tabs thrown away on exit, because the accessor
// returned `.toBool(false)`. User report: "The terminal is no longer
// remembering tabs between sessions." See spec.md §INV-1.
//
// The test drives Config through its public API only — it writes a
// controlled config.json under a throwaway XDG_CONFIG_HOME, constructs
// a Config (whose ctor calls load()), and asserts sessionPersistence()
// returns the expected value for each of three input states:
//
//   INV-1: no key           -> true   (the regression-critical case)
//   INV-2: key = false      -> false  (explicit opt-out honoured)
//   INV-3: key = true       -> true   (explicit opt-in honoured)
//
// No GUI, no mocking — Qt6::Core is the only link-time dep alongside
// src/config.cpp. Config uses QStandardPaths::ConfigLocation which,
// on Linux, honours $XDG_CONFIG_HOME; we set it to a per-test temp dir
// BEFORE constructing any Config instance so the real user config is
// never read or written.

#include "config.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <cstdio>

namespace {

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL [%s]: %s (line %d)\n",                  \
                         __FUNCTION__, msg, __LINE__);                         \
            ++failures;                                                        \
        }                                                                     \
    } while (0)

// Overwrite (or delete) the config.json that lives under the test's
// XDG_CONFIG_HOME. `contents` == nullptr means "no file at all" — the
// INV-1 fresh-user case. Otherwise the bytes are written verbatim.
//
// Must be called *between* Config instances: Config reads from disk
// exactly once, in its constructor (Config::load()), so each scenario
// needs its own fresh Config.
bool writeConfig(const QString &xdgHome, const char *contents) {
    const QString dir = xdgHome + "/ants-terminal";
    if (!QDir().mkpath(dir)) return false;
    const QString path = dir + "/config.json";
    QFile::remove(path);  // INV-1: ensure no key at all when contents==nullptr.
    if (!contents) return true;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const QByteArray bytes = QByteArray(contents);
    const bool ok = (f.write(bytes) == bytes.size());
    f.close();
    return ok;
}

// INV-1: missing key → true.
int runFreshUserDefaultsTrue(const QString &xdgHome) {
    int failures = 0;
    CHECK(writeConfig(xdgHome, nullptr), "could not clear config.json");

    Config cfg;
    const bool got = cfg.sessionPersistence();
    if (got != true) {
        std::fprintf(stderr,
                     "  INV-1: fresh config (no key) should return true, "
                     "got %s\n", got ? "true" : "false");
        ++failures;
    }
    return failures;
}

// INV-2: key=false → false.
int runExplicitOptOut(const QString &xdgHome) {
    int failures = 0;
    CHECK(writeConfig(xdgHome,
                      "{\"session_persistence\": false}\n"),
          "could not write opt-out config.json");

    Config cfg;
    const bool got = cfg.sessionPersistence();
    if (got != false) {
        std::fprintf(stderr,
                     "  INV-2: explicit false should return false, "
                     "got %s\n", got ? "true" : "false");
        ++failures;
    }
    return failures;
}

// INV-3: key=true → true.
int runExplicitOptIn(const QString &xdgHome) {
    int failures = 0;
    CHECK(writeConfig(xdgHome,
                      "{\"session_persistence\": true}\n"),
          "could not write opt-in config.json");

    Config cfg;
    const bool got = cfg.sessionPersistence();
    if (got != true) {
        std::fprintf(stderr,
                     "  INV-3: explicit true should return true, "
                     "got %s\n", got ? "true" : "false");
        ++failures;
    }
    return failures;
}

}  // namespace

int main() {
    // Isolate Config's reads/writes from the real user config. Must
    // happen BEFORE any Config is constructed — Config's ctor calls
    // load() which resolves QStandardPaths::ConfigLocation once.
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::fprintf(stderr, "FATAL: QTemporaryDir creation failed\n");
        return 2;
    }
    if (!qputenv("XDG_CONFIG_HOME", tmp.path().toLocal8Bit())) {
        std::fprintf(stderr, "FATAL: qputenv XDG_CONFIG_HOME failed\n");
        return 2;
    }
    // QStandardPaths caches paths; clear any inherited cache so our
    // env override takes effect for every subsequent resolution.
    QStandardPaths::setTestModeEnabled(false);

    int failures = 0;
    failures += runFreshUserDefaultsTrue(tmp.path());
    failures += runExplicitOptOut(tmp.path());
    failures += runExplicitOptIn(tmp.path());

    if (failures == 0) {
        std::printf("session_persistence_default: INV-1/2/3 pass "
                    "(fresh=true, opt-out=false, opt-in=true)\n");
        return 0;
    }
    std::fprintf(stderr,
                 "session_persistence_default: %d failure(s)\n", failures);
    return 1;
}
