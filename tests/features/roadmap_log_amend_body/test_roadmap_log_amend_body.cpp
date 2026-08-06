// ANTS-3406 — feature-conformance test for roadmap_log op:"amend_body"
// (exact single-line body patch). Behavioural: drives
// cmdRoadmapLogAmendBodyForTest against a seeded temp ROADMAP and asserts
// file content + envelope. Source-grep for the schema/descriptor surface
// and the harder-to-seed refusal arms. See spec.md.

#include "../../_support/expect.h"
#include "remotecontrol.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

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

ANTS_TEST_SCOPE();

namespace {

// ants-v1 seed, padded past kRoadmapMinParseableSize (1024 B) so the
// amend path's ants-v1 walker fallback engages. 📋 = U+1F4CB. The body
// carries a finbreak-style stale phrase ("pinned in the spec") to patch,
// and "the " appears twice in that line for the ambiguity test.
QByteArray seedV1() {
    return QByteArray(
        "# Test Roadmap\n\n"
        "Intro paragraph that exists purely to pad the file past the\n"
        "1 KiB minimum-parseable-size gate the amend path enforces\n"
        "before it will trust an ants-v1 walk. Lorem ipsum dolor sit\n"
        "amet, consectetur adipiscing elit, sed do eiusmod tempor\n"
        "incididunt ut labore et dolore magna aliqua. Ut enim ad minim\n"
        "veniam, quis nostrud exercitation ullamco laboris nisi ut\n"
        "aliquip ex ea commodo consequat. Duis aute irure dolor in\n"
        "reprehenderit in voluptate velit esse cillum dolore eu fugiat\n"
        "nulla pariatur. Excepteur sint occaecat cupidatat non\n"
        "proident, sunt in culpa qui officia deserunt mollit anim id\n"
        "est laborum. More padding to be safe and clear the gate with\n"
        "comfortable headroom for the parser and the size check above.\n"
        "Sed ut perspiciatis unde omnis iste natus error sit\n"
        "voluptatem accusantium doloremque laudantium, totam rem\n"
        "aperiam, eaque ipsa quae ab illo inventore veritatis et quasi\n"
        "architecto beatae vitae dicta sunt explicabo.\n"
        "\n"
        "## Work Items\n\n"
        "- \xF0\x9F\x93\x8B [ANTS-0042] **Seed bullet headline.**\n"
        "  Layman: the value is pinned in the spec here.\n"
        "  Kind: feature.\n"
        "  Source: seed.\n"
        "\n");
}

// GFM seed — walker runs regardless of size. Plain (non-bold) headline
// so a headline locator matches verbatim.
QByteArray seedGfm() {
    return QByteArray(
        "# Test Roadmap\n\n"
        "## Work Items\n\n"
        "- [ ] Some GFM headline.\n"
        "  Body says foo needs fixing.\n"
        "\n");
}

// ANTS-3467 seed — reuses seedV1's padding, then adds a bullet whose body
// carries a blank-line-separated nested "Scope:" sub-list (a common ROADMAP
// shape). "1/5/10/30 min" sits on a 4-space nested sub-bullet line AFTER a
// blank line; "make it user-configurable" straddles a hard-wrapped line
// break. Both cases the old blank-line-truncated span could not reach.
QByteArray seedV1Nested() {
    QByteArray b = seedV1();
    b.append(
        "- \xF0\x9F\x93\x8B [ANTS-0043] **Settings screen with scope list.**\n"
        "  Layman: direct body line here.\n"
        "\n"
        "  Scope:\n"
        "  - **Auto-lock timeout (the priority):** make it\n"
        "    user-configurable (e.g. 1/5/10/30 min) so users tune it.\n"
        "    - Theme: the toggle can live in this settings pane.\n"
        "\n");
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

QJsonObject req(const QString &root) {
    QJsonObject o;
    o[QStringLiteral("caller_cwd")] = root;
    o[QStringLiteral("op")]         = QStringLiteral("amend_body");
    return o;
}

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// INV-1 — a unique old_text in an ants-v1 bullet body is replaced and the
// file is written; the envelope reports amended:true + body_line + id.
TEST(roadmap_log_amend_body, Inv1AntsV1UniqueReplace) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedV1()));

    RemoteControl rc(nullptr);
    QJsonObject r = req(tmp.path());
    r[QStringLiteral("id")]       = QStringLiteral("ANTS-0042");
    r[QStringLiteral("old_text")] = QStringLiteral("pinned in the spec");
    r[QStringLiteral("new_text")] =
        QStringLiteral("pinned in security-model.md INV-2");
    const QJsonObject resp = rc.cmdRoadmapLogAmendBodyForTest(r).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << "amend_body should succeed";
    EXPECT_EQ(resp.value(QStringLiteral("op")).toString(),
              QStringLiteral("amend_body"));
    EXPECT_TRUE(resp.value(QStringLiteral("amended")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("id")).toString(),
              QStringLiteral("ANTS-0042"));
    EXPECT_GT(resp.value(QStringLiteral("body_line")).toInt(), 0);

    const std::string md = readFile(roadmapPath(tmp.path())).toStdString();
    EXPECT_TRUE(contains(md,
        "Layman: the value is pinned in security-model.md INV-2 here."));
    EXPECT_FALSE(contains(md, "pinned in the spec"))
        << "stale phrase must be gone";
    // Headline + status untouched.
    EXPECT_TRUE(contains(md,
        "- \xF0\x9F\x93\x8B [ANTS-0042] **Seed bullet headline.**"));
}

