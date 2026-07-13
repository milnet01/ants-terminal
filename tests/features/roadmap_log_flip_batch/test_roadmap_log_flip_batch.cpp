// ANTS-1690 — feature-conformance test for roadmap_log op:"flip_batch".
// Behavioural: drives cmdRoadmapLogFlipBatchForTest against seeded temp
// ROADMAPs and asserts file content + envelope; plus a source-grep for
// the dispatch + schema surface.

#include "../../_support/expect.h"
#include "remotecontrol.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>

#include <string>

ANTS_TEST_SCOPE();

namespace {

// ants-v1 seed with three bullets, padded past kRoadmapMinParseableSize
// (1024 B) so the ants-v1 walker fallback engages. 📋 = U+1F4CB.
QByteArray seedV1() {
    QByteArray b =
        "# Test Roadmap\n\n"
        "Intro paragraph that exists purely to pad the file past the\n"
        "1 KiB minimum-parseable-size gate the flip path enforces before\n"
        "it will trust an ants-v1 walk. Lorem ipsum dolor sit amet,\n"
        "consectetur adipiscing elit, sed do eiusmod tempor incididunt\n"
        "ut labore et dolore magna aliqua. Ut enim ad minim veniam, quis\n"
        "nostrud exercitation ullamco laboris nisi ut aliquip ex ea\n"
        "commodo consequat. Duis aute irure dolor in reprehenderit in\n"
        "voluptate velit esse cillum dolore eu fugiat nulla pariatur.\n"
        "Excepteur sint occaecat cupidatat non proident, sunt in culpa\n"
        "qui officia deserunt mollit anim id est laborum. More padding\n"
        "to be safe and clear the gate with comfortable headroom for the\n"
        "parser and the size check above. Sed ut perspiciatis unde omnis\n"
        "iste natus error sit voluptatem accusantium doloremque laudantium,\n"
        "totam rem aperiam, eaque ipsa quae ab illo inventore veritatis et\n"
        "quasi architecto beatae vitae dicta sunt explicabo. Nemo enim\n"
        "ipsam voluptatem quia voluptas sit aspernatur aut odit aut fugit.\n"
        "\n"
        "## Work Items\n\n"
        "- \xF0\x9F\x93\x8B [ANTS-0042] **First bullet.**\n"
        "  Source: seed.\n"
        "- \xF0\x9F\x93\x8B [ANTS-0043] **Second bullet.**\n"
        "  Source: seed.\n"
        "- \xF0\x9F\x93\x8B [ANTS-0044] **Third bullet.**\n"
        "  Source: seed.\n"
        "\n";
    return b;
}

// GFM seed — two id-less bullets so anchor injection engages.
QByteArray seedGfm() {
    return QByteArray(
        "# Test Roadmap\n\n"
        "## Work Items\n\n"
        "- [ ] First gfm bullet no id.\n"
        "- [ ] Second gfm bullet no id.\n"
        "\n");
}

bool writeFile(const QString &path, const QByteArray &body) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool ok = (f.write(body) == body.size());
    f.close();
    return ok;
}

QByteArray readFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

QString roadmapPath(const QString &root) {
    return QDir(root).filePath(QStringLiteral("ROADMAP.md"));
}

QJsonObject locId(const QString &id) {
    QJsonObject o; o[QStringLiteral("id")] = id; return o;
}

QJsonObject batchReq(const QString &root, const QString &toStatus,
                     const QJsonArray &locators) {
    QJsonObject o;
    o[QStringLiteral("caller_cwd")] = root;
    o[QStringLiteral("op")]         = QStringLiteral("flip_batch");
    o[QStringLiteral("to_status")]  = toStatus;
    o[QStringLiteral("locators")]   = locators;
    return o;
}


bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// INV-1 — batch flip three ants-v1 bullets by id in one call.
TEST(roadmap_log_flip_batch, Inv1FlipsAllById) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedV1()));

    RemoteControl rc(nullptr);
    QJsonArray locs;
    locs.append(locId(QStringLiteral("ANTS-0042")));
    locs.append(locId(QStringLiteral("ANTS-0043")));
    locs.append(locId(QStringLiteral("ANTS-0044")));
    const QJsonObject resp = rc.cmdRoadmapLogFlipBatchForTest(
        batchReq(tmp.path(), QStringLiteral("shipped"), locs)).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("op")).toString(),
              QStringLiteral("flip_batch"));
    EXPECT_EQ(resp.value(QStringLiteral("flipped_count")).toInt(), 3);
    EXPECT_EQ(resp.value(QStringLiteral("skipped_count")).toInt(), 0);

    const std::string md = readFile(roadmapPath(tmp.path())).toStdString();
    EXPECT_TRUE(contains(md, "- \xE2\x9C\x85 [ANTS-0042] **First bullet.**"));
    EXPECT_TRUE(contains(md, "- \xE2\x9C\x85 [ANTS-0043] **Second bullet.**"));
    EXPECT_TRUE(contains(md, "- \xE2\x9C\x85 [ANTS-0044] **Third bullet.**"));
    EXPECT_FALSE(contains(md, "\xF0\x9F\x93\x8B [ANTS-00"))
        << "no planned emoji should remain on the flipped bullets";
}

// INV-2 — index stability: per-locator notes land on the correct bullet.
TEST(roadmap_log_flip_batch, Inv2PerLocatorNotesStayWithTheirBullet) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedV1()));

    RemoteControl rc(nullptr);
    QJsonObject l1 = locId(QStringLiteral("ANTS-0042"));
    l1[QStringLiteral("note")] = QStringLiteral("Closed by bundle X (0042).");
    QJsonObject l2 = locId(QStringLiteral("ANTS-0043"));
    l2[QStringLiteral("note")] = QStringLiteral("Closed by bundle X (0043).");
    QJsonObject l3 = locId(QStringLiteral("ANTS-0044"));
    l3[QStringLiteral("note")] = QStringLiteral("Closed by bundle X (0044).");
    QJsonArray locs; locs.append(l1); locs.append(l2); locs.append(l3);
    const QJsonObject resp = rc.cmdRoadmapLogFlipBatchForTest(
        batchReq(tmp.path(), QStringLiteral("shipped"), locs)).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("flipped_count")).toInt(), 3);

    const std::string md = readFile(roadmapPath(tmp.path())).toStdString();
    // Each note must appear AFTER its own bullet's headline and BEFORE
    // the next bullet — i.e. the descending-apply order kept indices
    // valid and no note bled onto the wrong bullet.
    const auto p42  = md.find("[ANTS-0042]");
    const auto n42  = md.find("Closed by bundle X (0042).");
    const auto p43  = md.find("[ANTS-0043]");
    const auto n43  = md.find("Closed by bundle X (0043).");
    const auto p44  = md.find("[ANTS-0044]");
    const auto n44  = md.find("Closed by bundle X (0044).");
    ASSERT_NE(n42, std::string::npos);
    ASSERT_NE(n43, std::string::npos);
    ASSERT_NE(n44, std::string::npos);
    EXPECT_LT(p42, n42); EXPECT_LT(n42, p43);  // 0042's note before 0043
    EXPECT_LT(p43, n43); EXPECT_LT(n43, p44);  // 0043's note before 0044
    EXPECT_LT(p44, n44);                        // 0044's note after 0044
}

// INV-3 — partial success: a missing locator is skipped, the rest apply.
TEST(roadmap_log_flip_batch, Inv3PartialSuccess) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedV1()));

    RemoteControl rc(nullptr);
    QJsonArray locs;
    locs.append(locId(QStringLiteral("ANTS-0042")));
    locs.append(locId(QStringLiteral("ANTS-9999")));  // not present
    const QJsonObject resp = rc.cmdRoadmapLogFlipBatchForTest(
        batchReq(tmp.path(), QStringLiteral("shipped"), locs)).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("flipped_count")).toInt(), 1);
    EXPECT_EQ(resp.value(QStringLiteral("skipped_count")).toInt(), 1);
    const QJsonArray skipped = resp.value(QStringLiteral("skipped")).toArray();
    ASSERT_EQ(skipped.size(), 1);
    EXPECT_EQ(skipped.at(0).toObject().value(QStringLiteral("code")).toString(),
              QStringLiteral("bullet_not_found"));
    const std::string md = readFile(roadmapPath(tmp.path())).toStdString();
    EXPECT_TRUE(contains(md, "- \xE2\x9C\x85 [ANTS-0042] **First bullet.**"));
    EXPECT_TRUE(contains(md, "- \xF0\x9F\x93\x8B [ANTS-0043] **Second bullet.**"))
        << "0043 untouched (not in the batch)";
}

