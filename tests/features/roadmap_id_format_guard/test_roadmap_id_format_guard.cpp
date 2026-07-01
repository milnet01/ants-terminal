// ANTS-3387 — feature-conformance test: an `id`/`ids`/flip/annotate locator
// that is id-token SHAPED (`<prefix>-<digits>`) but fails the canonical
// PROJ-NNNN gate (letter-leading prefix, roadmap-format.md § 3.5.1) must be
// refused with a NAMED bad_id_format, not a silent found:false /
// bullet_not_found. The Vestige repro was `3D_E-0022` (digit-leading prefix)
// — a real bullet the parser gives a synthetic content-hash, so the authored
// token is unaddressable on both read and write paths.
//
// Write path drives the *ForTest seam behaviourally; the read (roadmap_query)
// path has no test seam (mirrors roadmap_query_by_id), so its two branches are
// locked via source-anchor scrapes.

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"
#include "remotecontrol.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>

#include <string>

#ifndef SRC_REMOTECONTROL_CPP_PATH
#error "SRC_REMOTECONTROL_CPP_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {

// ~1 KiB intro padding so the file clears kRoadmapMinParseableSize (1024 B)
// and the ants-v1 walker fallback engages (mirrors roadmap_log_flip_idless).
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
    "quia consequuntur magni dolores eos qui ratione voluptatem sequi.\n";

// An ants-v1 roadmap with one canonical bullet + the digit-leading
// `[3D_E-0022]` bullet (the exact Vestige shape). 📋 = U+1F4CB.
QByteArray seed() {
    QByteArray b = "# Test Roadmap\n\n";
    b += kPad;
    b += "\n## Work Items\n\n"
         "- \xF0\x9F\x93\x8B [ANTS-0042] **Canonical bullet.**\n"
         "  Source: seed.\n"
         "- \xF0\x9F\x93\x8B [3D_E-0022] **Digit-leading-prefix bullet.**\n"
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

QString roadmapPath(const QString &root) {
    return QDir(root).filePath(QStringLiteral("ROADMAP.md"));
}

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// INV-2 — op:flip with a nonconforming `id` → bad_id_format (the token that
// looks "vanished" to a plain lookup is named as the real cause).
TEST(roadmap_id_format_guard, Inv2FlipNonconformingIdBadFormat) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seed()));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("op")]         = QStringLiteral("flip");
    req[QStringLiteral("to_status")]  = QStringLiteral("shipped");
    req[QStringLiteral("id")]         = QStringLiteral("3D_E-0022");
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(req).object();

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("bad_id_format"))
        << "a digit-leading id-shaped locator must be named, not a silent "
           "bullet_not_found";
}

// INV-3 — op:annotate shares the same handler + early guard.
TEST(roadmap_id_format_guard, Inv3AnnotateNonconformingIdBadFormat) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seed()));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("op")]         = QStringLiteral("annotate");
    req[QStringLiteral("id")]         = QStringLiteral("3D_E-0022");
    req[QStringLiteral("note")]       = QStringLiteral("Progress note.");
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(req).object();

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("bad_id_format"));
}

// INV-4 — regression: a CANONICAL but genuinely-absent id is NOT
// bad_id_format; the guard keys on shape, not presence.
TEST(roadmap_id_format_guard, Inv4ConformingAbsentIdIsNotBadFormat) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seed()));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("op")]         = QStringLiteral("flip");
    req[QStringLiteral("to_status")]  = QStringLiteral("shipped");
    req[QStringLiteral("id")]         = QStringLiteral("ANTS-9999");
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(req).object();

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_NE(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("bad_id_format"))
        << "a conforming-but-absent id must keep the ordinary "
           "bullet_not_found path, not be miscoded bad_id_format";
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("bullet_not_found"));
}

// INV-5 — regression: the canonical `[ANTS-0042]` bullet still flips (the
// guard doesn't shadow a legitimate letter-led id).
TEST(roadmap_id_format_guard, Inv5CanonicalIdStillFlips) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seed()));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("op")]         = QStringLiteral("flip");
    req[QStringLiteral("to_status")]  = QStringLiteral("shipped");
    req[QStringLiteral("id")]         = QStringLiteral("ANTS-0042");
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(req).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << "canonical letter-led id must resolve and flip normally";
}

// INV-1 — the classifier helper is present with BOTH the loose id-ish and
// canonical regexes (a shape-only, non-letter-lead divergence).
TEST(roadmap_id_format_guard, Inv1ClassifierPresent) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "rcIsNonconformingIdToken"),
           "INV-1: classifier helper defined");
    expect(contains(cpp, "^[A-Za-z0-9][A-Za-z0-9_-]*-"),
           "INV-1: loose id-ish regex present");
    expect(contains(cpp, "^[A-Za-z][A-Za-z0-9_-]*-"),
           "INV-1: canonical PROJ-NNNN regex present");
    EXPECT_EQ(0, expect_failures());
}

// INV-6 — the read (roadmap_query) id + ids branches both emit
// bad_id_format (no behavioural seam; source-anchored).
TEST(roadmap_id_format_guard, Inv6ReadPathBranchesGuarded) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "\"bad_id_format\""),
           "INV-6: bad_id_format code emitted");
    expect(contains(cpp, "rcIsNonconformingIdToken(idArg)"),
           "INV-6: singular id branch calls the classifier");
    expect(contains(cpp, "bad_format_ids"),
           "INV-6: ids[] batch surfaces bad_format_ids");
    EXPECT_EQ(0, expect_failures());
}
