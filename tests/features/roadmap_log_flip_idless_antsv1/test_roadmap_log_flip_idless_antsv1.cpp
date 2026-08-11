// ANTS-2059 — feature-conformance test: roadmap_log flip/flip_batch must
// accept FULLY id-less ants-v1 bullets (`- 📋 **Headline.**`, no
// `[PROJ-NNNN]` token). Pre-fix walkAntsV1Bullets required a `[` after the
// emoji, so these bullets were skipped and every mutating verb refused
// with unrecognised_format even though roadmap_query reads them. Behavioural
// against the *ForTest seams; mirrors roadmap_log_flip_batch's harness.

#include "../../_support/expect.h"
#include "remotecontrol.h"

#include <gtest/gtest.h>

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

ANTS_TEST_SCOPE();

namespace {

// ~1 KiB intro padding so the file clears kRoadmapMinParseableSize (1024 B)
// and the ants-v1 walker fallback engages.
const char *kPad =
    "Intro paragraph that exists purely to pad the file past the 1 KiB\n"
    "minimum-parseable-size gate the flip path enforces before it will\n"
    "trust an ants-v1 walk. Lorem ipsum dolor sit amet, consectetur\n"
    "adipiscing elit, sed do eiusmod tempor incididunt ut labore et\n"
    "dolore magna aliqua. Ut enim ad minim veniam, quis nostrud\n"
    "exercitation ullamco laboris nisi ut aliquip ex ea commodo\n"
    "consequat. Duis aute irure dolor in reprehenderit in voluptate\n"
    "velit esse cillum dolore eu fugiat nulla pariatur. Excepteur sint\n"
    "occaecat cupidatat non proident, sunt in culpa qui officia deserunt\n"
    "mollit anim id est laborum. Sed ut perspiciatis unde omnis iste\n"
    "natus error sit voluptatem accusantium doloremque laudantium, totam\n"
    "rem aperiam, eaque ipsa quae ab illo inventore veritatis et quasi\n"
    "architecto beatae vitae dicta sunt explicabo. Nemo enim ipsam\n"
    "voluptatem quia voluptas sit aspernatur aut odit aut fugit, sed\n"
    "quia consequuntur magni dolores eos qui ratione voluptatem sequi\n"
    "nesciunt. Neque porro quisquam est, qui dolorem ipsum quia dolor\n"
    "sit amet, consectetur, adipisci velit, sed quia non numquam eius\n"
    "modi tempora incidunt ut labore et dolore magnam aliquam quaerat\n"
    "voluptatem. Ut enim ad minima veniam, quis nostrum exercitationem.\n";

// Three FULLY id-less ants-v1 bullets (📋 = U+1F4CB, no `[…]` token).
QByteArray seedIdless() {
    QByteArray b = "# Test Roadmap\n\n";
    b += kPad;
    b += "\n## Work Items\n\n"
         "- \xF0\x9F\x93\x8B **First id-less bullet.**\n"
         "  Source: seed.\n"
         "- \xF0\x9F\x93\x8B **Second id-less bullet.**\n"
         "  Source: seed.\n"
         "- \xF0\x9F\x93\x8B **Third id-less bullet.**\n"
         "  Source: seed.\n\n";
    return b;
}

// Mixed file: one id-ful bullet + one id-less bullet.
QByteArray seedMixed() {
    QByteArray b = "# Test Roadmap\n\n";
    b += kPad;
    b += "\n## Work Items\n\n"
         "- \xF0\x9F\x93\x8B [ANTS-0042] **Id-ful bullet.**\n"
         "  Source: seed.\n"
         "- \xF0\x9F\x93\x8B **Id-less bullet.**\n"
         "  Source: seed.\n\n";
    return b;
}

// ANTS-4109 — ids carried as a BOLD token instead of a `[…]` bracket, the
// shape Lotto Tracker's roadmap uses. The third bullet is bold PROSE, which
// must stay id-less (a multi-word bold span is a headline, not an id).
QByteArray seedBoldId() {
    QByteArray b = "# Test Roadmap\n\n";
    b += kPad;
    b += "\n## Work Items\n\n"
         "- \xF0\x9F\x93\x8B **LOTTO-0019** Persist draw selections.\n"
         "  Source: seed.\n"
         "- \xF0\x9F\x93\x8B **LOTTO-0020** Export the history CSV.\n"
         "  Source: seed.\n"
         "- \xF0\x9F\x9A\xA7 **In-progress prose headline.**\n"
         "  Source: seed.\n\n";
    return b;
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

bool contains(const QByteArray &hay, const char *needle) {
    return hay.contains(needle);
}

}  // namespace

// INV-1 — single op:flip locates a fully id-less bullet by headline and
// flips its emoji (pre-fix: unrecognised_format).
TEST(roadmap_log_flip_idless_antsv1, Inv1FlipByHeadline) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedIdless()));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("op")]         = QStringLiteral("flip");
    req[QStringLiteral("to_status")]  = QStringLiteral("shipped");
    req[QStringLiteral("headline")]   =
        QStringLiteral("Second id-less bullet.");
    const QJsonObject resp =
        rc.cmdRoadmapLogFlipForTest(req).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << "id-less ants-v1 bullet must be flippable, not unrecognised_format";
    EXPECT_EQ(resp.value(QStringLiteral("format")).toString(),
              QStringLiteral("ants-v1"));
    EXPECT_EQ(resp.value(QStringLiteral("to_status")).toString(),
              QStringLiteral("✅"));

    const QByteArray md = readFile(roadmapPath(tmp.path()));
    EXPECT_TRUE(contains(md, "- \xE2\x9C\x85 **Second id-less bullet.**"))
        << "the located bullet's emoji flipped to ✅";
    EXPECT_TRUE(contains(md, "- \xF0\x9F\x93\x8B **First id-less bullet.**"))
        << "siblings untouched";
}

