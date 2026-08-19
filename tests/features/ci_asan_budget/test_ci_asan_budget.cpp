// ANTS-4533 — ci.yml's build-asan job must stay inside its budget, and must
// go RED rather than `cancelled` when it does not. See spec.md for the
// measurements: 36 of 40 runs were cancelled at the 30-minute cap over two
// days, and a cancelled job neither reads as a failure nor saves its ccache.

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"

#include <regex>
#include <string>

#include <gtest/gtest.h>

#ifndef SRC_CI_WORKFLOW_PATH
#error "SRC_CI_WORKFLOW_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {

bool has(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

// The `build-asan` job's text, from its heading to the next top-level job.
// Slicing rather than parsing is deliberate (spec.md § What this does NOT
// cover): a rename defeats it loudly instead of passing silently.
std::string asanJob(const std::string &wf) {
    const std::size_t begin = wf.find("\n  build-asan:");
    if (begin == std::string::npos) return {};
    const std::size_t end = wf.find("\n  qt62-baseline:", begin);
    return wf.substr(begin, end == std::string::npos ? std::string::npos
                                                     : end - begin);
}

// The single `- name:` step block containing `needle`. Bounding this matters:
// the step AFTER the ccache save carries its own `if: always()`, so an
// unbounded search from the save onward passes even with the save's guard
// deleted -- caught by mutation probe, 2026-08-19.
std::string stepBlockContaining(const std::string &job,
                                const std::string &needle) {
    const std::size_t at = job.find(needle);
    if (at == std::string::npos) return {};
    const std::size_t begin = job.rfind("\n      - name:", at);
    if (begin == std::string::npos) return {};
    const std::size_t end = job.find("\n      - name:", at);
    return job.substr(begin, end == std::string::npos ? std::string::npos
                                                      : end - begin);
}

// The one line carrying `ctest`, so a flag assertion cannot be satisfied by
// an unrelated line elsewhere in the job.
std::string ctestLine(const std::string &job) {
    const std::size_t at = job.find("ctest ");
    if (at == std::string::npos) return {};
    const std::size_t bol = job.rfind('\n', at);
    const std::size_t eol = job.find('\n', at);
    return job.substr(bol + 1, eol == std::string::npos ? std::string::npos
                                                        : eol - bol - 1);
}

}  // namespace

// INV-1 — the sanitized suite runs in parallel and caps each test.
TEST(CiAsanBudget, Inv1SanitizedCtestIsParallelAndPerTestCapped) {
    const std::string job = asanJob(ants_test::slurpFile(SRC_CI_WORKFLOW_PATH));
    ASSERT_FALSE(job.empty()) << "no `build-asan:` job in .github/workflows/ci.yml";

    const std::string line = ctestLine(job);
    ASSERT_FALSE(line.empty()) << "build-asan runs no ctest at all";

    EXPECT_TRUE(std::regex_search(line, std::regex(R"(-j\s*[0-9]+)")))
        << "build-asan's ctest is SERIAL: " << line
        << "\n  Measured 922s for 3650 tests on a 4-vCPU runner, dominated by "
           "per-process startup under ASan. Parallelism is the only lever at "
           "this test count, and the pre-push hook has used -j2 since "
           "ANTS-3761.";

    EXPECT_TRUE(has(line, "--timeout"))
        << "build-asan's ctest has no per-test cap: " << line
        << "\n  Without it one hung test consumes the whole job budget and the "
           "run reports nothing about which test hung.";
}

// INV-2 — an overrun exits non-zero, so the run is RED and not `cancelled`.
TEST(CiAsanBudget, Inv2AnOverrunFailsRatherThanCancels) {
    const std::string job = asanJob(ants_test::slurpFile(SRC_CI_WORKFLOW_PATH));
    ASSERT_FALSE(job.empty());

    for (const char *cmd : {"cmake --build build-asan", "ctest "}) {
        const std::size_t at = job.find(cmd);
        ASSERT_NE(at, std::string::npos) << "build-asan no longer runs: " << cmd;
        const std::size_t bol = job.rfind('\n', at);
        const std::string line = job.substr(bol + 1, at - bol - 1);
        EXPECT_TRUE(has(line, "timeout "))
            << "`" << cmd << "` is not wrapped in `timeout`, so an overrun is "
               "killed by the job-level timeout-minutes instead. That "
               "concludes `cancelled`, NOT `failure` — which is exactly why "
               "this job failed unnoticed for two days (ANTS-4533).";
    }
}

// INV-3 — the ccache is saved even when the job does not complete, or a
// timeout deepens the spiral that caused it.
TEST(CiAsanBudget, Inv3CcacheIsSavedEvenWhenTheJobDoesNotComplete) {
    const std::string job = asanJob(ants_test::slurpFile(SRC_CI_WORKFLOW_PATH));
    ASSERT_FALSE(job.empty());

    ASSERT_NE(job.find("actions/cache/save@"), std::string::npos)
        << "build-asan uses actions/cache's implicit post-step to save. That "
           "post-step is SKIPPED when the job is cancelled, so a timed-out run "
           "leaves the cache frozen at the last completing run and every later "
           "build starts colder — measured 11m29s -> 16m42s in six hours.";

    const std::string block = stepBlockContaining(job, "actions/cache/save@");
    ASSERT_FALSE(block.empty()) << "the ccache save is not inside a named step";
    EXPECT_TRUE(has(block, "if: always()"))
        << "the ccache save step is not guarded `if: always()`, so it does not "
           "run on the overrun it exists to survive:\n"
        << block;
}

// INV-4 — the step budgets sum below the job cap, or the job-level cancel
// fires first and silently undoes INV-2.
TEST(CiAsanBudget, Inv4StepBudgetsSumBelowTheJobCap) {
    const std::string job = asanJob(ants_test::slurpFile(SRC_CI_WORKFLOW_PATH));
    ASSERT_FALSE(job.empty());

    std::smatch cap;
    ASSERT_TRUE(std::regex_search(job, cap,
                                  std::regex(R"(timeout-minutes:\s*([0-9]+))")))
        << "build-asan declares no job-level timeout-minutes";
    const int jobCap = std::stoi(cap[1].str());

    int stepBudget = 0;
    const std::regex step(R"(timeout\s+([0-9]+)m\b)");
    for (auto it = std::sregex_iterator(job.begin(), job.end(), step);
         it != std::sregex_iterator(); ++it) {
        stepBudget += std::stoi((*it)[1].str());
    }
    ASSERT_GT(stepBudget, 0) << "no `timeout <N>m` step guards found (see INV-2)";

    EXPECT_LT(stepBudget, jobCap)
        << "the step guards total " << stepBudget << "m against a " << jobCap
        << "m job cap, so the job-level timeout fires FIRST and the run "
           "concludes `cancelled` again. Raise timeout-minutes above the sum, "
           "or lower a step guard — the guards must be what fires.";
}
