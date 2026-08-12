// ANTS-1922 — feature-conformance test for roadmap_query mode:"bundles".
// Behavioural invariants drive the pure static builder
// RemoteControl::buildRoadmapBundlesEnvelope directly with hand-authored
// bullet arrays; the dispatch/schema wiring (INV-6/7/8/12-seam) is
// source-grepped. See tests/features/mcp_roadmap_bundles/spec.md.

#include "remotecontrol.h"

#include <gtest/gtest.h>
#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <string>

ANTS_TEST_SCOPE();

namespace {

const QString kPlanned  = QString::fromUtf8("\xF0\x9F\x93\x8B");  // 📋
const QString kProgress = QString::fromUtf8("\xF0\x9F\x9A\xA7");  // 🚧
const QString kDone     = QString::fromUtf8("\xE2\x9C\x85");      // ✅

QJsonObject bullet(const QString &id, const QString &status,
                   const QString &headline,
                   const QStringList &lanes = {},
                   const QString &body = {},
                   const QString &kind = {}) {   // ANTS-3388 — Kind: facet
    QJsonObject o;
    o["id"] = id;
    o["status"] = status;
    o["headline"] = headline;
    o["headline_oneline"] = headline;   // test fixtures: oneline == headline
    QJsonArray la;
    for (const QString &l : lanes) la.append(l);
    o["lanes"] = la;
    o["body"] = body;
    o["kind"] = kind;
    return o;
}

// Locate an item object by id across all bundles (empty object if none).
QJsonObject findItem(const QJsonObject &env, const QString &id) {
    for (const auto &bv : env.value("bundles").toArray()) {
        for (const auto &iv : bv.toObject().value("items").toArray()) {
            const QJsonObject io = iv.toObject();
            if (io.value("id").toString() == id) return io;
        }
    }
    return {};
}

// The bundle (object) that contains a given id (empty object if none).
QJsonObject findBundle(const QJsonObject &env, const QString &id) {
    for (const auto &bv : env.value("bundles").toArray()) {
        const QJsonObject bo = bv.toObject();
        for (const auto &iv : bo.value("items").toArray()) {
            if (iv.toObject().value("id").toString() == id) return bo;
        }
    }
    return {};
}

bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

const int kCap = 20480;   // PaginationEngine::kSoftCapBytes default

}  // namespace

// INV-1 — six keys always present; active-only; no ✅ leaks in.
TEST(mcp_roadmap_bundles, Inv1KeysAndActiveOnly) {
    QJsonArray a;
    a.append(bullet("ANTS-1001", kPlanned,  "alpha beta gamma"));
    a.append(bullet("ANTS-1002", kProgress, "alpha beta delta"));
    a.append(bullet("ANTS-1003", kDone,     "shipped done thing"));
    const QJsonObject env = RemoteControl::buildRoadmapBundlesEnvelope(a, kCap);

    for (const char *k : {"ok", "mode", "bundles", "bundle_count",
                          "active_total", "truncated"})
        EXPECT_TRUE(env.contains(k)) << "missing key " << k;
    EXPECT_TRUE(env.value("ok").toBool());
    EXPECT_EQ(env.value("mode").toString(), QStringLiteral("bundles"));
    EXPECT_EQ(env.value("active_total").toInt(), 2);
    EXPECT_TRUE(findItem(env, "ANTS-1003").isEmpty()) << "✅ item must not appear";
    EXPECT_FALSE(findItem(env, "ANTS-1001").isEmpty());
}

