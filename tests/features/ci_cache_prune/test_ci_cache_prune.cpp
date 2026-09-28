// ANTS-5532 — CI keeps one compile cache per job, and never deletes the only
// one. See spec.md: every push saved a new per-SHA entry, the repository
// passed GitHub's 10 GB limit, and eviction took the nightly ASan cache.

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"

#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>

#include <regex>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#ifndef SRC_CI_WORKFLOW_PATH
#error "SRC_CI_WORKFLOW_PATH compile definition required"
#endif
#ifndef SRC_CI_PRUNE_SCRIPT_PATH
#error "SRC_CI_PRUNE_SCRIPT_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {

bool has(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

// Each top-level job's text, heading to next heading. Slicing, as in
// ci_asan_budget: a rename defeats it loudly rather than passing silently.
std::vector<std::string> jobBlocks(const std::string &wf) {
    std::vector<std::string> out;
    const std::size_t jobs = wf.find("\njobs:\n");
    if (jobs == std::string::npos) return out;
    const std::regex head(R"(\n  [a-z0-9-]+:\n)");
    std::vector<std::size_t> starts;
    for (auto it = std::sregex_iterator(wf.begin() + static_cast<long>(jobs),
                                        wf.end(), head);
         it != std::sregex_iterator(); ++it)
        starts.push_back(jobs + static_cast<std::size_t>(it->position(0)));
    for (std::size_t i = 0; i < starts.size(); ++i) {
        const std::size_t end = i + 1 < starts.size() ? starts[i + 1] : wf.size();
        out.push_back(wf.substr(starts[i], end - starts[i]));
    }
    return out;
}

// The `- name:` step block starting at or after `from`.
std::string stepAt(const std::string &job, std::size_t at) {
    const std::size_t begin = job.rfind("\n      - name:", at);
    const std::size_t end = job.find("\n      - name:", at);
    if (begin == std::string::npos) return {};
    return job.substr(begin, end == std::string::npos ? std::string::npos
                                                      : end - begin);
}

std::string keyOf(const std::string &step) {
    std::smatch m;
    if (!std::regex_search(step, m, std::regex(R"(\n\s+key:\s*(\S[^\n]*))")))
        return {};
    return m[1].str();
}

struct PruneRun {
    int exitCode = -1;
    std::string out;
    std::string deletes;
};

// Runs the prune script with a fake `gh` first on PATH. The fake prints
// `listing` for the list call and appends each DELETE path to a log.
// `fail` is "list", "delete" or empty.
PruneRun runPrune(const std::string &listing, const std::string &keep,
             const std::string &fail = {}) {
    PruneRun r;
    QTemporaryDir dir;
    if (!dir.isValid()) return r;
    const QString log = dir.filePath(QStringLiteral("deletes.log"));
    QFile fake(dir.filePath(QStringLiteral("gh")));
    if (!fake.open(QIODevice::WriteOnly)) return r;
    fake.write(
        "#!/usr/bin/env bash\n"
        "if [[ \" $* \" == *\" DELETE \"* ]]; then\n"
        "  [[ $FAKE_FAIL == delete ]] && exit 1\n"
        "  echo \"${@: -1}\" >> \"$FAKE_LOG\"; exit 0\n"
        "fi\n"
        "[[ $FAKE_FAIL == list ]] && exit 1\n"
        "printf '%s' \"$FAKE_LIST\"\n");
    fake.close();
    fake.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PATH"),
               dir.path() + QLatin1Char(':') + env.value(QStringLiteral("PATH")));
    env.insert(QStringLiteral("GITHUB_REPOSITORY"), QStringLiteral("o/r"));
    env.insert(QStringLiteral("GITHUB_REF"), QStringLiteral("refs/heads/main"));
    env.insert(QStringLiteral("FAKE_LIST"), QString::fromStdString(listing));
    env.insert(QStringLiteral("FAKE_LOG"), log);
    env.insert(QStringLiteral("FAKE_FAIL"), QString::fromStdString(fail));

    QProcess p;
    p.setProcessEnvironment(env);
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(QStringLiteral("bash"),
            {QStringLiteral(SRC_CI_PRUNE_SCRIPT_PATH),
             QStringLiteral("P-"), QString::fromStdString(keep)});
    if (!p.waitForFinished(20000)) { p.kill(); return r; }
    r.exitCode = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -2;
    r.out = p.readAll().toStdString();
    r.deletes = ants_test::slurpFile(log.toStdString());
    return r;
}

}  // namespace

