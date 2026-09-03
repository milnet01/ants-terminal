// ANTS-3640 — feature-conformance test: roadmap_log never writes a body or
// note that opens a code fence it does not close, and the fenced-bullet
// refusal names the line that opened the fence.
//
// Pre-fix, a body quoting a bare ``` opener was written verbatim two spaces
// in — which still opens a fence under both CommonMark and the walkers in
// remotecontrol.cpp — so every bullet below it became uneditable. Spec:
// tests/features/roadmap_log_fence_guard/spec.md.

#include <gtest/gtest.h>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include "remotecontrol.h"

namespace {

// Padded past kRoadmapMinParseableSize (1024 B) so the flip path's ants-v1
// walker fallback engages. Same seed shape as roadmap_log_trailing_whitespace.
QString padding() {
    return QStringLiteral(
        "Intro paragraph padding the file past the 1 KiB minimum-parseable\n"
        "gate the flip/annotate path enforces before it will trust an\n"
        "ants-v1 walk. Lorem ipsum dolor sit amet, consectetur adipiscing\n"
        "elit, sed do eiusmod tempor incididunt ut labore et dolore magna\n"
        "aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco\n"
        "laboris nisi ut aliquip ex ea commodo consequat. Duis aute irure\n"
        "dolor in reprehenderit in voluptate velit esse cillum dolore eu\n"
        "fugiat nulla pariatur. Excepteur sint occaecat cupidatat non\n"
        "proident, sunt in culpa qui officia deserunt mollit anim id est\n"
        "laborum. Sed ut perspiciatis unde omnis iste natus error sit\n"
        "voluptatem accusantium doloremque laudantium, totam rem aperiam,\n"
        "eaque ipsa quae ab illo inventore veritatis et quasi architecto\n"
        "beatae vitae dicta sunt explicabo. Nemo enim ipsam voluptatem quia\n"
        "voluptas sit aspernatur aut odit aut fugit, sed quia consequuntur\n"
        "magni dolores eos qui ratione voluptatem sequi nesciunt. Neque\n"
        "porro quisquam est qui dolorem ipsum quia dolor sit amet, more\n"
        "padding to clear the 1024-byte gate with comfortable headroom.\n");
}

QString freshRoadmap() {
    return QStringLiteral("# Fresh Roadmap\n\n") + padding() +
           QString::fromUtf8(
               "\n## Backlog\n"
               "\n"
               "- \xF0\x9F\x93\x8B [ANTS-9001] **An existing bullet.**\n"
               "  Kind: implement.\n"
               "  Source: test.\n"
               "\n");
}

bool writeFile(const QString &path, const QString &content) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    f.write(content.toUtf8());
    f.close();
    return true;
}

