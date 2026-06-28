// ANTS-2062 — shared RAII guard for test isolation.
//
// Generalises the copy-pasted `XdgConfigHomeGuard` that several config
// tests carry: saves + restores QStandardPaths test mode AND arbitrary
// process environment variables (XDG_CONFIG_HOME, XDG_CACHE_HOME, TMPDIR,
// KDE_FULL_SESSION, …) so a TEST that mutates global process state doesn't
// leak it into sibling tests sharing the same gtest bundle binary. Several
// real order-dependent failures (e.g. UiStatePersistence first-launch
// defaults polluted by a sibling's leftover config) trace to exactly this.
//
// Header-only; restore happens in reverse order on scope exit, even when a
// fatal gtest ASSERT unwinds the TEST early (the win over hand-rolled
// teardown at the bottom of the function body).
//
// Usage:
//   ants_test::XdgGuard g;
//   g.setTestMode(true);                        // restored on scope exit
//   g.setEnv("XDG_CONFIG_HOME", dir.toUtf8());  // restored on scope exit

#pragma once

#include <QByteArray>
#include <QStandardPaths>

#include <utility>
#include <vector>

namespace ants_test {

class XdgGuard {
public:
    XdgGuard() = default;
    ~XdgGuard() {
        // Restore env in reverse order of first touch.
        for (auto it = m_env.rbegin(); it != m_env.rend(); ++it) {
            if (it->hadPrior) qputenv(it->name.constData(), it->prior);
            else qunsetenv(it->name.constData());
        }
        if (m_testModeSaved)
            QStandardPaths::setTestModeEnabled(m_priorTestMode);
    }

    XdgGuard(const XdgGuard &) = delete;
    XdgGuard &operator=(const XdgGuard &) = delete;
    // Move-only: lets a sandbox helper return a live guard to its caller
    // (`auto g = makeSandbox(tmp);`). The source is disarmed so the
    // moved-from object's destructor restores nothing (no double-restore).
    XdgGuard(XdgGuard &&o) noexcept
        : m_env(std::move(o.m_env)),
          m_testModeSaved(o.m_testModeSaved),
          m_priorTestMode(o.m_priorTestMode) {
        o.m_env.clear();
        o.m_testModeSaved = false;
    }
    XdgGuard &operator=(XdgGuard &&) = delete;

    // Set / unset an env var, snapshotting its prior value first.
    void setEnv(const char *name, const QByteArray &value) {
        snapshot(name);
        qputenv(name, value);
    }
    void unsetEnv(const char *name) {
        snapshot(name);
        qunsetenv(name);
    }

    // Snapshot an env var's prior value without changing it now — useful
    // when the test mutates it through another API (e.g. qputenv called
    // by production code) but still wants automatic restore.
    void guardEnv(const char *name) { snapshot(name); }

    // Save + set QStandardPaths test mode.
    void setTestMode(bool on) {
        if (!m_testModeSaved) {
            m_priorTestMode = QStandardPaths::isTestModeEnabled();
            m_testModeSaved = true;
        }
        QStandardPaths::setTestModeEnabled(on);
    }

private:
    struct Entry { QByteArray name; QByteArray prior; bool hadPrior{}; };

    void snapshot(const char *name) {
        for (const auto &e : m_env)
            if (e.name == name) return;  // first touch wins
        Entry e;
        e.name     = name;
        e.hadPrior = qEnvironmentVariableIsSet(name);
        if (e.hadPrior) e.prior = qgetenv(name);
        m_env.push_back(std::move(e));
    }

    std::vector<Entry> m_env;
    bool m_testModeSaved = false;
    bool m_priorTestMode = false;
};

}  // namespace ants_test
