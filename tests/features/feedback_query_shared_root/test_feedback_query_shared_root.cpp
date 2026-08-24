// ANTS-4471 — a feedback_query miss must offer candidates, or say why it
// cannot. See tests/features/feedback_query_shared_root/spec.md.
//
// The reported repro: `feedback_query {caller_cwd:"/home/ants/.claude"}` derives
// /home/ants/.claude_Ants_MCP_Feedback.md, so the ONLY directory searched was
// /home/ants — which holds no feedback file — while the real file is
// claude_config_Ants_MCP_Feedback.md under a scripts tree. Two mismatches at
// once: the corpus is not the caller's parent, and the leaf ".claude" is not
// the file's "claude_config". Nothing in the path connects them.
//
// The verb was never missing the candidate feature (ANTS-3366 shipped it); the
// feature had nothing to find. So the fix is WHERE it looks, plus an actionable
// answer when it still finds nothing. The session that hit this fell back to a
// shell `ls`.
//
// The topology here is deliberately the reported one: the corpus is NOT under
// the caller's parent, so a test that put them together would pass against the
// old code and prove nothing.

#include "../../_support/xdg_guard.h"

#include "config.h"
#include "remotecontrol.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>

namespace {

bool writeFile(const QString &path, const QByteArray &body) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool ok = (f.write(body) == body.size());
    f.close();
    return ok;
}

QByteArray corpusFile() {
    return "<!-- ants-mcp-feedback: 2 -->\n"
           "# Ants MCP Feedback — Other\n\n"
           "## 2026-08-24 — s\n\n"
           "### Finding A\n\n- **What:** a.\n- **Proposed ID:**\n";
}

// The reported shape, built in two UNRELATED branches of one temp tree:
//   <tmp>/caller/proj      — the project asking (its parent holds no feedback)
//   <tmp>/corpus           — where the *_Ants_MCP_Feedback.md files actually live
struct Layout {
    QString callerProj;
    QString corpusDir;
    QString corpusFilePath;
};

Layout makeLayout(const QTemporaryDir &tmp) {
    Layout l;
    l.callerProj = QDir(tmp.path()).filePath(QStringLiteral("caller/proj"));
    l.corpusDir  = QDir(tmp.path()).filePath(QStringLiteral("corpus"));
    l.corpusFilePath =
        QDir(l.corpusDir).filePath(QStringLiteral("other_Ants_MCP_Feedback.md"));
    QDir().mkpath(l.callerProj);
    return l;
}

QJsonObject queryFor(const QString &root) {
    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    return rc.cmdFeedbackQuery(req).object();
}

// Sandboxes XDG_CONFIG_HOME so Config reads and writes a throwaway
// config.json. Test mode is deliberately NOT enabled: QStandardPaths ignores
// XDG_CONFIG_HOME when it is, which would send Config at the real user file.
void sandboxConfig(ants_test::XdgGuard &g, const QTemporaryDir &tmp) {
    g.setEnv("XDG_CONFIG_HOME",
             QDir(tmp.path()).filePath(QStringLiteral("cfg")).toUtf8());
}

}  // namespace

// INV-1 — with no key set, a miss is ACTIONABLE rather than terminal: it names
// the directory it searched and the key that would widen the search.
TEST(feedback_query_shared_root, Inv1MissNamesWhatItSearchedAndHow) {
    ants_test::XdgGuard g;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    sandboxConfig(g, tmp);
    const Layout l = makeLayout(tmp);
    ASSERT_TRUE(writeFile(l.corpusFilePath, corpusFile()));

    const QJsonObject env = queryFor(l.callerProj);
    // ANTS-4104 — a DERIVED-path miss is ok:true with found:false ("nobody has
    // filed anything here yet" is the state every project starts in), not a
    // refusal. The candidate fields ride that envelope unchanged.
    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool());
    ASSERT_FALSE(env.value(QStringLiteral("found")).toBool())
        << "no feedback file for this project — the verb must not claim one";

    // The reported dead end, closed. `found:false` with nothing else is equally
    // consistent with "no file yet", "wrong basename" and "wrong directory".
    const QJsonArray searched = env.value(QStringLiteral("searched")).toArray();
    ASSERT_EQ(searched.size(), 1)
        << "with no key set, exactly one directory is searched and the "
           "envelope must say which";
    EXPECT_EQ(searched.at(0).toString(),
              QFileInfo(l.callerProj).absoluteDir().absolutePath());

    const QString hint = env.value(QStringLiteral("hint")).toString();
    EXPECT_TRUE(hint.contains(QStringLiteral("claude.mcp_feedback_root")))
        << "the hint must name the key that fixes this: " << hint.toStdString();
}

// INV-2 — with the key set, the corpus IS found, though it lives in a branch
// no path rule could have reached from caller_cwd.
TEST(feedback_query_shared_root, Inv2ConfiguredRootIsSearched) {
    ants_test::XdgGuard g;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    sandboxConfig(g, tmp);
    const Layout l = makeLayout(tmp);
    ASSERT_TRUE(writeFile(l.corpusFilePath, corpusFile()));

    {   // Point the key at the corpus, as a user would.
        Config cfg;
        cfg.setClaudeMcpFeedbackRoot(l.corpusDir);
        ASSERT_EQ(cfg.claudeMcpFeedbackRoot(), l.corpusDir)
            << "the key must round-trip through the sandboxed config file";
    }

    const QJsonObject env = queryFor(l.callerProj);
    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool());
    ASSERT_FALSE(env.value(QStringLiteral("found")).toBool());

    const QJsonArray cands = env.value(QStringLiteral("candidates")).toArray();
    ASSERT_FALSE(cands.isEmpty())
        << "the corpus is one directory away and the key points at it — this "
           "is the shell `ls` the reporter had to run by hand";
    bool found = false;
    for (const auto &c : cands)
        if (c.toString() == l.corpusFilePath) found = true;
    EXPECT_TRUE(found) << "the configured root's file must be offered";
}

// INV-3 — setting the key does not COST the default directory. Both are
// searched, so configuring it cannot break the common case (a project sitting
// beside its corpus), and a root that IS the parent yields one entry, not two.
TEST(feedback_query_shared_root, Inv3DefaultDirStillSearchedAndDeduped) {
    ants_test::XdgGuard g;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    sandboxConfig(g, tmp);
    const Layout l = makeLayout(tmp);

    // A feedback file in the DEFAULT location (the caller's parent), and the
    // key pointed at that very same directory.
    const QString parent = QFileInfo(l.callerProj).absoluteDir().absolutePath();
    const QString sibling =
        QDir(parent).filePath(QStringLiteral("neighbour_Ants_MCP_Feedback.md"));
    ASSERT_TRUE(writeFile(sibling, corpusFile()));
    {
        Config cfg;
        cfg.setClaudeMcpFeedbackRoot(parent);
    }

    const QJsonObject env = queryFor(l.callerProj);
    const QJsonArray cands = env.value(QStringLiteral("candidates")).toArray();
    ASSERT_FALSE(cands.isEmpty())
        << "the default directory must still be searched when a key is set";
    int hits = 0;
    for (const auto &c : cands)
        if (c.toString() == sibling) ++hits;
    EXPECT_EQ(hits, 1)
        << "scanning one directory twice must not offer the same file twice";
}
