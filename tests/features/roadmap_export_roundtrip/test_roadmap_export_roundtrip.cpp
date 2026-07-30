// Feature-conformance test for ANTS-3761. Contract:
// tests/features/roadmap_export_roundtrip/spec.md
//
// Covers INV-1, 2, 5, 12, 13, 18 and 19 — § 6 of the spec assigns all seven to
// this one directory.
//
// The vectors are the RFC's own, committed under vectors/. Measuring the
// writer against material we did not author is the entire point: INV-1
// compares the writer against itself, so any deterministic writer passes it.

#include <gtest/gtest.h>

#include "jsoncanonical.h"
#include "roadmapexport.h"
#include "roadmapstore.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QString>
#include <QTemporaryDir>
#include <QVariant>

#include <atomic>
#include <cstring>
#include <memory>
#include <thread>
#include <unistd.h>

#ifndef ANTS_JCS_VECTOR_DIR
#  error "ANTS_JCS_VECTOR_DIR compile definition required"
#endif
#ifndef ANTS_EXPORT_GOLDEN_DIR
#  error "ANTS_EXPORT_GOLDEN_DIR compile definition required"
#endif

namespace {

QByteArray slurp(const QString &name) {
    QFile f(QStringLiteral(ANTS_JCS_VECTOR_DIR "/") + name);
    EXPECT_TRUE(f.open(QIODevice::ReadOnly)) << "missing fixture " << name.toStdString();
    return f.readAll();
}

double fromBits(const char *hex) {
    const quint64 bits = QByteArray(hex).toULongLong(nullptr, 16);
    double d = 0;
    std::memcpy(&d, &bits, sizeof d);
    return d;
}

}  // namespace

// INV-19 leg 1 — the six published vector files, byte for byte.
TEST(RoadmapExportCanonical, Inv19PublishedVectorFiles) {
    for (const char *name : {"arrays", "french", "structures", "unicode", "values", "weird"}) {
        const QByteArray in = slurp(QStringLiteral("input-%1.json").arg(QLatin1String(name)));
        QByteArray want = slurp(QStringLiteral("output-%1.json").arg(QLatin1String(name)));
        while (want.endsWith('\n') || want.endsWith('\r'))
            want.chop(1);

        QJsonParseError perr{};
        const QJsonDocument doc = QJsonDocument::fromJson(in, &perr);
        ASSERT_EQ(perr.error, QJsonParseError::NoError)
            << name << ": " << perr.errorString().toStdString();

        QByteArray got;
        QString err;
        ASSERT_TRUE(JsonCanonical::serialise(
            doc.isArray() ? QJsonValue(doc.array()) : QJsonValue(doc.object()), &got, &err))
            << name << ": " << err.toStdString();
        EXPECT_EQ(got.toStdString(), want.toStdString()) << "vector: " << name;
    }
}

// INV-19 leg 2 — RFC 8785 Appendix B, Table 1. Kept separate from leg 1
// because this is the table Qt fails: 21 of 24 through QJsonDocument, and all
// three misses are ECMAScript's fixed-versus-exponential boundary. A leg-1-only
// test would go green against the very writer § 2.2 rules out.
TEST(RoadmapExportCanonical, Inv19AppendixBNumberSamples) {
    struct Sample { const char *bits; const char *want; const char *comment; };
    static const Sample samples[] = {
        {"0000000000000000", "0", "zero"},
        {"8000000000000000", "0", "minus zero"},
        {"0000000000000001", "5e-324", "min positive"},
        {"8000000000000001", "-5e-324", "min negative"},
        {"7fefffffffffffff", "1.7976931348623157e+308", "max positive"},
        {"ffefffffffffffff", "-1.7976931348623157e+308", "max negative"},
        {"4340000000000000", "9007199254740992", "max positive int"},
        {"c340000000000000", "-9007199254740992", "max negative int"},
        {"4430000000000000", "295147905179352830000", "~2**68"},
        {"44b52d02c7e14af5", "9.999999999999997e+22", ""},
        {"44b52d02c7e14af6", "1e+23", ""},
        {"44b52d02c7e14af7", "1.0000000000000001e+23", ""},
        {"444b1ae4d6e2ef4e", "999999999999999700000", ""},
        {"444b1ae4d6e2ef4f", "999999999999999900000", ""},
        {"444b1ae4d6e2ef50", "1e+21", ""},
        {"3eb0c6f7a0b5ed8c", "9.999999999999997e-7", "Qt pads the exponent"},
        {"3eb0c6f7a0b5ed8d", "0.000001", "Qt emits 1e-06"},
        {"41b3de4355555553", "333333333.3333332", ""},
        {"41b3de4355555554", "333333333.33333325", ""},
        {"41b3de4355555555", "333333333.3333333", ""},
        {"41b3de4355555556", "333333333.3333334", ""},
        {"41b3de4355555557", "333333333.33333343", ""},
        {"becbf647612f3696", "-0.0000033333333333333333", "Qt emits -3.33...e-06"},
        {"43143ff3c1cb0959", "1424953923781206.2", "round to even"},
    };

    for (const Sample &s : samples) {
        EXPECT_EQ(JsonCanonical::numberToString(fromBits(s.bits)).toStdString(),
                  std::string(s.want))
            << "IEEE 754 " << s.bits << (s.comment[0] ? "  — " : "") << s.comment;
    }
}

