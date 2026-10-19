// ANTS-5771 — shared RAII guard for the process umask.
//
// The umask is process-wide, so a test that sets one for a permissions check
// must put the old one back or every later test in the bundle inherits it.
// Restoring on scope exit also covers a fatal gtest ASSERT that unwinds the
// test early. XdgGuard (xdg_guard.h) is the same idea for the environment.
//
// Usage:
//   ants_test::ScopedUmask um(0022);   // previous umask restored on exit

#pragma once

#include <sys/stat.h>
#include <sys/types.h>

namespace ants_test {

class ScopedUmask {
public:
    explicit ScopedUmask(mode_t mask) : m_previous(::umask(mask)) {}
    ~ScopedUmask() { ::umask(m_previous); }
    ScopedUmask(const ScopedUmask &) = delete;
    ScopedUmask &operator=(const ScopedUmask &) = delete;

private:
    mode_t m_previous;
};

}  // namespace ants_test
