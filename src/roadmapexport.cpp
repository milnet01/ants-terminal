// ANTS-3761 — the roadmap export. Spec §§ 2.3–2.6.
#include "roadmapexport.h"

#include "configbackup.h"   // ConfigWriteLock (§ 2.6 — reused, not re-invented)
#include "jsoncanonical.h"
#include "roadmapstore.h"

#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSaveFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <algorithm>

namespace {

// § 7.3 of roadmap-data-model.md, in its DECLARED order — which is what § 2.4
// pins the `legend` record order to, not the alphabetical order the other rows
// use. The store's CHECK constraint carries the same five values.
constexpr const char *kStatusOrder[] = {"planned", "in-progress", "shipped",
                                        "considered", "dropped"};

// Every string comparison in § 2.4 and § 2.5 is UTF-16 code-unit order, which
// is exactly QString::compare — never localeAwareCompare, which varies with
// LC_COLLATE and would make the export machine-dependent. Named so the
// intent survives a reader who knows only that both compile.
inline int cmpCodeUnit(const QString &a, const QString &b) { return a.compare(b); }

bool fail(QString *error, const QString &msg) {
    if (error)
        *error = msg;
    return false;
}

bool failQuery(QString *error, const QSqlQuery &q) {
    return fail(error, QStringLiteral("%1 [%2]").arg(q.lastError().text(), q.lastQuery()));
}

// ---------------------------------------------------------------------------
// Emission

bool emitLine(QIODevice *out, const QJsonObject &o, QString *error) {
    QByteArray line;
    if (!JsonCanonical::serialise(o, &line, error))
        return false;
    // § 2.4 — '\n' after every line, the final one included. No BOM: the file
    // opens with '{' and JCS mandates UTF-8 without saying anything about a
    // mark, so this spec forbids one.
    line.append('\n');
    if (out->write(line) != line.size())
        return fail(error, QStringLiteral("export write failed: %1").arg(out->errorString()));
    return true;
}

// A JSON column (lanes / evidence / extras / provenance / a table payload) is
// STORED canonical by ANTS-3756 § 2.3, so this parse-and-re-serialise is an
// identity — the export copies those bytes rather than transforming them, and
// a column that fails to parse is a row that cannot be serialised (§ 2.4:
// abort and report, never a partial file).
bool jsonColumn(const QString &text, const char *what, QJsonValue *v, QString *error) {
    QJsonParseError pe{};
    const QJsonDocument d = QJsonDocument::fromJson(text.toUtf8(), &pe);
    if (pe.error != QJsonParseError::NoError)
        return fail(error, QStringLiteral("%1 is not valid JSON: %2")
                               .arg(QLatin1String(what), pe.errorString()));
    if (d.isArray())
        *v = d.array();
    else if (d.isObject())
        *v = d.object();
    else
        return fail(error, QStringLiteral("%1 is neither an object nor an array")
                               .arg(QLatin1String(what)));
    return true;
}

// Insert only when the column holds a value. § 2.4's per-field table: absence
// is omission, and an empty string is a VALUE that gets emitted.
void insertIfPresent(QJsonObject *o, const char *key, const QVariant &v) {
    if (!v.isNull())
        o->insert(QLatin1String(key), v.toString());
}

// ---------------------------------------------------------------------------
// § 2.5 — the id sort

// One maximal run. `numeric` runs hold ASCII digits with leading zeros already
// stripped, so two of them compare by length and then bytewise, which is exact
// numeric ordering with no width limit to overflow.
struct Seg {
    bool numeric = false;
    QString text;
};

struct SortKey {
    QList<Seg> segs;
    QString rawId;          // rule 5's tie-break — the id as authored
    bool quarantined = false;
};

SortKey buildSortKey(const QString &idFold, const QString &rawId, bool quarantined) {
    SortKey k;
    k.rawId = rawId;
    k.quarantined = quarantined;
    // Rule 6 — only `quarantined` ids skip the tuple and sort last among
    // themselves by raw id. `synthesised` ids use the tuple like any other:
    // a synthesised PASS-43-5-B is a real id, and burying it with the junk
    // would bury 144 live items.
    if (quarantined)
        return k;

    // Rule 1 — remove separators BEFORE splitting, which is the whole rule.
    // Splitting on them instead yields (3,"d","e",7) for 3d_e-0007 against
    // (3,"de",7) for 3de-0007, so the two sort differently — the exact outcome
    // the rule exists to prevent.
    QString s;
    s.reserve(idFold.size());
    for (const QChar c : idFold)
        if (c != u'-' && c != u'_')
            s.append(c);

    // ASCII digits, not QChar::isDigit(): isDigit() accepts every Unicode Nd
    // character, and the zero-strip and length compare below both assume
    // ASCII. Ids are ASCII by roadmap-format.md § 3.5.1's grammar, and one
    // that is not is quarantined above.
    const auto isAsciiDigit = [](QChar c) { return c >= u'0' && c <= u'9'; };
    qsizetype i = 0;
    while (i < s.size()) {
        const bool numeric = isAsciiDigit(s.at(i));
        qsizetype j = i;
        while (j < s.size() && isAsciiDigit(s.at(j)) == numeric)
            ++j;
        QString run = s.mid(i, j - i);
        if (numeric) {
            qsizetype z = 0;
            while (z + 1 < run.size() && run.at(z) == u'0')
                ++z;
            run = run.mid(z);
        }
        k.segs.append({numeric, run});
        i = j;
    }
    return k;
}

int cmpSeg(const Seg &a, const Seg &b) {
    // Rule 3 — numeric before alphabetic where the types differ. Not cosmetic:
    // 3D_E is a live prefix, so real ids begin with runs of both types and
    // without this the sort is undefined between two projects.
    if (a.numeric != b.numeric)
        return a.numeric ? -1 : 1;
    if (a.numeric && a.text.size() != b.text.size())
        return a.text.size() < b.text.size() ? -1 : 1;
    return cmpCodeUnit(a.text, b.text);
}

bool lessSortKey(const SortKey &a, const SortKey &b) {
    if (a.quarantined != b.quarantined)
        return !a.quarantined;                       // rule 6
    if (a.quarantined)
        return cmpCodeUnit(a.rawId, b.rawId) < 0;
    const qsizetype n = std::min(a.segs.size(), b.segs.size());
    for (qsizetype i = 0; i < n; ++i)
        if (const int c = cmpSeg(a.segs.at(i), b.segs.at(i)))
            return c < 0;
    if (a.segs.size() != b.segs.size())
        return a.segs.size() < b.segs.size();        // rule 4
    // Rule 5 — required, not defensive: zero-padding is write-side only, so
    // CL-9 and CL-0009 fold to the identical tuple and the sort would
    // otherwise be non-total, making INV-1 fail intermittently.
    return cmpCodeUnit(a.rawId, b.rawId) < 0;
}

// ---------------------------------------------------------------------------
// Row shapes the writer holds in memory. Every one of these is a KEY plus a
// short scalar: what INV-12 rules out is holding the two unbounded columns —
// `item.body` and the `history` table — and both are read one row at a time.

struct SectionRef {
    qint64 id = 0;
    QString slug;
    int depth = 0;
};

struct ItemRef {
    qint64 pk = 0;
    QString idFold;
};

struct RelRow {
    QString type, src, a, b;   // a/b: dst | (dst_project, dst) | dst_path
};

struct TripleRow {
    QString a, b, c;
};

// ---------------------------------------------------------------------------

bool loadSections(QSqlDatabase &db, qint64 projectId, QList<SectionRef> *emitOrder,
                  QList<SectionRef> *slugOrder, QHash<qint64, QString> *slugOf,
                  QString *error) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT section_id, slug, parent_id FROM section "
                             "WHERE project_id = ?"));
    q.addBindValue(projectId);
    if (!q.exec())
        return failQuery(error, q);

    QHash<qint64, qint64> parentOf;   // absent => root
    QList<SectionRef> rows;
    while (q.next()) {
        const qint64 id = q.value(0).toLongLong();
        rows.append({id, q.value(1).toString(), 0});
        slugOf->insert(id, q.value(1).toString());
        if (!q.value(2).isNull())
            parentOf.insert(id, q.value(2).toLongLong());
    }

    // Depth is what turns "parents before children" into a sort KEY rather
    // than a constraint — see § 2.4. Walked from parent_id rather than read
    // from `level`, which is the markdown heading level and may skip (## then
    // ####). The bound is the row count: nothing in the schema forbids a
    // parent cycle, and an unbounded walk would hang instead of reporting.
    for (SectionRef &r : rows) {
        qint64 cur = r.id;
        int depth = 0;
        while (parentOf.contains(cur)) {
            if (++depth > rows.size())
                return fail(error, QStringLiteral("section parent cycle at slug '%1'").arg(r.slug));
            cur = parentOf.value(cur);
        }
        r.depth = depth;
    }

    *emitOrder = rows;
    std::sort(emitOrder->begin(), emitOrder->end(), [](const SectionRef &a, const SectionRef &b) {
        if (a.depth != b.depth)
            return a.depth < b.depth;
        return cmpCodeUnit(a.slug, b.slug) < 0;
    });

    // § 2.4 gives `element` a DIFFERENT section ordering — plain slug code-unit
    // order, with no parents-first term. Two orders, deliberately.
    *slugOrder = rows;
    std::sort(slugOrder->begin(), slugOrder->end(), [](const SectionRef &a, const SectionRef &b) {
        return cmpCodeUnit(a.slug, b.slug) < 0;
    });
    return true;
}

