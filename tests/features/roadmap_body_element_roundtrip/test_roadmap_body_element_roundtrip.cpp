// Feature-conformance test for spec.md — ANTS-4616.
//
// Round-trip fidelity for the element vocabulary a bullet BODY can carry:
// markdown tables, fenced code blocks, and blockquotes. Write markdown ->
// migrate -> render -> read the file that lands, and check the element is
// still there and still shaped like itself.
//
// Once a project is store-backed the re-render is unavoidable and fires on any
// write, so every element type a body can hold has to survive it. Nested
// sub-bullet indentation was measured and does survive (ANTS-4558); these
// three were assumed and never exercised. ANTS-4596 is the argument for not
// assuming: a real prose loss shipped, and was caught only because a
// contributor grepped for a sentence they remembered writing.

#include <gtest/gtest.h>

#include "roadmapmigrate.h"
#include "roadmapmigrateload.h"
#include "roadmaprender.h"
#include "roadmapstore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <memory>

namespace {

bool writeFile(const QString &path, const QByteArray &text) {
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(text) == text.size();
}

QString readAll(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    return QString::fromUtf8(f.readAll());
}

// NEVER default-construct RoadmapStore — it resolves defaultPath(), the
// developer's REAL store under XDG_DATA_HOME, and every case here would write
// into it. `Access` is the third parameter and is named rather than defaulted.
std::unique_ptr<RoadmapStore> openStore(const QString &path) {
    auto store = std::make_unique<RoadmapStore>(
        path, RoadmapStore::kDefaultHistoryCapBytes, RoadmapStore::Access::Bulk);
    QString err;
    if (!store->open(&err)) {
        ADD_FAILURE() << "store open: " << err.toStdString();
        return nullptr;
    }
    return store;
}

// Write `body` as one bullet's continuation, migrate it, render the store back
// over ROADMAP.md, and hand back what actually landed on disk. That full path
// is the object under test: on a store-backed project the file IS the render's
// output, so anything the store cannot hold is gone from the file too.
QString roundTrip(QTemporaryDir &dir, const QByteArray &body) {
    const QString root = dir.filePath(QStringLiteral("proj"));
    QByteArray md =
        "<!-- ants-roadmap-format: 1 -->\n"
        "\n"
        "# Demo — Roadmap\n"
        "\n"
        "## Work\n"
        "\n"
        "- \xF0\x9F\x93\x8B [DEMO-0001] **An item whose body carries an element.**\n";
    md += body;
    md +=
        "  Layman: Carries an element.\n"
        "  Kind: implement.\n"
        "  Source: test.\n";
    if (!writeFile(root + QStringLiteral("/ROADMAP.md"), md)) {
        ADD_FAILURE() << "seed write failed";
        return QString();
    }

    auto store = openStore(dir.filePath(QStringLiteral("store.sqlite")));
    if (!store) return QString();

    QString err;
    const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
    if (!disc) { ADD_FAILURE() << "findRoadmaps: " << err.toStdString(); return QString(); }
    const auto plan = RoadmapMigrate::planFrom(
        *disc, QStringLiteral("Demo"), QStringLiteral("demo"));
    RoadmapMigrateLoad::Options lopts;
    lopts.changedAt   = QStringLiteral("2026-08-21T10:00:00Z");
    lopts.projectRoot = root;
    const auto loaded = RoadmapMigrateLoad::load(*store, plan, lopts);
    if (!loaded.ok) {
        ADD_FAILURE() << "migration load: " << loaded.error.toStdString();
        return QString();
    }

    // DELETE the seed before rendering. Without this the case cannot tell a
    // faithful round trip from a render that never ran — both leave the
    // author's own bytes on disk, and the test would pass vacuously for the
    // one reason it exists to rule out. Gone, anything the assertions find
    // came out of the store.
    if (!QFile::remove(root + QStringLiteral("/ROADMAP.md"))) {
        ADD_FAILURE() << "could not remove the seed before rendering";
        return QString();
    }

    RoadmapRender::Options ropts;
    ropts.liveRoadmapPath = QStringLiteral("ROADMAP.md");
    const auto outcome =
        RoadmapRender::render(*store, loaded.projectId, root, ropts, &err);
    if (!outcome) { ADD_FAILURE() << "render: " << err.toStdString(); return QString(); }
    if (!outcome->gateFailures.isEmpty()) {
        ADD_FAILURE() << "render gate: "
                      << outcome->gateFailures.join(QStringLiteral(", ")).toStdString();
        return QString();
    }
    return readAll(root + QStringLiteral("/ROADMAP.md"));
}

// The body's own lines, with the roadmap's 2-space continuation indent
// stripped, so a case can assert on the element as the author wrote it.
QStringList dedented(const QString &rendered) {
    QStringList out;
    for (const QString &l : rendered.split(QChar('\n'))) {
        if (l.startsWith(QStringLiteral("  ")) && !l.trimmed().isEmpty())
            out << l.mid(2);
    }
    return out;
}

}  // namespace

