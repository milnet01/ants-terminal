// ANTS-3417 — feature-conformance test: roadmap_log writers emit no trailing
// whitespace, so the very next commit isn't rejected by the ubiquitous
// trim-trailing-whitespace pre-commit hook.
//
// Pre-fix, formatRoadmapBullet's body loop emitted `"  " + ln + "\n"` for
// EVERY line — a blank body line rendered as `"  \n"` (two trailing spaces).
// appendBodyNote left space-only note lines dangling likewise. Both are now
// right-stripped. Drives the *ForTest handlers against a seeded temp ROADMAP.

#include <gtest/gtest.h>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include "remotecontrol.h"

namespace {

QString freshRoadmap() {
    // Padded past kRoadmapMinParseableSize (1024 B) so the flip/annotate
    // path's ants-v1 walker fallback engages (the append path has no such
    // gate). Padding lives in the intro prose; the bullet shape is what the
    // tests assert on.
    return QString::fromUtf8(
        "# Fresh Roadmap\n"
        "\n"
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
        "padding to clear the 1024-byte gate with comfortable headroom.\n"
        "\n"
        "## Backlog\n"
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

// Assert not a single line in the written document ends in a space/tab.
void expectNoTrailingWhitespace(const QString &md) {
    const QStringList lines = md.split(QChar('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        const QString &ln = lines.at(i);
        EXPECT_FALSE(ln.endsWith(QChar(' ')) || ln.endsWith(QChar('\t')))
            << "line " << i << " has trailing whitespace: \""
            << ln.toStdString() << "\"";
    }
}

}  // namespace

// INV-1 — append with a body that contains a blank line writes no trailing
// whitespace (the blank line is a truly empty line, not "  ").
TEST(roadmap_log_trailing_whitespace, Inv1AppendBlankBodyLine) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(tmp.path() + QStringLiteral("/ROADMAP.md"),
                          freshRoadmap()));
    ASSERT_TRUE(writeFile(tmp.path() + QStringLiteral("/.roadmap-counter"),
                          QStringLiteral("9001\n")));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req["caller_cwd"] = tmp.path();
    req["op"]         = QStringLiteral("append");
    req["section"]    = QStringLiteral("backlog");
    req["status"]     = QStringLiteral("planned");
    req["headline"]   = QStringLiteral("Item with a multi-paragraph body.");
    req["kind"]       = QStringLiteral("fix");
    req["source"]     = QStringLiteral("test");
    // A body whose paragraphs are separated by a blank line — the exact
    // shape that produced "  \n" before the fix.
    req["body"] = QStringLiteral(
        "First paragraph of the body.\n"
        "\n"
        "Second paragraph after a blank line.");
    const QJsonObject out = rc.cmdRoadmapLogAppendForTest(req).object();
    ASSERT_TRUE(out.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(out).toJson().toStdString();

    const QString md = readRoadmap(tmp.path());
    // The blank body line is emitted empty, not as the "  " hang indent.
    EXPECT_TRUE(md.contains(QStringLiteral(
        "  First paragraph of the body.\n"
        "\n"
        "  Second paragraph after a blank line.\n")))
        << md.toStdString();
    expectNoTrailingWhitespace(md);
}

// INV-2 — an appended note containing a blank line writes no trailing
// whitespace either (appendBodyNote path, shared by op:annotate and
// op:flip-with-note). Driven via op:annotate — the pure note-append path.
TEST(roadmap_log_trailing_whitespace, Inv2NoteBlankLine) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(tmp.path() + QStringLiteral("/ROADMAP.md"),
                          freshRoadmap()));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req["caller_cwd"]  = tmp.path();
    req["op"]          = QStringLiteral("annotate");
    req["id"]          = QStringLiteral("ANTS-9001");
    req["note"] = QStringLiteral(
        "Resolved: first line of the note.\n"
        "\n"
        "A second line after a blank.");
    const QJsonObject out = rc.cmdRoadmapLogFlipForTest(req).object();
    ASSERT_TRUE(out.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(out).toJson().toStdString();

    expectNoTrailingWhitespace(readRoadmap(tmp.path()));
}