// INV-4 — empty locators / missing to_status / bad to_status refusals.
TEST(roadmap_log_flip_batch, Inv4Refusals) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedV1()));
    RemoteControl rc(nullptr);

    const QJsonObject empty = rc.cmdRoadmapLogFlipBatchForTest(
        batchReq(tmp.path(), QStringLiteral("shipped"), QJsonArray())).object();
    EXPECT_FALSE(empty.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(empty.value(QStringLiteral("code")).toString(),
              QStringLiteral("missing_field"));

    QJsonArray locs; locs.append(locId(QStringLiteral("ANTS-0042")));
    const QJsonObject bad = rc.cmdRoadmapLogFlipBatchForTest(
        batchReq(tmp.path(), QStringLiteral("frobnicated"), locs)).object();
    EXPECT_FALSE(bad.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(bad.value(QStringLiteral("code")).toString(),
              QStringLiteral("bad_status"));
}

// INV-5 — line_range flips every bullet in the inclusive range.
TEST(roadmap_log_flip_batch, Inv5LineRange) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedV1()));

    RemoteControl rc(nullptr);
    QJsonObject l; QJsonArray rng; rng.append(1); rng.append(9999);
    l[QStringLiteral("line_range")] = rng;
    QJsonArray locs; locs.append(l);
    const QJsonObject resp = rc.cmdRoadmapLogFlipBatchForTest(
        batchReq(tmp.path(), QStringLiteral("shipped"), locs)).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("flipped_count")).toInt(), 3)
        << "a [1,9999] range covers all three bullets";
}

// INV-6 — GFM anchor injection consumes the counter once across a batch;
// no_anchor suppresses injection.
TEST(roadmap_log_flip_batch, Inv6GfmAnchorInjectionCounterOnce) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedGfm()));

    RemoteControl rc(nullptr);
    QJsonObject l1; l1[QStringLiteral("headline")] =
        QStringLiteral("First gfm bullet no id.");
    QJsonObject l2; l2[QStringLiteral("headline")] =
        QStringLiteral("Second gfm bullet no id.");
    QJsonArray locs; locs.append(l1); locs.append(l2);
    const QJsonObject resp = rc.cmdRoadmapLogFlipBatchForTest(
        batchReq(tmp.path(), QStringLiteral("shipped"), locs)).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("flipped_count")).toInt(), 2);
    EXPECT_EQ(resp.value(QStringLiteral("counter")).toInt(), 2)
        << "two injections -> counter advances to 2 (was 0)";

    const QByteArray counter = readFile(
        QDir(tmp.path()).filePath(QStringLiteral(".roadmap-counter")));
    EXPECT_EQ(counter.trimmed(), QByteArray("2"))
        << ".roadmap-counter written exactly once with the final value";

    // no_anchor:true suppresses injection on a fresh GFM seed.
    QTemporaryDir tmp2; ASSERT_TRUE(tmp2.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp2.path()), seedGfm()));
    QJsonObject n1; n1[QStringLiteral("headline")] =
        QStringLiteral("First gfm bullet no id.");
    n1[QStringLiteral("no_anchor")] = true;
    QJsonArray nlocs; nlocs.append(n1);
    const QJsonObject nresp = rc.cmdRoadmapLogFlipBatchForTest(
        batchReq(tmp2.path(), QStringLiteral("shipped"), nlocs)).object();
    EXPECT_TRUE(nresp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(nresp.value(QStringLiteral("flipped_count")).toInt(), 1);
    EXPECT_FALSE(nresp.contains(QStringLiteral("counter")))
        << "no_anchor:true ⇒ no injection ⇒ no counter consumption";
}

// INV-7 — a bullet named by two locators flips exactly once.
TEST(roadmap_log_flip_batch, Inv7Dedup) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedV1()));

    RemoteControl rc(nullptr);
    QJsonArray locs;
    locs.append(locId(QStringLiteral("ANTS-0042")));
    locs.append(locId(QStringLiteral("ANTS-0042")));  // duplicate
    const QJsonObject resp = rc.cmdRoadmapLogFlipBatchForTest(
        batchReq(tmp.path(), QStringLiteral("shipped"), locs)).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("flipped_count")).toInt(), 1)
        << "the same bullet must flip exactly once";
}