QString readRoadmap(const QString &dir) {
    QFile f(dir + QStringLiteral("/ROADMAP.md"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    return QString::fromUtf8(f.readAll());
}

// Seed a temp project with ROADMAP.md + .roadmap-counter.
bool seed(const QTemporaryDir &tmp, const QString &roadmap) {
    return writeFile(tmp.path() + QStringLiteral("/ROADMAP.md"), roadmap) &&
           writeFile(tmp.path() + QStringLiteral("/.roadmap-counter"),
                     QStringLiteral("9001\n"));
}

QJsonObject appendBullet(RemoteControl &rc, const QString &root,
                         const QString &headline, const QString &body) {
    QJsonObject req;
    req["caller_cwd"] = root;
    req["op"]         = QStringLiteral("append");
    req["section"]    = QStringLiteral("backlog");
    req["status"]     = QStringLiteral("planned");
    req["headline"]   = headline;
    req["kind"]       = QStringLiteral("fix");
    req["source"]     = QStringLiteral("test");
    if (!body.isEmpty()) req["body"] = body;
    return rc.cmdRoadmapLogAppendForTest(req).object();
}

}  // namespace

// INV-1 — a body that opens a fence and never closes it is escaped on the
// way in, and a bullet appended below it stays editable. Pre-fix the flip
// refused anchor_unsafe_context: the unclosed opener had swallowed it.
TEST(roadmap_log_fence_guard, Inv1UnclosedFenceEscapedAndBelowStaysEditable) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(seed(tmp, freshRoadmap()));

    RemoteControl rc(nullptr);
    // The exact shape that broke ROADMAP.md live: prose quoting a fence
    // opener, with no closer anywhere in the body.
    const QJsonObject a = appendBullet(
        rc, tmp.path(), QStringLiteral("Bullet whose body quotes a fence."),
        QStringLiteral("The offending line looked like this:\n"
                       "```cpp\n"
                       "and the rest of the prose continues after it."));
    ASSERT_TRUE(a.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(a).toJson().toStdString();

    const QJsonObject b = appendBullet(
        rc, tmp.path(), QStringLiteral("Bullet below the quoted fence."),
        QString());
    ASSERT_TRUE(b.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(b).toJson().toStdString();
    const QString belowId = b.value(QStringLiteral("id")).toString();
    ASSERT_FALSE(belowId.isEmpty());

    const QString md = readRoadmap(tmp.path());
    EXPECT_TRUE(md.contains(QStringLiteral("  \\```cpp\n")))
        << "unclosed fence opener was not escaped:\n" << md.toStdString();

    // The real regression guard: the bullet below is still editable.
    QJsonObject flip;
    flip["caller_cwd"] = tmp.path();
    flip["op"]         = QStringLiteral("flip");
    flip["id"]         = belowId;
    flip["to_status"]  = QStringLiteral("shipped");
    const QJsonObject out = rc.cmdRoadmapLogFlipForTest(flip).object();
    EXPECT_TRUE(out.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(out).toJson().toStdString();
}

// ANTS-4572 — a scrub that removed something must SAY so, even when what it
// removed carries no parameter name. `scrubbedNames` only ever held matched
// <parameter name="X"> pairs, so a stray closing tag, or ANTS-4609's
// `<tag>scalar` line, was stripped in silence. A caller who has read that
// bodies are scrubbed then takes ok:true for "the body is clean" and never
// re-reads the file — which is how ANTS-4609's half-scrub reached disk. A
// partial scrub is worse than no scrubber, because a caller who knew there was
// none would have sanitised the body themselves.
TEST(roadmap_log_fence_guard, Ants4572UnnamedScrubIsReported) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(seed(tmp, freshRoadmap()));

    RemoteControl rc(nullptr);
    const QJsonObject a = appendBullet(
        rc, tmp.path(), QStringLiteral("Bullet whose body leaked a bare tag."),
        QStringLiteral("The prose is fine and complete.\n</invoke>"));
    ASSERT_TRUE(a.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(a).toJson().toStdString();

    const QJsonArray warns = a.value(QStringLiteral("warnings")).toArray();
    ASSERT_EQ(warns.size(), 1)
        << "the scrub removed a fragment and said nothing: "
        << QJsonDocument(a).toJson().toStdString();
    const QJsonObject w = warns.at(0).toObject();
    EXPECT_EQ(w.value(QStringLiteral("code")).toString(),
              QStringLiteral("body_scrubbed_tool_xml"));
    EXPECT_GT(w.value(QStringLiteral("unnamed_fragments_removed")).toInt(), 0)
        << "the fragment carries no parameter name, so lost_parameters cannot "
           "carry it and a count is what remains";
    EXPECT_FALSE(w.contains(QStringLiteral("lost_parameters")))
        << "nothing NAMED was lost, so that field must not appear empty";

    // …and an ordinary body still reports nothing: the cosmetic half of the
    // scrub (blank runs, trailing whitespace, the final newline) fires on
    // almost every body and must never read as leakage.
    const QJsonObject clean = appendBullet(
        rc, tmp.path(), QStringLiteral("Bullet with an ordinary body."),
        QStringLiteral("Plain prose, no markup at all.\n\n\nWith a gap."));
    ASSERT_TRUE(clean.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(clean).toJson().toStdString();
    EXPECT_FALSE(clean.contains(QStringLiteral("warnings")))
        << "cosmetic normalisation must not read as a scrub: "
        << QJsonDocument(clean).toJson().toStdString();
}

// INV-2 — a balanced fence pair is legitimate prose and is written verbatim.
TEST(roadmap_log_fence_guard, Inv2BalancedFencePassesThroughUntouched) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(seed(tmp, freshRoadmap()));

    RemoteControl rc(nullptr);
    const QJsonObject a = appendBullet(
        rc, tmp.path(), QStringLiteral("Bullet quoting a whole code block."),
        QStringLiteral("Repro:\n"
                       "```sh\n"
                       "ctest --preset=default\n"
                       "```\n"
                       "which stays green."));
    ASSERT_TRUE(a.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(a).toJson().toStdString();

    const QString md = readRoadmap(tmp.path());
    EXPECT_TRUE(md.contains(QStringLiteral("  ```sh\n"))) << md.toStdString();
    EXPECT_FALSE(md.contains(QStringLiteral("\\```")))
        << "balanced fence must not be escaped:\n" << md.toStdString();
}

// INV-3 — the note path shares rcScrubLeakedToolXml, so it escapes too.
TEST(roadmap_log_fence_guard, Inv3AnnotateNoteEscapesUnclosedFence) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(seed(tmp, freshRoadmap()));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req["caller_cwd"] = tmp.path();
    req["op"]         = QStringLiteral("annotate");
    req["id"]         = QStringLiteral("ANTS-9001");
    req["note"]       = QStringLiteral("Progress: the culprit line was\n"
                                       "~~~\n"
                                       "an unterminated tilde fence.");
    const QJsonObject out = rc.cmdRoadmapLogFlipForTest(req).object();
    ASSERT_TRUE(out.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(out).toJson().toStdString();

    EXPECT_TRUE(readRoadmap(tmp.path()).contains(QStringLiteral("  \\~~~\n")))
        << readRoadmap(tmp.path()).toStdString();
}

// INV-4 — on a hand-written file the guard never saw, the refusal names the
// line that opened the fence, not just the bullet that got swallowed.
TEST(roadmap_log_fence_guard, Inv4RefusalNamesTheFenceOpener) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString md = QStringLiteral("# Fenced Roadmap\n\n") + padding() +
                       QString::fromUtf8(
                           "\n## Backlog\n"
                           "\n"
                           "```\n"
                           "- \xF0\x9F\x93\x8B [ANTS-9002] **Swallowed.**\n"
                           "  Kind: fix.\n"
                           "\n");
    ASSERT_TRUE(seed(tmp, md));

    // 1-based line number of the opener, derived from the seed rather than
    // hardcoded so padding edits cannot silently invalidate the assertion.
    const int openerLine =
        md.split(QChar('\n')).indexOf(QStringLiteral("```")) + 1;
    ASSERT_GT(openerLine, 0);

    RemoteControl rc(nullptr);
    QJsonObject req;
    req["caller_cwd"] = tmp.path();
    req["op"]         = QStringLiteral("flip");
    req["id"]         = QStringLiteral("ANTS-9002");
    req["to_status"]  = QStringLiteral("shipped");
    const QJsonObject out = rc.cmdRoadmapLogFlipForTest(req).object();

    ASSERT_FALSE(out.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(out).toJson().toStdString();
    EXPECT_EQ(out.value(QStringLiteral("code")).toString(),
              QStringLiteral("anchor_unsafe_context"));
    const QString err = out.value(QStringLiteral("error")).toString();
    EXPECT_TRUE(err.contains(QStringLiteral("the fence opens at line %1")
                                 .arg(openerLine)))
        << err.toStdString();
}

// INV-6 (ANTS-4450) — the guard must agree with MarkdownScan::fenceMask on
// the CommonMark § 4.5 closer-length rule. A 4-backtick opener is not closed
// by a 3-backtick line, so this body is unterminated and must be escaped.
//
// Pre-fix the guard's own toggle counted both lines and called the body
// balanced, so nothing was escaped and the walkers — which DO honour run
// length since ANTS-3678 — fenced off every bullet below. That is the exact
// shape that took ANTS-3678's own body out of service (ANTS-4823).
TEST(roadmap_log_fence_guard, Inv6ShortRunDoesNotCloseALongerOpener) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(seed(tmp, freshRoadmap()));

    RemoteControl rc(nullptr);
    const QJsonObject a = appendBullet(
        rc, tmp.path(), QStringLiteral("Bullet quoting fence syntax."),
        QStringLiteral("A 3-backtick line does not close a 4-backtick block:\n"
                       "````\n"
                       "```\n"
                       "and the prose continues after it."));
    ASSERT_TRUE(a.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(a).toJson().toStdString();

    const QJsonObject b = appendBullet(
        rc, tmp.path(), QStringLiteral("Bullet below the quoted fence."),
        QString());
    ASSERT_TRUE(b.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(b).toJson().toStdString();
    const QString belowId = b.value(QStringLiteral("id")).toString();
    ASSERT_FALSE(belowId.isEmpty());

    const QString md = readRoadmap(tmp.path());
    // Both openers must be neutralised: escaping only the 4-backtick line
    // promotes the 3-backtick line to a live opener that never closes.
    EXPECT_TRUE(md.contains(QStringLiteral("  \\````\n")))
        << "4-backtick opener was not escaped:\n" << md.toStdString();
    EXPECT_TRUE(md.contains(QStringLiteral("  \\```\n")))
        << "3-backtick line was left as a live opener:\n" << md.toStdString();

    QJsonObject flip;
    flip["caller_cwd"] = tmp.path();
    flip["op"]         = QStringLiteral("flip");
    flip["id"]         = belowId;
    flip["to_status"]  = QStringLiteral("shipped");
    const QJsonObject out = rc.cmdRoadmapLogFlipForTest(flip).object();
    EXPECT_TRUE(out.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(out).toJson().toStdString();
}

// INV-7 (ANTS-4450) — a fence is closed only by its OWN character. A tilde
// line does not close a backtick block. Pre-fix the guard counted the pair
// and called it balanced.
TEST(roadmap_log_fence_guard, Inv7TildeDoesNotCloseABacktickFence) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(seed(tmp, freshRoadmap()));

    RemoteControl rc(nullptr);
    const QJsonObject a = appendBullet(
        rc, tmp.path(), QStringLiteral("Bullet mixing fence characters."),
        QStringLiteral("The culprit pair looked like this:\n"
                       "```\n"
                       "~~~\n"
                       "and the prose continues after it."));
    ASSERT_TRUE(a.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(a).toJson().toStdString();

    const QString md = readRoadmap(tmp.path());
    EXPECT_TRUE(md.contains(QStringLiteral("  \\```\n")))
        << "backtick opener was not escaped:\n" << md.toStdString();
    EXPECT_TRUE(md.contains(QStringLiteral("  \\~~~\n")))
        << "tilde line was left as a live opener:\n" << md.toStdString();
}

// INV-8 (ANTS-4450) — the other direction, and the one the item's headline
// names: a BALANCED 4-backtick block containing a 3-backtick line is
// legitimate prose and must pass through untouched.
//
// Pre-fix the guard saw three fence-looking lines, called the count odd, and
// spliced a backslash into the CLOSER — corrupting the author's block and
// leaving the file with the unterminated fence it was meant to prevent.
TEST(roadmap_log_fence_guard, Inv8BalancedLongFenceKeepsItsShortInnerLine) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(seed(tmp, freshRoadmap()));

    RemoteControl rc(nullptr);
    const QJsonObject a = appendBullet(
        rc, tmp.path(), QStringLiteral("Bullet quoting a whole fence sample."),
        QStringLiteral("How this document quotes fence syntax:\n"
                       "````\n"
                       "```\n"
                       "````\n"
                       "which is balanced and must survive verbatim."));
    ASSERT_TRUE(a.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(a).toJson().toStdString();

    const QString md = readRoadmap(tmp.path());
    EXPECT_FALSE(md.contains(QStringLiteral("\\```")))
        << "balanced block must not be escaped:\n" << md.toStdString();
    EXPECT_TRUE(md.contains(QStringLiteral("  ````\n  ```\n  ````\n")))
        << "balanced block was not written verbatim:\n" << md.toStdString();
}
