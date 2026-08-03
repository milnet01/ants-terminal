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

#include <algorithm>
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
                  int level, int position, std::optional<qint64> parent = std::nullopt) {
    QString err;
    const auto id = s.addSection(project, slug, title, level, position, parent, &err);
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

    // ANTS-3796 — positions are insertion order here, which for alpha is
    // already neither slug order nor (depth, slug) order. This fixture is
    // golden-backed, so its only job for the new column is to carry a value
    // through § 2.4's record shape; the ORDERING invariants (INV-1, INV-5) are
    // built in-test against their own stores, deliberately not seeded here —
    // adding sections to this project would rewrite every record in
    // alpha.jsonl and make the reviewed golden diff unreadable in the same pass
    // that changes the record shape.
    const qint64 aRoot = addSection(s, alpha, QStringLiteral(""), QStringLiteral(""), 0, 0);
    const qint64 perf = addSection(s, alpha, QStringLiteral("performance-2"),
                                   QStringLiteral("Performance"), 3, 1);
    addSection(s, alpha, QStringLiteral("vt-parser"), QStringLiteral("VT parser"), 4, 2, perf);
    // A child whose slug sorts BEFORE its parent's. Without the parents-first
    // term in § 2.4 the child would be emitted first, and a single-pass rebuild
    // could not resolve its parent.
    const qint64 zParent = addSection(s, alpha, QStringLiteral("z-parent"),
                                      QStringLiteral("Z parent"), 2, 3);
    const qint64 aChild = addSection(s, alpha, QStringLiteral("a-child"),
                                     QStringLiteral("A child"), 3, 4, zParent);

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
    const qint64 bRoot = addSection(s, beta, QStringLiteral(""), QStringLiteral(""), 0, 0);
    addItem(s, beta, bRoot, 0, QStringLiteral("BETA-1"));
    addItem(s, beta, bRoot, 1, QStringLiteral("BETA-2"));
    sqlExec(db, QStringLiteral("INSERT INTO id_prefix VALUES (?,?,?)"),
            {beta, QStringLiteral("beta"), 2});

    // ---- gamma: the sparse project ----
    const qint64 gamma = addProject(s, rootDir, QStringLiteral("gamma"), QStringLiteral("Gamma"));
    const qint64 gRoot = addSection(s, gamma, QStringLiteral(""), QStringLiteral(""), 0, 0);
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
// table). Each is projected onto its non-surrogate columns and joined on the
// stable identity INV-2 names, and — since ANTS-3796 § 2.5 — the COLUMN LIST IS
// DERIVED FROM `PRAGMA table_info`, never written out. Three sets:
//
//   derived     every column the pragma reports. This is the point: an
//               enumeration built from a literal can confirm what is listed and
//               can never catch an omission, which is how section.source_path
//               (ANTS-3797) survived a shipped INV-2 for two specs.
//   excluded    project.root, which INV-2 excludes by name, plus each table's
//               OWN surrogate primary key. Named, not derived — and excluded
//               rather than substituted, because a surrogate PK need have no
//               single stable rendering: `element` is identified by the
//               composite (section slug, position), so demanding a one-column
//               substitute for element_id would fail against the correct
//               projection.
//   substituted every FOREIGN-KEY rowid column, replaced by the stable
//               rendering compared in its place. NOT the same set as "rowid
//               columns" and not the join keys: the old literals did not merely
//               omit parent_id / item_pk / project_id, they projected par.slug /
//               i.id_fold / p.export_slug INSTEAD, so a rule reading "derived
//               minus the rowid columns" would stop comparing section parentage
//               and element→item membership altogether.
//
// A foreign-key rowid with NO entry in the substitution map FAILS the diff
// rather than being quietly skipped — that is what keeps the map exhaustive as
// the schema grows, and INV-3 is what makes it a contract.

struct TableDiff {
    const char *table;
    const char *alias;
    const char *from;      // FROM + JOINs, aliased
    const char *orderBy;   // explicit expressions — never positional, because
                           // the select list is derived and its order is the
                           // pragma's, not this file's
};

const QList<TableDiff> &inv2Tables() {
    static const QList<TableDiff> kT = {
        {"project", "p", "FROM project p", "p.export_slug"},
        {"id_prefix", "x", "FROM id_prefix x JOIN project p USING(project_id)",
         "p.export_slug, x.prefix"},
        {"section", "s",
         "FROM section s JOIN project p USING(project_id) "
         "LEFT JOIN section par ON par.section_id = s.parent_id",
         "p.export_slug, s.slug"},
        {"item", "i", "FROM item i JOIN project p USING(project_id)",
         "p.export_slug, i.id_fold"},
        {"element", "e",
         "FROM element e JOIN section s USING(section_id) "
         "JOIN project p ON p.project_id = s.project_id "
         "LEFT JOIN item i ON i.item_pk = e.item_pk",
         "p.export_slug, s.slug, e.position"},
        // dst_project leads the store's own rel_xproj_uq, and two projects can
        // hold the same folded id — a key omitting it merges a cross-project
        // edge with a same-project one.
        {"relationship", "r",
         "FROM relationship r JOIN item s ON s.item_pk = r.src_pk "
         "JOIN project p ON p.project_id = s.project_id "
         "LEFT JOIN item d ON d.item_pk = r.dst_pk",
         "p.export_slug, r.type, s.id_fold, d.id_fold, r.dst_project, r.dst_id_fold, r.dst_path"},
        // project likewise LEADS cite_doc_uq: two projects each citing their
        // own README.md are two rows, not a collision.
        {"citation", "c",
         "FROM citation c JOIN project p USING(project_id) "
         "LEFT JOIN item i ON i.item_pk = c.item_pk",
         "p.export_slug, i.id_fold, c.doc_path, c.target_file, c.symbol"},
        {"feedback_ref", "f",
         "FROM feedback_ref f JOIN item i USING(item_pk) "
         "JOIN project p ON p.project_id = i.project_id",
         "p.export_slug, i.id_fold, f.file"},
        {"history", "h",
         "FROM history h JOIN item i USING(item_pk) "
         "JOIN project p ON p.project_id = i.project_id",
         "p.export_slug, i.id_fold, h.changed_at, h.seq"},
    };
    return kT;
}

// Excluded, keyed "<table>.<column>". project.root is INV-2's own named
// exclusion; the rest are each table's surrogate PK. id_prefix and feedback_ref
// appear nowhere here — their primary keys are composite over REAL columns,
// which are compared like any other.
const QSet<QString> &inv2Excluded() {
    static const QSet<QString> kE = {
        QStringLiteral("project.root"),      QStringLiteral("project.project_id"),
        QStringLiteral("section.section_id"), QStringLiteral("item.item_pk"),
        QStringLiteral("element.element_id"), QStringLiteral("relationship.rel_id"),
        QStringLiteral("history.history_id"), QStringLiteral("citation.citation_id"),
    };
    return kE;
}

// The substitution map, keyed "<table>.<column>". Returned BY VALUE and taken
// as a parameter by the diff rather than closed over, so INV-3's third leg can
// inject a deliberately incomplete one — without that, the invariant guarding
// exhaustiveness would be the one part of § 2.5 that ships untested.
QHash<QString, QString> inv2Substitutions() {
    return {
        {QStringLiteral("id_prefix.project_id"), QStringLiteral("p.export_slug")},
        {QStringLiteral("section.project_id"), QStringLiteral("p.export_slug")},
        {QStringLiteral("section.parent_id"), QStringLiteral("par.slug")},
        {QStringLiteral("item.project_id"), QStringLiteral("p.export_slug")},
        {QStringLiteral("element.section_id"), QStringLiteral("s.slug")},
        {QStringLiteral("element.item_pk"), QStringLiteral("i.id_fold")},
        {QStringLiteral("relationship.src_pk"), QStringLiteral("s.id_fold")},
        {QStringLiteral("relationship.dst_pk"), QStringLiteral("d.id_fold")},
        {QStringLiteral("citation.project_id"), QStringLiteral("p.export_slug")},
        {QStringLiteral("citation.item_pk"), QStringLiteral("i.id_fold")},
        {QStringLiteral("feedback_ref.item_pk"), QStringLiteral("i.id_fold")},
        {QStringLiteral("history.item_pk"), QStringLiteral("i.id_fold")},
    };
}

QStringList pragmaColumns(QSqlDatabase db, const QString &table) {
    QStringList out;
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table)))
        return out;
    while (q.next())
        out << q.value(1).toString();
    return out;
}