bool loadItems(QSqlDatabase &db, qint64 projectId, QList<ItemRef> *sortOrder,
               QList<ItemRef> *foldOrder, QString *error) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT item_pk, id, id_fold, id_origin FROM item "
                             "WHERE project_id = ?"));
    q.addBindValue(projectId);
    if (!q.exec())
        return failQuery(error, q);

    QList<QPair<SortKey, ItemRef>> keyed;
    while (q.next()) {
        ItemRef r{q.value(0).toLongLong(), q.value(2).toString()};
        keyed.append({buildSortKey(r.idFold, q.value(1).toString(),
                                   q.value(3).toString() == QLatin1String("quarantined")),
                      r});
    }
    std::sort(keyed.begin(), keyed.end(),
              [](const auto &a, const auto &b) { return lessSortKey(a.first, b.first); });

    sortOrder->clear();
    for (const auto &kv : keyed)
        sortOrder->append(kv.second);

    // `history`, `feedback_ref` and item-anchored `citation` all key on the
    // folded id in code-unit order, which is NOT § 2.5's tuple order. Two
    // orderings of the same small list, held so those three can stream.
    *foldOrder = *sortOrder;
    std::sort(foldOrder->begin(), foldOrder->end(),
              [](const ItemRef &a, const ItemRef &b) { return cmpCodeUnit(a.idFold, b.idFold) < 0; });
    return true;
}

