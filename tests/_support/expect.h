// ANTS-1382 — shared test-support header.
//
// Replaces the per-TU `int g_failures; void expect(bool, const char*, …)`
// helper that ~46 feature tests copy-paste, plus the
// `if (runMain() != 0) FAIL();` shim that ~50 of them use.
//
// Buffer-on-success: PASS labels are counted but NOT printed; only on
// the first FAIL do we flush a single `(N prior ok)` summary line, then
// the FAIL line. With `ctest --output-on-failure`, a 7-PASS + 1-FAIL
// test now produces 2 stderr lines instead of 8 — the FAIL signal is no
// longer buried under PASS noise. Major win for AI-assistant
// readability of failure tails (see ROADMAP.md ANTS-1382).
//
// Macros:
//   ANTS_TEST_SCOPE()              Emit per-TU `expect()`,
//                                  `expect_reset()`, `expect_finish()`
//                                  in an anonymous namespace. Use once
//                                  at file scope (outside any
//                                  namespace).
//
// Pattern for a converted test file:
//   #include "../../_support/expect.h"
//   ANTS_TEST_SCOPE();
//   namespace {
//     void runChecks() { expect(cond, "INV1"); … }
//     int runMain() { expect_reset(); runChecks(); return expect_finish(); }
//   }
//   TEST(SuiteName, Main) { ASSERT_EQ(0, runMain()); }

#pragma once

#include <QString>
#include <cstdio>
#include <string>  // IWYU pragma: keep — used by ANTS_TEST_SCOPE macro

namespace ants_test {

struct Scope {
    int failures = 0;
    int passes   = 0;

    void check(bool cond, const char *label, const char *detail) {
        if (cond) { ++passes; return; }
        flushPriorPasses();
        std::fprintf(stderr, "[FAIL] %s  %s\n",
                     label, detail ? detail : "");
        ++failures;
    }

    void reset() { failures = 0; passes = 0; }

    int finish() const {
        if (failures) {
            std::fprintf(stderr, "%d invariant(s) failed\n", failures);
        }
        return failures;
    }

private:
    void flushPriorPasses() {
        if (passes > 0) {
            std::fprintf(stderr, "(%d prior ok)\n", passes);
            passes = 0;
        }
    }
};

}  // namespace ants_test

// Per-TU helpers. The `static` storage on `s` is function-local, so
// each TU that includes this header in an anonymous namespace gets its
// own scope — no cross-test leakage even when several test_*.cpp files
// share a bundle binary.
#define ANTS_TEST_SCOPE()                                                  \
    namespace {                                                            \
        inline ants_test::Scope &expect_scope_() {                         \
            static ants_test::Scope s;                                     \
            return s;                                                      \
        }                                                                  \
        [[maybe_unused]] inline void expect(bool cond, const char *label,  \
                                            const char *detail = "") {    \
            expect_scope_().check(cond, label, detail);                    \
        }                                                                  \
        [[maybe_unused]] inline void expect(bool cond, const char *label,  \
                                            const QString &detail) {       \
            const QByteArray u = detail.toUtf8();                          \
            expect_scope_().check(cond, label, u.constData());             \
        }                                                                  \
        [[maybe_unused]] inline void expect(bool cond, const char *label,  \
                                            const std::string &detail) {   \
            expect_scope_().check(cond, label, detail.c_str());            \
        }                                                                  \
        [[maybe_unused]] inline void expect_reset() {                      \
            expect_scope_().reset();                                       \
        }                                                                  \
        [[maybe_unused]] inline int expect_finish() {                      \
            return expect_scope_().finish();                               \
        }                                                                  \
        [[maybe_unused]] inline int expect_failures() {                    \
            return expect_scope_().failures;                               \
        }                                                                  \
    }
