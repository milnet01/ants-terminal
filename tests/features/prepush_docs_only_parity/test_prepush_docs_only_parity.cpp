// ANTS-4726 / ANTS-5322 — the pre-push hook's docs-only decision comes from
// ci.yml's push `paths-ignore`. It used to be a hand-maintained twin of that
// list; since ANTS-5322 the hook asks tools/ci_workflow.py, which reads ci.yml,
// so the invariant is that no second list exists and that the one decision is
// anchored the way the old regex was. See spec.md.

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"

#include <QProcess>
#include <QString>

#include <algorithm>
#include <set>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#ifndef SRC_CI_WORKFLOW_PATH
#error "SRC_CI_WORKFLOW_PATH compile definition required"
#endif
#ifndef ANTS_SOURCE_DIR
#error "ANTS_SOURCE_DIR compile definition required"
#endif
#ifndef SRC_PREPUSH_HOOK_PATH
#error "SRC_PREPUSH_HOOK_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {

std::string trim(std::string s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string unquote(std::string s) {
    if (s.size() >= 2 && (s.front() == '\'' || s.front() == '"')
        && s.back() == s.front())
        return s.substr(1, s.size() - 2);
    return s;
}

// ci.yml's push `paths-ignore` block: the list items between that key and the
// next key at the same or shallower indent. Scoped to the FIRST occurrence,
// which is the push trigger — the pull_request trigger deliberately has none.
std::set<std::string> ciPathsIgnore(const std::string &yaml) {
    std::set<std::string> out;
    std::istringstream in(yaml);
    std::string line;
    bool inBlock = false;
    size_t keyIndent = 0;
    while (std::getline(in, line)) {
        const size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        const std::string body = trim(line);
        if (body[0] == '#') continue;
        if (!inBlock) {
            if (body.rfind("paths-ignore:", 0) == 0) {
                inBlock   = true;
                keyIndent = first;
            }
            continue;
        }
        if (body[0] != '-' || first <= keyIndent) break;   // block ended
        out.insert(unquote(trim(body.substr(1))));
    }
    return out;
}

// Asks the runner, as the hook does. -1 when python3 or PyYAML is missing.
int docsOnly(const QString &paths) {
    QProcess p;
    p.start(QStringLiteral("python3"),
            {QStringLiteral(ANTS_SOURCE_DIR "/tools/ci_workflow.py"),
             QStringLiteral("docs-only")});
    if (!p.waitForStarted(5000)) return -1;
    p.write(paths.toUtf8());
    p.closeWriteChannel();
    if (!p.waitForFinished(20000)) return -1;
    return p.exitCode();
}

bool pyyamlPresent() {
    QProcess p;
    p.start(QStringLiteral("python3"), {QStringLiteral("-c"), QStringLiteral("import yaml")});
    return p.waitForFinished(10000) && p.exitCode() == 0;
}

}  // namespace

// INV-1 — the hook keeps no list of its own; it asks the runner, which reads
// ci.yml. A second list is the drift ANTS-4726 was filed about.
TEST(PrepushDocsOnlyParity, Inv1HookAsksTheWorkflowAndKeepsNoList) {
    expect_reset();
    const std::string sh = ants_test::slurpFile(SRC_PREPUSH_HOOK_PATH);
    ASSERT_FALSE(sh.empty());
    expect(sh.find("docs_only_re") == std::string::npos,
           "5322/no-hand-list", QString());
    expect(sh.find("docs-only <<<\"$changed\"") != std::string::npos,
           "5322/asks-the-runner", QString());
    ASSERT_EQ(0, expect_finish());
}

// INV-2 — the decision is anchored at the start of the path, and every literal
// ci.yml entry is honoured. Unanchored, any path containing `docs/` would read
// as documentation and skip a gate CI will run.
TEST(PrepushDocsOnlyParity, Inv2DecisionIsAnchoredAndFollowsTheWorkflow) {
    if (!pyyamlPresent()) GTEST_SKIP() << "python3 + PyYAML not available";
    expect_reset();
    const std::set<std::string> ci =
        ciPathsIgnore(ants_test::slurpFile(SRC_CI_WORKFLOW_PATH));
    expect(!ci.empty(), "4726/ci-paths-ignore-parsed", QString());
    for (const std::string &g : ci) {
        if (g.find('*') != std::string::npos) continue;   // globs: below
        expect(docsOnly(QString::fromStdString(g) + QStringLiteral("\n")) == 0,
               "5322/literal-entry-honoured", QString::fromStdString(g));
    }
    expect(docsOnly(QStringLiteral("docs/specs/x.md\n")) == 0,
           "5322/docs-subtree-is-docs", QString());
    expect(docsOnly(QStringLiteral("src/docs/x.md\n")) == 1,
           "4726/anchored", QStringLiteral("src/docs/x.md read as docs"));
    expect(docsOnly(QStringLiteral("ROADMAP.md\nsrc/a.cpp\n")) == 1,
           "5322/code-path-runs-the-gate", QString());
    ASSERT_EQ(0, expect_finish());
}