// INV-2 — clustering, transitive closure, separation.
TEST(mcp_roadmap_bundles, Inv2ClusterTransitiveSeparate) {
    QJsonArray a;
    // (i) thematic cluster spanning 3 distinct lanes (each pair shares
    // {diff,viewer} = 2, jac = 2/4 = 0.50).
    a.append(bullet("ANTS-1864", kPlanned, "diff viewer gutter", {"difftools"}));
    a.append(bullet("ANTS-1874", kPlanned, "diff viewer scroll", {"diffviewer"}));
    a.append(bullet("ANTS-1875", kPlanned, "diff viewer inline", {"claudediff"}));
    // (ii) transitive A–B–C: A∩B and B∩C edge (0.50), A∩C share only
    // {gamma} (1 token) → no direct edge.
    a.append(bullet("ANTS-2001", kPlanned, "alpha beta gamma"));
    a.append(bullet("ANTS-2002", kPlanned, "beta gamma delta"));
    a.append(bullet("ANTS-2003", kPlanned, "gamma delta epsilon"));
    // (iii) two mutually-unrelated singletons.
    a.append(bullet("ANTS-3001", kPlanned, "config persistence layer"));
    a.append(bullet("ANTS-3002", kPlanned, "audio latency tuning"));

    const QJsonObject env = RemoteControl::buildRoadmapBundlesEnvelope(a, kCap);
    EXPECT_EQ(env.value("active_total").toInt(), 8);
    EXPECT_EQ(env.value("bundle_count").toInt(), 4);   // 3 + 3 + 1 + 1

    // diff cluster: all three in one bundle.
    const QJsonObject diff = findBundle(env, "ANTS-1864");
    EXPECT_EQ(diff.value("size").toInt(), 3);
    EXPECT_FALSE(findBundle(env, "ANTS-1874").value("items").toArray().isEmpty());
    EXPECT_EQ(findBundle(env, "ANTS-1875").value("size").toInt(), 3);

    // transitive chain: A and C share no direct edge but land together.
    const QJsonObject chain = findBundle(env, "ANTS-2001");
    EXPECT_EQ(chain.value("size").toInt(), 3);
    EXPECT_EQ(findBundle(env, "ANTS-2003").value("size").toInt(), 3);

    // singletons stay separate.
    EXPECT_EQ(findBundle(env, "ANTS-3001").value("size").toInt(), 1);
    EXPECT_EQ(findBundle(env, "ANTS-3002").value("size").toInt(), 1);
}

// INV-3 — sum of sizes equals active_total when not truncated.
TEST(mcp_roadmap_bundles, Inv3SumEqualsActive) {
    QJsonArray a;
    a.append(bullet("ANTS-1864", kPlanned, "diff viewer gutter"));
    a.append(bullet("ANTS-1874", kPlanned, "diff viewer scroll"));
    a.append(bullet("ANTS-3001", kPlanned, "config persistence layer"));
    a.append(bullet("ANTS-3002", kProgress, "audio latency tuning"));
    const QJsonObject env = RemoteControl::buildRoadmapBundlesEnvelope(a, kCap);
    EXPECT_FALSE(env.value("truncated").toBool());
    int sum = 0;
    for (const auto &bv : env.value("bundles").toArray())
        sum += bv.toObject().value("size").toInt();
    EXPECT_EQ(sum, env.value("active_total").toInt());
    EXPECT_EQ(sum, 4);
}

// INV-4 — possibly_resolved_by / score against the ✅ subset.
TEST(mcp_roadmap_bundles, Inv4PossiblyResolved) {
    QJsonArray a;
    // P1 vs S1: share {render,gutter,scroll} = 3, union 5 → jac 0.60.
    a.append(bullet("ANTS-4001", kPlanned, "render gutter scroll sync"));
    a.append(bullet("ANTS-4101", kDone,    "render gutter scroll inline"));
    // P2: no ✅ match.
    a.append(bullet("ANTS-4002", kPlanned, "audio latency tuning knob"));
    // P3 vs S3: share {config,disk} = 2, union 6 → jac 0.33 (below gate).
    a.append(bullet("ANTS-4003", kPlanned, "config persist disk write"));
    a.append(bullet("ANTS-4103", kDone,    "config backup disk read"));
    // P4 vs S4: identical → jac 1.0 → score 100.
    a.append(bullet("ANTS-4004", kPlanned, "menu shortcut binding panel"));
    a.append(bullet("ANTS-4104", kDone,    "menu shortcut binding panel"));

    const QJsonObject env = RemoteControl::buildRoadmapBundlesEnvelope(a, kCap);

    const QJsonObject p1 = findItem(env, "ANTS-4001");
    EXPECT_EQ(p1.value("possibly_resolved_by").toString(), QStringLiteral("ANTS-4101"));
    EXPECT_EQ(p1.value("possibly_resolved_score").toInt(), 60)
        << "exactly-0.60 must attach with score 60 (inclusive >= gate)";

    const QJsonObject p2 = findItem(env, "ANTS-4002");
    EXPECT_FALSE(p2.contains("possibly_resolved_by"));
    EXPECT_FALSE(p2.contains("possibly_resolved_score"));

    const QJsonObject p3 = findItem(env, "ANTS-4003");
    EXPECT_FALSE(p3.contains("possibly_resolved_by")) << "below-0.60 must not attach";

    const QJsonObject p4 = findItem(env, "ANTS-4004");
    EXPECT_EQ(p4.value("possibly_resolved_by").toString(), QStringLiteral("ANTS-4104"));
    EXPECT_EQ(p4.value("possibly_resolved_score").toInt(), 100);
}