QSet<QString> pragmaForeignKeyColumns(QSqlDatabase db, const QString &table) {
    QSet<QString> out;
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("PRAGMA foreign_key_list(%1)").arg(table)))
        return out;
    // Column 3 is `from` — the local column the constraint is declared on.
    while (q.next())
        out.insert(q.value(3).toString());
    return out;
}

// True when the two stores agree on every table. On disagreement `why` names
// the table and the reason, and the caller decides whether that is a pass
// (INV-3) or a failure (INV-2).
bool inv2Diff(QSqlDatabase live, QSqlDatabase rebuilt, const QHash<QString, QString> &subs,
              QString *why) {
    const auto no = [&](const QString &msg) {
        *why = msg;
        return false;
    };
    for (const TableDiff &t : inv2Tables()) {
        const QString table = QLatin1String(t.table);

        // Read the pragma on BOTH stores and compare the SETS first. Reading it
        // once and reusing it makes this comparison vacuous, and a column
        // present on one side only would otherwise surface as a SQL error
        // against a projection naming a column the other store lacks — a
        // failure that reads as a broken test rather than as the finding.
        const QStringList liveCols = pragmaColumns(live, table);
        const QStringList rebuiltCols = pragmaColumns(rebuilt, table);
        if (liveCols.isEmpty())
            return no(table + QStringLiteral(": PRAGMA table_info returned nothing"));
        if (liveCols != rebuiltCols)
            return no(table + QStringLiteral(": column sets differ — live [") +
                      liveCols.join(QLatin1Char(',')) + QStringLiteral("] vs rebuilt [") +
                      rebuiltCols.join(QLatin1Char(',')) + QLatin1Char(']'));

        const QSet<QString> fks = pragmaForeignKeyColumns(live, table);
        QStringList select;
        for (const QString &col : liveCols) {
            const QString key = table + QLatin1Char('.') + col;
            if (inv2Excluded().contains(key))
                continue;
            if (fks.contains(col)) {
                const auto it = subs.constFind(key);
                if (it == subs.constEnd())
                    return no(key + QStringLiteral(" is a foreign-key rowid with no entry in "
                                                   "§ 2.5's substitution map — skipping it "
                                                   "silently would narrow INV-2"));
                select << *it;
                continue;
            }
            select << QLatin1String(t.alias) + QLatin1Char('.') + col;
        }
        if (select.isEmpty())
            return no(table + QStringLiteral(": every column was excluded"));

        const QString sql = QStringLiteral("SELECT ") + select.join(QStringLiteral(", ")) +
                            QLatin1Char(' ') + QLatin1String(t.from) +
                            QStringLiteral(" ORDER BY ") + QLatin1String(t.orderBy);
        const QString countSql = QStringLiteral("SELECT COUNT(*) FROM ") + table;
        if (sqlRows(live, countSql) != sqlRows(rebuilt, countSql))
            return no(table + QStringLiteral(": row count differs"));
        const QStringList before = sqlRows(live, sql);
        if (before.isEmpty())
            return no(table + QStringLiteral(" has no fixture rows, so this projection proves "
                                             "nothing"));
        if (before != sqlRows(rebuilt, sql))
            return no(table + QStringLiteral(": column diff — ") + sql);
    }
    return true;
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

    QString why;
    EXPECT_TRUE(inv2Diff(fx.store->db(), rebuilt.db(), inv2Substitutions(), &why))
        << why.toStdString();
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

// ============================================================================
// ANTS-3796 / ANTS-3797 — section record completeness. Test names carry the
// spec id because this file already holds an ANTS-3761 INV-1, INV-2, INV-3 and
// INV-5, and a bare Inv<N> would resolve two ways.
// Contract: docs/specs/ANTS-3796-section-record-completeness.md
// ============================================================================

namespace {

// Document order: zeta (level 2), alpha (level 3, child of zeta), mid (level
// 2). Chosen so that all THREE candidate orders are different sequences —
// document (zeta, alpha, mid), slug (alpha, mid, zeta) and the export's
// (depth, slug) emission order (mid, zeta, alpha). A two-section fixture, or a
// three-section one that happened to be in slug order, would pass against a
// rebuild that recomputed the ordinal, which is the vacuity § 6 warns about.
//
// Built in-test rather than seeded into buildFixture()'s golden-backed
// projects: this exercises an ORDERING, which a committed golden cannot witness
// any better than an assertion can, and adding five sections to alpha would
// rewrite every record in alpha.jsonl in the same pass that changes the record
// shape — making INV-18's reviewed diff unreadable.
qint64 buildOrderFixture(RoadmapStore &s, const QString &rootDir) {
    const qint64 p = addProject(s, rootDir, QStringLiteral("ord"), QStringLiteral("Ord"));
    const qint64 zeta = addSection(s, p, QStringLiteral("zeta"), QStringLiteral("Zeta"), 2, 0);
    addSection(s, p, QStringLiteral("alpha"), QStringLiteral("Alpha"), 3, 1, zeta);
    addSection(s, p, QStringLiteral("mid"), QStringLiteral("Mid"), 2, 2);

    // ANTS-3797's leg: one section carries a rotated-archive path and one
    // carries NULL, so a rebuild that drops the column fails on the first and
    // would still pass on the second.
    QString err;
    const auto alphaId = s.findSection(p, QStringLiteral("alpha"), &err);
    EXPECT_TRUE(alphaId.has_value()) << err.toStdString();
    if (alphaId)
        EXPECT_TRUE(s.setSectionSource(*alphaId, QStringLiteral("docs/roadmap/0.6.md"), &err))
            << err.toStdString();
    return p;
}

qint64 projectIdOf(QSqlDatabase db, const QString &exportSlug) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT project_id FROM project WHERE export_slug = ?"));
    q.addBindValue(exportSlug);
    EXPECT_TRUE(q.exec() && q.next()) << q.lastError().text().toStdString();
    return q.value(0).toLongLong();
}

// The walk ANTS-3758's render will make: every section of one project, ordered
// by the SHIPPED comparator. Testing through sectionOrderLess() rather than a
// sort written here is the difference between an invariant and a fixture
// agreeing with itself.
QStringList sectionWalk(RoadmapStore &s, qint64 projectId) {
    QString err;
    auto rows = s.listSections(projectId, &err);
    EXPECT_TRUE(rows.has_value()) << err.toStdString();
    if (!rows)
        return {};
    std::sort(rows->begin(), rows->end(), sectionOrderLess);
    QStringList out;
    for (const RoadmapStore::SectionRow &r : std::as_const(*rows))
        out << r.slug;
    return out;
}

// A store rebuilt from `bytes`, so each test states its own round trip.
void rebuildInto(RoadmapStore &target, QByteArray bytes) {
    QBuffer buf(&bytes);
    ASSERT_TRUE(buf.open(QIODevice::ReadOnly));
    QString err;
    ASSERT_TRUE(RoadmapExport::rebuildProject(target, &buf, &err)) << err.toStdString();
}

}  // namespace

