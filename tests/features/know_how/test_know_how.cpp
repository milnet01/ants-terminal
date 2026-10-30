// Feature-conformance test for ANTS-6310 — the know-how store.
// Contract: docs/specs/ANTS-6310-know-how-store.md (pointer: spec.md here).
//
// Every store is a file inside a QTemporaryDir. The verb's handler, whose
// default path is the real store, is test_know_how_verb.cpp's, under a sandbox.

#include <gtest/gtest.h>

#include "knowhowstore.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSet>
#include <QTemporaryDir>
#include <QVariant>

#include <sys/stat.h>

#include <functional>

#ifndef ANTS_SRC_DIR
#  error "ANTS_SRC_DIR compile definition required"
#endif

namespace {

std::string dump(const QJsonObject &o) {
    return QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString();
}

QString code(const QJsonObject &o) { return o.value(QStringLiteral("code")).toString(); }
bool isOk(const QJsonObject &o) { return o.value(QStringLiteral("ok")).toBool(); }
QString bodyOf(const QJsonObject &o) { return o.value(QStringLiteral("body")).toString(); }

// A connection of the test's own, separate from any the store opens.
class Peer {
public:
    explicit Peer(const QString &path) {
        static int counter = 0;
        m_name = QStringLiteral("know_how_test_peer_%1").arg(counter++);
        m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_name);
        m_db.setDatabaseName(path);
        m_db.open();
    }
    ~Peer() {
        m_db.close();
        m_db = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_name);
    }
    bool exec(const QString &sql) { return QSqlQuery(m_db).exec(sql); }
    int count(const QString &sql) {
        QSqlQuery q(m_db);
        return q.exec(sql) && q.next() ? q.value(0).toInt() : -1;
    }
    QString text(const QString &sql) {
        QSqlQuery q(m_db);
        return q.exec(sql) && q.next() ? q.value(0).toString() : QString();
    }
    QSqlDatabase &db() { return m_db; }

private:
    QString m_name;
    QSqlDatabase m_db;
};

struct Fixture {
    QTemporaryDir dir;
    QString path = dir.filePath(QStringLiteral("ants-terminal/knowhow.sqlite"));
    QDateTime now = QDateTime::fromString(QStringLiteral("2026-10-30T12:00:00Z"), Qt::ISODate);

    QJsonObject call(const char *op, const QJsonObject &args = {},
                     const QString &author = QStringLiteral("alpha")) {
        return KnowHowStore(path, [this] { return now; })
            .run(QString::fromLatin1(op), args, author);
    }
    QJsonObject add(const QString &topic, const QString &body = QStringLiteral("some body"),
                    const QString &title = QStringLiteral("A title"),
                    const QStringList &tags = {}) {
        return call("add", {{QStringLiteral("topic"), topic},
                            {QStringLiteral("title"), title},
                            {QStringLiteral("body"), body},
                            {QStringLiteral("tags"), QJsonArray::fromStringList(tags)}});
    }
    QJsonObject get(const QString &topic, bool history = false) {
        return call("get", {{QStringLiteral("topic"), topic},
                            {QStringLiteral("history"), history}});
    }
    QJsonObject find(const QString &query, int limit = 0) {
        QJsonObject args{{QStringLiteral("query"), query}};
        if (limit)
            args[QStringLiteral("limit")] = limit;
        return call("find", args);
    }
    QJsonObject update(const QString &topic, const QString &body) {
        return call("update", {{QStringLiteral("topic"), topic},
                               {QStringLiteral("body"), body}});
    }
    QJsonObject retire(const QString &topic) {
        return call("retire", {{QStringLiteral("topic"), topic}});
    }
    QJsonObject purge(const QString &topic, qint64 expected, qint64 revision = 0) {
        QJsonObject args{{QStringLiteral("topic"), topic},
                         {QStringLiteral("expected_revision"), expected},
                         {QStringLiteral("reason"), QStringLiteral("a leaked value")}};
        if (revision)
            args[QStringLiteral("revision")] = revision;
        return call("purge", args);
    }
    bool fileExists() const { return QFileInfo::exists(path); }
    // True when any of the store's three files holds `marker`'s raw bytes.
    bool onDisk(const QByteArray &marker) const {
        for (const QString &f : {path, path + QStringLiteral("-wal"), path + QStringLiteral("-shm")}) {
            QFile file(f);
            if (file.open(QIODevice::ReadOnly) && file.readAll().contains(marker))
                return true;
        }
        return false;
    }
};

QStringList topicsOf(const QJsonObject &reply, const char *key = "results") {
    QStringList out;
    for (const QJsonValue v : reply.value(QString::fromLatin1(key)).toArray())
        out << v.toObject().value(QStringLiteral("topic")).toString();
    return out;
}

int modeOf(const QString &path) {
    struct stat st{};
    return ::stat(QFile::encodeName(path).constData(), &st) == 0 ? int(st.st_mode & 0777) : -1;
}

}  // namespace

