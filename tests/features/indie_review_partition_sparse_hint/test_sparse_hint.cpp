// ANTS-3567 — feature-conformance test: cmdIndieReviewPartition emits a
// sparse_partition_hint pointing at indie_review_brief's source_paths[] ad-hoc
// mode (ANTS-3375) + the .indie-review/partition.json override, mirroring
// cmdColdEyesPartition's ANTS-1634a hint. Mostly a source-scrape harness, like
// the sibling mcp_cold_eyes test; the ANTS-6639 case runs the handler.

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"

#include <gtest/gtest.h>

#include "remotecontrol.h"
#include "rootprovider.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <string>

ANTS_TEST_SCOPE();

namespace {

// Scope a lookup to a named function body: from its definition to the next
// top-level `\n}\n` (matches the mcp_cold_eyes FoldIn test's approach).
std::string bodyOf(const std::string &src, const std::string &fnMarker) {
    const auto pos = src.find(fnMarker);
    if (pos == std::string::npos) return {};
    const auto end = src.find("\n}\n", pos);
    if (end == std::string::npos) return src.substr(pos);
    return src.substr(pos, end - pos);
}

}  // namespace

// INV-1/2 — the hint lives inside cmdIndieReviewPartition and names both the
// review-code skill and the override file.
TEST(indie_review_partition_sparse_hint, HintMentionsAdhocAndOverride) {
    expect_reset();
    const std::string rc = ants_test::slurpRemoteControl();
    const std::string body =
        bodyOf(rc, "RemoteControl::cmdIndieReviewPartition");
    expect(!body.empty(),
           "cmdIndieReviewPartition body present in remotecontrol.cpp");
    expect(body.find("sparse_partition_hint") != std::string::npos,
           "INV: cmdIndieReviewPartition emits sparse_partition_hint");
    expect(body.find("ANTS-3567") != std::string::npos,
           "INV: ANTS-3567 anchor present");
    // ANTS-6226 — indie_review_brief was removed (ANTS-5485); the hint names
    // what replaced it.
    expect(body.find("review-code skill") != std::string::npos,
           "INV-1: hint points at the review-code skill");
    expect(body.find("indie_review_brief") == std::string::npos,
           "INV-1: no hint names the removed indie_review_brief verb");
    expect(body.find(".indie-review/partition.json") != std::string::npos,
           "INV-2: hint points at the override file");
    EXPECT_EQ(0, expect_failures());
}

// INV-3 — the hint is gated on a ≤1-lane partition (the sparse case), mirroring
// cmdColdEyesPartition's `lanes.size() <= 1` gate, not emitted unconditionally.
TEST(indie_review_partition_sparse_hint, GatedOnSparsePartition) {
    expect_reset();
    const std::string rc = ants_test::slurpRemoteControl();
    const std::string body =
        bodyOf(rc, "RemoteControl::cmdIndieReviewPartition");
    expect(!body.empty(),
           "cmdIndieReviewPartition body present");
    expect(body.find("lanes.size() <= 1") != std::string::npos,
           "INV-3: hint gated on a ≤1-lane (sparse) partition");
    expect(body.find("sparse_partition") != std::string::npos,
           "INV-3: sparse_partition boolean flag emitted alongside the hint");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-4846 — the partition reply names a rejected override instead of
// reporting its path as the source it used.
TEST(indie_review_partition_sparse_hint, Ants4846RejectedOverrideIsReported) {
    expect_reset();
    const std::string rc = ants_test::slurpRemoteControl();
    const std::string body =
        bodyOf(rc, "RemoteControl::cmdIndieReviewPartition");
    expect(!body.empty(), "cmdIndieReviewPartition body present");
    expect(body.find("IndieReviewEngine::partitionOverrideRejection(root)") !=
               std::string::npos,
           "ANTS-4846: the reply asks the engine whether the override was used");
    expect(body.find("\"map_rejected\"") != std::string::npos,
           "ANTS-4846: a rejected override is reported as map_rejected");
    expect(body.find("overrideRejected.isEmpty() &&") != std::string::npos,
           "ANTS-4846: `path` names the override only when it was used");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-6639 — run for real: a one-lane .indie-review/partition.json is
// reported as the override, not the module map.
TEST(indie_review_partition_sparse_hint, Ants6639OneLaneOverrideIsNamedOverride) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const auto write = [&](const QString &rel, const QByteArray &body) {
        const QString p = QDir(tmp.path()).filePath(rel);
        QDir().mkpath(QFileInfo(p).absolutePath());
        QFile f(p);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(body);
    };
    write(QStringLiteral("src/a.cpp"), "int a() { return 0; }\n");
    write(QStringLiteral(".indie-review/partition.json"),
          R"({"version":1,"lanes":[{"name":"core","summary":"s",)"
          R"("sourcePaths":["src/a.cpp"]}]})");

    ants::ServerCwdRootProvider roots;
    RemoteControl rc(nullptr, nullptr, &roots);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    const QJsonObject out = rc.cmdIndieReviewPartition(req).object();
    ASSERT_TRUE(out.value(QStringLiteral("sparse_partition")).toBool())
        << QJsonDocument(out).toJson().toStdString();
    EXPECT_EQ(out.value(QStringLiteral("partition_source")).toString(),
              QStringLiteral("override"))
        << QJsonDocument(out).toJson().toStdString();
}