bool writeMeta(QSqlDatabase &db, const QString &slug, QIODevice *out, qint64 *projectId,
               QString *legendText, QString *error) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT project_id, name, legend FROM project WHERE export_slug = ?"));
    q.addBindValue(slug);
    if (!q.exec())
        return failQuery(error, q);
    if (!q.next())
        return fail(error, QStringLiteral("no project with export_slug '%1'").arg(slug));

    *projectId = q.value(0).toLongLong();
    *legendText = q.value(2).toString();

    QJsonObject o;
    o.insert(QStringLiteral("t"), QStringLiteral("meta"));
    o.insert(QStringLiteral("schema"), RoadmapStore::kSchemaVersion);
    o.insert(QStringLiteral("project"), slug);
    o.insert(QStringLiteral("name"), q.value(1).toString());
    // § 2.3 — no export timestamp. A date inside a byte-identity contract
    // defeats it: two exports of an unchanged store would differ across
    // midnight and every regeneration would churn the committed file.
    return emitLine(out, o, error);
}

bool writeIdPrefixes(QSqlDatabase &db, qint64 projectId, QIODevice *out, QString *error) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT prefix, high_water FROM id_prefix WHERE project_id = ?"));
    q.addBindValue(projectId);
    if (!q.exec())
        return failQuery(error, q);

    QList<QPair<QString, qint64>> rows;
    while (q.next())
        rows.append({q.value(0).toString(), q.value(1).toLongLong()});
    std::sort(rows.begin(), rows.end(),
              [](const auto &a, const auto &b) { return cmpCodeUnit(a.first, b.first) < 0; });

    for (const auto &r : rows) {
        QJsonObject o;
        o.insert(QStringLiteral("t"), QStringLiteral("id_prefix"));
        o.insert(QStringLiteral("prefix"), r.first);
        o.insert(QStringLiteral("high_water"), double(r.second));
        if (!emitLine(out, o, error))
            return false;
    }
    return true;
}

bool writeLegend(const QString &legendText, QIODevice *out, QString *error) {
    QJsonValue v;
    if (!jsonColumn(legendText, "project.legend", &v, error))
        return false;
    const QJsonObject legend = v.toObject();
    // § 2.4 — a project with no legend emits ZERO legend lines; the rebuild
    // restores {}, which is the store's default.
    int emitted = 0;
    for (const char *status : kStatusOrder) {
        const QString key = QLatin1String(status);
        if (!legend.contains(key))
            continue;
        QJsonObject o;
        o.insert(QStringLiteral("t"), QStringLiteral("legend"));
        o.insert(QStringLiteral("status"), key);
        o.insert(QStringLiteral("wording"), legend.value(key).toString());
        if (!emitLine(out, o, error))
            return false;
        ++emitted;
    }
    // A key outside § 7.3's enum has no position in the declared order, so
    // there is no rule that would emit it — and silently dropping it would
    // lose a store row. § 2.4's "when a row cannot be serialised": abort.
    if (emitted != legend.size())
        return fail(error, QStringLiteral("project.legend holds a key outside the § 7.3 status enum"));
    return true;
}

bool writeSections(QSqlDatabase &db, const QList<SectionRef> &order,
                   const QHash<qint64, QString> &slugOf, QIODevice *out, QString *error) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT title, level, intro, parent_id FROM section "
                             "WHERE section_id = ?"));
    for (const SectionRef &s : order) {
        q.addBindValue(s.id);
        if (!q.exec() || !q.next())
            return failQuery(error, q);

        QJsonObject o;
        o.insert(QStringLiteral("t"), QStringLiteral("section"));
        o.insert(QStringLiteral("slug"), s.slug);
        o.insert(QStringLiteral("title"), q.value(0).toString());
        o.insert(QStringLiteral("level"), q.value(1).toInt());
        // parent and intro are the "always emitted, as null" row of § 2.4.
        o.insert(QStringLiteral("parent"), q.value(3).isNull()
                                               ? QJsonValue()
                                               : QJsonValue(slugOf.value(q.value(3).toLongLong())));
        o.insert(QStringLiteral("intro"),
                 q.value(2).isNull() ? QJsonValue() : QJsonValue(q.value(2).toString()));
        if (!emitLine(out, o, error))
            return false;
    }
    return true;
}