// INV-1 — only a committed row creates the file.
TEST(KnowHow, ReadsDoNotCreateTheFile) {
    Fixture f;
    const QJsonObject found = f.find(QStringLiteral("anything"));
    ASSERT_TRUE(isOk(found)) << dump(found);
    ASSERT_TRUE(found.value(QStringLiteral("searched")).isDouble()) << dump(found);
    EXPECT_EQ(found.value(QStringLiteral("searched")).toInt(), 0);
    EXPECT_TRUE(found.value(QStringLiteral("results")).toArray().isEmpty());

    const QJsonObject listed = f.call("list");
    ASSERT_TRUE(isOk(listed)) << dump(listed);
    EXPECT_EQ(listed.value(QStringLiteral("total")).toInt(-1), 0);

    EXPECT_EQ(code(f.get(QStringLiteral("nothing"))), QStringLiteral("not_found"));
    EXPECT_EQ(code(f.update(QStringLiteral("nothing"), QStringLiteral("b"))),
              QStringLiteral("not_found"));
    EXPECT_EQ(code(f.retire(QStringLiteral("nothing"))), QStringLiteral("not_found"));
    EXPECT_EQ(code(f.purge(QStringLiteral("nothing"), 1)), QStringLiteral("not_found"));

    const QJsonObject preview = f.call("add", {{QStringLiteral("topic"), QStringLiteral("t")},
                                               {QStringLiteral("title"), QStringLiteral("T")},
                                               {QStringLiteral("body"), QStringLiteral("B")},
                                               {QStringLiteral("dry_run"), true}});
    ASSERT_TRUE(isOk(preview)) << dump(preview);
    EXPECT_TRUE(preview.value(QStringLiteral("dry_run")).toBool());
    EXPECT_EQ(preview.value(QStringLiteral("revision")).toInt(), 1);

    EXPECT_FALSE(f.fileExists()) << "a call that committed no row created the store";
    EXPECT_FALSE(QFileInfo::exists(QFileInfo(f.path).absolutePath()));

    ASSERT_TRUE(isOk(f.add(QStringLiteral("t"))));
    EXPECT_TRUE(f.fileExists());
}

// A dry run on a store that exists changes nothing either, on any write op.
TEST(KnowHow, DryRunWritesNothing) {
    Fixture f;
    ASSERT_TRUE(isOk(f.add(QStringLiteral("t"), QStringLiteral("old"))));
    ASSERT_TRUE(isOk(f.update(QStringLiteral("t"), QStringLiteral("new"))));

    const QJsonObject add = f.call("add", {{QStringLiteral("topic"), QStringLiteral("other")},
                                           {QStringLiteral("title"), QStringLiteral("T")},
                                           {QStringLiteral("body"), QStringLiteral("B")},
                                           {QStringLiteral("dry_run"), true}});
    EXPECT_TRUE(isOk(add)) << dump(add);
    EXPECT_EQ(code(f.get(QStringLiteral("other"))), QStringLiteral("not_found"));

    const QJsonObject update = f.call("update", {{QStringLiteral("topic"), QStringLiteral("t")},
                                                 {QStringLiteral("body"), QStringLiteral("x")},
                                                 {QStringLiteral("dry_run"), true}});
    EXPECT_EQ(update.value(QStringLiteral("revision")).toInt(), 3) << dump(update);

    const QJsonObject retire = f.call("retire", {{QStringLiteral("topic"), QStringLiteral("t")},
                                                 {QStringLiteral("dry_run"), true}});
    EXPECT_TRUE(isOk(retire)) << dump(retire);

    const QJsonObject purge = f.call("purge", {{QStringLiteral("topic"), QStringLiteral("t")},
                                               {QStringLiteral("expected_revision"), 2},
                                               {QStringLiteral("reason"), QStringLiteral("r")},
                                               {QStringLiteral("dry_run"), true}});
    EXPECT_EQ(purge.value(QStringLiteral("rows_erased")).toInt(), 2) << dump(purge);
    EXPECT_FALSE(purge.contains(QStringLiteral("file_scrubbed"))) << dump(purge);

    const QJsonObject got = f.get(QStringLiteral("t"), true);
    EXPECT_EQ(bodyOf(got), QStringLiteral("new"));
    EXPECT_EQ(got.value(QStringLiteral("revision")).toInt(), 2);
    ASSERT_TRUE(got.value(QStringLiteral("retired")).isBool());
    EXPECT_FALSE(got.value(QStringLiteral("retired")).toBool());
    EXPECT_EQ(got.value(QStringLiteral("history")).toArray().size(), 1);
}

// INV-2 — one entry per topic.
TEST(KnowHow, AddAndUpdateRefuseTheWrongState) {
    Fixture f;
    ASSERT_TRUE(isOk(f.add(QStringLiteral("t"), QStringLiteral("first"))));

    const QJsonObject again = f.add(QStringLiteral("t"), QStringLiteral("second"));
    EXPECT_EQ(code(again), QStringLiteral("topic_exists")) << dump(again);
    EXPECT_EQ(bodyOf(f.get(QStringLiteral("t"))), QStringLiteral("first"));

    ASSERT_TRUE(isOk(f.retire(QStringLiteral("t"))));
    const QJsonObject retired = f.add(QStringLiteral("t"), QStringLiteral("third"));
    EXPECT_EQ(code(retired), QStringLiteral("topic_exists")) << dump(retired);
    EXPECT_TRUE(retired.value(QStringLiteral("error")).toString().contains(QStringLiteral("update")));
    EXPECT_EQ(bodyOf(f.get(QStringLiteral("t"))), QStringLiteral("first"));

    EXPECT_EQ(code(f.update(QStringLiteral("missing"), QStringLiteral("b"))),
              QStringLiteral("not_found"));
    EXPECT_EQ(code(f.retire(QStringLiteral("missing"))), QStringLiteral("not_found"));
}