// INV-2 — flip_batch with a line_range flips every id-less bullet.
TEST(roadmap_log_flip_idless_antsv1, Inv2FlipBatchByLineRange) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedIdless()));

    RemoteControl rc(nullptr);
    QJsonObject loc;
    QJsonArray rng; rng.append(1); rng.append(9999);
    loc[QStringLiteral("line_range")] = rng;
    QJsonArray locs; locs.append(loc);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("op")]         = QStringLiteral("flip_batch");
    req[QStringLiteral("to_status")]  = QStringLiteral("shipped");
    req[QStringLiteral("locators")]   = locs;
    const QJsonObject resp =
        rc.cmdRoadmapLogFlipBatchForTest(req).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("flipped_count")).toInt(), 3)
        << "all three id-less bullets in [1,9999] flip";

    const QByteArray md = readFile(roadmapPath(tmp.path()));
    EXPECT_FALSE(contains(md, "\xF0\x9F\x93\x8B **"))
        << "no planned emoji remains on any id-less bullet";
}

// INV-3 — regression: a mixed file resolves the id-ful bullet by id and
// the id-less bullet by headline; neither is dropped.
TEST(roadmap_log_flip_idless_antsv1, Inv3MixedFilePreservesIdfulPath) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedMixed()));

    RemoteControl rc(nullptr);

    QJsonObject byId;
    byId[QStringLiteral("caller_cwd")] = tmp.path();
    byId[QStringLiteral("op")]         = QStringLiteral("flip");
    byId[QStringLiteral("to_status")]  = QStringLiteral("shipped");
    byId[QStringLiteral("id")]         = QStringLiteral("ANTS-0042");
    EXPECT_TRUE(rc.cmdRoadmapLogFlipForTest(byId)
                    .object().value(QStringLiteral("ok")).toBool())
        << "id-ful bullet still resolves by id in a mixed file";

    QJsonObject byHead;
    byHead[QStringLiteral("caller_cwd")] = tmp.path();
    byHead[QStringLiteral("op")]         = QStringLiteral("flip");
    byHead[QStringLiteral("to_status")]  = QStringLiteral("shipped");
    byHead[QStringLiteral("headline")]   = QStringLiteral("Id-less bullet.");
    EXPECT_TRUE(rc.cmdRoadmapLogFlipForTest(byHead)
                    .object().value(QStringLiteral("ok")).toBool())
        << "id-less bullet resolves by headline in the same file";

    const QByteArray md = readFile(roadmapPath(tmp.path()));
    EXPECT_TRUE(contains(md, "- \xE2\x9C\x85 [ANTS-0042] **Id-ful bullet.**"));
    EXPECT_TRUE(contains(md, "- \xE2\x9C\x85 **Id-less bullet.**"));
}