bool writeItems(QSqlDatabase &db, const QList<ItemRef> &order, QIODevice *out, QString *error) {
    QSqlQuery q(db);
    q.setForwardOnly(true);
    q.prepare(QStringLiteral(
        "SELECT id, id_origin, status, headline, layman, kind, source, priority, visibility, "
        "milestone, resolution, body, created, last_modified, shipped, "
        "lanes, evidence, extras, provenance FROM item WHERE item_pk = ?"));

    for (const ItemRef &it : order) {
        q.addBindValue(it.pk);
        if (!q.exec() || !q.next())
            return failQuery(error, q);

        QJsonObject o;
        o.insert(QStringLiteral("t"), QStringLiteral("item"));
        // The AUTHORED spelling declares the item; every reference to it
        // elsewhere carries the fold (§ 2.3). id_fold is never a field.
        o.insert(QStringLiteral("id"), q.value(0).toString());
        o.insert(QStringLiteral("id_origin"), q.value(1).toString());
        o.insert(QStringLiteral("status"), q.value(2).toString());
        o.insert(QStringLiteral("headline"), q.value(3).toString());
        insertIfPresent(&o, "layman", q.value(4));
        o.insert(QStringLiteral("kind"), q.value(5).toString());
        // NOT NULL in the store, so never absent — § 2.4 lists it explicitly
        // because an earlier draft filed it under "omitted", a state the
        // column cannot produce.
        o.insert(QStringLiteral("source"), q.value(6).toString());
        if (!q.value(7).isNull())
            o.insert(QStringLiteral("priority"), q.value(7).toInt());
        o.insert(QStringLiteral("visibility"), q.value(8).toString());
        o.insert(QStringLiteral("milestone"),
                 q.value(9).isNull() ? QJsonValue() : QJsonValue(q.value(9).toString()));
        insertIfPresent(&o, "resolution", q.value(10));
        insertIfPresent(&o, "body", q.value(11));
        insertIfPresent(&o, "created", q.value(12));
        insertIfPresent(&o, "last_modified", q.value(13));
        insertIfPresent(&o, "shipped", q.value(14));

        static const char *const kJsonCols[] = {"lanes", "evidence", "extras", "provenance"};
        for (int i = 0; i < 4; ++i) {
            QJsonValue v;
            if (!jsonColumn(q.value(15 + i).toString(), kJsonCols[i], &v, error))
                return false;
            o.insert(QLatin1String(kJsonCols[i]), v);
        }
        // No `section` key: an item's filing is its element row, and a second
        // copy here would be the encoding ANTS-3756 § 2.3 removed.
        if (!emitLine(out, o, error))
            return false;
    }
    return true;
}

bool writeElements(QSqlDatabase &db, const QList<SectionRef> &slugOrder, QIODevice *out,
                   QString *error) {
    QSqlQuery q(db);
    q.setForwardOnly(true);
    q.prepare(QStringLiteral(
        "SELECT e.position, e.kind, e.payload, i.id_fold FROM element e "
        "LEFT JOIN item i ON i.item_pk = e.item_pk WHERE e.section_id = ? ORDER BY e.position"));

    for (const SectionRef &s : slugOrder) {
        q.addBindValue(s.id);
        if (!q.exec())
            return failQuery(error, q);
        while (q.next()) {
            const QString kind = q.value(1).toString();
            QJsonObject o;
            o.insert(QStringLiteral("t"), QStringLiteral("element"));
            o.insert(QStringLiteral("section"), s.slug);
            o.insert(QStringLiteral("position"), q.value(0).toInt());
            o.insert(QStringLiteral("kind"), kind);
            if (kind == QLatin1String("item")) {
                o.insert(QStringLiteral("ref"), q.value(3).toString());
            } else if (kind == QLatin1String("table")) {
                QJsonValue v;
                if (!jsonColumn(q.value(2).toString(), "element.payload", &v, error))
                    return false;
                o.insert(QStringLiteral("payload"), v);
            } else {
                // Narration prose is stored as the author's text and emitted
                // as a JSON string, which JCS then escapes like any other
                // (ANTS-3756 § 2.3 — canonicalising prose as JSON is undefined,
                // not merely wasteful).
                o.insert(QStringLiteral("payload"), q.value(2).toString());
            }
            if (!emitLine(out, o, error))
                return false;
        }
    }
    return true;
}

// The three `rel` partitions and the two `citation` partitions are read whole
// and sorted here rather than by SQLite, because SQLite's BINARY collation is
// UTF-8 byte order and § 2.4 pins UTF-16 code-unit order — the two disagree on
// any key holding a supplementary-plane character. Each row is a handful of
// short keys, so the memory is a few hundred bytes per relationship; the two
// columns INV-12 is actually about, `item.body` and `history`, stream.
bool writeRelationships(QSqlDatabase &db, qint64 projectId, QIODevice *out, QString *error) {
    struct Partition {
        const char *where;
        const char *select;
        bool crossProject;
        bool document;
    };
    // § 2.4 — same-project item targets, then cross-project, then document.
    // Partitioning BEFORE sorting is what makes the order total: on a document
    // target `dst` is absent, and nothing defines how an absent member
    // collates against a present one.
    static const Partition kPartitions[] = {
        {"r.dst_pk IS NOT NULL", "d.id_fold, ''", false, false},
        {"r.dst_project IS NOT NULL", "r.dst_project, r.dst_id_fold", true, false},
        {"r.dst_path IS NOT NULL", "r.dst_path, ''", false, true},
    };

    for (const Partition &p : kPartitions) {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("SELECT r.type, s.id_fold, %1 FROM relationship r "
                                 "JOIN item s ON s.item_pk = r.src_pk "
                                 "LEFT JOIN item d ON d.item_pk = r.dst_pk "
                                 "WHERE s.project_id = ? AND %2")
                      .arg(QLatin1String(p.select), QLatin1String(p.where)));
        q.addBindValue(projectId);
        if (!q.exec())
            return failQuery(error, q);

        QList<RelRow> rows;
        while (q.next())
            rows.append({q.value(0).toString(), q.value(1).toString(), q.value(2).toString(),
                         q.value(3).toString()});
        std::sort(rows.begin(), rows.end(), [](const RelRow &x, const RelRow &y) {
            if (const int c = cmpCodeUnit(x.type, y.type))
                return c < 0;
            if (const int c = cmpCodeUnit(x.src, y.src))
                return c < 0;
            if (const int c = cmpCodeUnit(x.a, y.a))
                return c < 0;
            return cmpCodeUnit(x.b, y.b) < 0;
        });

        for (const RelRow &r : rows) {
            QJsonObject o;
            o.insert(QStringLiteral("t"), QStringLiteral("rel"));
            o.insert(QStringLiteral("type"), r.type);
            o.insert(QStringLiteral("src"), r.src);
            if (p.document) {
                o.insert(QStringLiteral("dst_path"), r.a);
            } else if (p.crossProject) {
                // The SOURCE project's file carries a cross-project edge, with
                // dst_project naming the far side (§ 2.3). A rebuild that
                // cannot see that project keeps the edge unresolved.
                o.insert(QStringLiteral("dst_project"), r.a);
                o.insert(QStringLiteral("dst"), r.b);
            } else {
                o.insert(QStringLiteral("dst"), r.a);
            }
            if (!emitLine(out, o, error))
                return false;
        }
    }
    return true;
}