// INV-5 — gate_note / blocked markers (directional only).
TEST(mcp_roadmap_bundles, Inv5GateNote) {
    QJsonArray a;
    a.append(bullet("ANTS-5001", kPlanned, "feature one work", {},
                    "blocked by ANTS-9999 (composer exposure)"));
    a.append(bullet("ANTS-5002", kPlanned, "feature two work", {},
                    "deferred until ANTS-8888 lands first"));
    a.append(bullet("ANTS-5003", kPlanned, "feature three work", {},
                    "clean implementation notes here"));
    a.append(bullet("ANTS-5004", kPlanned, "feature four work", {},
                    "blocks the cursor when active"));
    a.append(bullet("ANTS-5005", kPlanned, "feature five work", {},
                    "Wait for P10 to close before drafting"));
    const QJsonObject env = RemoteControl::buildRoadmapBundlesEnvelope(a, kCap);

    const QJsonObject g1 = findItem(env, "ANTS-5001");
    EXPECT_TRUE(g1.value("blocked").toBool());
    EXPECT_FALSE(g1.value("gate_note").toString().isEmpty());
    EXPECT_LE(g1.value("gate_note").toString().size(), 160);

    EXPECT_TRUE(findItem(env, "ANTS-5002").value("blocked").toBool())
        << "until+lands must flag blocked";

    EXPECT_FALSE(findItem(env, "ANTS-5003").contains("blocked"));
    EXPECT_FALSE(findItem(env, "ANTS-5004").contains("blocked"))
        << "bare 'blocks' is deliberately not a marker";

    // ANTS-3389 — plain-prose imperative gate, no marker formatting.
    EXPECT_TRUE(findItem(env, "ANTS-5005").value("blocked").toBool())
        << "'wait for X' prose must flag blocked";
}

// INV-9 — label non-empty + deterministic; lowest-id fallback.
TEST(mcp_roadmap_bundles, Inv9LabelDeterministicAndFallback) {
    QJsonArray a;
    a.append(bullet("ANTS-1864", kPlanned, "diff viewer gutter"));
    a.append(bullet("ANTS-1874", kPlanned, "diff viewer scroll"));
    a.append(bullet("ANTS-1875", kPlanned, "diff viewer inline"));
    // all-stop-word headline, no lanes → forces lowest-id fallback.
    a.append(bullet("ANTS-6001", kPlanned, "the for the"));

    const QJsonObject env1 = RemoteControl::buildRoadmapBundlesEnvelope(a, kCap);
    const QJsonObject env2 = RemoteControl::buildRoadmapBundlesEnvelope(a, kCap);

    for (const auto &bv : env1.value("bundles").toArray())
        EXPECT_FALSE(bv.toObject().value("bundle_label").toString().isEmpty());

    EXPECT_EQ(findBundle(env1, "ANTS-1864").value("bundle_label").toString(),
              QStringLiteral("diff viewer"));
    EXPECT_EQ(findBundle(env1, "ANTS-6001").value("bundle_label").toString(),
              QStringLiteral("ANTS-6001"))
        << "no-majority-token, no-lane bundle falls back to lowest member id";

    // deterministic: labels identical across calls.
    EXPECT_EQ(findBundle(env1, "ANTS-1864").value("bundle_label"),
              findBundle(env2, "ANTS-1864").value("bundle_label"));
}