// INV-19 leg 3 — the RFC's two mandatory error cases. § 3.2.2.2 requires a
// conforming implementation to TERMINATE on a lone surrogate, and ANTS-3761
// § 2.4 turns that into "abort the export and report the row" — never a
// replacement character, never a partial file. Reachable in practice: SQLite
// TEXT does not validate encoding.
TEST(RoadmapExportCanonical, Inv19LoneSurrogateAborts) {
    QJsonObject o;
    o.insert(QStringLiteral("body"), QString(QChar(0xD800)) + QStringLiteral("x"));

    QByteArray out;
    QString err;
    EXPECT_FALSE(JsonCanonical::serialise(o, &out, &err))
        << "a lone surrogate must terminate serialisation, not be substituted";
    EXPECT_TRUE(err.contains(QStringLiteral("surrogate"))) << err.toStdString();

    // And a well-formed surrogate PAIR must still serialise — the check must
    // reject invalid UTF-16, not all non-BMP text.
    QJsonObject ok;
    // U+1F41C ANT, spelled as a code point: a narrow "\xF0\x9F…" literal would
    // be widened as Latin-1 and never reach the surrogate-pair path at all.
    ok.insert(QStringLiteral("body"), QStringLiteral("\U0001F41C"));
    err.clear();
    EXPECT_TRUE(JsonCanonical::serialise(ok, &out, &err)) << err.toStdString();
    EXPECT_EQ(out.toStdString(), std::string("{\"body\":\"\xF0\x9F\x90\x9C\"}"));
}

// INV-19 leg 4 — key order is JCS's, not insertion order or a reading order.
// § 2.2 warns that § 2.3's record shapes are written readably for humans and
// are NOT byte-exact; this is what makes that warning testable.
TEST(RoadmapExportCanonical, Inv19KeysSortByUtf16CodeUnit) {
    QJsonObject o;
    o.insert(QStringLiteral("t"), QStringLiteral("item"));
    o.insert(QStringLiteral("id"), QStringLiteral("ANTS-1"));
    o.insert(QStringLiteral("é"), 1);   // é, U+00E9 — sorts after ASCII
    o.insert(QStringLiteral("Z"), 2);        // uppercase sorts before lowercase

    QByteArray out;
    QString err;
    ASSERT_TRUE(JsonCanonical::serialise(o, &out, &err)) << err.toStdString();
    EXPECT_EQ(out.toStdString(),
              std::string("{\"Z\":2,\"id\":\"ANTS-1\",\"t\":\"item\",\"\xC3\xA9\":1}"));
}

// ---------------------------------------------------------------------------
// The synthetic three-project fixture INV-1 names. Never the machine's real
// corpus, which is not present in CI — and never anything clock-derived: every
// date here is a literal, because a fixture that changes at midnight cannot
// support a byte-identity invariant.