// ANTS-3796 INV-1 — document order survives the round trip. Breaks when
// writeSections() emits the column but rebuildProject() binds a RECOMPUTED
// value: the insertion ordinal, or the (depth, slug) rank loadSections()
// already has in hand.
TEST(RoadmapExportRoundtrip, Ants3796Inv1DocumentOrderSurvivesTheRoundTrip) {
    QTemporaryDir liveDir;
    RoadmapStore live(liveDir.path() + QStringLiteral("/roadmap.sqlite"),
                      RoadmapStore::kDefaultHistoryCapBytes, RoadmapStore::Access::Bulk);
    QString err;
    ASSERT_TRUE(live.open(&err)) << err.toStdString();
    const qint64 p = buildOrderFixture(live, liveDir.path());

    const QStringList want{QStringLiteral("zeta"), QStringLiteral("alpha"),
                           QStringLiteral("mid")};
    ASSERT_EQ(sectionWalk(live, p), want) << "the fixture is not in the order this test assumes";

    const QByteArray jsonl = exportOf(live, QStringLiteral("ord"));

    // The export EMITS in (depth, slug) order and § 2.4 keeps it that way, so
    // position rides along as data. Asserting the two differ is what stops this
    // test passing for the wrong reason — if emission order already equalled
    // document order, a rebuild that recomputed the ordinal from its own
    // insertion sequence would reproduce `want` by accident.
    QStringList emitted;
    for (const QString &line : recordsOfType(jsonl, "section"))
        emitted << QJsonDocument::fromJson(line.toUtf8())
                       .object().value(QStringLiteral("slug")).toString();
    ASSERT_NE(emitted, want) << "emission order coincides with document order, so this fixture "
                                "cannot detect a recomputed position: " << emitted.join(',').toStdString();

    QTemporaryDir rebuiltDir;
    RoadmapStore rebuilt(rebuiltDir.path() + QStringLiteral("/roadmap.sqlite"),
                         RoadmapStore::kDefaultHistoryCapBytes, RoadmapStore::Access::Bulk);
    ASSERT_TRUE(rebuilt.open(&err)) << err.toStdString();
    rebuildInto(rebuilt, jsonl);

    EXPECT_EQ(sectionWalk(rebuilt, projectIdOf(rebuilt.db(), QStringLiteral("ord"))), want)
        << "INV-1: document order did not survive export → rebuild. A rebuild that reassigns "
           "section_id in (depth, slug) order and recomputes position from it yields "
           << emitted.join(',').toStdString();
}