bool writeCitations(QSqlDatabase &db, qint64 projectId, const QString &slug, QIODevice *out,
                    QString *error) {
    // Item-anchored rows before doc-anchored rows (§ 2.4).
    for (int pass = 0; pass < 2; ++pass) {
        const bool byItem = pass == 0;
        QSqlQuery q(db);
        q.prepare(byItem
                      ? QStringLiteral("SELECT i.id_fold, c.target_file, c.symbol FROM citation c "
                                       "JOIN item i ON i.item_pk = c.item_pk "
                                       "WHERE c.project_id = ?")
                      : QStringLiteral("SELECT doc_path, target_file, symbol FROM citation "
                                       "WHERE project_id = ? AND doc_path IS NOT NULL"));
        q.addBindValue(projectId);
        if (!q.exec())
            return failQuery(error, q);

        QList<TripleRow> rows;
        while (q.next())
            rows.append({q.value(0).toString(), q.value(1).toString(), q.value(2).toString()});
        std::sort(rows.begin(), rows.end(), [](const TripleRow &x, const TripleRow &y) {
            if (const int c = cmpCodeUnit(x.a, y.a))
                return c < 0;
            if (const int c = cmpCodeUnit(x.b, y.b))
                return c < 0;
            return cmpCodeUnit(x.c, y.c) < 0;
        });

        for (const TripleRow &r : rows) {
            QJsonObject o;
            o.insert(QStringLiteral("t"), QStringLiteral("citation"));
            // `project` is constant across one file and emitted anyway: it
            // LEADS the store's cite_doc_uq index, so INV-2's column diff
            // needs it to tell two projects' README.md citations apart.
            o.insert(QStringLiteral("project"), slug);
            o.insert(QLatin1String(byItem ? "src" : "doc"), r.a);
            o.insert(QStringLiteral("file"), r.b);
            o.insert(QStringLiteral("symbol"), r.c);
            if (!emitLine(out, o, error))
                return false;
        }
    }
    return true;
}

bool writeFeedbackRefs(QSqlDatabase &db, qint64 projectId, QIODevice *out, QString *error) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT i.id_fold, f.file FROM feedback_ref f "
                             "JOIN item i ON i.item_pk = f.item_pk WHERE i.project_id = ?"));
    q.addBindValue(projectId);
    if (!q.exec())
        return failQuery(error, q);

    QList<QPair<QString, QString>> rows;
    while (q.next())
        rows.append({q.value(0).toString(), q.value(1).toString()});
    std::sort(rows.begin(), rows.end(), [](const auto &x, const auto &y) {
        if (const int c = cmpCodeUnit(x.first, y.first))
            return c < 0;
        return cmpCodeUnit(x.second, y.second) < 0;
    });

    for (const auto &r : rows) {
        QJsonObject o;
        o.insert(QStringLiteral("t"), QStringLiteral("feedback_ref"));
        o.insert(QStringLiteral("item"), r.first);
        o.insert(QStringLiteral("file"), r.second);
        if (!emitLine(out, o, error))
            return false;
    }
    return true;
}

bool writeHistory(QSqlDatabase &db, const QList<ItemRef> &foldOrder, QIODevice *out,
                  QString *error) {
    // The one unbounded table — ANTS-3756's INV-14 bounds it at 250 MiB, which
    // is far past INV-12's 4 MiB budget, so it streams: the outer loop walks
    // the (small) item list in fold order and SQLite orders within each item.
    // BINARY collation is EXACT here and nowhere else in this file, because
    // changed_at is fixed-width ASCII and seq is an integer.
    QSqlQuery q(db);
    q.setForwardOnly(true);
    q.prepare(QStringLiteral("SELECT changed_at, seq, field, old_value, new_value FROM history "
                             "WHERE item_pk = ? ORDER BY changed_at, seq"));
    for (const ItemRef &it : foldOrder) {
        q.addBindValue(it.pk);
        if (!q.exec())
            return failQuery(error, q);
        while (q.next()) {
            QJsonObject o;
            o.insert(QStringLiteral("t"), QStringLiteral("history"));
            o.insert(QStringLiteral("item"), it.idFold);
            o.insert(QStringLiteral("at"), q.value(0).toString());
            // seq disambiguates two edits to one field within the same second;
            // without it this sort is not total.
            o.insert(QStringLiteral("seq"), q.value(1).toInt());
            o.insert(QStringLiteral("field"), q.value(2).toString());
            // Both nullable, and the first revision of any field has no `old`
            // — the commonest null in § 2.4's table, hence "omitted".
            insertIfPresent(&o, "old", q.value(3));
            insertIfPresent(&o, "new", q.value(4));
            if (!emitLine(out, o, error))
                return false;
        }
    }
    return true;
}