namespace {

void sqlExec(QSqlDatabase db, const QString &sql, const QVariantList &binds = {}) {
    QSqlQuery q(db);
    ASSERT_TRUE(q.prepare(sql)) << q.lastError().text().toStdString() << " :: "
                                << sql.toStdString();
    for (const QVariant &b : binds)
        q.addBindValue(b);
    ASSERT_TRUE(q.exec()) << q.lastError().text().toStdString() << " :: " << sql.toStdString();
}

QStringList sqlRows(QSqlDatabase db, const QString &sql) {
    QStringList out;
    QSqlQuery q(db);
    EXPECT_TRUE(q.exec(sql)) << q.lastError().text().toStdString() << " :: " << sql.toStdString();
    while (q.next()) {
        QStringList cells;
        for (int i = 0; i < q.record().count(); ++i)
            cells << (q.value(i).isNull() ? QStringLiteral("\x01NULL") : q.value(i).toString());
        out << cells.join(QChar(0x1F));
    }
    return out;
}

QString canon(const QJsonValue &v) {
    QByteArray b;
    QString err;
    EXPECT_TRUE(JsonCanonical::serialise(v, &b, &err)) << err.toStdString();
    return QString::fromUtf8(b);
}

qint64 addProject(RoadmapStore &s, const QString &rootDir, const QString &slug,
                  const QString &name) {
    const QString root = rootDir + QStringLiteral("/") + slug;
    QDir().mkpath(root);
    QString err;
    const auto pk = s.registerProject(root, name, slug, &err);
    EXPECT_TRUE(pk.has_value()) << err.toStdString();
    return pk.value_or(-1);
}

qint64 addSection(RoadmapStore &s, qint64 project, const QString &slug, const QString &title,
                  int level, std::optional<qint64> parent = std::nullopt) {
    QString err;
    const auto id = s.addSection(project, slug, title, level, parent, &err);
    EXPECT_TRUE(id.has_value()) << err.toStdString();
    return id.value_or(-1);
}

qint64 addItem(RoadmapStore &s, qint64 project, qint64 section, int position, const QString &id,
               const QString &origin = QStringLiteral("parsed")) {
    RoadmapStore::ItemWrite w;
    w.projectId = project;
    w.sectionId = section;
    w.position = position;
    w.id = id;
    w.idOrigin = origin;
    w.status = QStringLiteral("planned");
    w.headline = QStringLiteral("Headline for ") + id;
    w.kind = QStringLiteral("implement");
    w.source = QStringLiteral("fixture");
    QString err;
    const auto pk = s.putItem(w, &err);
    EXPECT_TRUE(pk.has_value()) << err.toStdString();
    return pk.value_or(-1);
}

// alpha exercises every record type and every variant of the multi-variant
// ones; beta is the far side of the cross-project edge; gamma is the sparse
// case — no legend, no id_prefix, every optional item column NULL — which is
// what walks § 2.4's "omitted" column rather than its "always emitted" one.
void buildFixture(RoadmapStore &s, const QString &rootDir) {
    QSqlDatabase db = s.db();

    // ---- alpha ----
    const qint64 alpha = addProject(s, rootDir, QStringLiteral("alpha"), QStringLiteral("Alpha"));
    // Out of prefix order on purpose: § 2.4 sorts id_prefix by `prefix`.
    sqlExec(db, QStringLiteral("INSERT INTO id_prefix VALUES (?,?,?)"),
            {alpha, QStringLiteral("cl"), 9});
    sqlExec(db, QStringLiteral("INSERT INTO id_prefix VALUES (?,?,?)"),
            {alpha, QStringLiteral("ants"), 3761});
    // Four statuses whose DECLARED order (§ 7.3) differs from their
    // alphabetical order, so the test can tell the two apart.
    QJsonObject legend;
    legend.insert(QStringLiteral("considered"), QStringLiteral("Considered"));
    legend.insert(QStringLiteral("in-progress"), QStringLiteral("In progress"));
    legend.insert(QStringLiteral("planned"), QStringLiteral("Planned"));
    legend.insert(QStringLiteral("shipped"), QStringLiteral("Shipped"));
    sqlExec(db, QStringLiteral("UPDATE project SET legend = ? WHERE project_id = ?"),
            {RoadmapStore::canonicalJson(legend), alpha});

    const qint64 aRoot = addSection(s, alpha, QStringLiteral(""), QStringLiteral(""), 0);
    const qint64 perf = addSection(s, alpha, QStringLiteral("performance-2"),
                                   QStringLiteral("Performance"), 3);
    addSection(s, alpha, QStringLiteral("vt-parser"), QStringLiteral("VT parser"), 4, perf);
    // A child whose slug sorts BEFORE its parent's. Without the parents-first
    // term in § 2.4 the child would be emitted first, and a single-pass rebuild
    // could not resolve its parent.
    const qint64 zParent = addSection(s, alpha, QStringLiteral("z-parent"),
                                      QStringLiteral("Z parent"), 2);
    const qint64 aChild = addSection(s, alpha, QStringLiteral("a-child"),
                                     QStringLiteral("A child"), 3, zParent);

    // Insertion order is deliberately NOT id order (ANTS-10 before ANTS-9), so
    // an export that emits in rowid order fails INV-5 and INV-18.
    addItem(s, alpha, perf, 0, QStringLiteral("ANTS-10"));
    const qint64 ants9 = addItem(s, alpha, perf, 1, QStringLiteral("ANTS-9"));
    addItem(s, alpha, perf, 2, QStringLiteral("CL-9"));
    addItem(s, alpha, perf, 3, QStringLiteral("CL-0009"));
    addItem(s, alpha, perf, 4, QStringLiteral("PASS-43-5"));
    addItem(s, alpha, perf, 5, QStringLiteral("PASS-43-5-B"), QStringLiteral("synthesised"));
    addItem(s, alpha, perf, 6, QStringLiteral("PASS-9-1"), QStringLiteral("synthesised"));
    addItem(s, alpha, perf, 7, QStringLiteral("3D_E-0007"));
    addItem(s, alpha, perf, 8, QStringLiteral("3DE-0007"));
    addItem(s, alpha, aRoot, 0, QStringLiteral("@@weird"), QStringLiteral("quarantined"));

    // A deleted row, so rowids carry a gap the rebuild cannot reproduce — the
    // condition under which a serialised surrogate breaks INV-1.
    const qint64 doomed = addItem(s, alpha, aChild, 0, QStringLiteral("ANTS-99"));
    sqlExec(db, QStringLiteral("DELETE FROM element WHERE item_pk = ?"), {doomed});
    sqlExec(db, QStringLiteral("DELETE FROM item WHERE item_pk = ?"), {doomed});

    // ANTS-9 carries every optional column, so the "always emitted" half of
    // § 2.4's table is exercised alongside gamma's "omitted" half.
    sqlExec(db,
            QStringLiteral("UPDATE item SET layman=?, priority=?, milestone=?, resolution=?, "
                           "body=?, created=?, last_modified=?, shipped=?, visibility=?, "
                           "lanes=?, evidence=?, extras=? WHERE item_pk=?"),
            {QStringLiteral("A plain-English line."), 2, QStringLiteral("0.8.0"),
             QStringLiteral("Resolved by measurement."),
             QStringLiteral("Body prose.\nSecond line, with a \"quote\" and a \\ backslash."),
             QStringLiteral("2026-07-01"), QStringLiteral("2026-07-30"),
             QStringLiteral("2026-07-30"), QStringLiteral("internal"),
             canon(QJsonArray{QStringLiteral("vt"), QStringLiteral("render")}),
             canon(QJsonArray{QStringLiteral("docs/qa/shot.png")}),
             // 0.000001 is the exact value ECMAScript keeps in fixed notation
             // and QJsonDocument writes as 1e-06 — so this column carries the
             // number-rendering contract all the way through the round trip.
             canon(QJsonObject{{QStringLiteral("note"), QStringLiteral("héllo")},
                               {QStringLiteral("ratio"), 0.000001},
                               {QStringLiteral("tally"), 42}}),
             ants9});

    sqlExec(db, QStringLiteral("INSERT INTO element (section_id, position, kind, payload) "
                               "VALUES (?, ?, 'narration', ?)"),
            {perf, 9, QStringLiteral("Prose belonging to no item.\nWith a \"quote\".")});
    sqlExec(db, QStringLiteral("INSERT INTO element (section_id, position, kind, payload) "
                               "VALUES (?, ?, 'table', ?)"),
            {perf, 10,
             canon(QJsonObject{
                 {QStringLiteral("header"), QJsonArray{QStringLiteral("A"), QStringLiteral("B")}},
                 {QStringLiteral("rows"),
                  QJsonArray{QJsonArray{QStringLiteral("1"), QStringLiteral("2")}}}})});

    // ---- beta ----
    const qint64 beta = addProject(s, rootDir, QStringLiteral("beta"), QStringLiteral("Beta"));
    const qint64 bRoot = addSection(s, beta, QStringLiteral(""), QStringLiteral(""), 0);
    addItem(s, beta, bRoot, 0, QStringLiteral("BETA-1"));
    addItem(s, beta, bRoot, 1, QStringLiteral("BETA-2"));
    sqlExec(db, QStringLiteral("INSERT INTO id_prefix VALUES (?,?,?)"),
            {beta, QStringLiteral("beta"), 2});

    // ---- gamma: the sparse project ----
    const qint64 gamma = addProject(s, rootDir, QStringLiteral("gamma"), QStringLiteral("Gamma"));
    const qint64 gRoot = addSection(s, gamma, QStringLiteral(""), QStringLiteral(""), 0);
    addItem(s, gamma, gRoot, 0, QStringLiteral("GAMMA-1"));

    // ---- the three relationship variants, all anchored on alpha ----
    const qint64 ants10 =
        sqlRows(db, QStringLiteral("SELECT item_pk FROM item WHERE id = 'ANTS-10'")).first().toLongLong();
    QString err;
    EXPECT_TRUE(s.relateItems(QStringLiteral("blocked-by"), ants9, ants10, &err))
        << err.toStdString();
    EXPECT_TRUE(s.relateCrossProject(QStringLiteral("blocked-by"), ants9, QStringLiteral("beta"),
                                     QStringLiteral("beta-1"), &err))
        << err.toStdString();
    sqlExec(db, QStringLiteral("INSERT INTO relationship (type, src_pk, dst_path) "
                               "VALUES ('specified-by', ?, ?)"),
            {ants9, QStringLiteral("docs/specs/ANTS-9-thing.md")});

    // ---- citations, both anchorings; a feedback ref; two history rows ----
    sqlExec(db, QStringLiteral("INSERT INTO citation (project_id, item_pk, target_file, symbol) "
                               "VALUES (?, ?, ?, ?)"),
            {alpha, ants9, QStringLiteral("src/vtparser.cpp"), QStringLiteral("VtParser::feed")});
    sqlExec(db, QStringLiteral("INSERT INTO citation (project_id, doc_path, target_file, symbol) "
                               "VALUES (?, ?, ?, ?)"),
            {alpha, QStringLiteral("docs/specs/ANTS-9-thing.md"), QStringLiteral("src/vtparser.cpp"),
             QStringLiteral("VtParser::feed")});
    sqlExec(db, QStringLiteral("INSERT INTO feedback_ref VALUES (?, ?)"),
            {ants9, QStringLiteral("Vestige_Ants_MCP_Feedback.md")});
    // Same second, two seq values: without `seq` the history sort is not total.
    // The first revision has no `old`, which is § 2.4's commonest omitted field.
    EXPECT_TRUE(s.appendHistory(ants9, QStringLiteral("2026-07-30T09:15:00Z"), 0,
                                QStringLiteral("status"), QString(), QStringLiteral("planned"),
                                &err))
        << err.toStdString();
    EXPECT_TRUE(s.appendHistory(ants9, QStringLiteral("2026-07-30T09:15:00Z"), 1,
                                QStringLiteral("status"), QStringLiteral("planned"),
                                QStringLiteral("shipped"), &err))
        << err.toStdString();
}

QByteArray exportOf(RoadmapStore &s, const QString &slug) {
    QByteArray bytes;
    QBuffer buf(&bytes);
    EXPECT_TRUE(buf.open(QIODevice::WriteOnly));
    QString err;
    EXPECT_TRUE(RoadmapExport::writeProject(s, slug, &buf, &err)) << err.toStdString();
    return bytes;
}

// A store plus its fixture, opened Bulk — ANTS-3756 § 2.5 assigns the export
// that profile (a 30 s deadline, because an export knows it may queue behind a
// long transaction).
struct Fixture {
    QTemporaryDir dir;
    std::unique_ptr<RoadmapStore> store;