// ANTS-3797 INV-2 — section.source_path survives the round trip. Asserted
// through readSection() and not raw SQL: ANTS-3782 INV-26 already pins the
// reader against SQL, and repeating that here would test the other invariant.
TEST(RoadmapExportRoundtrip, Ants3796Inv2SourcePathSurvivesTheRoundTrip) {
    QTemporaryDir liveDir;
    RoadmapStore live(liveDir.path() + QStringLiteral("/roadmap.sqlite"),
                      RoadmapStore::kDefaultHistoryCapBytes, RoadmapStore::Access::Bulk);
    QString err;
    ASSERT_TRUE(live.open(&err)) << err.toStdString();
    buildOrderFixture(live, liveDir.path());

    QTemporaryDir rebuiltDir;
    RoadmapStore rebuilt(rebuiltDir.path() + QStringLiteral("/roadmap.sqlite"),
                         RoadmapStore::kDefaultHistoryCapBytes, RoadmapStore::Access::Bulk);
    ASSERT_TRUE(rebuilt.open(&err)) << err.toStdString();
    rebuildInto(rebuilt, exportOf(live, QStringLiteral("ord")));

    const qint64 p = projectIdOf(rebuilt.db(), QStringLiteral("ord"));
    const auto archId = rebuilt.findSection(p, QStringLiteral("alpha"), &err);
    ASSERT_TRUE(archId.has_value()) << err.toStdString();
    const auto arch = rebuilt.readSection(*archId, &err);
    ASSERT_TRUE(arch.has_value()) << err.toStdString();
    ASSERT_TRUE(arch->sourcePath.has_value())
        << "INV-2: breaks when either leg is left unfixed — writeSections() not selecting "
           "source_path, or rebuildProject() not inserting it. Every rebuilt section then "
           "reads back as the LIVE roadmap, which is exactly what the column's DDL comment "
           "says it exists to prevent";
    EXPECT_EQ(*arch->sourcePath, QStringLiteral("docs/roadmap/0.6.md"));

    const auto liveId = rebuilt.findSection(p, QStringLiteral("zeta"), &err);
    ASSERT_TRUE(liveId.has_value()) << err.toStdString();
    const auto liveRow = rebuilt.readSection(*liveId, &err);
    ASSERT_TRUE(liveRow.has_value()) << err.toStdString();
    EXPECT_FALSE(liveRow->sourcePath.has_value())
        << "a NULL source_path must round-trip as NULL, not as ''";
}

