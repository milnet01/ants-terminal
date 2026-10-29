// Feature-conformance test for spec.md —
//
// Source-grep test (no Qt link). Five persistence sites must keep
// both the pre-write fd chmod AND the post-rename path chmod.
//
//   I1: src/secureio.h writeFileAtomicOwnerOnly — chmod after
//       std::rename success.
//   I2: src/config.cpp and src/sessionmanager.cpp write through it.
//   I3: src/settingsdialog.cpp installClaudeHooks +
//       installClaudeGitContextHook — chmod after settingsOut.commit().
//   I4: every site retains the pre-write fd chmod.

#include "../../_support/expect.h"

#include <cstdio>
#include <string>


#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"
#ifndef SRC_SECUREIO_H_PATH
#  error "SRC_SECUREIO_H_PATH compile definition required"
#endif
#ifndef SRC_CONFIG_CPP_PATH
#  error "SRC_CONFIG_CPP_PATH compile definition required"
#endif
#ifndef SRC_SESSIONMANAGER_CPP_PATH
#  error "SRC_SESSIONMANAGER_CPP_PATH compile definition required"
#endif
#ifndef SRC_CLAUDESETUP_CPP_PATH
#  error "SRC_CLAUDESETUP_CPP_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {




int countOccurrences(const std::string &haystack, const std::string &needle) {
    if (needle.empty()) return 0;
    int n = 0;
    size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

void checkSharedWriter() {
    const std::string src = ants_test::slurpFile(SRC_SECUREIO_H_PATH);
    const size_t fnIdx = src.find("inline bool writeFileAtomicOwnerOnly(");
    expect(fnIdx != std::string::npos, "I1/shared-writer-present");
    if (fnIdx == std::string::npos) return;
    const std::string body = src.substr(fnIdx);

    // I4 — pre-write fd chmod retained.
    const size_t fdIdx = body.find("setOwnerOnlyPerms(file)");
    // I1 — post-rename chmod, after the rename-failure branch returns.
    const size_t renameIdx = body.find("std::rename(tmpPath.toLocal8Bit().constData()");
    const size_t failBranchIdx = body.find("if (rc != 0)");
    const size_t chmodPathIdx = body.find("setOwnerOnlyPerms(path)");
    expect(fdIdx != std::string::npos, "I4/shared-writer-pre-write-fd-chmod");
    expect(renameIdx != std::string::npos && failBranchIdx != std::string::npos
               && chmodPathIdx != std::string::npos,
           "I1/shared-writer-rename-failure-branch-and-chmod-present");
    expect(fdIdx < renameIdx && renameIdx < failBranchIdx
               && failBranchIdx < chmodPathIdx,
           "I1/shared-writer-order",
           "expected fd chmod, std::rename, `if (rc != 0)`, then "
           "setOwnerOnlyPerms(path)");
}

void checkCallers() {
    char buf[64];
    const std::string cfg = ants_test::slurpFile(SRC_CONFIG_CPP_PATH);
    int n = countOccurrences(cfg, "writeFileAtomicOwnerOnly(");
    std::snprintf(buf, sizeof(buf), "got %d", n);
    expect(n >= 1, "I2/config-writes-through-shared-writer", buf);

    const std::string sm = ants_test::slurpFile(SRC_SESSIONMANAGER_CPP_PATH);
    n = countOccurrences(sm, "writeFileAtomicOwnerOnly(");
    std::snprintf(buf, sizeof(buf), "got %d", n);
    expect(n >= 2, "I2/sessionmanager-writes-through-shared-writer-twice", buf);
}

void checkSettingsDialog() {
    // ANTS-5558 — the two hook installers moved to ants::claude_setup and
    // write settings.json through one helper, writeClaudeSettings.
    const std::string src = ants_test::slurpFile(SRC_CLAUDESETUP_CPP_PATH);

    // I4 — pre-write fd chmod in the shared writer.
    int fdCount = countOccurrences(src, "setOwnerOnlyPerms(settingsOut)");
    char buf[64];
    std::snprintf(buf, sizeof(buf), "got %d", fdCount);
    expect(fdCount >= 1,
           "I4/claudesetup-pre-write-fd-chmod", buf);

    // I3 — post-commit path chmod in the shared writer.
    int pathCount = countOccurrences(src, "setOwnerOnlyPerms(settingsPath)");
    std::snprintf(buf, sizeof(buf), "got %d", pathCount);
    expect(pathCount >= 1,
           "I3/claudesetup-post-commit-path-chmod", buf);

    // Both installers write through it.
    int writerCalls = countOccurrences(src, "writeClaudeSettings(settingsPath, root)");
    std::snprintf(buf, sizeof(buf), "got %d", writerCalls);
    expect(writerCalls >= 2,
           "I3/claudesetup-both-installers-write-through-it", buf);

    // I3 (gating) — chmod must come after settingsOut.commit().
    const size_t commitIdx = src.find("settingsOut.commit");
    const size_t chmodIdx = src.find("setOwnerOnlyPerms(settingsPath)");
    expect(commitIdx != std::string::npos &&
               chmodIdx != std::string::npos &&
               commitIdx < chmodIdx,
           "I3/settingsdialog-chmod-after-commit",
           "expected settingsOut.commit before setOwnerOnlyPerms(settingsPath)");
}

}  // namespace

static int runMain() {
    expect_reset();
    checkSharedWriter();
    checkCallers();
    checkSettingsDialog();

    return expect_finish();
}

TEST(PersistencePostRenameChmod, Main) {
    ASSERT_EQ(0, runMain());
}