    Fixture() {
        store = std::make_unique<RoadmapStore>(dir.path() + QStringLiteral("/roadmap.sqlite"),
                                               RoadmapStore::kDefaultHistoryCapBytes,
                                               RoadmapStore::Access::Bulk);
        QString err;
        EXPECT_TRUE(store->open(&err)) << err.toStdString();
        buildFixture(*store, dir.path());
    }
};

// The nine TABLES (ten record types — `legend` is a column on `project`, not a
// table), each projected onto its non-surrogate columns and joined on the
// stable identity INV-2 names. Every rowid-valued column and project.root are
// excluded: § 2.3 guarantees rowids differ after a rebuild, so a diff
// including them would fail against a CORRECT implementation.
const QList<QPair<const char *, const char *>> &inv2Projections() {
    static const QList<QPair<const char *, const char *>> kP = {
        {"project", "SELECT export_slug, name, legend FROM project ORDER BY export_slug"},
        {"id_prefix",
         "SELECT p.export_slug, x.prefix, x.high_water FROM id_prefix x "
         "JOIN project p USING(project_id) ORDER BY 1, 2"},
        {"section",
         "SELECT p.export_slug, s.slug, s.title, s.level, s.intro, par.slug FROM section s "
         "JOIN project p USING(project_id) LEFT JOIN section par ON par.section_id = s.parent_id "
         "ORDER BY 1, 2"},
        {"item",
         "SELECT p.export_slug, i.id_fold, i.id, i.id_origin, i.status, i.headline, i.layman, "
         "i.kind, i.source, i.priority, i.visibility, i.milestone, i.resolution, i.body, "
         "i.created, i.last_modified, i.shipped, i.lanes, i.evidence, i.extras, i.provenance "
         "FROM item i JOIN project p USING(project_id) ORDER BY 1, 2"},
        {"element",
         "SELECT p.export_slug, s.slug, e.position, e.kind, i.id_fold, e.payload FROM element e "
         "JOIN section s USING(section_id) JOIN project p ON p.project_id = s.project_id "
         "LEFT JOIN item i ON i.item_pk = e.item_pk ORDER BY 1, 2, 3"},
        // dst_project leads the store's own rel_xproj_uq, and two projects can
        // hold the same folded id — a key omitting it merges a cross-project
        // edge with a same-project one.
        {"relationship",
         "SELECT p.export_slug, r.type, s.id_fold, d.id_fold, r.dst_project, r.dst_id_fold, "
         "r.dst_path FROM relationship r JOIN item s ON s.item_pk = r.src_pk "
         "JOIN project p ON p.project_id = s.project_id LEFT JOIN item d ON d.item_pk = r.dst_pk "
         "ORDER BY 1, 2, 3, 4, 5, 6, 7"},
        // project likewise LEADS cite_doc_uq: two projects each citing their
        // own README.md are two rows, not a collision.
        {"citation",
         "SELECT p.export_slug, i.id_fold, c.doc_path, c.target_file, c.symbol FROM citation c "
         "JOIN project p USING(project_id) LEFT JOIN item i ON i.item_pk = c.item_pk "
         "ORDER BY 1, 2, 3, 4, 5"},
        {"feedback_ref",
         "SELECT p.export_slug, i.id_fold, f.file FROM feedback_ref f "
         "JOIN item i USING(item_pk) JOIN project p ON p.project_id = i.project_id ORDER BY 1, 2, 3"},
        {"history",
         "SELECT p.export_slug, i.id_fold, h.changed_at, h.seq, h.field, h.old_value, h.new_value "
         "FROM history h JOIN item i USING(item_pk) JOIN project p ON p.project_id = i.project_id "
         "ORDER BY 1, 2, 3, 4"},
    };
    return kP;
}

QStringList recordsOfType(const QByteArray &jsonl, const char *type) {
    QStringList out;
    for (const QByteArray &line : jsonl.split('\n')) {
        if (line.isEmpty())
            continue;
        const QJsonObject o = QJsonDocument::fromJson(line).object();
        if (o.value(QStringLiteral("t")).toString() == QLatin1String(type))
            out << QString::fromUtf8(line);
    }
    return out;
}

}  // namespace