// ANTS-3796 INV-3 — the column diff § 2.5 derives from the schema FAILS BY
// DEFAULT: on a column the export does not carry, and on a foreign-key rowid
// missing from the substitution map. This is what makes § 2.5 a contract rather
// than a tidier way to write the same test.
TEST(RoadmapExportRoundtrip, Ants3796Inv3ColumnDiffFailsByDefaultOnAnUncarriedColumn) {
    Fixture fx;
    QTemporaryDir rebuiltDir;
    RoadmapStore rebuilt(rebuiltDir.path() + QStringLiteral("/roadmap.sqlite"),
                         RoadmapStore::kDefaultHistoryCapBytes, RoadmapStore::Access::Bulk);
    QString err;
    ASSERT_TRUE(rebuilt.open(&err)) << err.toStdString();
    for (const char *slug : {"alpha", "beta", "gamma"})
        rebuildInto(rebuilt, exportOf(*fx.store, QLatin1String(slug)));

    QString why;
    ASSERT_TRUE(inv2Diff(fx.store->db(), rebuilt.db(), inv2Substitutions(), &why))
        << "baseline: " << why.toStdString();

    // Leg 1 — an unsubstituted foreign-key rowid must FAIL, not be skipped.
    // Run before the ALTER below, or the column-set check would reject the
    // stores first and this leg would pass for the wrong reason.
    QHash<QString, QString> incomplete = inv2Substitutions();
    ASSERT_EQ(incomplete.remove(QStringLiteral("section.parent_id")), 1);
    why.clear();
    EXPECT_FALSE(inv2Diff(fx.store->db(), rebuilt.db(), incomplete, &why))
        << "a foreign-key rowid with no substitution entry was silently skipped, which is "
           "§ 2.5's own failure mode: the map stops being exhaustive as the schema grows";
    EXPECT_TRUE(why.contains(QStringLiteral("substitution map"))) << why.toStdString();

    // Leg 2 — a column the export does not carry. Added to the LIVE store only,
    // and AFTER the export, so no other rule can reject it first: not the
    // importer's strictness (INV-7), not the golden comparison.
    sqlExec(fx.store->db(), QStringLiteral("ALTER TABLE section ADD COLUMN scratch TEXT"));
    why.clear();
    EXPECT_FALSE(inv2Diff(fx.store->db(), rebuilt.db(), inv2Substitutions(), &why))
        << "INV-3: breaks when the column list is a literal — the pre-ANTS-3796 diff passed "
           "here, which is how section.source_path went unnoticed for two specs";
    EXPECT_TRUE(why.contains(QStringLiteral("column sets differ")))
        << "breaks when PRAGMA table_info is read once and reused for both stores, which "
           "makes the set comparison vacuous: " << why.toStdString();
}