// INV-4 (ANTS-4109) — a bold-ID ants-v1 bullet resolves by the `id` locator.
// Pre-fix walkAntsV1Bullets only read a `[…]` bracket, so every bullet in a
// bold-ID roadmap came back id-less and flip refused bullet_not_found while
// roadmap_query resolved the same id.
TEST(roadmap_log_flip_idless_antsv1, Inv4FlipBoldIdById) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedBoldId()));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("op")]         = QStringLiteral("flip");
    req[QStringLiteral("to_status")]  = QStringLiteral("shipped");
    req[QStringLiteral("id")]         = QStringLiteral("LOTTO-0019");
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(req).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << "a bold-ID ants-v1 bullet must resolve by `id`, not bullet_not_found";
    EXPECT_EQ(resp.value(QStringLiteral("id")).toString(),
              QStringLiteral("LOTTO-0019"))
        << "the envelope reports the id it resolved, not an empty string";

    const QByteArray md = readFile(roadmapPath(tmp.path()));
    EXPECT_TRUE(contains(md, "- \xE2\x9C\x85 **LOTTO-0019** Persist"));
    EXPECT_TRUE(contains(md, "- \xF0\x9F\x93\x8B **LOTTO-0020** Export"))
        << "siblings untouched";

    // The bold-PROSE bullet stays id-less: adopting its span as an id would
    // make every narrator bullet addressable by its own headline text.
    QJsonObject prose;
    prose[QStringLiteral("caller_cwd")] = tmp.path();
    prose[QStringLiteral("op")]         = QStringLiteral("flip");
    prose[QStringLiteral("to_status")]  = QStringLiteral("shipped");
    prose[QStringLiteral("id")] =
        QStringLiteral("In-progress prose headline");
    EXPECT_FALSE(rc.cmdRoadmapLogFlipForTest(prose)
                     .object().value(QStringLiteral("ok")).toBool())
        << "a multi-word bold span is a headline, not an id";
}

// INV-5 (ANTS-4109) — flip_batch where EVERY locator fails is a refusal.
// Pre-fix it returned ok:true with flipped_count:0, so a caller that did not
// also read flipped_count reported a bundle shipped that was still planned.
TEST(roadmap_log_flip_idless_antsv1, Inv5FlipBatchAllSkippedIsNotOk) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedBoldId()));

    RemoteControl rc(nullptr);
    QJsonObject a, b;
    a[QStringLiteral("id")] = QStringLiteral("LOTTO-9998");
    b[QStringLiteral("id")] = QStringLiteral("LOTTO-9999");
    QJsonArray locs; locs.append(a); locs.append(b);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("op")]         = QStringLiteral("flip_batch");
    req[QStringLiteral("to_status")]  = QStringLiteral("shipped");
    req[QStringLiteral("locators")]   = locs;
    const QJsonObject resp = rc.cmdRoadmapLogFlipBatchForTest(req).object();

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool())
        << "nothing resolved is a refusal, not a partial success";
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("bullet_not_found"));
    EXPECT_EQ(resp.value(QStringLiteral("skipped")).toArray().size(), 2)
        << "skipped[] still carries the per-locator detail";

    // A batch with one resolvable locator stays a partial success.
    QJsonObject good;
    good[QStringLiteral("id")] = QStringLiteral("LOTTO-0019");
    QJsonArray mixed; mixed.append(good); mixed.append(b);
    QJsonObject req2;
    req2[QStringLiteral("caller_cwd")] = tmp.path();
    req2[QStringLiteral("op")]         = QStringLiteral("flip_batch");
    req2[QStringLiteral("to_status")]  = QStringLiteral("shipped");
    req2[QStringLiteral("locators")]   = mixed;
    const QJsonObject resp2 = rc.cmdRoadmapLogFlipBatchForTest(req2).object();
    EXPECT_TRUE(resp2.value(QStringLiteral("ok")).toBool())
        << "partial success is unchanged";
    EXPECT_EQ(resp2.value(QStringLiteral("flipped_count")).toInt(), 1);
}