// INV-3 — a change keeps what it replaced.
TEST(KnowHow, UpdateKeepsHistory) {
    Fixture f;
    ASSERT_TRUE(isOk(f.add(QStringLiteral("t"), QStringLiteral("body 1"))));
    for (int i = 2; i <= 23; ++i) {
        f.now = f.now.addSecs(60);
        ASSERT_TRUE(isOk(f.call("update", {{QStringLiteral("topic"), QStringLiteral("t")},
                                           {QStringLiteral("body"), QStringLiteral("body %1").arg(i)}},
                                QStringLiteral("beta"))));
    }
    QJsonObject got = f.get(QStringLiteral("t"), true);
    EXPECT_EQ(got.value(QStringLiteral("revision")).toInt(), 23);
    EXPECT_EQ(got.value(QStringLiteral("author")).toString(), QStringLiteral("beta"));
    QJsonArray history = got.value(QStringLiteral("history")).toArray();
    ASSERT_EQ(history.size(), KnowHowStore::kHistoryKept);
    // The newest 20 survive: revisions 3 to 22.
    QSet<int> revisions;
    for (const QJsonValue v : history)
        revisions.insert(v.toObject().value(QStringLiteral("revision")).toInt());
    EXPECT_TRUE(revisions.contains(22));
    EXPECT_TRUE(revisions.contains(3));
    EXPECT_FALSE(revisions.contains(2)) << "the trim kept the oldest rows";
    for (const QJsonValue v : history) {
        const QJsonObject row = v.toObject();
        if (row.value(QStringLiteral("revision")).toInt() == 22) {
            EXPECT_EQ(bodyOf(row), QStringLiteral("body 22"));
            EXPECT_TRUE(row.value(QStringLiteral("tags")).isArray());
        }
    }

    // A fieldless update records a check: last_verified moves, nothing else.
    const QString verified = got.value(QStringLiteral("last_verified")).toString();
    f.now = f.now.addDays(1);
    const QJsonObject checked = f.call("update", {{QStringLiteral("topic"), QStringLiteral("t")}},
                                       QStringLiteral("gamma"));
    ASSERT_TRUE(isOk(checked)) << dump(checked);
    EXPECT_EQ(checked.value(QStringLiteral("revision")).toInt(), 23);
    got = f.get(QStringLiteral("t"), true);
    EXPECT_NE(got.value(QStringLiteral("last_verified")).toString(), verified);
    EXPECT_EQ(got.value(QStringLiteral("revision")).toInt(), 23);
    EXPECT_EQ(got.value(QStringLiteral("author")).toString(), QStringLiteral("beta"));
    EXPECT_EQ(got.value(QStringLiteral("history")).toArray().size(), KnowHowStore::kHistoryKept);

    // Retire raises the revision and adds no history; a fieldless update then
    // makes the entry active at that revision.
    ASSERT_TRUE(isOk(f.retire(QStringLiteral("t"))));
    got = f.get(QStringLiteral("t"));
    EXPECT_EQ(got.value(QStringLiteral("revision")).toInt(), 24);
    EXPECT_TRUE(got.value(QStringLiteral("retired")).toBool());
    ASSERT_TRUE(isOk(f.call("update", {{QStringLiteral("topic"), QStringLiteral("t")}})));
    got = f.get(QStringLiteral("t"));
    ASSERT_TRUE(got.value(QStringLiteral("retired")).isBool());
    EXPECT_FALSE(got.value(QStringLiteral("retired")).toBool());
    EXPECT_EQ(got.value(QStringLiteral("revision")).toInt(), 24);

    // So does an update that changes a field.
    ASSERT_TRUE(isOk(f.retire(QStringLiteral("t"))));
    ASSERT_TRUE(isOk(f.update(QStringLiteral("t"), QStringLiteral("back"))));
    got = f.get(QStringLiteral("t"));
    EXPECT_FALSE(got.value(QStringLiteral("retired")).toBool());
    EXPECT_EQ(got.value(QStringLiteral("revision")).toInt(), 26);
}

// INV-4 — a stale writer cannot overwrite.
TEST(KnowHow, ExpectedRevisionRefusesAStaleWrite) {
    Fixture f;
    ASSERT_TRUE(isOk(f.add(QStringLiteral("t"), QStringLiteral("one"))));
    const auto write = [&](int expected, const QString &body) {
        return f.call("update", {{QStringLiteral("topic"), QStringLiteral("t")},
                                 {QStringLiteral("body"), body},
                                 {QStringLiteral("expected_revision"), expected}});
    };
    ASSERT_TRUE(isOk(write(1, QStringLiteral("two"))));
    const QJsonObject stale = write(1, QStringLiteral("lost"));
    EXPECT_EQ(code(stale), QStringLiteral("conflict")) << dump(stale);
    EXPECT_EQ(bodyOf(f.get(QStringLiteral("t"))), QStringLiteral("two"));

    // A retire moved it: the writer who read before the retire is stale too.
    ASSERT_TRUE(isOk(f.retire(QStringLiteral("t"))));
    const QJsonObject afterRetire = write(2, QStringLiteral("lost"));
    EXPECT_EQ(code(afterRetire), QStringLiteral("conflict")) << dump(afterRetire);
    const QJsonObject got = f.get(QStringLiteral("t"));
    EXPECT_EQ(bodyOf(got), QStringLiteral("two"));
    EXPECT_TRUE(got.value(QStringLiteral("retired")).toBool()) << "a refusal reactivated it";

    EXPECT_EQ(code(f.call("update", {{QStringLiteral("topic"), QStringLiteral("t")},
                                     {QStringLiteral("expected_revision"), 1.5}})),
              QStringLiteral("bad_args"));
}

// INV-5 — a secret-shaped value is never stored or echoed. The shapes are built
// at run time, so no secret scanner flags this source.
TEST(KnowHow, SecretShapesAreRefused) {
    Fixture f;
    const QString pat = QStringLiteral("gh") + QStringLiteral("p_") + QString(36, QLatin1Char('a'));
    const QString lab = QStringLiteral("gl") + QStringLiteral("pat-") + QString(20, QLatin1Char('a'));
    ASSERT_TRUE(isOk(f.add(QStringLiteral("t"), QStringLiteral("clean"))));

    const auto expectRefused = [&](const QJsonObject &reply, const QString &secret) {
        EXPECT_EQ(code(reply), QStringLiteral("secret_detected")) << dump(reply);
        EXPECT_FALSE(QString::fromStdString(dump(reply)).contains(secret)) << "the refusal echoed it";
    };
    const auto writes = [&](const char *op, const QString &topic) {
        const QJsonObject base{{QStringLiteral("topic"), topic},
                               {QStringLiteral("title"), QStringLiteral("T")},
                               {QStringLiteral("body"), QStringLiteral("B")}};
        QJsonObject withBody = base, withTitle = base, withTag = base;
        withBody[QStringLiteral("body")] = QStringLiteral("use ") + pat;
        withTitle[QStringLiteral("title")] = pat;
        withTag[QStringLiteral("tags")] = QJsonArray{lab};
        expectRefused(f.call(op, withBody), pat);
        expectRefused(f.call(op, withTitle), pat);
        expectRefused(f.call(op, withTag), lab);
    };
    writes("add", QStringLiteral("new-topic"));
    writes("update", QStringLiteral("t"));
    expectRefused(f.add(lab), lab);
    expectRefused(f.update(lab, QStringLiteral("B")), lab);

    EXPECT_EQ(code(f.get(QStringLiteral("new-topic"))), QStringLiteral("not_found"));
    const QJsonObject got = f.get(QStringLiteral("t"), true);
    EXPECT_EQ(bodyOf(got), QStringLiteral("clean"));
    EXPECT_EQ(got.value(QStringLiteral("revision")).toInt(), 1);
    EXPECT_TRUE(got.value(QStringLiteral("history")).toArray().isEmpty());

    const QJsonObject how = f.add(QStringLiteral("win"), QStringLiteral("SSH key ~/.ssh/win_ed25519"));
    EXPECT_TRUE(isOk(how)) << dump(how);
}