// INV-10 — empty active set.
TEST(mcp_roadmap_bundles, Inv10EmptyActiveSet) {
    QJsonArray a;
    a.append(bullet("ANTS-7001", kDone, "shipped one"));
    a.append(bullet("ANTS-7002", kDone, "shipped two"));
    const QJsonObject env = RemoteControl::buildRoadmapBundlesEnvelope(a, kCap);
    EXPECT_EQ(env.value("active_total").toInt(), 0);
    EXPECT_EQ(env.value("bundle_count").toInt(), 0);
    EXPECT_TRUE(env.value("bundles").toArray().isEmpty());
    EXPECT_FALSE(env.value("truncated").toBool());
}

// INV-11 — byte-stable across repeated calls.
TEST(mcp_roadmap_bundles, Inv11ByteStable) {
    QJsonArray a;
    a.append(bullet("ANTS-1875", kPlanned, "diff viewer inline", {"claudediff"}));
    a.append(bullet("ANTS-1864", kPlanned, "diff viewer gutter", {"difftools"}));
    a.append(bullet("ANTS-1874", kPlanned, "diff viewer scroll", {"diffviewer"}));
    a.append(bullet("ANTS-3001", kProgress, "audio latency tuning"));
    a.append(bullet("ANTS-9100", kDone, "diff viewer gutter"));   // ✅ sibling
    const QByteArray j1 =
        QJsonDocument(RemoteControl::buildRoadmapBundlesEnvelope(a, kCap))
            .toJson(QJsonDocument::Compact);
    const QByteArray j2 =
        QJsonDocument(RemoteControl::buildRoadmapBundlesEnvelope(a, kCap))
            .toJson(QJsonDocument::Compact);
    EXPECT_EQ(j1, j2);
}

// INV-12 — truncation drops whole trailing bundles.
TEST(mcp_roadmap_bundles, Inv12Truncation) {
    QJsonArray a;
    // 5 disjoint singletons → 5 one-item bundles.
    a.append(bullet("ANTS-8001", kPlanned, "config persistence layer"));
    a.append(bullet("ANTS-8002", kPlanned, "audio latency tuning"));
    a.append(bullet("ANTS-8003", kPlanned, "render shader pipeline"));
    a.append(bullet("ANTS-8004", kPlanned, "network retry backoff"));
    a.append(bullet("ANTS-8005", kPlanned, "keyboard macro recorder"));
    const QJsonObject env =
        RemoteControl::buildRoadmapBundlesEnvelope(a, /*softCapBytes=*/50);
    EXPECT_TRUE(env.value("truncated").toBool());
    EXPECT_EQ(env.value("active_total").toInt(), 5);
    int sum = 0;
    for (const auto &bv : env.value("bundles").toArray()) {
        const QJsonObject bo = bv.toObject();
        // each emitted bundle is whole (its items[] count == its size).
        EXPECT_EQ(bo.value("items").toArray().size(), bo.value("size").toInt());
        sum += bo.value("size").toInt();
    }
    EXPECT_GE(env.value("bundle_count").toInt(), 1) << "always emit ≥1 bundle";
    EXPECT_LT(sum, env.value("active_total").toInt())
        << "truncation must drop at least one bundle";
}

