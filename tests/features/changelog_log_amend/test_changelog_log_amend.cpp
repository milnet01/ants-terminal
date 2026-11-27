// Feature-conformance test for tests/features/changelog_log_amend/spec.md.
// ANTS-5246 — changelog_log op:"amend". Drives RemoteControl::cmdChangelogLog
// with a null MainWindow over a temp project.

#include "remotecontrol.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

namespace {

const char *kChangelog =
    "# Changelog\n\n"
    "## [Unreleased]\n\n"
    "### Added\n\n"
    "- **First entry** (ANTS-0001)\n"
    "  A phrase that wraps over\n"
    "  two lines here.\n"
    "- **Second entry** (ANTS-0002)\n"
    "  A shared phrase.\n"
    "- **Third entry** (ANTS-0003)\n"
    "  A shared phrase.\n\n"
    "## [0.1.0] - 2026-01-01\n\n"
    "- **Frozen record** (ANTS-0009)\n";

class ChangelogLogAmend : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(tmp.isValid());
        write(kChangelog);
    }

    void write(const char *text) const {
        QFile f(path());
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(text);
    }

    QString path() const { return tmp.path() + QStringLiteral("/CHANGELOG.md"); }

    QByteArray bytes() const {
        QFile f(path());
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }

    QJsonObject amend(const QString &oldText, const QString &newText,
                      QJsonObject extra = {}) {
        extra[QStringLiteral("caller_cwd")] = tmp.path();
        extra[QStringLiteral("op")]         = QStringLiteral("amend");
        extra[QStringLiteral("old_text")]   = oldText;
        extra[QStringLiteral("new_text")]   = newText;
        return rc.cmdChangelogLog(extra).object();
    }

    static QString code(const QJsonObject &o) {
        return o.value(QStringLiteral("code")).toString();
    }

    QTemporaryDir tmp;
    RemoteControl rc{nullptr};
};

}  // namespace

// INV-1 — one phrase replaced, every other byte kept.
TEST_F(ChangelogLogAmend, Inv1ReplacesOnePhrase) {
    const QJsonObject r = amend(QStringLiteral("First entry"),
                                QStringLiteral("Corrected entry"));
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_TRUE(r.value(QStringLiteral("changed")).toBool());
    EXPECT_EQ(r.value(QStringLiteral("line")).toInt(), 7);
    EXPECT_EQ(QString::fromUtf8(bytes()),
              QString::fromUtf8(kChangelog)
                  .replace(QStringLiteral("First entry"),
                           QStringLiteral("Corrected entry")));
}

// INV-2 — a phrase across a hard wrap is found.
TEST_F(ChangelogLogAmend, Inv2SpansAHardWrap) {
    const QJsonObject r = amend(QStringLiteral("wraps over two lines"),
                                QStringLiteral("fits"));
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_TRUE(r.value(QStringLiteral("wrapped")).toBool());
    EXPECT_TRUE(bytes().contains("  A phrase that fits here.\n"));
}