// INV-6 — find answers small, and answers a sentence.
TEST(KnowHow, FindRanksAndStaysSmall) {
    Fixture f;
    ASSERT_TRUE(isOk(f.add(QStringLiteral("windows-box"), QStringLiteral("use ssh to it"),
                           QStringLiteral("Windows PC"), {QStringLiteral("windows")})));
    ASSERT_TRUE(isOk(f.add(QStringLiteral("general-notes"),
                           QStringLiteral("how do you reach the shed"), QStringLiteral("Notes"))));

    const QJsonObject sentence = f.find(QStringLiteral("how do I reach the Windows box"));
    ASSERT_TRUE(isOk(sentence)) << dump(sentence);
    ASSERT_EQ(topicsOf(sentence).size(), 2) << dump(sentence);
    EXPECT_EQ(topicsOf(sentence).first(), QStringLiteral("windows-box"))
        << "a body-only entry matching more tokens outranked the topic match";
    EXPECT_EQ(sentence.value(QStringLiteral("count")).toInt(), 2);
    EXPECT_EQ(sentence.value(QStringLiteral("searched")).toInt(), 2);
    for (const QJsonValue v : sentence.value(QStringLiteral("results")).toArray()) {
        const QJsonObject row = v.toObject();
        EXPECT_FALSE(row.contains(QStringLiteral("body"))) << "the body leaked into a row";
        EXPECT_TRUE(row.value(QStringLiteral("stale")).isBool());
        EXPECT_TRUE(row.value(QStringLiteral("age_days")).isDouble());
    }

    // Case folds for ASCII, and a body-only hit still answers.
    EXPECT_EQ(topicsOf(f.find(QStringLiteral("SHED"))), QStringList{QStringLiteral("general-notes")});

    // The snippet is the line holding the first matched token, clipped.
    const QString longLine = QStringLiteral("needle ") + QString(300, QLatin1Char('x'));
    ASSERT_TRUE(isOk(f.add(QStringLiteral("long"), QStringLiteral("first line\n") + longLine)));
    const QJsonObject longFound = f.find(QStringLiteral("needle"));
    ASSERT_EQ(topicsOf(longFound), QStringList{QStringLiteral("long")}) << dump(longFound);
    const QString snippet = longFound.value(QStringLiteral("results")).toArray().first()
                                .toObject().value(QStringLiteral("snippet")).toString();
    EXPECT_TRUE(snippet.startsWith(QStringLiteral("needle")));
    EXPECT_LE(snippet.size(), KnowHowStore::kSnippetChars);
    EXPECT_GT(snippet.size(), 100);

    // A clip never splits a surrogate pair.
    QString emoji(KnowHowStore::kSnippetChars - 1, QLatin1Char('y'));
    emoji += QString::fromUtf8("\xF0\x9F\x90\x9C");
    ASSERT_TRUE(isOk(f.add(QStringLiteral("clip"), emoji)));
    const QString clipped = f.find(QStringLiteral("yyyy")).value(QStringLiteral("results"))
                                .toArray().first().toObject().value(QStringLiteral("snippet")).toString();
    EXPECT_EQ(clipped.size(), KnowHowStore::kSnippetChars - 1);

    // A token past the eighth is ignored, a one-character token dropped.
    EXPECT_TRUE(topicsOf(f.find(QStringLiteral("q1 q2 q3 q4 q5 q6 q7 q8 windows"))).isEmpty());
    const QJsonObject oneChar = f.find(QStringLiteral("I"));
    ASSERT_TRUE(isOk(oneChar));
    EXPECT_TRUE(topicsOf(oneChar).isEmpty());
    EXPECT_GT(oneChar.value(QStringLiteral("searched")).toInt(), 0);

    // limit is honoured; one over the maximum is clamped, not refused.
    EXPECT_EQ(topicsOf(f.find(QStringLiteral("use how needle yyyy"), 1)).size(), 1);
    EXPECT_TRUE(isOk(f.find(QStringLiteral("use"), 500)));

    // A retired entry is gone from find, list and total; searched counts active.
    const int before = f.find(QStringLiteral("use")).value(QStringLiteral("searched")).toInt();
    ASSERT_TRUE(isOk(f.retire(QStringLiteral("windows-box"))));
    const QJsonObject afterRetire = f.find(QStringLiteral("windows"));
    EXPECT_TRUE(topicsOf(afterRetire).isEmpty()) << dump(afterRetire);
    EXPECT_EQ(afterRetire.value(QStringLiteral("searched")).toInt(), before - 1);
    const QJsonObject listed = f.call("list");
    EXPECT_FALSE(topicsOf(listed, "topics").contains(QStringLiteral("windows-box")));
    EXPECT_EQ(listed.value(QStringLiteral("total")).toInt(), before - 1);
    ASSERT_TRUE(isOk(f.call("update", {{QStringLiteral("topic"), QStringLiteral("windows-box")}})));

    // A tag alone returns the active entries holding it, whole, newest first.
    f.now = f.now.addDays(1);
    ASSERT_TRUE(isOk(f.add(QStringLiteral("newer-win"), QStringLiteral("b"), QStringLiteral("T"),
                           {QStringLiteral("windows")})));
    const QJsonObject tagged = f.call("find", {{QStringLiteral("tag"), QStringLiteral("windows")}});
    EXPECT_EQ(topicsOf(tagged), (QStringList{QStringLiteral("newer-win"), QStringLiteral("windows-box")}))
        << dump(tagged);
    EXPECT_TRUE(topicsOf(f.call("find", {{QStringLiteral("tag"), QStringLiteral("window")}})).isEmpty())
        << "a tag must match one tag whole";
    EXPECT_EQ(topicsOf(f.call("list", {{QStringLiteral("tag"), QStringLiteral("windows")}}), "topics").size(), 2);
    EXPECT_EQ(code(f.call("find")), QStringLiteral("missing_field"));

    // LIKE's wildcards are literal in a query.
    ASSERT_TRUE(isOk(f.add(QStringLiteral("percent"), QStringLiteral("runs at 50% load"))));
    ASSERT_TRUE(isOk(f.add(QStringLiteral("number"), QStringLiteral("runs at 500 load"))));
    EXPECT_EQ(topicsOf(f.find(QStringLiteral("50%"))), QStringList{QStringLiteral("percent")});

    // list pages, and next_offset appears only while more remain.
    const QJsonObject page = f.call("list", {{QStringLiteral("limit"), 2}});
    EXPECT_EQ(topicsOf(page, "topics").size(), 2);
    ASSERT_TRUE(page.value(QStringLiteral("next_offset")).isDouble()) << dump(page);
    EXPECT_EQ(page.value(QStringLiteral("next_offset")).toInt(), 2);
    const QJsonObject tail = f.call("list", {{QStringLiteral("offset"), page.value(QStringLiteral("total")).toInt() - 1}});
    EXPECT_EQ(topicsOf(tail, "topics").size(), 1);
    EXPECT_FALSE(tail.contains(QStringLiteral("next_offset"))) << dump(tail);
}