// INV-18 — the fixture's export matches a COMMITTED golden file, byte for
// byte. This is the invariant that makes § 2.4 testable at all: INV-1 compares
// the writer against itself, so any deterministic writer passes it, including
// one that emits records in insertion order. Regenerating a golden file is a
// reviewable diff and must never be done to make a test pass.
TEST(RoadmapExportRoundtrip, Inv18MatchesCommittedGoldenFiles) {
    Fixture fx;
    // Regeneration is deliberately NOT a way to turn a red run green: with
    // ANTS_REGENERATE_EXPORT_GOLDEN=1 the files are rewritten AND the test
    // still fails, so the only way to a green run is to read the resulting
    // diff and commit it. The escape hatch exists because the alternative —
    // hand-editing a golden file — is strictly worse.
    if (qEnvironmentVariableIsSet("ANTS_REGENERATE_EXPORT_GOLDEN")) {
        QDir().mkpath(QStringLiteral(ANTS_EXPORT_GOLDEN_DIR));
        for (const char *slug : {"alpha", "beta", "gamma"}) {
            QFile g(QStringLiteral(ANTS_EXPORT_GOLDEN_DIR "/") + QLatin1String(slug) +
                    QStringLiteral(".jsonl"));
            ASSERT_TRUE(g.open(QIODevice::WriteOnly));
            g.write(exportOf(*fx.store, QLatin1String(slug)));
        }
        FAIL() << "golden files regenerated — review the diff, then re-run without "
                  "ANTS_REGENERATE_EXPORT_GOLDEN";
    }
    for (const char *slug : {"alpha", "beta", "gamma"}) {
        QFile g(QStringLiteral(ANTS_EXPORT_GOLDEN_DIR "/") + QLatin1String(slug) +
                QStringLiteral(".jsonl"));
        ASSERT_TRUE(g.open(QIODevice::ReadOnly)) << "missing golden file for " << slug;
        EXPECT_EQ(exportOf(*fx.store, QLatin1String(slug)).toStdString(),
                  g.readAll().toStdString())
            << "golden mismatch: " << slug;
    }
}