// INV-2 — old_text absent/empty → missing_field; new_text key absent →
// missing_field.
TEST(roadmap_log_amend_body, Inv2RequiredFields) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedV1()));
    RemoteControl rc(nullptr);

    {   // no old_text
        QJsonObject r = req(tmp.path());
        r[QStringLiteral("id")]       = QStringLiteral("ANTS-0042");
        r[QStringLiteral("new_text")] = QStringLiteral("x");
        const QJsonObject resp = rc.cmdRoadmapLogAmendBodyForTest(r).object();
        EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
        EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
                  QStringLiteral("missing_field"));
    }
    {   // old_text present, new_text key absent
        QJsonObject r = req(tmp.path());
        r[QStringLiteral("id")]       = QStringLiteral("ANTS-0042");
        r[QStringLiteral("old_text")] = QStringLiteral("the spec");
        const QJsonObject resp = rc.cmdRoadmapLogAmendBodyForTest(r).object();
        EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
        EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
                  QStringLiteral("missing_field"));
    }
}

// INV-2 tail — an empty new_text is allowed and DELETES the matched phrase.
TEST(roadmap_log_amend_body, EmptyNewTextDeletesPhrase) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedV1()));

    RemoteControl rc(nullptr);
    QJsonObject r = req(tmp.path());
    r[QStringLiteral("id")]       = QStringLiteral("ANTS-0042");
    r[QStringLiteral("old_text")] = QStringLiteral(" here");
    r[QStringLiteral("new_text")] = QStringLiteral("");
    const QJsonObject resp = rc.cmdRoadmapLogAmendBodyForTest(r).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool());
    const std::string md = readFile(roadmapPath(tmp.path())).toStdString();
    EXPECT_TRUE(contains(md, "pinned in the spec."));
    EXPECT_FALSE(contains(md, "the spec here."));
}

// INV-3 — old_text not present in the body → body_match_not_found.
TEST(roadmap_log_amend_body, Inv3NotFound) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedV1()));

    RemoteControl rc(nullptr);
    QJsonObject r = req(tmp.path());
    r[QStringLiteral("id")]       = QStringLiteral("ANTS-0042");
    r[QStringLiteral("old_text")] = QStringLiteral("nonexistent phrase");
    r[QStringLiteral("new_text")] = QStringLiteral("x");
    const QJsonObject resp = rc.cmdRoadmapLogAmendBodyForTest(r).object();

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("body_match_not_found"));
}

// INV-4 — old_text occurring more than once → body_match_ambiguous, and
// the file is left byte-for-byte untouched.
TEST(roadmap_log_amend_body, Inv4AmbiguousLeavesFileUntouched) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QByteArray before = seedV1();
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), before));

    RemoteControl rc(nullptr);
    QJsonObject r = req(tmp.path());
    r[QStringLiteral("id")]       = QStringLiteral("ANTS-0042");
    r[QStringLiteral("old_text")] = QStringLiteral("the ");  // twice in body line
    r[QStringLiteral("new_text")] = QStringLiteral("THE ");
    const QJsonObject resp = rc.cmdRoadmapLogAmendBodyForTest(r).object();

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("body_match_ambiguous"));
    EXPECT_EQ(readFile(roadmapPath(tmp.path())), before)
        << "an ambiguous match must not modify the file";
}