bool writeAll(QSqlDatabase &db, const QString &slug, QIODevice *out, QString *error) {
    qint64 projectId = 0;
    QString legendText;
    if (!writeMeta(db, slug, out, &projectId, &legendText, error))
        return false;
    if (!writeIdPrefixes(db, projectId, out, error))
        return false;
    if (!writeLegend(legendText, out, error))
        return false;

    QList<SectionRef> sectionsByDepth, sectionsBySlug;
    QHash<qint64, QString> slugOf;
    if (!loadSections(db, projectId, &sectionsByDepth, &sectionsBySlug, &slugOf, error))
        return false;
    if (!writeSections(db, sectionsByDepth, slugOf, out, error))
        return false;

    QList<ItemRef> itemsSorted, itemsByFold;
    if (!loadItems(db, projectId, &itemsSorted, &itemsByFold, error))
        return false;
    if (!writeItems(db, itemsSorted, out, error))
        return false;

    return writeElements(db, sectionsBySlug, out, error)
        && writeRelationships(db, projectId, out, error)
        && writeCitations(db, projectId, slug, out, error)
        && writeFeedbackRefs(db, projectId, out, error)
        && writeHistory(db, itemsByFold, out, error);
}

// ---------------------------------------------------------------------------
// Rebuild

// The store's JSON columns are held canonical (ANTS-3756 § 2.3) so that the
// next export copies these bytes rather than transforming them.
//
// It is tempting to treat this as infallible — the value came off a line this
// same serialiser produced. It is not: an export missing the key entirely
// yields Undefined, and swallowing that writes '' into a NOT NULL column,
// producing a store whose next export ABORTS on a row nothing reported. § 2.4
// gives the writer "abort and report, never a partial file"; the reader owes
// the same. Found by mutation — dropping the `provenance` column reddened
// INV-1 as well as INV-2, and only the extra failure was a real defect.
bool canonicalText(const QJsonValue &v, const char *what, QString *out, QString *why) {
    if (v.isUndefined())
        return fail(why, QStringLiteral("the record has no '%1'").arg(QLatin1String(what)));
    QByteArray b;
    if (!JsonCanonical::serialise(v, &b, why))
        return false;
    *out = QString::fromUtf8(b);
    return true;
}

// A key that § 2.4 omits when absent binds as NULL, which is what makes the
// round trip exact: an omitted key and a NULL column are the same state.
QVariant textOrNull(const QJsonObject &o, const char *key) {
    const QJsonValue v = o.value(QLatin1String(key));
    return (v.isUndefined() || v.isNull()) ? QVariant() : QVariant(v.toString());
}

}  // namespace

// ---------------------------------------------------------------------------

bool RoadmapExport::writeProject(RoadmapStore &store, const QString &exportSlug, QIODevice *out,
                                 QString *error) {
    if (!store.isOpen())
        return fail(error, QStringLiteral("store is not open"));
    QSqlDatabase db = store.db();

    // § 2.6 — every read inside ONE deferred transaction. Without it a commit
    // landing mid-export tears the file into half pre-change and half
    // post-change, and INV-1 fails against a store nobody corrupted. Deferred
    // rather than the store's usual BEGIN IMMEDIATE: this path only reads, and
    // taking the write lock would block every other writer for the whole run.
    if (!db.transaction())
        return fail(error, QStringLiteral("could not begin the export transaction: %1")
                               .arg(db.lastError().text()));
    const bool ok = writeAll(db, exportSlug, out, error);
    db.rollback();   // read-only — there is nothing to commit
    return ok;
}

bool RoadmapExport::exportProject(RoadmapStore &store, const QString &exportSlug,
                                  const QString &destPath, QString *error) {
    // INV-9 — the lock comes FIRST, before any file exists. The guard is
    // advisory and its header leaves the choice to the caller; proceeding
    // unprotected is not available here, because the model's § 9 makes a
    // silent backup failure worse than no backup: it stops anyone checking.
    ConfigWriteLock lock(destPath);
    if (!lock.acquired())
        return fail(error, QStringLiteral("could not acquire the export write lock on %1")
                               .arg(destPath));

    // Temp-then-rename(2), inside the lock's scope. QSaveFile is the project's
    // existing form of that (auditcache, buildcache, codebaseindex, docsindex);
    // JsonlFile::writeLinesAtomic is the closer match by shape but takes every
    // line in memory at once, which is exactly what INV-12 forbids.
    QSaveFile f(destPath);
    if (!f.open(QIODevice::WriteOnly))
        return fail(error, QStringLiteral("could not open %1 for writing: %2")
                               .arg(destPath, f.errorString()));
    if (!writeProject(store, exportSlug, &f, error)) {
        // A crash midway through an in-place write truncates the only durable
        // copy of a primary store; cancelWriting() removes the temp instead.
        f.cancelWriting();
        return false;
    }
    if (!f.commit())
        return fail(error, QStringLiteral("could not commit %1: %2").arg(destPath, f.errorString()));
    return true;
}