// ANTS-2136 — dry_run preview: flip / flip_batch / create_section resolve
// the locator/target and return the would-be edit WITHOUT mutating the file.
TEST(roadmap_log_flip_batch, Inv8DryRunWritesNothing) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedV1()));
    const QByteArray before = readFile(roadmapPath(tmp.path()));
    RemoteControl rc(nullptr);

    // flip_batch dry_run — resolves two locators, writes nothing.
    QJsonArray locs;
    locs.append(locId(QStringLiteral("ANTS-0042")));
    locs.append(locId(QStringLiteral("ANTS-0043")));
    QJsonObject breq = batchReq(tmp.path(), QStringLiteral("shipped"), locs);
    breq[QStringLiteral("dry_run")] = true;
    const QJsonObject bresp = rc.cmdRoadmapLogFlipBatchForTest(breq).object();
    EXPECT_TRUE(bresp.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(bresp.value(QStringLiteral("dry_run")).toBool());
    EXPECT_EQ(bresp.value(QStringLiteral("would_flip_count")).toInt(), 2);
    EXPECT_FALSE(bresp.contains(QStringLiteral("bytes_written")))
        << "dry_run reports `bytes`, not a real `bytes_written`";
    EXPECT_EQ(readFile(roadmapPath(tmp.path())), before)
        << "flip_batch dry_run must leave ROADMAP.md byte-identical";

    // single flip dry_run.
    QJsonObject freq;
    freq[QStringLiteral("caller_cwd")] = tmp.path();
    freq[QStringLiteral("op")]         = QStringLiteral("flip");
    freq[QStringLiteral("to_status")]  = QStringLiteral("shipped");
    freq[QStringLiteral("id")]         = QStringLiteral("ANTS-0042");
    freq[QStringLiteral("dry_run")]    = true;
    const QJsonObject fresp = rc.cmdRoadmapLogFlipForTest(freq).object();
    EXPECT_TRUE(fresp.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(fresp.value(QStringLiteral("dry_run")).toBool());
    EXPECT_EQ(fresp.value(QStringLiteral("to_status")).toString(),
              QString::fromUtf8("\xE2\x9C\x85"));  // ✅
    EXPECT_EQ(readFile(roadmapPath(tmp.path())), before)
        << "flip dry_run must leave ROADMAP.md byte-identical";

    // create_section dry_run — resolves after_section, writes nothing.
    QJsonObject creq;
    creq[QStringLiteral("caller_cwd")]    = tmp.path();
    creq[QStringLiteral("op")]            = QStringLiteral("create_section");
    creq[QStringLiteral("after_section")] = QStringLiteral("work-items");
    creq[QStringLiteral("level")]         = 3;
    creq[QStringLiteral("title")]         = QStringLiteral("New Section");
    creq[QStringLiteral("dry_run")]       = true;
    const QJsonObject cresp =
        rc.cmdRoadmapLogCreateSectionForTest(creq).object();
    EXPECT_TRUE(cresp.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(cresp.value(QStringLiteral("dry_run")).toBool());
    EXPECT_FALSE(cresp.value(QStringLiteral("slug")).toString().isEmpty());
    EXPECT_EQ(readFile(roadmapPath(tmp.path())), before)
        << "create_section dry_run must leave ROADMAP.md byte-identical";
}

// INV (source) — dispatch routes flip_batch + schema advertises it.
TEST(roadmap_log_flip_batch, SourceSurface) {
    expect_reset();
    const std::string rc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);

    expect(contains(rc, "cmdRoadmapLogFlipBatch(req)"),
           "dispatch routes op:flip_batch to the handler");
    expect(contains(rc, "QJsonDocument RemoteControl::cmdRoadmapLogFlipBatch"),
           "handler defined");
    expect(contains(ci, "opEnum.append(\"flip_batch\")"),
           "schema op enum advertises flip_batch");
    expect(contains(ci, "props[\"locators\"]"),
           "schema registers the locators property");
    // ANTS-2089 — return:"headline_only" echoes post_bullets, built in
    // the same firstLine order as `flipped`, from the live bullet headline
    // captured during the apply loop.
    expect(contains(rc, "headlineByFirstLine") &&
               contains(rc, "out[\"post_bullets\"] = postBullets"),
           "ANTS-2089: flip_batch echoes post_bullets under headline_only");
    EXPECT_EQ(0, expect_failures());
}