// INV-5 — to_status → bad_op_combo; headline alongside id → bad_op_combo.
TEST(roadmap_log_amend_body, Inv5OpCombos) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedV1()));
    RemoteControl rc(nullptr);

    {
        QJsonObject r = req(tmp.path());
        r[QStringLiteral("id")]        = QStringLiteral("ANTS-0042");
        r[QStringLiteral("old_text")]  = QStringLiteral("the spec");
        r[QStringLiteral("new_text")]  = QStringLiteral("x");
        r[QStringLiteral("to_status")] = QStringLiteral("shipped");
        const QJsonObject resp = rc.cmdRoadmapLogAmendBodyForTest(r).object();
        EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
        EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
                  QStringLiteral("bad_op_combo"));
    }
    {
        QJsonObject r = req(tmp.path());
        r[QStringLiteral("id")]       = QStringLiteral("ANTS-0042");
        r[QStringLiteral("headline")] = QStringLiteral("Seed bullet headline.");
        r[QStringLiteral("old_text")] = QStringLiteral("the spec");
        r[QStringLiteral("new_text")] = QStringLiteral("x");
        const QJsonObject resp = rc.cmdRoadmapLogAmendBodyForTest(r).object();
        EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
        EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
                  QStringLiteral("bad_op_combo"));
    }
}

// INV-6 — new_text is scrubbed of leaked tool-call XML.
TEST(roadmap_log_amend_body, Inv6NewTextScrubbed) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedV1()));

    RemoteControl rc(nullptr);
    QJsonObject r = req(tmp.path());
    r[QStringLiteral("id")]       = QStringLiteral("ANTS-0042");
    r[QStringLiteral("old_text")] = QStringLiteral("pinned in the spec");
    r[QStringLiteral("new_text")] = QStringLiteral(
        "clean text<parameter name=\"lanes\">a,b</parameter>");
    const QJsonObject resp = rc.cmdRoadmapLogAmendBodyForTest(r).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool());
    const std::string md = readFile(roadmapPath(tmp.path())).toStdString();
    EXPECT_TRUE(contains(md, "clean text"));
    EXPECT_FALSE(contains(md, "<parameter"))
        << "leaked <parameter> wrapper must be scrubbed from new_text";
}

// INV-7 — dry_run returns the would-be edit and writes nothing.
TEST(roadmap_log_amend_body, Inv7DryRun) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QByteArray before = seedV1();
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), before));

    RemoteControl rc(nullptr);
    QJsonObject r = req(tmp.path());
    r[QStringLiteral("id")]       = QStringLiteral("ANTS-0042");
    r[QStringLiteral("old_text")] = QStringLiteral("pinned in the spec");
    r[QStringLiteral("new_text")] = QStringLiteral("moved elsewhere");
    r[QStringLiteral("dry_run")]  = true;
    const QJsonObject resp = rc.cmdRoadmapLogAmendBodyForTest(r).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(resp.value(QStringLiteral("dry_run")).toBool());
    EXPECT_GT(resp.value(QStringLiteral("bytes")).toInt(), 0);
    EXPECT_FALSE(resp.contains(QStringLiteral("bytes_written")));
    EXPECT_EQ(readFile(roadmapPath(tmp.path())), before)
        << "dry_run must not modify the file";
}

// INV-8 — amend works on a GFM-task-list bullet located by headline.
TEST(roadmap_log_amend_body, Inv8GfmByHeadline) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedGfm()));

    RemoteControl rc(nullptr);
    QJsonObject r = req(tmp.path());
    r[QStringLiteral("headline")] = QStringLiteral("Some GFM headline.");
    r[QStringLiteral("old_text")] = QStringLiteral("foo");
    r[QStringLiteral("new_text")] = QStringLiteral("bar");
    const QJsonObject resp = rc.cmdRoadmapLogAmendBodyForTest(r).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << "GFM amend by headline should succeed";
    EXPECT_EQ(resp.value(QStringLiteral("format")).toString(),
              QStringLiteral("gfm"));
    const std::string md = readFile(roadmapPath(tmp.path())).toStdString();
    EXPECT_TRUE(contains(md, "Body says bar needs fixing."));
    EXPECT_TRUE(contains(md, "- [ ] Some GFM headline."))
        << "checkbox status untouched";
}

// A missing id refuses bullet_not_found (locator resolution).
TEST(roadmap_log_amend_body, MissingIdBulletNotFound) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedV1()));

    RemoteControl rc(nullptr);
    QJsonObject r = req(tmp.path());
    r[QStringLiteral("id")]       = QStringLiteral("ANTS-9999");
    r[QStringLiteral("old_text")] = QStringLiteral("the spec");
    r[QStringLiteral("new_text")] = QStringLiteral("x");
    const QJsonObject resp = rc.cmdRoadmapLogAmendBodyForTest(r).object();

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("bullet_not_found"));
}