// ANTS-3796 INV-5 — the sort key is TOTAL: two sections sharing a position
// order by slug, stably, in both directions of insertion. Breaks when the sort
// is on position alone, which is stable only by accident of the underlying
// container and reorders when the query plan changes.
TEST(RoadmapExportRoundtrip, Ants3796Inv5SortKeyIsTotalUnderADuplicatePosition) {
    for (const bool reversed : {false, true}) {
        QTemporaryDir dir;
        RoadmapStore s(dir.path() + QStringLiteral("/roadmap.sqlite"),
                       RoadmapStore::kDefaultHistoryCapBytes, RoadmapStore::Access::Bulk);
        QString err;
        ASSERT_TRUE(s.open(&err)) << err.toStdString();
        const qint64 p = addProject(s, dir.path(), QStringLiteral("dup"), QStringLiteral("Dup"));

        // The SAME position for both — permitted, because § 2.1 leaves
        // distinctness to a writer's obligation rather than a UNIQUE
        // constraint. The contract is that a duplicate yields a wrong-but-
        // STABLE order, never an unstable one.
        if (reversed) {
            addSection(s, p, QStringLiteral("a-slug"), QStringLiteral("A"), 2, 7);
            addSection(s, p, QStringLiteral("b-slug"), QStringLiteral("B"), 2, 7);
        } else {
            addSection(s, p, QStringLiteral("b-slug"), QStringLiteral("B"), 2, 7);
            addSection(s, p, QStringLiteral("a-slug"), QStringLiteral("A"), 2, 7);
        }

        EXPECT_EQ(sectionWalk(s, p),
                  (QStringList{QStringLiteral("a-slug"), QStringLiteral("b-slug")}))
            << "INV-5, insertion order " << (reversed ? "a then b" : "b then a");
    }
}