// INV-1 — export, rebuild from that export, re-export ⇒ byte-identical files.
// Stated across the WHOLE CORPUS as well as per project: a corpus-wide rebuild
// must preserve the cross-project relationships the model's INV-4 allows,
// which a per-project round trip cannot witness.
TEST(RoadmapExportRoundtrip, Inv1RoundTripIsByteIdentical) {
    Fixture fx;

    QHash<QString, QByteArray> first;
    for (const char *slug : {"alpha", "beta", "gamma"})
        first.insert(QLatin1String(slug), exportOf(*fx.store, QLatin1String(slug)));

    QTemporaryDir rebuiltDir;
    RoadmapStore rebuilt(rebuiltDir.path() + QStringLiteral("/roadmap.sqlite"),
                         RoadmapStore::kDefaultHistoryCapBytes, RoadmapStore::Access::Bulk);
    QString err;
    ASSERT_TRUE(rebuilt.open(&err)) << err.toStdString();
    for (const char *slug : {"alpha", "beta", "gamma"}) {
        QByteArray bytes = first.value(QLatin1String(slug));
        QBuffer buf(&bytes);
        ASSERT_TRUE(buf.open(QIODevice::ReadOnly));
        ASSERT_TRUE(RoadmapExport::rebuildProject(rebuilt, &buf, &err))
            << slug << ": " << err.toStdString();
    }

    for (const char *slug : {"alpha", "beta", "gamma"})
        EXPECT_EQ(exportOf(rebuilt, QLatin1String(slug)).toStdString(),
                  first.value(QLatin1String(slug)).toStdString())
            << "round trip differs: " << slug;

    // The cross-project edge survives on the SOURCE project's file, with
    // dst_project naming the far side.
    const QStringList rels = recordsOfType(first.value(QStringLiteral("alpha")), "rel");
    EXPECT_TRUE(rels.filter(QStringLiteral("\"dst_project\":\"beta\"")).size() == 1)
        << rels.join(QChar('\n')).toStdString();
    EXPECT_TRUE(recordsOfType(first.value(QStringLiteral("beta")), "rel").isEmpty())
        << "the far side must not carry a duplicate of the same logical edge";
}