// INV-1 — a markdown TABLE survives. Row order, the pipe count per row and the
// header separator all have to hold: a table whose separator is re-flowed away
// stops being a table and renders as a run of prose with stray pipes.
TEST(roadmap_body_element_roundtrip, Inv1TableSurvives) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString out = roundTrip(dir,
        "  The routing table:\n"
        "\n"
        "  | Verb | Owner |\n"
        "  |------|-------|\n"
        "  | one  | alpha |\n"
        "  | two  | beta  |\n"
        "\n");
    ASSERT_FALSE(out.isEmpty());

    const QStringList body = dedented(out);
    EXPECT_TRUE(body.contains(QStringLiteral("| Verb | Owner |")))
        << "INV-1: the header row must survive";
    EXPECT_TRUE(body.contains(QStringLiteral("|------|-------|")))
        << "INV-1: the separator row is what makes it a table";
    EXPECT_TRUE(body.contains(QStringLiteral("| one  | alpha |")));
    EXPECT_TRUE(body.contains(QStringLiteral("| two  | beta  |")));
    EXPECT_LT(body.indexOf(QStringLiteral("| one  | alpha |")),
              body.indexOf(QStringLiteral("| two  | beta  |")))
        << "INV-1: row order must survive";
}

// INV-2 — a FENCED CODE BLOCK survives, fences included. Losing a fence is the
// worst of the three: the content becomes prose, and an unbalanced fence
// swallows everything after it when the file is next read as markdown.
TEST(roadmap_body_element_roundtrip, Inv2CodeFenceSurvives) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString out = roundTrip(dir,
        "  Reproduce with:\n"
        "\n"
        "  ```bash\n"
        "  cmake --build build\n"
        "  ctest --preset=default\n"
        "  ```\n"
        "\n");
    ASSERT_FALSE(out.isEmpty());

    const QStringList body = dedented(out);
    EXPECT_EQ(body.count(QStringLiteral("```bash")) +
              body.count(QStringLiteral("```")), 2)
        << "INV-2: exactly two fences — an unbalanced fence swallows the file";
    EXPECT_TRUE(body.contains(QStringLiteral("cmake --build build")));
    EXPECT_TRUE(body.contains(QStringLiteral("ctest --preset=default")));
}

// INV-3 — a BLOCKQUOTE survives with its markers. The wrapped-match rule reads
// `>` as whitespace by design (ANTS-4547), so a blockquote is exactly the
// shape most likely to be normalised away somewhere in this pipeline.
TEST(roadmap_body_element_roundtrip, Inv3BlockquoteSurvives) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString out = roundTrip(dir,
        "  The reporter wrote:\n"
        "\n"
        "  > The verdict is right; the wording misdirects.\n"
        "  > Nothing in the envelope says where it looked.\n"
        "\n");
    ASSERT_FALSE(out.isEmpty());

    const QStringList body = dedented(out);
    EXPECT_TRUE(body.contains(
        QStringLiteral("> The verdict is right; the wording misdirects.")))
        << "INV-3: the quote's marker must survive, not just its text";
    EXPECT_TRUE(body.contains(
        QStringLiteral("> Nothing in the envelope says where it looked.")));
}

// INV-4 — the three together in one body, in order. Each case above proves its
// element in isolation; a pipeline can still lose one only when another is
// present, and a real body is not a single-element fixture.
TEST(roadmap_body_element_roundtrip, Inv4MixedElementsKeepTheirOrder) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString out = roundTrip(dir,
        "  Context first.\n"
        "\n"
        "  | Verb | Owner |\n"
        "  |------|-------|\n"
        "  | one  | alpha |\n"
        "\n"
        "  ```bash\n"
        "  ctest --preset=default\n"
        "  ```\n"
        "\n"
        "  > And a closing quotation.\n"
        "\n");
    ASSERT_FALSE(out.isEmpty());

    const QStringList body = dedented(out);
    const int table = body.indexOf(QStringLiteral("| Verb | Owner |"));
    const int fence = body.indexOf(QStringLiteral("```bash"));
    const int quote = body.indexOf(QStringLiteral("> And a closing quotation."));
    ASSERT_GE(table, 0) << "INV-4: table lost when other elements are present";
    ASSERT_GE(fence, 0) << "INV-4: fence lost when other elements are present";
    ASSERT_GE(quote, 0) << "INV-4: blockquote lost when other elements are present";
    EXPECT_LT(table, fence);
    EXPECT_LT(fence, quote);
}