// INV-3 — deleting a phrase, a whole line, and a whole entry.
TEST_F(ChangelogLogAmend, Inv3EmptyNewTextDeletes) {
    ASSERT_TRUE(amend(QStringLiteral(" over"), QString())
                    .value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(bytes().contains("  A phrase that wraps\n"));

    ASSERT_TRUE(amend(QStringLiteral("two lines here."), QString())
                    .value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(bytes().contains("  A phrase that wraps\n- **Second entry**"))
        << "INV-3: the emptied line goes\n" << bytes().toStdString();

    EXPECT_EQ(code(amend(QStringLiteral("**Second entry** (ANTS-0002)"), QString())),
              QStringLiteral("empty_bullet"));
    EXPECT_TRUE(bytes().contains("**Second entry**"));
}

// INV-4 — required inputs.
TEST_F(ChangelogLogAmend, Inv4RequiredInputs) {
    EXPECT_EQ(code(amend(QStringLiteral("  "), QStringLiteral("x"))),
              QStringLiteral("missing_field"));

    QJsonObject noNew;
    noNew[QStringLiteral("caller_cwd")] = tmp.path();
    noNew[QStringLiteral("op")]         = QStringLiteral("amend");
    noNew[QStringLiteral("old_text")]   = QStringLiteral("First entry");
    EXPECT_EQ(code(rc.cmdChangelogLog(noNew).object()),
              QStringLiteral("missing_field"));
    EXPECT_EQ(bytes(), QByteArray(kChangelog));
}

// INV-5 — a miss and an ambiguity refuse and write nothing.
TEST_F(ChangelogLogAmend, Inv5MissAndAmbiguityRefuse) {
    EXPECT_EQ(code(amend(QStringLiteral("no such phrase"), QStringLiteral("x"))),
              QStringLiteral("text_not_found"));
    EXPECT_EQ(code(amend(QStringLiteral("A shared phrase."), QStringLiteral("x"))),
              QStringLiteral("text_ambiguous"));
    EXPECT_EQ(bytes(), QByteArray(kChangelog));
}

// INV-6 — id settles an ambiguity and cannot reach another entry.
TEST_F(ChangelogLogAmend, Inv6IdNarrows) {
    QJsonObject byId;
    byId[QStringLiteral("id")] = QStringLiteral("ANTS-0003");
    const QJsonObject r = amend(QStringLiteral("A shared phrase."),
                                QStringLiteral("Third's own."), byId);
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_TRUE(bytes().contains(
        "(ANTS-0002)\n  A shared phrase.\n- **Third entry** (ANTS-0003)\n"
        "  Third's own.\n"));

    EXPECT_EQ(code(amend(QStringLiteral("First entry"), QStringLiteral("x"), byId)),
              QStringLiteral("text_not_found"))
        << "INV-6: the phrase is in another entry";

    byId[QStringLiteral("id")] = QStringLiteral("ANTS-0404");
    EXPECT_EQ(code(amend(QStringLiteral("First entry"), QStringLiteral("x"), byId)),
              QStringLiteral("id_not_in_unreleased"));
}

// INV-7 — released sections cannot be reached.
TEST_F(ChangelogLogAmend, Inv7ReleasedIsOutOfReach) {
    EXPECT_EQ(code(amend(QStringLiteral("Frozen record"), QStringLiteral("x"))),
              QStringLiteral("text_not_found"));

    QJsonObject byId;
    byId[QStringLiteral("id")] = QStringLiteral("ANTS-0009");
    EXPECT_EQ(code(amend(QStringLiteral("Frozen record"), QStringLiteral("x"), byId)),
              QStringLiteral("id_not_in_unreleased"));

    QJsonObject versioned;
    versioned[QStringLiteral("version")] = QStringLiteral("0.1.0");
    EXPECT_EQ(code(amend(QStringLiteral("Frozen record"), QStringLiteral("x"),
                         versioned)),
              QStringLiteral("not_unreleased"));
    EXPECT_EQ(bytes(), QByteArray(kChangelog));
}

// INV-8 — dry_run predicts the write and leaves the bytes alone.
TEST_F(ChangelogLogAmend, Inv8DryRunWritesNothing) {
    QJsonObject dry;
    dry[QStringLiteral("dry_run")] = true;
    const QJsonObject r = amend(QStringLiteral("Second entry"),
                                QStringLiteral("Corrected"), dry);
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(r.value(QStringLiteral("dry_run")).toBool());
    EXPECT_TRUE(r.value(QStringLiteral("changed")).toBool());
    EXPECT_EQ(r.value(QStringLiteral("line")).toInt(), 10);
    EXPECT_EQ(bytes(), QByteArray(kChangelog));
}

// INV-9 — replacing a phrase with itself changes nothing.
TEST_F(ChangelogLogAmend, Inv9SameTextChangesNothing) {
    const QJsonObject r = amend(QStringLiteral("First entry"),
                                QStringLiteral("First entry"));
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(r.value(QStringLiteral("changed")).toBool());
    EXPECT_EQ(bytes(), QByteArray(kChangelog));
}

// INV-10 (ANTS-6762) — a match across a hard wrap joins the lines it spanned.
// The joined line is wrapped again at the width those lines had, with the
// entry's hanging indent, so the entry keeps its neighbours' width.
TEST_F(ChangelogLogAmend, Inv10AJoinedLineIsWrappedAgain) {
    const char *kLine1 = "  The quick brown fox jumps over the lazy dog and then keeps";
    const char *kLine2 = "  running through the field until the evening comes around.";
    const QByteArray doc = QByteArray("# Changelog\n\n## [Unreleased]\n\n"
                                      "- **Long entry** (ANTS-0004)\n") +
                           kLine1 + "\n" + kLine2 + "\n\n## [0.1.0] - 2026-01-01\n";
    write(doc.constData());
    const QJsonObject r = amend(
        QStringLiteral("then keeps running through"),
        QStringLiteral("afterwards keeps on running very quickly indeed through"));
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_TRUE(r.value(QStringLiteral("wrapped")).toBool());

    const QString after = QString::fromUtf8(bytes());
    const int width = static_cast<int>(qstrlen(kLine1));
    for (const QString &line : after.split(QLatin1Char('\n'))) {
        EXPECT_LE(line.size(), width) << line.toStdString();
        if (!line.isEmpty() && !line.startsWith(QLatin1Char('#')) &&
            !line.startsWith(QLatin1Char('-'))) {
            EXPECT_TRUE(line.startsWith(QLatin1String("  ")) && line.at(2) != QLatin1Char(' '))
                << "hanging indent: " << line.toStdString();
        }
    }
    EXPECT_TRUE(after.simplified().contains(QStringLiteral(
        "over the lazy dog and afterwards keeps on running very quickly indeed "
        "through the field until the evening comes around. ## [0.1.0]")))
        << after.toStdString();
    EXPECT_EQ(r.value(QStringLiteral("line_text")).toString(),
              after.split(QLatin1Char('\n')).at(r.value(QStringLiteral("line")).toInt() - 1));
}

// INV-11 (ANTS-6796) — new_text written in the file's own layout is not
// indented again, and the reply echoes every line the amend wrote.
TEST_F(ChangelogLogAmend, Inv11QuotedIndentIsNotDoubled) {
    const QJsonObject r = amend(
        QStringLiteral("A phrase that wraps over\n  two lines here."),
        QStringLiteral("A phrase that now wraps over\n  two lines there."));
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_TRUE(bytes().contains("  A phrase that now wraps over\n  two lines there.\n"))
        << bytes().toStdString();
    const QJsonArray texts = r.value(QStringLiteral("line_texts")).toArray();
    ASSERT_EQ(texts.size(), 2) << QJsonDocument(r).toJson().toStdString();
    EXPECT_EQ(texts.at(0).toString(), QStringLiteral("  A phrase that now wraps over"));
    EXPECT_EQ(texts.at(1).toString(), QStringLiteral("  two lines there."));

    // A new line the caller left unindented still takes the entry's indent.
    QJsonObject only;
    only[QStringLiteral("id")] = QStringLiteral("ANTS-0002");
    const QJsonObject r2 = amend(QStringLiteral("A shared phrase."),
                                 QStringLiteral("A shared phrase.\nA second line."), only);
    ASSERT_TRUE(r2.value(QStringLiteral("ok")).toBool())
        << r2.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_TRUE(bytes().contains("  A shared phrase.\n  A second line.\n- **Third"))
        << bytes().toStdString();
}

// INV-12 / ANTS-6624 — a bullet under a dated topic that cites the same id is
// inside the topic's entry, so text in that bullet is found once, not twice.
TEST_F(ChangelogLogAmend, Inv12TopicAndBulletCitingOneIdCountOnce) {
    write("# Changelog\n\n"
          "## [Unreleased]\n\n"
          "### 2026-11-01 Added — A topic (ANTS-0004)\n\n"
          "- **A bullet** (ANTS-0004)\n"
          "  Only here.\n\n"
          "## [0.1.0] - 2026-01-01\n\n"
          "- **Frozen record** (ANTS-0009)\n");
    QJsonObject byId;
    byId[QStringLiteral("id")] = QStringLiteral("ANTS-0004");
    const QJsonObject r = amend(QStringLiteral("Only here."),
                                QStringLiteral("Changed here."), byId);
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << code(r).toStdString() << " "
        << r.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_TRUE(bytes().contains("  Changed here.\n")) << bytes().toStdString();
}