// INV-2 — the export is COMPLETE: every store row, and every non-surrogate
// column of it, survives the round trip. Distinct from INV-1, which a writer
// that drops a whole column passes: the drop round-trips byte-identically and
// preserves every row count.
TEST(RoadmapExportRoundtrip, Inv2EveryRowAndColumnSurvives) {
    Fixture fx;
    QTemporaryDir rebuiltDir;
    RoadmapStore rebuilt(rebuiltDir.path() + QStringLiteral("/roadmap.sqlite"),
                         RoadmapStore::kDefaultHistoryCapBytes, RoadmapStore::Access::Bulk);
    QString err;
    ASSERT_TRUE(rebuilt.open(&err)) << err.toStdString();
    for (const char *slug : {"alpha", "beta", "gamma"}) {
        QByteArray bytes = exportOf(*fx.store, QLatin1String(slug));
        QBuffer buf(&bytes);
        ASSERT_TRUE(buf.open(QIODevice::ReadOnly));
        ASSERT_TRUE(RoadmapExport::rebuildProject(rebuilt, &buf, &err)) << err.toStdString();
    }

    for (const auto &p : inv2Projections()) {
        const QString countSql =
            QStringLiteral("SELECT COUNT(*) FROM ") + QLatin1String(p.first);
        EXPECT_EQ(sqlRows(fx.store->db(), countSql), sqlRows(rebuilt.db(), countSql))
            << "row count differs for table " << p.first;
        const QStringList before = sqlRows(fx.store->db(), QLatin1String(p.second));
        const QStringList after = sqlRows(rebuilt.db(), QLatin1String(p.second));
        EXPECT_EQ(before, after) << "column diff for table " << p.first;
        EXPECT_FALSE(before.isEmpty()) << "table " << p.first << " has no fixture rows, so this "
                                          "projection proves nothing";
    }
}

// INV-5 — item order follows § 2.5's numeric-segment sort, and is TOTAL.
TEST(RoadmapExportRoundtrip, Inv5ItemOrderIsTheNumericSegmentSort) {
    Fixture fx;
    QStringList ids;
    for (const QString &line : recordsOfType(exportOf(*fx.store, QStringLiteral("alpha")), "item"))
        ids << QJsonDocument::fromJson(line.toUtf8()).object().value(QStringLiteral("id")).toString();

    EXPECT_EQ(ids, (QStringList{
                       // Rule 3 — a numeric first run sorts before an alphabetic one.
                       // Rule 1 — remove-then-split makes these two tuples IDENTICAL,
                       // so rule 5's raw-id tie-break is what orders them ('E' < '_').
                       QStringLiteral("3DE-0007"), QStringLiteral("3D_E-0007"),
                       // Rule 2 — numeric runs compare by value: 9 before 10.
                       QStringLiteral("ANTS-9"), QStringLiteral("ANTS-10"),
                       // Rule 5 again — one tuple, ordered only by the raw id.
                       QStringLiteral("CL-0009"), QStringLiteral("CL-9"),
                       // Rule 1 — splitting at the LAST hyphen would make the
                       // prefix `pass-43` and put PASS-9-1 last.
                       QStringLiteral("PASS-9-1"), QStringLiteral("PASS-43-5"),
                       // Rule 4 — the shorter tuple sorts first.
                       QStringLiteral("PASS-43-5-B"),
                       // Rule 6 — quarantined ids last, and only those.
                       QStringLiteral("@@weird"),
                   }));
}

// INV-13 — every cross-record reference resolves to a declared stable key, and
// no surrogate value is emitted under any name. A name-based grep for item_pk
// is NOT sufficient: it passes against a writer emitting the same rowids under
// a different key ("section":3), and false-positives on free-text body.
TEST(RoadmapExportRoundtrip, Inv13EveryReferenceResolves) {
    Fixture fx;
    const QByteArray jsonl = exportOf(*fx.store, QStringLiteral("alpha"));

    QSet<QString> itemFolds, sectionSlugs, docPaths;
    QList<QJsonObject> records;
    for (const QByteArray &line : jsonl.split('\n')) {
        if (line.isEmpty())
            continue;
        const QJsonObject o = QJsonDocument::fromJson(line).object();
        records << o;
        const QString t = o.value(QStringLiteral("t")).toString();
        if (t == QLatin1String("item"))
            // Folding before comparison: references are folded, item.id is authored.
            itemFolds.insert(o.value(QStringLiteral("id")).toString().toLower());
        else if (t == QLatin1String("section"))
            sectionSlugs.insert(o.value(QStringLiteral("slug")).toString());
        else if (t == QLatin1String("rel") && o.contains(QStringLiteral("dst_path")))
            docPaths.insert(o.value(QStringLiteral("dst_path")).toString());
    }
    ASSERT_FALSE(itemFolds.isEmpty());

    int checked = 0;
    for (const QJsonObject &o : records) {
        const QString t = o.value(QStringLiteral("t")).toString();
        const auto resolvesItem = [&](const char *key) {
            if (!o.contains(QLatin1String(key)))
                return;
            ++checked;
            EXPECT_TRUE(itemFolds.contains(o.value(QLatin1String(key)).toString()))
                << t.toStdString() << '.' << key << " → "
                << o.value(QLatin1String(key)).toString().toStdString();
        };
        if (t == QLatin1String("element"))
            resolvesItem("ref");
        if (t == QLatin1String("feedback_ref") || t == QLatin1String("history"))
            resolvesItem("item");
        if (t == QLatin1String("rel")) {
            resolvesItem("src");
            // A cross-project dst names the far project's item and is
            // deliberately NOT resolvable here — INV-4 allows the edge and the
            // far project may be absent entirely.
            if (!o.contains(QStringLiteral("dst_project")))
                resolvesItem("dst");
        }
        if (t == QLatin1String("citation"))
            resolvesItem("src");
        for (const char *key : {"section", "parent"}) {
            if (!o.contains(QLatin1String(key)) || o.value(QLatin1String(key)).isNull())
                continue;
            ++checked;
            EXPECT_TRUE(sectionSlugs.contains(o.value(QLatin1String(key)).toString()))
                << t.toStdString() << '.' << key;
        }
        if (t == QLatin1String("citation") && o.contains(QStringLiteral("doc"))) {
            ++checked;
            EXPECT_TRUE(docPaths.contains(o.value(QStringLiteral("doc")).toString()))
                << "citation.doc names a document no rel declares";
        }
    }
    EXPECT_GT(checked, 15) << "the fixture stopped exercising the reference kinds";

    // No surrogate under ANY name. Every value the export emits for a
    // reference is a string; a rowid would arrive as a bare number.
    for (const QJsonObject &o : records) {
        for (const char *key : {"ref", "src", "dst", "item", "section", "parent", "doc"}) {
            if (o.contains(QLatin1String(key))) {
                EXPECT_FALSE(o.value(QLatin1String(key)).isDouble())
                    << key << " is a number, which can only be a rowid";
            }
        }
    }
}