// INV-1 — every cache save is followed by a prune of the same prefix, in a
// job that grants actions: write.
TEST(CiCachePrune, Inv1EverySaveIsFollowedByAPrune) {
    const std::string wf = ants_test::slurpFile(SRC_CI_WORKFLOW_PATH);
    ASSERT_FALSE(wf.empty()) << SRC_CI_WORKFLOW_PATH;

    int saves = 0;
    for (const std::string &job : jobBlocks(wf)) {
        for (std::size_t at = job.find("actions/cache/save@");
             at != std::string::npos;
             at = job.find("actions/cache/save@", at + 1)) {
            ++saves;
            const std::string key = keyOf(stepAt(job, at));
            ASSERT_FALSE(key.empty()) << "a cache save with no key:\n" << job;
            const std::size_t sha = key.rfind("${{ github.sha }}");
            ASSERT_NE(sha, std::string::npos) << "save key not per-SHA: " << key;
            const std::string prefix = key.substr(0, sha);

            const std::size_t prune = job.find("tools/ci-prune-caches.sh", at);
            ASSERT_NE(prune, std::string::npos)
                << "no prune after the save of " << key
                << "\n  Each push saves a new per-SHA entry and keys cannot be "
                   "overwritten, so without a prune the job's entries pile up "
                   "until GitHub evicts another job's cache (ANTS-5532).";
            const std::string step = stepAt(job, prune);
            EXPECT_TRUE(has(step, "CACHE_KEY: " + key))
                << "the prune after " << key << " keeps a different key:\n" << step;
            EXPECT_TRUE(has(step, "CACHE_PREFIX: " + prefix + "\n"))
                << "the prune after " << key << " names a different prefix:\n"
                << step;
            EXPECT_TRUE(has(step, "if: always()"))
                << "the prune after " << key << " is skipped on a failed build, "
                   "though the save before it runs:\n" << step;
            EXPECT_TRUE(has(job, "actions: write"))
                << "the job saving " << key << " cannot delete caches without "
                   "`actions: write`";
        }
    }
    EXPECT_GE(saves, 4) << "fewer cache saves than build-test, build-asan, "
                           "qt62-baseline and cppcheck carry";
}

// INV-2 — a kept key missing from the listing (a failed save) deletes nothing.
TEST(CiCachePrune, Inv2NeverDeletesTheOnlyCache) {
    const PruneRun r = runPrune("1 P-a\n2 P-b\n", "P-c");
    EXPECT_EQ(r.exitCode, 0) << r.out;
    EXPECT_TRUE(r.deletes.empty())
        << "deleted with the kept key unsaved:\n" << r.deletes;
    EXPECT_TRUE(has(r.out, "deleting nothing")) << r.out;
}

// INV-3 — with the kept key present, every other entry is deleted by id.
TEST(CiCachePrune, Inv3DeletesEveryOtherEntry) {
    const PruneRun r = runPrune("1 P-a\n2 P-b\n3 P-c\n", "P-c");
    EXPECT_EQ(r.exitCode, 0) << r.out;
    EXPECT_EQ(r.deletes,
              "repos/o/r/actions/caches/1\nrepos/o/r/actions/caches/2\n")
        << r.out;
}

// INV-4 — a listing or delete failure warns and exits 0.
TEST(CiCachePrune, Inv4FailureWarnsAndDoesNotFail) {
    for (const char *fail : {"list", "delete"}) {
        const PruneRun r = runPrune("1 P-a\n2 P-c\n", "P-c", fail);
        EXPECT_EQ(r.exitCode, 0) << fail << ": " << r.out;
        EXPECT_TRUE(has(r.out, "::warning::")) << fail << ": " << r.out;
    }
}