// ANTS-3467 — old_text on a blank-line-separated NESTED sub-bullet line is
// reached and patched (the body span spans internal blank lines up to the
// next top-level bullet). Reproduced the finbreak FIBR-0055 failure before
// the span-widen fix: previously body_match_not_found.
TEST(roadmap_log_amend_body, Ants3467NestedSubBulletAcrossBlankLine) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedV1Nested()));

    RemoteControl rc(nullptr);
    QJsonObject r = req(tmp.path());
    r[QStringLiteral("id")]       = QStringLiteral("ANTS-0043");
    r[QStringLiteral("old_text")] = QStringLiteral("1/5/10/30 min");
    r[QStringLiteral("new_text")] = QStringLiteral("1/5/10/15/30 min");
    const QJsonObject resp = rc.cmdRoadmapLogAmendBodyForTest(r).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << "amend on a blank-line-separated nested sub-bullet must succeed";
    const std::string md = readFile(roadmapPath(tmp.path())).toStdString();
    EXPECT_TRUE(contains(md, "(e.g. 1/5/10/15/30 min)"));
    EXPECT_FALSE(contains(md, "1/5/10/30 min"));
}

// ANTS-3467 — a phrase that spans a hard-wrapped line break can't match a
// single physical line, but body_match_not_found now carries a hint naming
// the wrap so the caller self-corrects rather than reading "text absent".
TEST(roadmap_log_amend_body, Ants3467WrapSpanHint) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedV1Nested()));

    RemoteControl rc(nullptr);
    QJsonObject r = req(tmp.path());
    r[QStringLiteral("id")]       = QStringLiteral("ANTS-0043");
    // "make it user-configurable" straddles the wrap (…make it\n…user-config…).
    r[QStringLiteral("old_text")] = QStringLiteral("make it user-configurable");
    r[QStringLiteral("new_text")] = QStringLiteral("x");
    const QJsonObject resp = rc.cmdRoadmapLogAmendBodyForTest(r).object();

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("body_match_not_found"));
    EXPECT_TRUE(contains(resp.value(QStringLiteral("hint"))
                             .toString().toStdString(),
                         "hard-wrapped"))
        << "wrap-spanning failure must hint at the line break";
}

// ANTS-3467 — old_text present in the file but outside the located bullet's
// block returns a distinct found-outside-body hint (wrong bullet targeted).
TEST(roadmap_log_amend_body, Ants3467FoundOutsideBodyHint) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedV1Nested()));

    RemoteControl rc(nullptr);
    QJsonObject r = req(tmp.path());
    // Target ANTS-0042, but the phrase lives in ANTS-0043's body.
    r[QStringLiteral("id")]       = QStringLiteral("ANTS-0042");
    r[QStringLiteral("old_text")] = QStringLiteral("direct body line here");
    r[QStringLiteral("new_text")] = QStringLiteral("x");
    const QJsonObject resp = rc.cmdRoadmapLogAmendBodyForTest(r).object();

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("body_match_not_found"));
    EXPECT_TRUE(contains(resp.value(QStringLiteral("hint"))
                             .toString().toStdString(),
                         "outside"))
        << "a phrase in another bullet must hint found-outside-body";
}

// INV-8 arms + INV-9 — source surface: schema enum/props/descriptor +
// the harder-to-seed refusal codes exist in the handler.
TEST(roadmap_log_amend_body, Inv8Inv9SourceSurface) {
    expect_reset();
    const std::string rc = ants_test::slurpRemoteControl();
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);

    expect(contains(rc, "int amendBodyExact("),
           "INV-1: shared amendBodyExact helper present");
    expect(contains(rc, "cmdRoadmapLogAmendBody"),
           "handler defined");
    expect(contains(rc, "\"body_match_not_found\""),
           "INV-3: body_match_not_found code present");
    expect(contains(rc, "\"body_match_ambiguous\""),
           "INV-4: body_match_ambiguous code present");
    expect(contains(rc, "\"unsupported_format\""),
           "INV-8: pass-headings unsupported_format arm present");
    expect(contains(rc, "\"anchor_unsafe_context\""),
           "INV-8: fenced-bullet refusal arm present");

    expect(contains(ci, "opEnum.append(\"amend_body\")"),
           "INV-9: schema op enum advertises amend_body");
    expect(contains(ci, "props[\"old_text\"]"),
           "INV-9: schema registers old_text property");
    expect(contains(ci, "props[\"new_text\"]"),
           "INV-9: schema registers new_text property");
    expect(contains(ci, "amend_body\\\" (ANTS-3406)"),
           "INV-9: op descriptor documents amend_body");
    EXPECT_EQ(0, expect_failures());
}