// INV-9 — staleness flips after 90 days, not at them, on every op.
TEST(KnowHow, StaleAfterNinetyDays) {
    Fixture f;
    ASSERT_TRUE(isOk(f.add(QStringLiteral("t"), QStringLiteral("needle"))));
    const auto expectStale = [&](bool stale, int age) {
        const QJsonObject row = f.find(QStringLiteral("needle")).value(QStringLiteral("results"))
                                    .toArray().first().toObject();
        ASSERT_TRUE(row.value(QStringLiteral("stale")).isBool()) << dump(row);
        EXPECT_EQ(row.value(QStringLiteral("stale")).toBool(), stale) << "find";
        EXPECT_EQ(row.value(QStringLiteral("age_days")).toInt(), age);
        const QJsonObject got = f.get(QStringLiteral("t"));
        ASSERT_TRUE(got.value(QStringLiteral("stale")).isBool()) << dump(got);
        EXPECT_EQ(got.value(QStringLiteral("stale")).toBool(), stale) << "get";
        EXPECT_EQ(got.value(QStringLiteral("age_days")).toInt(), age);
        const QJsonObject listed = f.call("list").value(QStringLiteral("topics")).toArray()
                                       .first().toObject();
        ASSERT_TRUE(listed.value(QStringLiteral("stale")).isBool()) << dump(listed);
        EXPECT_EQ(listed.value(QStringLiteral("stale")).toBool(), stale) << "list";
    };
    const QDateTime start = f.now;
    expectStale(false, 0);
    f.now = start.addDays(90);
    expectStale(false, 90);
    f.now = start.addDays(91);
    expectStale(true, 91);
    // A check resets it.
    ASSERT_TRUE(isOk(f.call("update", {{QStringLiteral("topic"), QStringLiteral("t")}})));
    expectStale(false, 0);
}

// INV-10 — the file is owner-only and a newer one is refused.
TEST(KnowHow, FileModeAndNewerSchema) {
    Fixture f;
    ASSERT_TRUE(isOk(f.add(QStringLiteral("t"))));
    EXPECT_EQ(modeOf(f.path), 0600);
    {
        // A second connection keeps the sidecars alive after the write closes.
        Peer peer(f.path);
        ASSERT_EQ(peer.count(QStringLiteral("SELECT count(*) FROM know_how")), 1);
        ASSERT_TRUE(QFile::setPermissions(f.path, QFile::ReadOwner | QFile::WriteOwner
                                                      | QFile::ReadGroup | QFile::ReadOther));
        ASSERT_TRUE(isOk(f.add(QStringLiteral("u"))));
        EXPECT_EQ(modeOf(f.path), 0600) << "a write left a wider mode in place";
        ASSERT_TRUE(QFileInfo::exists(f.path + QStringLiteral("-wal")));
        ASSERT_TRUE(QFileInfo::exists(f.path + QStringLiteral("-shm")));
        EXPECT_EQ(modeOf(f.path + QStringLiteral("-wal")), 0600);
        EXPECT_EQ(modeOf(f.path + QStringLiteral("-shm")), 0600);
    }

    // A newer file: every op refuses, and the bytes stay as they were.
    Fixture newer;
    // A real store, so a build that ignored the version would answer rather than
    // fail on a missing table.
    ASSERT_TRUE(isOk(newer.add(QStringLiteral("existing"))));
    {
        Peer peer(newer.path);
        ASSERT_TRUE(peer.exec(QStringLiteral("PRAGMA user_version = %1")
                                  .arg(KnowHowStore::kSchemaVersion + 1)));
    }
    QFile file(newer.path);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const QByteArray bytes = file.readAll();
    file.close();
    EXPECT_EQ(code(newer.add(QStringLiteral("t"))), QStringLiteral("io_error"));
    EXPECT_EQ(code(newer.find(QStringLiteral("t"))), QStringLiteral("io_error"));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_EQ(file.readAll(), bytes);
    file.close();

    // § 6 — a corrupt file refuses and is not recreated, which would drop entries.
    Fixture corrupt;
    ASSERT_TRUE(QDir().mkpath(QFileInfo(corrupt.path).absolutePath()));
    QFile bad(corrupt.path);
    ASSERT_TRUE(bad.open(QIODevice::WriteOnly));
    const QByteArray junk(8192, 'j');
    bad.write(junk);
    bad.close();
    const QJsonObject refused = corrupt.add(QStringLiteral("t"));
    EXPECT_EQ(code(refused), QStringLiteral("io_error")) << dump(refused);
    EXPECT_TRUE(refused.value(QStringLiteral("error")).toString().contains(corrupt.path));
    ASSERT_TRUE(bad.open(QIODevice::ReadOnly));
    EXPECT_EQ(bad.readAll(), junk);
}