namespace {

// The delta INV-12 measures is against /proc, not against Qt's own accounting:
// what the invariant claims is that the process does not grow, and only the
// kernel knows that.
qint64 rssBytes() {
    QFile f(QStringLiteral("/proc/self/statm"));
    if (!f.open(QIODevice::ReadOnly))
        return -1;
    const QList<QByteArray> fields = f.readAll().simplified().split(' ');
    return fields.size() > 1 ? fields.at(1).toLongLong() * ::sysconf(_SC_PAGESIZE) : -1;
}

}  // namespace

// INV-12 — the export writer STREAMS: peak RSS across the export call rises by
// less than 4 MiB above the pre-call baseline, against an export several times
// that size. Breaks when the writer builds one QString and writes it at the
// end, which passes every other invariant here.
//
// The measurement must be a delta and it must be sampled. An absolute ceiling
// is unachievable — a Qt process's RSS exceeds the export's byte size before
// any work is done — and reading RSS before and after would miss the peak
// entirely, an export being synchronous.
TEST(RoadmapExportRoundtrip, Inv12PeakRssDeltaStaysUnderFourMiB) {
    QTemporaryDir dir;
    const QString dbPath = dir.path() + QStringLiteral("/roadmap.sqlite");
    constexpr int kItems = 50;
    constexpr int kBodyBytes = 100 * 1024;   // 50 × 100 KiB ≈ 5 MiB of body text

    {
        RoadmapStore seed(dbPath, RoadmapStore::kDefaultHistoryCapBytes,
                          RoadmapStore::Access::Bulk);
        QString err;
        ASSERT_TRUE(seed.open(&err)) << err.toStdString();
        const qint64 p = addProject(seed, dir.path(), QStringLiteral("big"), QStringLiteral("Big"));
        const qint64 sect = addSection(seed, p, QStringLiteral(""), QStringLiteral(""), 0);
        for (int i = 0; i < kItems; ++i) {
            const qint64 pk =
                addItem(seed, p, sect, i, QStringLiteral("BIG-%1").arg(i + 1, 4, 10, QChar('0')));
            sqlExec(seed.db(), QStringLiteral("UPDATE item SET body = ? WHERE item_pk = ?"),
                    {QString(kBodyBytes, QChar('a' + (i % 26))), pk});
        }
    }

    // Reopened, so SQLite's page cache starts cold and the export's reads are
    // real reads. Measuring against a store still warm from its own writes
    // would pass without the writer streaming at all.
    RoadmapStore store(dbPath, RoadmapStore::kDefaultHistoryCapBytes, RoadmapStore::Access::Bulk);
    QString err;
    ASSERT_TRUE(store.open(&err)) << err.toStdString();

    QFile out(dir.path() + QStringLiteral("/big.jsonl"));
    ASSERT_TRUE(out.open(QIODevice::WriteOnly));

    const qint64 baseline = rssBytes();
    ASSERT_GT(baseline, 0);
    std::atomic<qint64> peak{baseline};
    std::atomic<bool> stop{false};
    std::thread watcher([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            const qint64 now = rssBytes();
            if (now > peak.load(std::memory_order_relaxed))
                peak.store(now, std::memory_order_relaxed);
            ::usleep(10 * 1000);   // 10 ms
        }
    });

    const bool ok = RoadmapExport::writeProject(store, QStringLiteral("big"), &out, &err);
    stop.store(true, std::memory_order_relaxed);
    watcher.join();
    out.close();

    ASSERT_TRUE(ok) << err.toStdString();
    ASSERT_GT(QFileInfo(out.fileName()).size(), 4 * 1024 * 1024)
        << "the fixture is not large enough for this measurement to mean anything";

    const qint64 delta = peak.load() - baseline;
    EXPECT_LT(delta, 4 * 1024 * 1024)
        << "peak RSS grew by " << delta / 1024 << " KiB across an export of "
        << QFileInfo(out.fileName()).size() / 1024 << " KiB";
}