// ANTS-2155 — long, vocabulary-varied headlines that share 2-3 real topic
// words now cluster (the old union-penalising Jaccard left them singletons),
// while numeric / filename tokens never form a spurious edge.
TEST(mcp_roadmap_bundles, Ants2155ClusterLongHeadlines) {
    QJsonArray a;
    // (i) 3 shared topic tokens despite long varied headlines (Jaccard < 0.5).
    a.append(bullet("ANTS-4001", kPlanned,
        "incremental codebase index refresh on file change"));
    a.append(bullet("ANTS-4002", kPlanned,
        "codebase index incremental cache invalidation strategy"));
    // (ii) exactly 2 shared topic tokens + a shared lane → lane assist.
    a.append(bullet("ANTS-4101", kPlanned,
        "auditdialog severity filter dropdown persistence", {"auditdialog"}));
    a.append(bullet("ANTS-4102", kPlanned,
        "auditdialog confidence column sort persistence", {"auditdialog"}));
    // (iii) numeric + filename tokens must NOT create a spurious edge.
    a.append(bullet("ANTS-4201", kPlanned, "fix 6162 in foo.cpp"));
    a.append(bullet("ANTS-4202", kPlanned, "fix 6162 in bar.cpp"));

    const QJsonObject env = RemoteControl::buildRoadmapBundlesEnvelope(a, kCap);
    EXPECT_EQ(findBundle(env, "ANTS-4001").value("size").toInt(), 2);  // abs-count (≥3 shared)
    EXPECT_EQ(findBundle(env, "ANTS-4101").value("size").toInt(), 2);  // lane assist (2 shared + lane)
    EXPECT_EQ(findBundle(env, "ANTS-4201").value("size").toInt(), 1);  // 6162 / *.cpp denoised away
    EXPECT_EQ(findBundle(env, "ANTS-4202").value("size").toInt(), 1);
}

// ANTS-2155 — under truncation the envelope reports the full pre-cap total
// so a caller can tell bundles were hidden.
TEST(mcp_roadmap_bundles, Ants2155TotalBundleCount) {
    QJsonArray a;
    a.append(bullet("ANTS-5001", kPlanned, "distinctalpha separatealpha uniquealpha"));
    a.append(bullet("ANTS-5002", kPlanned, "distinctbeta separatebeta uniquebeta"));
    a.append(bullet("ANTS-5003", kPlanned, "distinctgamma separategamma uniquegamma"));
    a.append(bullet("ANTS-5004", kPlanned, "distinctdelta separatedelta uniquedelta"));
    const QJsonObject env =
        RemoteControl::buildRoadmapBundlesEnvelope(a, /*softCapBytes=*/50);
    EXPECT_TRUE(env.value("truncated").toBool());
    EXPECT_EQ(env.value("total_bundle_count").toInt(), 4);   // all 4 singletons counted
    EXPECT_LT(env.value("bundle_count").toInt(), 4);         // fewer emitted
    EXPECT_EQ(env.value("bundles_omitted").toInt(),
              4 - env.value("bundle_count").toInt());
}

// INV-13 — label lane-fallback folds case.
TEST(mcp_roadmap_bundles, Inv13MixedCaseLaneLabel) {
    QJsonArray a;
    // singleton, all-stop-word headline (no qualifying token), two lanes
    // differing only by case → fold to one bucket for the label.
    a.append(bullet("ANTS-9001", kPlanned, "the for the", {"claude", "Claude"}));
    const QJsonObject env = RemoteControl::buildRoadmapBundlesEnvelope(a, kCap);
    const QJsonObject b = findBundle(env, "ANTS-9001");
    EXPECT_EQ(b.value("bundle_label").toString(), QStringLiteral("claude"))
        << "case-folded lane tally must resolve to one folded lane";
    // the emitted union stays case-sensitive verbatim (two entries).
    EXPECT_EQ(b.value("lanes").toArray().size(), 2);
}