// INV-11 — limits refuse rather than truncate.
TEST(KnowHow, LimitsRefuse) {
    Fixture f;
    const auto addRaw = [&](const QString &topic, const QString &title, const QString &body,
                            const QJsonArray &tags = {}) {
        return f.call("add", {{QStringLiteral("topic"), topic}, {QStringLiteral("title"), title},
                              {QStringLiteral("body"), body}, {QStringLiteral("tags"), tags}});
    };
    const auto chars = [](int n, char c = 'a') { return QString(n, QLatin1Char(c)); };
    const QString T = QStringLiteral("T"), B = QStringLiteral("B");

    EXPECT_TRUE(isOk(addRaw(chars(KnowHowStore::kMaxTopicBytes), T, B)));
    EXPECT_EQ(code(addRaw(chars(KnowHowStore::kMaxTopicBytes + 1), T, B)), QStringLiteral("bad_args"));
    EXPECT_EQ(code(addRaw(QStringLiteral("Upper"), T, B)), QStringLiteral("bad_args"));
    EXPECT_EQ(code(addRaw(QStringLiteral("two\n"), T, B)), QStringLiteral("bad_args"));

    EXPECT_TRUE(isOk(addRaw(QStringLiteral("t1"), chars(KnowHowStore::kMaxTitleBytes), B)));
    EXPECT_EQ(code(addRaw(QStringLiteral("t2"), chars(KnowHowStore::kMaxTitleBytes + 1), B)),
              QStringLiteral("bad_args"));
    EXPECT_EQ(code(addRaw(QStringLiteral("t3"), QStringLiteral("two\nlines"), B)), QStringLiteral("bad_args"));
    EXPECT_EQ(code(addRaw(QStringLiteral("t4"), QString(), B)), QStringLiteral("bad_args"));

    EXPECT_TRUE(isOk(addRaw(QStringLiteral("b1"), T, chars(KnowHowStore::kMaxBodyBytes))));
    EXPECT_EQ(code(addRaw(QStringLiteral("b2"), T, chars(KnowHowStore::kMaxBodyBytes + 1))),
              QStringLiteral("bad_args"));
    EXPECT_EQ(code(addRaw(QStringLiteral("b3"), T, QString())), QStringLiteral("bad_args"));
    // Bytes, not characters: under the limit in characters, one over it in UTF-8.
    const QString twoByte = QString(KnowHowStore::kMaxBodyBytes / 2, QChar(0x00E9)) + QLatin1Char('a');
    EXPECT_EQ(code(addRaw(QStringLiteral("b4"), T, twoByte)), QStringLiteral("bad_args"));

    QJsonArray eight;
    for (int i = 0; i < KnowHowStore::kMaxTags; ++i)
        eight.append(QStringLiteral("tag-%1").arg(i));
    EXPECT_TRUE(isOk(addRaw(QStringLiteral("g1"), T, B, eight)));
    QJsonArray nine = eight;
    nine.append(QStringLiteral("one-more"));
    EXPECT_EQ(code(addRaw(QStringLiteral("g2"), T, B, nine)), QStringLiteral("bad_args"));
    EXPECT_TRUE(isOk(addRaw(QStringLiteral("g3"), T, B, {chars(KnowHowStore::kMaxTagBytes)})));
    EXPECT_EQ(code(addRaw(QStringLiteral("g4"), T, B, {chars(KnowHowStore::kMaxTagBytes + 1)})),
              QStringLiteral("bad_args"));
    EXPECT_EQ(code(addRaw(QStringLiteral("g5"), T, B, {QStringLiteral("Bad Tag")})), QStringLiteral("bad_args"));

    for (const char *refused : {"t2", "t3", "t4", "b2", "b3", "b4", "g2", "g4", "g5"})
        EXPECT_EQ(code(f.get(QString::fromLatin1(refused))), QStringLiteral("not_found")) << refused;

    EXPECT_EQ(code(f.call("add", {{QStringLiteral("title"), T}, {QStringLiteral("body"), B}})),
              QStringLiteral("missing_field"));
    EXPECT_EQ(code(f.call("add", {{QStringLiteral("topic"), QStringLiteral("x")}, {QStringLiteral("body"), B}})),
              QStringLiteral("missing_field"));

    // The topic cap counts retired rows. Filled through a connection of the
    // test's own, because a thousand adds each fsync.
    {
        Peer peer(f.path);
        const int have = peer.count(QStringLiteral("SELECT count(*) FROM know_how"));
        ASSERT_GT(have, 0);
        ASSERT_TRUE(peer.exec(QStringLiteral("BEGIN")));
        for (int i = have; i < KnowHowStore::kMaxTopics; ++i)
            ASSERT_TRUE(peer.exec(QStringLiteral(
                "INSERT INTO know_how (topic, title, body, tags, author, created, last_verified,"
                " revision, retired_at) VALUES ('fill-%1', 'T', 'B', ' ', 'a',"
                " '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z', 1, '2026-01-01T00:00:00Z')").arg(i)));
        ASSERT_TRUE(peer.exec(QStringLiteral("COMMIT")));
    }
    const QJsonObject full = addRaw(QStringLiteral("one-too-many"), T, B);
    EXPECT_EQ(code(full), QStringLiteral("store_full")) << dump(full);
    EXPECT_EQ(code(f.get(QStringLiteral("one-too-many"))), QStringLiteral("not_found"));
}