// ANTS-3796 INV-7 — an export whose section record omits either field is
// refused by the REBUILD IMPORTER, loudly, with no partial store written.
// Breaks when the importer defaults a missing position to 0, which reads as
// defensive and silently flattens the document order of every project it
// restores.
TEST(RoadmapExportRoundtrip, Ants3796Inv7ImporterRefusesAnIncompleteSectionRecord) {
    struct Case {
        const char *missing;
        const char *section;
    };
    static const Case kCases[] = {
        {"position",
         R"({"intro":null,"level":2,"parent":null,"slug":"zeta","source":null,)"
         R"("t":"section","title":"Zeta"})"},
        {"source",
         R"({"intro":null,"level":2,"parent":null,"position":0,"slug":"zeta",)"
         R"("t":"section","title":"Zeta"})"},
    };

    for (const Case &c : kCases) {
        QTemporaryDir dir;
        RoadmapStore s(dir.path() + QStringLiteral("/roadmap.sqlite"),
                       RoadmapStore::kDefaultHistoryCapBytes, RoadmapStore::Access::Bulk);
        QString err;
        ASSERT_TRUE(s.open(&err)) << err.toStdString();

        QByteArray bytes = QByteArray(R"({"name":"Ord","project":"ord","schema":1,"t":"meta"})")
                               .append('\n')
                               .append(c.section)
                               .append('\n');
        QBuffer buf(&bytes);
        ASSERT_TRUE(buf.open(QIODevice::ReadOnly));

        EXPECT_FALSE(RoadmapExport::rebuildProject(s, &buf, &err))
            << "a section record with no " << c.missing << " was imported anyway";
        EXPECT_FALSE(err.isEmpty()) << "the refusal must say what is wrong; a version mismatch "
                                       "would only say the binary is too new";
        EXPECT_TRUE(err.contains(QLatin1String(c.missing))) << err.toStdString();

        // No partial store. rebuildProject() wraps its inserts in BEGIN
        // IMMEDIATE and ROLLBACKs on failure, so this pins behaviour the code
        // already has and a future refactor could lose.
        for (const char *table : {"section", "project"}) {
            QSqlQuery q(s.db());
            ASSERT_TRUE(q.exec(QStringLiteral("SELECT COUNT(*) FROM ") + QLatin1String(table)) &&
                        q.next());
            EXPECT_EQ(q.value(0).toInt(), 0)
                << table << " is non-empty after a refused import (" << c.missing << ")";
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

// ASan replaces the allocator, so under it this measurement stops describing
// the writer at all — see the GTEST_SKIP below.
#if defined(__SANITIZE_ADDRESS__)
#  define ANTS_TEST_ASAN 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define ANTS_TEST_ASAN 1
#  endif
#endif

// INV-12 — the export writer STREAMS: peak RSS across the export call rises by
// less than 4 MiB above the pre-call baseline, against an export several times
// that size. Breaks when the writer builds one QString and writes it at the
// end, which passes every other invariant here.
//
// The measurement must be a delta and it must be sampled. An absolute ceiling
// is unachievable — a Qt process's RSS exceeds the export's byte size before
// any work is done — and reading RSS before and after would miss the peak
// entirely, an export being synchronous.
//
// It must also be an UNSANITIZED build, which is a precondition of the
// instrument rather than a tolerance to widen. ASan surrounds every allocation
// with redzones and holds freed chunks in a quarantine instead of returning
// them, so process RSS tracks the sanitizer's bookkeeping and not the writer's:
// measured 2026-07-31, the streaming writer that passes this assertion in
// Release by a wide margin shows a 120 MiB delta under ASan, 30x the budget.
// Raising the budget to fit would leave nothing for a non-streaming writer to
// exceed, so the honest move is to report the gap and let the Release build —
// ci.yml's build-test job, and every local preset — hold the invariant.
TEST(RoadmapExportRoundtrip, Inv12PeakRssDeltaStaysUnderFourMiB) {
#ifdef ANTS_TEST_ASAN
    GTEST_SKIP() << "INV-12's RSS delta is unmeasurable under ASan: redzones "
                    "and the free quarantine dominate process RSS. Enforced by "
                    "the Release build.";
#endif
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
        const qint64 sect = addSection(seed, p, QStringLiteral(""), QStringLiteral(""), 0, 0);
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