// INV-17 (ANTS-4088) — label tokens are denoised + punctuation-stripped.
// rcNormaliseHeadline only chops trailing punctuation off the WHOLE headline
// and isNoiseToken never ran on the label tokens, so a real roadmap produced
// labels like `"most` / `- be bookmarked` / `57 blocks duplication:`.
TEST(mcp_roadmap_bundles, Ants4088LabelTokensDenoised) {
    QJsonArray a;
    // Singleton: every token qualifies (need = 1), so the label is the first
    // three surviving tokens in ascending order — pre-fix `"most" - 57`,
    // because '"' (0x22) and '-' (0x2D) sort before any letter.
    a.append(bullet("ANTS-4401", kPlanned,
                    "\"most\" 57 - duplication: bookmarked in foo.cpp"));
    const QJsonObject env = RemoteControl::buildRoadmapBundlesEnvelope(a, kCap);
    const QString label =
        findBundle(env, "ANTS-4401").value("bundle_label").toString();
    EXPECT_EQ(label, QStringLiteral("bookmarked duplication most"))
        << "label must strip per-token punctuation and drop noise tokens "
           "(digits, ≤2 chars, path-like) — got \""
        << label.toStdString() << "\"";

    // Stripping also merges `"bookmark"` and `bookmark` into ONE frequency
    // bucket, so the token ranks by its true frequency instead of splitting
    // its votes across two spellings and losing the freq-desc sort.
    QJsonArray b;
    b.append(bullet("ANTS-4402", kPlanned,
                    "\"bookmark\" session restore duplication"));
    b.append(bullet("ANTS-4403", kPlanned,
                    "bookmark session restore ordering"));
    const QJsonObject env2 = RemoteControl::buildRoadmapBundlesEnvelope(b, kCap);
    const QJsonObject bun = findBundle(env2, "ANTS-4402");
    ASSERT_EQ(bun.value("size").toInt(), 2) << "fixture must form one bundle";
    EXPECT_EQ(bun.value("bundle_label").toString(),
              QStringLiteral("bookmark restore session"))
        << "quoted and bare spellings of a token must tally as one";
}

// INV-6 — combo refusals (source-grep on the three guards).
TEST(mcp_roadmap_bundles, Inv6ComboGuards) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    expect(contains(cpp, "bundles mode does not accept section= filter"),
           "INV-6: bundles+section guard present");
    expect(contains(cpp, "id selector does not combine with mode:bundles"),
           "INV-6: bundles+id guard present");
    expect(contains(cpp, "ids selector does not combine with mode:bundles"),
           "INV-6: bundles+ids guard present");
    // ANTS-3617 replaced the `mode != QLatin1String("bundles")` chain with
    // a single kModes list (so the bad_mode refusal can echo `accepted`).
    // Same invariant — bundles is in the allow-list — scraped at its new
    // spelling, inside that list rather than in a comparison.
    expect(contains(cpp, "QStringLiteral(\"bundles\") };"),
           "INV-6: bundles in the kModes allow-list");
    EXPECT_EQ(0, expect_failures());
}

// INV-7 — passed status ignored; branch is an early return before the
// status filter switch (source-grep). INV-8 — warm path is const, no
// parse. INV-12 seam — setBundleSoftCapOverride wired into the branch.
TEST(mcp_roadmap_bundles, Inv7Inv8BranchWiring) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    expect(contains(cpp, "if (mode == QLatin1String(\"bundles\"))"),
           "INV-7/8: bundles branch present");
    expect(contains(cpp, "buildRoadmapBundlesEnvelope(m_roadmapCacheBullets"),
           "INV-8: branch passes the cache (const) to the pure builder");
    expect(contains(cpp, "m_bundleSoftCapOverride"),
           "INV-12: soft-cap override seam read in the branch");
    expect(contains(cpp, "a passed `status`"),
           "INV-7: branch documents status-ignored behaviour");
    EXPECT_EQ(0, expect_failures());
}

// Schema — "bundles" advertised on the roadmap_query mode enum.
TEST(mcp_roadmap_bundles, SchemaBundlesEnum) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    expect(contains(cpp, "modeEnum.append(\"bundles\")"),
           "schema: bundles value in the mode enum");
    EXPECT_EQ(0, expect_failures());
}

// ── ANTS-3388 — Kind+lane structural-template clustering + no_clusters_found ──