// INV-13 — purge erases the text from the file, and only what it names.
TEST(KnowHow, PurgeErasesFromTheFile) {
    // Long enough to need its own overflow pages. The marker is short and
    // repeated, because a page boundary would split one long search string.
    const QByteArray marker = "PURGE-MARKER";
    const QString secretBody = QString::fromLatin1(marker + '-').repeated(800);

    // Every connection closed: the marker's bytes are gone, the current body stays.
    {
        Fixture f;
        ASSERT_TRUE(isOk(f.add(QStringLiteral("t"), secretBody)));
        ASSERT_TRUE(isOk(f.update(QStringLiteral("t"), QStringLiteral("clean text"))));
        ASSERT_TRUE(f.onDisk(marker)) << "the read cannot see the marker at all";
        const QJsonObject purged = f.purge(QStringLiteral("t"), 2, 1);
        ASSERT_TRUE(isOk(purged)) << dump(purged);
        EXPECT_EQ(purged.value(QStringLiteral("rows_erased")).toInt(), 1);
        EXPECT_EQ(purged.value(QStringLiteral("revision")).toInt(), 1);
        ASSERT_TRUE(purged.value(QStringLiteral("file_scrubbed")).isBool());
        EXPECT_TRUE(purged.value(QStringLiteral("file_scrubbed")).toBool());
        EXPECT_FALSE(f.onDisk(marker)) << "the purged text is still on disk";
        const QJsonObject got = f.get(QStringLiteral("t"), true);
        EXPECT_EQ(bodyOf(got), QStringLiteral("clean text"));
        EXPECT_TRUE(got.value(QStringLiteral("history")).toArray().isEmpty());
    }

    // An idle second connection: the checkpoint still runs, and the marker goes
    // while that connection stays open. Only this case fails with no checkpoint.
    {
        Fixture f;
        ASSERT_TRUE(isOk(f.add(QStringLiteral("t"), secretBody)));
        Peer idle(f.path);
        ASSERT_EQ(idle.count(QStringLiteral("SELECT count(*) FROM know_how")), 1);
        ASSERT_TRUE(isOk(f.update(QStringLiteral("t"), QStringLiteral("clean text"))));
        ASSERT_TRUE(f.onDisk(marker));
        const QJsonObject purged = f.purge(QStringLiteral("t"), 2, 1);
        ASSERT_TRUE(isOk(purged)) << dump(purged);
        ASSERT_TRUE(purged.value(QStringLiteral("file_scrubbed")).isBool());
        EXPECT_TRUE(purged.value(QStringLiteral("file_scrubbed")).toBool()) << dump(purged);
        EXPECT_FALSE(f.onDisk(marker)) << "the WAL still holds it: the checkpoint did not truncate";
    }

    // A second connection inside a read transaction: the reply says so.
    {
        Fixture f;
        ASSERT_TRUE(isOk(f.add(QStringLiteral("t"), secretBody)));
        Peer reader(f.path);
        ASSERT_TRUE(reader.exec(QStringLiteral("BEGIN")));
        ASSERT_EQ(reader.count(QStringLiteral("SELECT count(*) FROM know_how")), 1);
        ASSERT_TRUE(isOk(f.update(QStringLiteral("t"), QStringLiteral("clean text"))));
        const QJsonObject purged = f.purge(QStringLiteral("t"), 2, 1);
        ASSERT_TRUE(isOk(purged)) << dump(purged);
        ASSERT_TRUE(purged.value(QStringLiteral("file_scrubbed")).isBool());
        EXPECT_FALSE(purged.value(QStringLiteral("file_scrubbed")).toBool()) << dump(purged);
        EXPECT_TRUE(purged.contains(QStringLiteral("note")));
        reader.exec(QStringLiteral("COMMIT"));
    }

    // The whole topic: row and history go, and its place under the cap is free.
    {
        Fixture f;
        ASSERT_TRUE(isOk(f.add(QStringLiteral("t"), secretBody)));
        ASSERT_TRUE(isOk(f.update(QStringLiteral("t"), QStringLiteral("two"))));
        ASSERT_TRUE(isOk(f.add(QStringLiteral("keep"), QStringLiteral("keep this"))));
        const QJsonObject purged = f.purge(QStringLiteral("t"), 2);
        ASSERT_TRUE(isOk(purged)) << dump(purged);
        EXPECT_EQ(purged.value(QStringLiteral("rows_erased")).toInt(), 2);
        EXPECT_TRUE(purged.value(QStringLiteral("revision")).isNull()) << dump(purged);
        EXPECT_EQ(code(f.get(QStringLiteral("t"))), QStringLiteral("not_found"));
        EXPECT_EQ(bodyOf(f.get(QStringLiteral("keep"))), QStringLiteral("keep this"));
        EXPECT_FALSE(f.onDisk(marker));
        EXPECT_TRUE(isOk(f.add(QStringLiteral("t"), QStringLiteral("fresh"))));
        {
            Peer peer(f.path);
            EXPECT_EQ(peer.count(QStringLiteral("SELECT count(*) FROM know_how_history")), 0);
        }
    }

    // What purge refuses changes nothing, and the log holds no text of the entry.
    {
        Fixture f;
        ASSERT_TRUE(isOk(f.add(QStringLiteral("t"), QStringLiteral("old body"), QStringLiteral("old title"))));
        ASSERT_TRUE(isOk(f.update(QStringLiteral("t"), QStringLiteral("new body"))));
        const auto purgeWith = [&](const QJsonObject &extra) {
            QJsonObject args{{QStringLiteral("topic"), QStringLiteral("t")}};
            for (auto it = extra.begin(); it != extra.end(); ++it)
                args[it.key()] = it.value();
            return f.call("purge", args);
        };
        const QString why = QStringLiteral("why");
        EXPECT_EQ(code(purgeWith({{QStringLiteral("reason"), why}})), QStringLiteral("missing_field"));
        EXPECT_EQ(code(purgeWith({{QStringLiteral("expected_revision"), 2}})), QStringLiteral("missing_field"));
        EXPECT_EQ(code(purgeWith({{QStringLiteral("expected_revision"), 1}, {QStringLiteral("reason"), why}})),
                  QStringLiteral("conflict"));
        const QJsonObject current = purgeWith({{QStringLiteral("expected_revision"), 2},
                                               {QStringLiteral("revision"), 2},
                                               {QStringLiteral("reason"), why}});
        EXPECT_EQ(code(current), QStringLiteral("bad_args")) << dump(current);
        EXPECT_TRUE(current.value(QStringLiteral("error")).toString().contains(QStringLiteral("update")));
        EXPECT_EQ(code(purgeWith({{QStringLiteral("expected_revision"), 2},
                                  {QStringLiteral("revision"), 9},
                                  {QStringLiteral("reason"), why}})),
                  QStringLiteral("not_found"));
        EXPECT_EQ(code(purgeWith({{QStringLiteral("expected_revision"), 2},
                                  {QStringLiteral("reason"), QString(KnowHowStore::kMaxReasonBytes + 1, QLatin1Char('r'))}})),
                  QStringLiteral("bad_args"));
        const QString lab = QStringLiteral("gl") + QStringLiteral("pat-") + QString(20, QLatin1Char('a'));
        const QJsonObject secret = purgeWith({{QStringLiteral("expected_revision"), 2},
                                              {QStringLiteral("reason"), lab}});
        EXPECT_EQ(code(secret), QStringLiteral("secret_detected"));
        EXPECT_FALSE(QString::fromStdString(dump(secret)).contains(lab));

        const QJsonObject got = f.get(QStringLiteral("t"), true);
        EXPECT_EQ(bodyOf(got), QStringLiteral("new body"));
        EXPECT_EQ(got.value(QStringLiteral("history")).toArray().size(), 1);

        ASSERT_TRUE(isOk(purgeWith({{QStringLiteral("expected_revision"), 2},
                                    {QStringLiteral("revision"), 1},
                                    {QStringLiteral("reason"), QStringLiteral("a stated reason")}})));
        Peer peer(f.path);
        EXPECT_EQ(peer.count(QStringLiteral("SELECT count(*) FROM know_how_purge_log")), 1);
        EXPECT_EQ(peer.text(QStringLiteral("SELECT reason FROM know_how_purge_log")), QStringLiteral("a stated reason"));
        EXPECT_EQ(peer.text(QStringLiteral("SELECT author FROM know_how_purge_log")), QStringLiteral("alpha"));
        EXPECT_EQ(peer.text(QStringLiteral("SELECT topic || revision FROM know_how_purge_log")), QStringLiteral("t1"));
        // Only the newest 1000 log rows are kept.
        ASSERT_TRUE(peer.exec(QStringLiteral("BEGIN")));
        for (int i = 0; i < KnowHowStore::kPurgeLogKept; ++i)
            ASSERT_TRUE(peer.exec(QStringLiteral("INSERT INTO know_how_purge_log VALUES ('2026-01-01T00:00:00Z', 'old', NULL, 'a', 'r')")));
        ASSERT_TRUE(peer.exec(QStringLiteral("COMMIT")));
        ASSERT_TRUE(isOk(f.purge(QStringLiteral("t"), 2)));
        EXPECT_EQ(peer.count(QStringLiteral("SELECT count(*) FROM know_how_purge_log")), KnowHowStore::kPurgeLogKept);
        EXPECT_EQ(peer.count(QStringLiteral("SELECT count(*) FROM know_how_purge_log WHERE topic = 't' AND revision IS NULL")), 1);
    }

    // The control: with secure_delete OFF, the same sequence leaves the marker in
    // the file, so the raw read above can fail.
    {
        Fixture f;
        ASSERT_TRUE(QDir().mkpath(QFileInfo(f.path).absolutePath()));
        {
            Peer peer(f.path);
            ASSERT_TRUE(peer.exec(QStringLiteral("PRAGMA secure_delete = OFF")));
            ASSERT_TRUE(peer.exec(QStringLiteral("PRAGMA journal_mode = WAL")));
            ASSERT_TRUE(peer.exec(QStringLiteral("CREATE TABLE t (body TEXT)")));
            QSqlQuery insert(peer.db());
            ASSERT_TRUE(insert.prepare(QStringLiteral("INSERT INTO t VALUES (?)")));
            insert.addBindValue(secretBody);
            ASSERT_TRUE(insert.exec());
            insert.finish();
            ASSERT_TRUE(peer.exec(QStringLiteral("DELETE FROM t")));
            ASSERT_TRUE(peer.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)")));
        }
        EXPECT_TRUE(f.onDisk(marker)) << "the control lost the marker, so the clean reads prove nothing";
    }

    // This machine's SQLite is built with SECURE_DELETE, so the bytes stay clean
    // here with the pragma gone. The source check is what sees it go.
    QFile src(QStringLiteral(ANTS_SRC_DIR "/knowhowstore.cpp"));
    ASSERT_TRUE(src.open(QIODevice::ReadOnly));
    EXPECT_TRUE(src.readAll().contains("\"PRAGMA secure_delete = ON\""));
}