bool RoadmapExport::rebuildProject(RoadmapStore &store, QIODevice *in, QString *error) {
    if (!store.isOpen())
        return fail(error, QStringLiteral("store is not open"));
    QSqlDatabase db = store.db();

    // BEGIN IMMEDIATE, never plain BEGIN (ANTS-3756 § 2.5): a deferred
    // transaction that reads and then writes must upgrade to a write lock, and
    // SQLite returns SQLITE_BUSY on that upgrade WITHOUT honouring busy_timeout.
    QSqlQuery begin(db);
    if (!begin.exec(QStringLiteral("BEGIN IMMEDIATE")))
        return failQuery(error, begin);

    const auto abort = [&](const QString &msg) {
        QSqlQuery(db).exec(QStringLiteral("ROLLBACK"));
        return fail(error, msg);
    };

    qint64 projectId = 0;
    QJsonObject legend;
    QHash<QString, qint64> sectionIdOf;   // slug   -> section_id
    QHash<QString, qint64> itemPkOf;      // id_fold -> item_pk

    int lineNo = 0;
    while (!in->atEnd()) {
        // Line by line, for the same reason the writer streams (§ 4).
        const QByteArray raw = in->readLine();
        ++lineNo;
        if (raw.trimmed().isEmpty())
            continue;

        QJsonParseError pe{};
        const QJsonDocument doc = QJsonDocument::fromJson(raw, &pe);
        if (pe.error != QJsonParseError::NoError || !doc.isObject())
            return abort(QStringLiteral("line %1 is not a JSON object: %2")
                             .arg(lineNo)
                             .arg(pe.errorString()));
        const QJsonObject o = doc.object();
        const QString t = o.value(QStringLiteral("t")).toString();

        QSqlQuery q(db);
        if (t == QLatin1String("meta")) {
            if (o.value(QStringLiteral("schema")).toInt() != RoadmapStore::kSchemaVersion)
                return abort(QStringLiteral("line %1: export schema %2, this binary knows %3")
                                 .arg(lineNo)
                                 .arg(o.value(QStringLiteral("schema")).toInt())
                                 .arg(RoadmapStore::kSchemaVersion));
            // root is NULL: it is store-local and deliberately unexported
            // (§ 2.3), which is why ANTS-3756 makes the column nullable.
            q.prepare(QStringLiteral(
                "INSERT INTO project (root, name, export_slug, legend) VALUES (NULL, ?, ?, '{}')"));
            q.addBindValue(o.value(QStringLiteral("name")).toString());
            q.addBindValue(o.value(QStringLiteral("project")).toString());
            if (!q.exec())
                return abort(q.lastError().text());
            projectId = q.lastInsertId().toLongLong();
        } else if (t == QLatin1String("id_prefix")) {
            q.prepare(QStringLiteral(
                "INSERT INTO id_prefix (project_id, prefix, high_water) VALUES (?, ?, ?)"));
            q.addBindValue(projectId);
            q.addBindValue(o.value(QStringLiteral("prefix")).toString());
            q.addBindValue(qint64(o.value(QStringLiteral("high_water")).toDouble()));
            if (!q.exec())
                return abort(q.lastError().text());
        } else if (t == QLatin1String("legend")) {
            legend.insert(o.value(QStringLiteral("status")).toString(),
                          o.value(QStringLiteral("wording")));
        } else if (t == QLatin1String("section")) {
            const QJsonValue parent = o.value(QStringLiteral("parent"));
            q.prepare(QStringLiteral("INSERT INTO section (project_id, slug, title, level, intro, "
                                     "parent_id) VALUES (?, ?, ?, ?, ?, ?)"));
            q.addBindValue(projectId);
            q.addBindValue(o.value(QStringLiteral("slug")).toString());
            q.addBindValue(o.value(QStringLiteral("title")).toString());
            q.addBindValue(o.value(QStringLiteral("level")).toInt());
            q.addBindValue(textOrNull(o, "intro"));
            q.addBindValue(parent.isNull() ? QVariant()
                                           : QVariant(sectionIdOf.value(parent.toString())));
            if (!q.exec())
                return abort(q.lastError().text());
            sectionIdOf.insert(o.value(QStringLiteral("slug")).toString(),
                               q.lastInsertId().toLongLong());
        } else if (t == QLatin1String("item")) {
            q.prepare(QStringLiteral(
                "INSERT INTO item (project_id, id, id_origin, status, headline, layman, kind, "
                "source, priority, visibility, milestone, resolution, body, created, "
                "last_modified, shipped, lanes, evidence, extras, provenance) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
            q.addBindValue(projectId);
            q.addBindValue(o.value(QStringLiteral("id")).toString());
            q.addBindValue(o.value(QStringLiteral("id_origin")).toString());
            q.addBindValue(o.value(QStringLiteral("status")).toString());
            q.addBindValue(o.value(QStringLiteral("headline")).toString());
            q.addBindValue(textOrNull(o, "layman"));
            q.addBindValue(o.value(QStringLiteral("kind")).toString());
            q.addBindValue(o.value(QStringLiteral("source")).toString());
            q.addBindValue(o.contains(QStringLiteral("priority"))
                               ? QVariant(o.value(QStringLiteral("priority")).toInt())
                               : QVariant());
            q.addBindValue(o.value(QStringLiteral("visibility")).toString());
            q.addBindValue(textOrNull(o, "milestone"));
            q.addBindValue(textOrNull(o, "resolution"));
            q.addBindValue(textOrNull(o, "body"));
            q.addBindValue(textOrNull(o, "created"));
            q.addBindValue(textOrNull(o, "last_modified"));
            q.addBindValue(textOrNull(o, "shipped"));
            for (const char *key : {"lanes", "evidence", "extras", "provenance"}) {
                QString text, why;
                if (!canonicalText(o.value(QLatin1String(key)), key, &text, &why))
                    return abort(QStringLiteral("line %1: %2").arg(lineNo).arg(why));
                q.addBindValue(text);
            }
            if (!q.exec())
                return abort(q.lastError().text());
            itemPkOf.insert(o.value(QStringLiteral("id")).toString().toLower(),
                            q.lastInsertId().toLongLong());
        } else if (t == QLatin1String("element")) {
            const QString kind = o.value(QStringLiteral("kind")).toString();
            QVariant payload;
            if (kind == QLatin1String("table")) {
                QString text, why;
                if (!canonicalText(o.value(QStringLiteral("payload")), "payload", &text, &why))
                    return abort(QStringLiteral("line %1: %2").arg(lineNo).arg(why));
                payload = text;
            } else if (kind != QLatin1String("item"))
                payload = o.value(QStringLiteral("payload")).toString();
            q.prepare(QStringLiteral("INSERT INTO element (section_id, position, kind, item_pk, "
                                     "payload) VALUES (?, ?, ?, ?, ?)"));
            q.addBindValue(sectionIdOf.value(o.value(QStringLiteral("section")).toString()));
            q.addBindValue(o.value(QStringLiteral("position")).toInt());
            q.addBindValue(kind);
            q.addBindValue(kind == QLatin1String("item")
                               ? QVariant(itemPkOf.value(o.value(QStringLiteral("ref")).toString()))
                               : QVariant());
            q.addBindValue(payload);
            if (!q.exec())
                return abort(q.lastError().text());
        } else if (t == QLatin1String("rel")) {
            q.prepare(QStringLiteral(
                "INSERT INTO relationship (type, src_pk, dst_pk, dst_project, dst_id_fold, "
                "dst_path) VALUES (?, ?, ?, ?, ?, ?)"));
            q.addBindValue(o.value(QStringLiteral("type")).toString());
            q.addBindValue(itemPkOf.value(o.value(QStringLiteral("src")).toString()));
            const bool crossProject = o.contains(QStringLiteral("dst_project"));
            const bool document = o.contains(QStringLiteral("dst_path"));
            q.addBindValue(crossProject || document
                               ? QVariant()
                               : QVariant(itemPkOf.value(o.value(QStringLiteral("dst")).toString())));
            q.addBindValue(crossProject ? QVariant(o.value(QStringLiteral("dst_project")).toString())
                                        : QVariant());
            q.addBindValue(crossProject ? QVariant(o.value(QStringLiteral("dst")).toString())
                                        : QVariant());
            q.addBindValue(document ? QVariant(o.value(QStringLiteral("dst_path")).toString())
                                    : QVariant());
            if (!q.exec())
                return abort(q.lastError().text());
        } else if (t == QLatin1String("citation")) {
            const bool byItem = o.contains(QStringLiteral("src"));
            q.prepare(QStringLiteral("INSERT INTO citation (project_id, item_pk, doc_path, "
                                     "target_file, symbol) VALUES (?, ?, ?, ?, ?)"));
            q.addBindValue(projectId);
            q.addBindValue(byItem ? QVariant(itemPkOf.value(o.value(QStringLiteral("src")).toString()))
                                  : QVariant());
            q.addBindValue(byItem ? QVariant() : QVariant(o.value(QStringLiteral("doc")).toString()));
            q.addBindValue(o.value(QStringLiteral("file")).toString());
            q.addBindValue(o.value(QStringLiteral("symbol")).toString());
            if (!q.exec())
                return abort(q.lastError().text());
        } else if (t == QLatin1String("feedback_ref")) {
            q.prepare(QStringLiteral("INSERT INTO feedback_ref (item_pk, file) VALUES (?, ?)"));
            q.addBindValue(itemPkOf.value(o.value(QStringLiteral("item")).toString()));
            q.addBindValue(o.value(QStringLiteral("file")).toString());
            if (!q.exec())
                return abort(q.lastError().text());
        } else if (t == QLatin1String("history")) {
            // Inserted directly rather than through appendHistory(): that path
            // enforces INV-14's cap because it APPENDS a revision, and a
            // rebuild restores rows that were already inside the cap when they
            // were written. Refusing them would make a store at its bound
            // unrebuildable from its own backup.
            q.prepare(QStringLiteral("INSERT INTO history (item_pk, changed_at, seq, field, "
                                     "old_value, new_value) VALUES (?, ?, ?, ?, ?, ?)"));
            q.addBindValue(itemPkOf.value(o.value(QStringLiteral("item")).toString()));
            q.addBindValue(o.value(QStringLiteral("at")).toString());
            q.addBindValue(o.value(QStringLiteral("seq")).toInt());
            q.addBindValue(o.value(QStringLiteral("field")).toString());
            q.addBindValue(textOrNull(o, "old"));
            q.addBindValue(textOrNull(o, "new"));
            if (!q.exec())
                return abort(q.lastError().text());
        } else {
            return abort(QStringLiteral("line %1: unknown record type '%2'").arg(lineNo).arg(t));
        }
    }

    if (!projectId)
        return abort(QStringLiteral("the export has no meta record"));

    if (!legend.isEmpty()) {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("UPDATE project SET legend = ? WHERE project_id = ?"));
        q.addBindValue(RoadmapStore::canonicalJson(legend));
        q.addBindValue(projectId);
        if (!q.exec())
            return abort(q.lastError().text());
    }

    QSqlQuery commit(db);
    if (!commit.exec(QStringLiteral("COMMIT")))
        return abort(commit.lastError().text());
    return true;
}