// INV-14 — per-module "<verb> <path>/spec.md" template bullets (same Kind,
// same lane, denoised tokens < 2) cluster via the structural-stem assist.
TEST(mcp_roadmap_bundles, Inv14StructuralTemplateClusters) {
    expect_reset();
    QJsonArray a;
    a.append(bullet("ANTS-1057", kPlanned, "Author src/mame_curator/alpha/spec.md",
                    {"docs"}, {}, "doc"));
    a.append(bullet("ANTS-1058", kPlanned, "Author src/mame_curator/beta/spec.md",
                    {"docs"}, {}, "doc"));
    a.append(bullet("ANTS-1059", kPlanned, "Author src/mame_curator/gamma/spec.md",
                    {"docs"}, {}, "doc"));
    const QJsonObject env = RemoteControl::buildRoadmapBundlesEnvelope(a, kCap);

    const QJsonObject b = findBundle(env, "ANTS-1057");
    expect(b.value("size").toInt() == 3, "three template bullets in one bundle");
    expect(findBundle(env, "ANTS-1058").value("size").toInt() == 3,
           "1058 in the same size-3 bundle");
    expect(findBundle(env, "ANTS-1059").value("size").toInt() == 3,
           "1059 in the same size-3 bundle");
    expect(env.value("no_clusters_found").toBool() == false,
           "a real cluster ⟹ no_clusters_found:false");
    EXPECT_EQ(0, expect_failures());
}

// INV-15 — the structural assist is guarded on all three facets: a different
// Kind, a different lane, or a different template does NOT merge.
TEST(mcp_roadmap_bundles, Inv15StructuralAssistGuarded) {
    expect_reset();

    // Different Kind (doc vs test) → no structural edge.
    {
        QJsonArray a;
        a.append(bullet("ANTS-2001", kPlanned, "Author src/mod/x/spec.md",
                        {"docs"}, {}, "doc"));
        a.append(bullet("ANTS-2002", kPlanned, "Author src/mod/y/spec.md",
                        {"docs"}, {}, "test"));
        const QJsonObject env = RemoteControl::buildRoadmapBundlesEnvelope(a, kCap);
        expect(findBundle(env, "ANTS-2001").value("size").toInt() == 1,
               "different Kind ⟹ not merged");
    }
    // Different lane → no structural edge.
    {
        QJsonArray a;
        a.append(bullet("ANTS-2003", kPlanned, "Author src/mod/x/spec.md",
                        {"docs"}, {}, "doc"));
        a.append(bullet("ANTS-2004", kPlanned, "Author src/mod/y/spec.md",
                        {"core"}, {}, "doc"));
        const QJsonObject env = RemoteControl::buildRoadmapBundlesEnvelope(a, kCap);
        expect(findBundle(env, "ANTS-2003").value("size").toInt() == 1,
               "different lane ⟹ not merged");
    }
    // Different template shape (different root + leaf + depth) → no edge.
    {
        QJsonArray a;
        a.append(bullet("ANTS-2005", kPlanned, "Author src/mod/x/spec.md",
                        {"docs"}, {}, "doc"));
        a.append(bullet("ANTS-2006", kPlanned, "Author docs/readme.md",
                        {"docs"}, {}, "doc"));
        const QJsonObject env = RemoteControl::buildRoadmapBundlesEnvelope(a, kCap);
        expect(findBundle(env, "ANTS-2005").value("size").toInt() == 1,
               "different template ⟹ not merged");
    }
    EXPECT_EQ(0, expect_failures());
}

// INV-16 — no_clusters_found:true when every bundle is a singleton, and the
// flag is always present (true even for an empty active set).
TEST(mcp_roadmap_bundles, Inv16NoClustersFoundFlag) {
    expect_reset();

    // All-singletons: three unrelated bullets.
    {
        QJsonArray a;
        a.append(bullet("ANTS-3001", kPlanned, "alpha widget rendering"));
        a.append(bullet("ANTS-3002", kPlanned, "beta parser rework"));
        a.append(bullet("ANTS-3003", kPlanned, "gamma socket timeout"));
        const QJsonObject env = RemoteControl::buildRoadmapBundlesEnvelope(a, kCap);
        expect(env.contains("no_clusters_found"), "flag always present");
        expect(env.value("no_clusters_found").toBool() == true,
               "all-singletons ⟹ no_clusters_found:true");
    }
    // Empty active set → vacuously true, flag present.
    {
        QJsonArray a;
        const QJsonObject env = RemoteControl::buildRoadmapBundlesEnvelope(a, kCap);
        expect(env.contains("no_clusters_found"), "flag present on empty set");
        expect(env.value("no_clusters_found").toBool() == true,
               "empty active ⟹ no_clusters_found:true");
    }
    EXPECT_EQ(0, expect_failures());
}
