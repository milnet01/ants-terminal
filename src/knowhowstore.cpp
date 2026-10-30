// ANTS-6310 — see knowhowstore.h and docs/specs/ANTS-6310-know-how-store.md.

#include "knowhowstore.h"

#include "roadmapstore.h"
#include "secretredact.h"
#include "secureio.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QStringList>
#include <QThread>
#include <QVariant>

#include <atomic>
#include <cmath>
#include <optional>
#include <utility>

namespace {

constexpr const char *kSchemaDdl[] = {
    "CREATE TABLE know_how ("
    " topic TEXT PRIMARY KEY, title TEXT NOT NULL, body TEXT NOT NULL,"
    " tags TEXT NOT NULL, author TEXT NOT NULL, created TEXT NOT NULL,"
    " last_verified TEXT NOT NULL, revision INTEGER NOT NULL, retired_at TEXT)",
    "CREATE TABLE know_how_history ("
    " topic TEXT NOT NULL REFERENCES know_how(topic), revision INTEGER NOT NULL,"
    " title TEXT NOT NULL, body TEXT NOT NULL, tags TEXT NOT NULL,"
    " author TEXT NOT NULL, replaced_at TEXT NOT NULL,"
    " PRIMARY KEY (topic, revision))",
    "CREATE TABLE know_how_purge_log ("
    " purged_at TEXT NOT NULL, topic TEXT NOT NULL, revision INTEGER,"
    " author TEXT NOT NULL, reason TEXT NOT NULL)",
};

// A checkpoint blocked by a reader answers file_scrubbed:false rather than
// holding the verb's lane for the whole write deadline.
constexpr int kCheckpointBusyMs = 250;

QJsonObject refuse(const char *code, const QString &message) {
    QJsonObject e;
    e[QStringLiteral("ok")] = false;
    e[QStringLiteral("code")] = QString::fromLatin1(code);
    e[QStringLiteral("error")] = QStringLiteral("know_how: ") + message;
    return e;
}

QJsonObject refuseNotFound(const QString &topic) {
    return refuse("not_found", QStringLiteral("no entry for topic \"%1\"").arg(topic));
}

QJsonObject okReply(const QString &op, bool dryRun = false) {
    QJsonObject o{{QStringLiteral("ok"), true}, {QStringLiteral("op"), op}};
    if (dryRun)
        o[QStringLiteral("dry_run")] = true;
    return o;
}

// One QSqlDatabase per call, named uniquely: two threads must not share one.
class Connection {
public:
    explicit Connection(const QString &path) {
        static std::atomic<quint64> counter{0};
        m_name = QStringLiteral("ants_knowhow_%1").arg(counter.fetch_add(1));
        m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_name);
        m_db.setDatabaseName(path);
    }
    ~Connection() {
        m_db.close();
        m_db = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_name);
    }
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;
    QSqlDatabase &db() { return m_db; }

private:
    QString m_name;
    QSqlDatabase m_db;
};

bool bindAndRun(QSqlQuery &q, const QString &sql, const QVariantList &binds = {}) {
    if (!q.prepare(sql))
        return false;
    for (const QVariant &v : binds)
        q.addBindValue(v);
    return q.exec();
}

QJsonObject ioError(const QSqlQuery &q) {
    return refuse("io_error", q.lastError().text());
}

struct Stmt {
    QString sql;
    QVariantList binds;
};

// An empty object when every statement ran.
QJsonObject execAll(QSqlDatabase &db, std::initializer_list<Stmt> stmts) {
    for (const Stmt &s : stmts) {
        QSqlQuery q(db);
        if (!bindAndRun(q, s.sql, s.binds))
            return ioError(q);
    }
    return {};
}

std::optional<qint64> scalar(QSqlDatabase &db, const QString &sql,
                             const QVariantList &binds, QJsonObject *refusal) {
    QSqlQuery q(db);
    if (!bindAndRun(q, sql, binds) || !q.next()) {
        *refusal = ioError(q);
        return std::nullopt;
    }
    return q.value(0).toLongLong();
}

// Same reason as RoadmapStore::enableWal: the switch to WAL takes its lock below
// the busy handler, so two sessions creating the store at once need this retry.
bool enableWal(QSqlDatabase &db, QString *error) {
    QElapsedTimer clock;
    clock.start();
    for (;;) {
        QSqlQuery q(db);
        if (q.exec(QStringLiteral("PRAGMA journal_mode = WAL")) && q.next()
            && q.value(0).toString().compare(QLatin1String("wal"), Qt::CaseInsensitive) == 0)
            return true;
        *error = q.lastError().text();
        if (clock.elapsed() >= RoadmapStore::kBusyTimeoutMs)
            return false;
        QThread::msleep(10);
    }
}

QStringList storeFiles(const QString &path) {
    return {path, path + QStringLiteral("-wal"), path + QStringLiteral("-shm")};
}

// The store's files that exist and could not be made owner-only.
QStringList unsecuredFiles(const QString &path) {
    QStringList bad;
    for (const QString &f : storeFiles(path)) {
        if (QFileInfo::exists(f) && !setOwnerOnlyPerms(f))
            bad << f;
    }
    return bad;
}

bool isSlug(const QString &s) {
    if (s.isEmpty())
        return false;
    for (const QChar c : s) {
        const char16_t u = c.unicode();
        if (!((u >= u'a' && u <= u'z') || (u >= u'0' && u <= u'9') || u == u'-'))
            return false;
    }
    return true;
}

int bytesOf(const QString &s) { return int(s.toUtf8().size()); }

// ---------------------------------------------------------------- arguments --

// Refuses before anything else, so the secret never reaches a later message.
// The refusal names the count and never the matched text.
QJsonObject secretRefusal(const QJsonObject &args, const QStringList &keys) {
    int found = 0;
    for (const QString &key : keys) {
        const QJsonValue v = args.value(key);
        if (v.isString())
            found += SecretRedact::scrub(v.toString()).redactedCount;
        for (const QJsonValue item : v.toArray())
            found += SecretRedact::scrub(item.toString()).redactedCount;
    }
    if (found == 0)
        return {};
    return refuse("secret_detected",
                  QStringLiteral("%1 value(s) look like a secret, so nothing was written. "
                                 "An entry says how to authenticate, never the secret.")
                      .arg(found));
}

// Never echoes the value: a bad topic may be a secret of an unknown shape.
QJsonObject topicArg(const QJsonObject &args, QString *topic) {
    const QJsonValue v = args.value(QStringLiteral("topic"));
    if (v.isUndefined())
        return refuse("missing_field", QStringLiteral("`topic` is required"));
    *topic = v.toString();
    if (!isSlug(*topic) || bytesOf(*topic) > KnowHowStore::kMaxTopicBytes)
        return refuse("bad_args", QStringLiteral("`topic` must match ^[a-z0-9-]+$, at most "
                                                 "%1 bytes").arg(KnowHowStore::kMaxTopicBytes));
    return {};
}

// Range-checked before the cast, as session_message's message_id is.
QJsonObject revisionArg(const QJsonObject &args, const QString &key,
                        std::optional<qint64> *out) {
    const QJsonValue v = args.value(key);
    if (v.isUndefined())
        return {};
    const double raw = v.toDouble(-1);
    if (!(raw >= 1.0 && raw <= 9007199254740992.0) || raw != std::floor(raw))
        return refuse("bad_args", QStringLiteral("`%1` must be a positive integer").arg(key));
    *out = qint64(raw);
    return {};
}

struct Fields {
    std::optional<QString> title, body, tags;  // tags in stored form
    bool any() const { return title || body || tags; }
};

QString encodeTags(const QStringList &tags) {
    return QLatin1Char(' ') + tags.join(QLatin1Char(' ')) + QLatin1Char(' ');
}

QJsonArray decodeTags(const QString &stored) {
    return QJsonArray::fromStringList(stored.split(QLatin1Char(' '), Qt::SkipEmptyParts));
}

QJsonObject titleArg(const QJsonValue &v, Fields *f) {
    const QString title = v.toString();
    if (title.isEmpty() || bytesOf(title) > KnowHowStore::kMaxTitleBytes
        || title.contains(QLatin1Char('\n')) || title.contains(QLatin1Char('\r')))
        return refuse("bad_args", QStringLiteral("`title` must be non-empty, at most %1 bytes, "
                                                 "one line").arg(KnowHowStore::kMaxTitleBytes));
    f->title = title;
    return {};
}

QJsonObject bodyArg(const QJsonValue &v, Fields *f) {
    const QString body = v.toString();
    if (body.isEmpty() || bytesOf(body) > KnowHowStore::kMaxBodyBytes)
        return refuse("bad_args", QStringLiteral("`body` must be non-empty, at most %1 bytes")
                                      .arg(KnowHowStore::kMaxBodyBytes));
    f->body = body;
    return {};
}

QJsonObject tagsArg(const QJsonValue &v, Fields *f) {
    const QJsonArray arr = v.toArray();
    const QJsonObject bad = refuse(
        "bad_args", QStringLiteral("`tags` must be an array of at most %1, each matching "
                                   "^[a-z0-9-]+$, at most %2 bytes")
                        .arg(KnowHowStore::kMaxTags).arg(KnowHowStore::kMaxTagBytes));
    if (!v.isArray() || arr.size() > KnowHowStore::kMaxTags)
        return bad;
    QStringList tags;
    for (const QJsonValue item : arr) {
        const QString tag = item.toString();
        if (!isSlug(tag) || bytesOf(tag) > KnowHowStore::kMaxTagBytes)
            return bad;
        tags << tag;
    }
    f->tags = encodeTags(tags);
    return {};
}

// Reads whichever of title, body and tags are present.
QJsonObject fieldsArg(const QJsonObject &args, Fields *f) {
    const auto read = [&](const char *key, QJsonObject (*parse)(const QJsonValue &, Fields *)) {
        const QJsonValue v = args.value(QString::fromLatin1(key));
        return v.isUndefined() ? QJsonObject() : parse(v, f);
    };
    QJsonObject r = read("title", titleArg);
    if (r.isEmpty())
        r = read("body", bodyArg);
    if (r.isEmpty())
        r = read("tags", tagsArg);
    return r;
}

int limitArg(const QJsonObject &args, int fallback, int max) {
    const int limit = args.value(QStringLiteral("limit")).toInt(fallback);
    return limit < 1 ? fallback : qMin(limit, max);
}

// ----------------------------------------------------------------- find/list --

// `%`, `_` and the escape character are literal in a query, and a `%` query must
// not match everything.
QString likePattern(const QString &needle) {
    QString escaped = needle;
    escaped.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
    escaped.replace(QLatin1Char('%'), QLatin1String("\\%"));
    escaped.replace(QLatin1Char('_'), QLatin1String("\\_"));
    return QLatin1Char('%') + escaped + QLatin1Char('%');
}

QStringList queryTokens(const QString &query) {
    QStringList kept;
    const QStringList words = query.split(QRegularExpression(QStringLiteral("\\s+")),
                                          Qt::SkipEmptyParts);
    for (const QString &w : words.mid(0, 8)) {
        if (w.size() >= 2)
            kept << w;
    }
    return kept;
}

QString clipSnippet(const QString &line) {
    QString s = line.trimmed().left(KnowHowStore::kSnippetChars);
    if (!s.isEmpty() && s.back().isHighSurrogate())
        s.chop(1);
    return s;
}

// The first body line holding the entry's first matched token, or else the first
// non-blank line.
QString snippetFor(const QString &body, const QStringList &tokens, const QString &heading) {
    const QStringList lines = body.split(QLatin1Char('\n'));
    for (const QString &token : tokens) {
        if (!heading.contains(token, Qt::CaseInsensitive)
            && !body.contains(token, Qt::CaseInsensitive))
            continue;
        for (const QString &line : lines) {
            if (line.contains(token, Qt::CaseInsensitive))
                return clipSnippet(line);
        }
        break;
    }
    for (const QString &line : lines) {
        if (!line.trimmed().isEmpty())
            return clipSnippet(line);
    }
    return {};
}

constexpr const char *kActive = "retired_at IS NULL";
constexpr const char *kHasTag = " AND tags LIKE ? ESCAPE '\\'";

// Its SQL sum over the tokens: 3 for a match in topic, title or tags, else 1 for
// a match in body.
QString scoreSql(int tokens) {
    QStringList terms;
    for (int i = 0; i < tokens; ++i)
        terms << QStringLiteral("CASE WHEN topic LIKE ? ESCAPE '\\' OR title LIKE ? ESCAPE '\\'"
                                " OR tags LIKE ? ESCAPE '\\' THEN 3"
                                " WHEN body LIKE ? ESCAPE '\\' THEN 1 ELSE 0 END");
    return terms.isEmpty() ? QStringLiteral("0") : terms.join(QLatin1String(" + "));
}

// ----------------------------------------------------------------- one entry --

struct Row {
    QString title, body, tags, author, created, lastVerified, retiredAt;
    qint64 revision = 0;
};

// An empty object when `row` was filled.
QJsonObject loadRow(QSqlDatabase &db, const QString &topic, Row *row) {
    QSqlQuery q(db);
    if (!bindAndRun(q, QStringLiteral("SELECT title, body, tags, author, created, last_verified,"
                                      " revision, retired_at FROM know_how WHERE topic = ?"),
                    {topic}))
        return ioError(q);
    if (!q.next())
        return refuseNotFound(topic);
    *row = Row{q.value(0).toString(), q.value(1).toString(), q.value(2).toString(),
               q.value(3).toString(), q.value(4).toString(), q.value(5).toString(),
               q.value(7).toString(), q.value(6).toLongLong()};
    return {};
}

QJsonObject conflictRefusal(const std::optional<qint64> &expected, qint64 current) {
    if (!expected || *expected == current)
        return {};
    return refuse("conflict",
                  QStringLiteral("the entry is at revision %1, not %2: it changed since it was "
                                 "read, so nothing was changed. Read it again.")
                      .arg(current).arg(*expected));
}

QJsonObject historyRows(QSqlDatabase &db, const QString &topic, QJsonArray *out) {
    QSqlQuery q(db);
    if (!bindAndRun(q, QStringLiteral("SELECT revision, title, body, tags, author, replaced_at"
                                      " FROM know_how_history WHERE topic = ?"
                                      " ORDER BY revision DESC"),
                    {topic}))
        return ioError(q);
    while (q.next()) {
        out->append(QJsonObject{{QStringLiteral("revision"), q.value(0).toLongLong()},
                                {QStringLiteral("title"), q.value(1).toString()},
                                {QStringLiteral("body"), q.value(2).toString()},
                                {QStringLiteral("tags"), decodeTags(q.value(3).toString())},
                                {QStringLiteral("author"), q.value(4).toString()},
                                {QStringLiteral("replaced_at"), q.value(5).toString()}});
    }
    return {};
}

}  // namespace

// ------------------------------------------------------------------- the class --

KnowHowStore::KnowHowStore(QString path, Clock clock)
    : m_path(std::move(path)), m_clock(std::move(clock)) {}

QString KnowHowStore::defaultPath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
           + QStringLiteral("/ants-terminal/knowhow.sqlite");
}

bool KnowHowStore::isWriteOp(const QString &op) {
    return op == QLatin1String("add") || op == QLatin1String("update")
           || op == QLatin1String("retire") || op == QLatin1String("purge");
}

bool KnowHowStore::isOp(const QString &op) {
    return op == QLatin1String("find") || op == QLatin1String("get")
           || op == QLatin1String("list") || isWriteOp(op);
}

QDateTime KnowHowStore::now() const {
    return m_clock ? m_clock() : QDateTime::currentDateTimeUtc();
}

QString KnowHowStore::stamp() const {
    return now().toUTC().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ssZ"));
}

int KnowHowStore::ageDays(const QString &lastVerified) const {
    const qint64 secs = QDateTime::fromString(lastVerified, Qt::ISODate).secsTo(now());
    return int(qMax<qint64>(0, secs) / 86400);
}

QJsonObject KnowHowStore::run(const QString &op, const QJsonObject &args,
                              const QString &author) {
    if (op == QLatin1String("find"))
        return opFind(args);
    if (op == QLatin1String("get"))
        return opGet(args);
    if (op == QLatin1String("list"))
        return opList(args);
    if (op == QLatin1String("add"))
        return opAdd(args, author);
    if (op == QLatin1String("update"))
        return opUpdate(args, author);
    if (op == QLatin1String("retire"))
        return opRetire(args);
    return opPurge(args, author);
}

QJsonObject KnowHowStore::session(Access access, const QJsonObject &whenEmpty, const Body &body,
                                  After after) {
    bool created = false;
    if (!QFileInfo::exists(m_path)) {
        if (!whenEmpty.isEmpty())
            return whenEmpty;
        // § 4.1 — secured empty, before SQLite writes to it. NewOnly loses a race
        // to another creator harmlessly: that file is then simply opened.
        if (!QDir().mkpath(QFileInfo(m_path).absolutePath()))
            return refuse("io_error", QStringLiteral("cannot create the folder of %1").arg(m_path));
        QFile fresh(m_path);
        if (fresh.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            created = true;
            if (!setOwnerOnlyPerms(fresh)) {
                fresh.close();
                QFile::remove(m_path);
                return refuse("io_error", QStringLiteral("cannot make %1 owner-only").arg(m_path));
            }
        }
    }
    QJsonObject reply;
    {
        Connection c(m_path);
        reply = sessionOn(c.db(), access, whenEmpty, body, after);
    }
    if (created && !reply.value(QStringLiteral("ok")).toBool()) {
        for (const QString &f : storeFiles(m_path))
            QFile::remove(f);
    }
    return reply;
}

QJsonObject KnowHowStore::sessionOn(QSqlDatabase &db, Access access, const QJsonObject &whenEmpty,
                                    const Body &body, After after) {
    const auto ioFailure = [this](const QString &why) {
        return refuse("io_error", QStringLiteral("%1: %2").arg(m_path, why));
    };
    if (!db.open())
        return ioFailure(db.lastError().text());
    // The order matters: a newer file must be left exactly as it was, so nothing
    // below this version check runs on one. secure_delete is per connection, and
    // update moves text that purge may later have to erase (§ 4.1).
    if (const QJsonObject r = execAll(
            db, {{QStringLiteral("PRAGMA busy_timeout = %1").arg(RoadmapStore::kBusyTimeoutMs), {}},
                 {QStringLiteral("PRAGMA secure_delete = ON"), {}},
                 {QStringLiteral("PRAGMA foreign_keys = ON"), {}},
                 {QStringLiteral("PRAGMA synchronous = FULL"), {}}});
        !r.isEmpty())
        return ioFailure(r.value(QStringLiteral("error")).toString());

    QJsonObject failed;
    const auto version = scalar(db, QStringLiteral("PRAGMA user_version"), {}, &failed);
    if (!version)
        return ioFailure(failed.value(QStringLiteral("error")).toString());
    if (*version > kSchemaVersion)
        return ioFailure(QStringLiteral("schema version %1 is newer than this build's %2")
                             .arg(*version).arg(kSchemaVersion));
    const bool needsSchema = *version == 0;
    if (needsSchema && !whenEmpty.isEmpty())
        return whenEmpty;

    if (needsSchema) {
        QString why;
        if (!enableWal(db, &why))
            return ioFailure(why);
    }
    const QStringList unsecured = unsecuredFiles(m_path);
    if (access == Access::Write && !unsecured.isEmpty())
        return ioFailure(QStringLiteral("cannot make %1 owner-only").arg(unsecured.join(u' ')));
    for (const QString &f : unsecured)
        warnNotOwnerOnly(f, "know-how store");

    const QString begin = access == Access::Write ? QStringLiteral("BEGIN IMMEDIATE")
                                                  : QStringLiteral("BEGIN");
    if (const QJsonObject r = execAll(db, {{begin, {}}}); !r.isEmpty())
        return ioFailure(r.value(QStringLiteral("error")).toString());
    const auto abandon = [&](const QJsonObject &answer) {
        execAll(db, {{QStringLiteral("ROLLBACK"), {}}});
        return answer;
    };

    if (needsSchema) {
        // Decided inside the write lock: two creators both found no schema above.
        const auto seen = scalar(db, QStringLiteral("PRAGMA user_version"), {}, &failed);
        if (!seen)
            return abandon(failed);
        if (*seen == 0) {
            for (const char *ddl : kSchemaDdl) {
                if (const QJsonObject r = execAll(db, {{QString::fromLatin1(ddl), {}}}); !r.isEmpty())
                    return abandon(r);
            }
            if (const QJsonObject r = execAll(
                    db, {{QStringLiteral("PRAGMA user_version = %1").arg(kSchemaVersion), {}}});
                !r.isEmpty())
                return abandon(r);
        }
    }

    QJsonObject reply = body(db);
    if (access == Access::Read || !reply.value(QStringLiteral("ok")).toBool())
        return abandon(reply);
    if (const QJsonObject r = execAll(db, {{QStringLiteral("COMMIT"), {}}}); !r.isEmpty())
        return abandon(r);

    if (after == After::Checkpoint) {
        // Text the purge freed is still in the WAL until this truncates it.
        execAll(db, {{QStringLiteral("PRAGMA busy_timeout = %1").arg(kCheckpointBusyMs), {}}});
        QSqlQuery q(db);
        const bool scrubbed = q.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"))
                              && q.next() && q.value(0).toInt() == 0;
        reply[QStringLiteral("file_scrubbed")] = scrubbed;
        if (!scrubbed)
            reply[QStringLiteral("note")] = QStringLiteral(
                "Another connection is inside a transaction on the file, so the text stays on "
                "disk until the last connection closes.");
    }
    return reply;
}

QJsonObject KnowHowStore::opFind(const QJsonObject &args) {
    if (!args.contains(QStringLiteral("query")) && !args.contains(QStringLiteral("tag")))
        return refuse("missing_field", QStringLiteral("op:\"find\" needs `query` or `tag`"));
    const bool hasQuery = args.contains(QStringLiteral("query"));
    const QStringList tokens = queryTokens(args.value(QStringLiteral("query")).toString());
    const QString tag = args.value(QStringLiteral("tag")).toString();
    const int limit = limitArg(args, 5, 20);

    QJsonObject empty = okReply(QStringLiteral("find"));
    empty[QStringLiteral("results")] = QJsonArray();
    empty[QStringLiteral("count")] = 0;
    empty[QStringLiteral("searched")] = 0;

    return session(Access::Read, empty, [&, empty](QSqlDatabase &db) -> QJsonObject {
        QJsonObject failed;
        const auto searched = scalar(
            db, QStringLiteral("SELECT count(*) FROM know_how WHERE %1").arg(QLatin1String(kActive)),
            {}, &failed);
        if (!searched)
            return failed;
        QJsonObject reply = empty;
        reply[QStringLiteral("searched")] = *searched;
        if (hasQuery && tokens.isEmpty())
            return reply;

        QVariantList binds;
        for (const QString &t : tokens) {
            const QString p = likePattern(t);
            binds << p << p << p << p;
        }
        QString where = QLatin1String(kActive);
        if (!tag.isEmpty()) {
            where += QLatin1String(kHasTag);
            binds << likePattern(QLatin1Char(' ') + tag + QLatin1Char(' '));
        }
        binds << limit;
        const QString sql = QStringLiteral(
            "SELECT topic, title, body, tags, last_verified, score FROM ("
            " SELECT *, (%1) AS score FROM know_how WHERE %2)%3"
            " ORDER BY score DESC, last_verified DESC, topic LIMIT ?")
            .arg(scoreSql(int(tokens.size())), where,
                 hasQuery ? QStringLiteral(" WHERE score > 0") : QString());
        QSqlQuery q(db);
        if (!bindAndRun(q, sql, binds))
            return ioError(q);
        QJsonArray results;
        while (q.next()) {
            const QString topic = q.value(0).toString();
            const QString title = q.value(1).toString();
            const QString heading = topic + QLatin1Char(' ') + title + q.value(3).toString();
            const int age = ageDays(q.value(4).toString());
            results.append(QJsonObject{
                {QStringLiteral("topic"), topic},
                {QStringLiteral("title"), title},
                {QStringLiteral("snippet"), snippetFor(q.value(2).toString(), tokens, heading)},
                {QStringLiteral("age_days"), age},
                {QStringLiteral("stale"), age > kStaleAfterDays}});
        }
        reply[QStringLiteral("results")] = results;
        reply[QStringLiteral("count")] = results.size();
        return reply;
    });
}

QJsonObject KnowHowStore::opGet(const QJsonObject &args) {
    QString topic;
    if (const QJsonObject r = topicArg(args, &topic); !r.isEmpty())
        return r;
    const bool withHistory = args.value(QStringLiteral("history")).toBool(false);
    return session(Access::Read, refuseNotFound(topic), [&](QSqlDatabase &db) -> QJsonObject {
        Row row;
        if (const QJsonObject r = loadRow(db, topic, &row); !r.isEmpty())
            return r;
        const int age = ageDays(row.lastVerified);
        QJsonObject reply = okReply(QStringLiteral("get"));
        reply[QStringLiteral("topic")] = topic;
        reply[QStringLiteral("title")] = row.title;
        reply[QStringLiteral("body")] = row.body;
        reply[QStringLiteral("tags")] = decodeTags(row.tags);
        reply[QStringLiteral("author")] = row.author;
        reply[QStringLiteral("created")] = row.created;
        reply[QStringLiteral("last_verified")] = row.lastVerified;
        reply[QStringLiteral("revision")] = row.revision;
        reply[QStringLiteral("age_days")] = age;
        reply[QStringLiteral("stale")] = age > kStaleAfterDays;
        reply[QStringLiteral("retired")] = !row.retiredAt.isEmpty();
        if (withHistory) {
            QJsonArray rows;
            if (const QJsonObject r = historyRows(db, topic, &rows); !r.isEmpty())
                return r;
            reply[QStringLiteral("history")] = rows;
        }
        return reply;
    });
}

QJsonObject KnowHowStore::opList(const QJsonObject &args) {
    const QString tag = args.value(QStringLiteral("tag")).toString();
    const int limit = limitArg(args, 50, 200);
    const int offset = qMax(0, args.value(QStringLiteral("offset")).toInt(0));

    QJsonObject empty = okReply(QStringLiteral("list"));
    empty[QStringLiteral("topics")] = QJsonArray();
    empty[QStringLiteral("total")] = 0;

    return session(Access::Read, empty, [&, empty](QSqlDatabase &db) -> QJsonObject {
        QString where = QLatin1String(kActive);
        QVariantList binds;
        if (!tag.isEmpty()) {
            where += QLatin1String(kHasTag);
            binds << likePattern(QLatin1Char(' ') + tag + QLatin1Char(' '));
        }
        QJsonObject failed;
        const auto total =
            scalar(db, QStringLiteral("SELECT count(*) FROM know_how WHERE %1").arg(where), binds, &failed);
        if (!total)
            return failed;
        QSqlQuery q(db);
        if (!bindAndRun(q, QStringLiteral("SELECT topic, title, last_verified FROM know_how"
                                          " WHERE %1 ORDER BY topic LIMIT ? OFFSET ?").arg(where),
                        binds + QVariantList{limit, offset}))
            return ioError(q);
        QJsonArray topics;
        while (q.next())
            topics.append(QJsonObject{
                {QStringLiteral("topic"), q.value(0).toString()},
                {QStringLiteral("title"), q.value(1).toString()},
                {QStringLiteral("stale"), ageDays(q.value(2).toString()) > kStaleAfterDays}});
        QJsonObject reply = empty;
        reply[QStringLiteral("topics")] = topics;
        reply[QStringLiteral("total")] = *total;
        if (offset + topics.size() < *total)
            reply[QStringLiteral("next_offset")] = offset + topics.size();
        return reply;
    });
}

QJsonObject KnowHowStore::opAdd(const QJsonObject &args, const QString &author) {
    if (const QJsonObject r = secretRefusal(args, {QStringLiteral("topic"), QStringLiteral("title"),
                                                   QStringLiteral("body"), QStringLiteral("tags")});
        !r.isEmpty())
        return r;
    QString topic;
    if (const QJsonObject r = topicArg(args, &topic); !r.isEmpty())
        return r;
    Fields f;
    if (const QJsonObject r = fieldsArg(args, &f); !r.isEmpty())
        return r;
    if (!f.title || !f.body)
        return refuse("missing_field", QStringLiteral("op:\"add\" needs `title` and `body`"));
    const bool dryRun = args.value(QStringLiteral("dry_run")).toBool(false);

    QJsonObject reply = okReply(QStringLiteral("add"), dryRun);
    reply[QStringLiteral("topic")] = topic;
    reply[QStringLiteral("revision")] = 1;

    return session(dryRun ? Access::Read : Access::Write, dryRun ? reply : QJsonObject(),
                   [&, reply](QSqlDatabase &db) -> QJsonObject {
        QJsonObject failed;
        const auto taken = scalar(db, QStringLiteral("SELECT count(*) FROM know_how WHERE topic = ?"),
                                  {topic}, &failed);
        if (!taken)
            return failed;
        if (*taken > 0)
            return refuse("topic_exists",
                          QStringLiteral("topic \"%1\" exists, perhaps retired: use op:\"update\"")
                              .arg(topic));
        const auto total = scalar(db, QStringLiteral("SELECT count(*) FROM know_how"), {}, &failed);
        if (!total)
            return failed;
        if (*total >= kMaxTopics)
            return refuse("store_full", QStringLiteral("the store holds its limit of %1 topics, "
                                                       "retired ones included").arg(kMaxTopics));
        if (dryRun)
            return reply;
        const QString t = stamp();
        const QJsonObject r = execAll(
            db, {{QStringLiteral("INSERT INTO know_how (topic, title, body, tags, author, created,"
                                 " last_verified, revision) VALUES (?, ?, ?, ?, ?, ?, ?, 1)"),
                  {topic, *f.title, *f.body, f.tags.value_or(encodeTags({})), author, t, t}}});
        return r.isEmpty() ? reply : r;
    });
}

QJsonObject KnowHowStore::opUpdate(const QJsonObject &args, const QString &author) {
    if (const QJsonObject r = secretRefusal(args, {QStringLiteral("topic"), QStringLiteral("title"),
                                                   QStringLiteral("body"), QStringLiteral("tags")});
        !r.isEmpty())
        return r;
    QString topic;
    if (const QJsonObject r = topicArg(args, &topic); !r.isEmpty())
        return r;
    Fields f;
    if (const QJsonObject r = fieldsArg(args, &f); !r.isEmpty())
        return r;
    std::optional<qint64> expected;
    if (const QJsonObject r = revisionArg(args, QStringLiteral("expected_revision"), &expected);
        !r.isEmpty())
        return r;
    const bool dryRun = args.value(QStringLiteral("dry_run")).toBool(false);

    return session(dryRun ? Access::Read : Access::Write, refuseNotFound(topic),
                   [&](QSqlDatabase &db) -> QJsonObject {
        Row row;
        if (const QJsonObject r = loadRow(db, topic, &row); !r.isEmpty())
            return r;
        if (const QJsonObject r = conflictRefusal(expected, row.revision); !r.isEmpty())
            return r;
        // A value equal to the current one changes nothing, so it keeps no history.
        const bool changed = (f.title && *f.title != row.title) || (f.body && *f.body != row.body)
                             || (f.tags && *f.tags != row.tags);
        const QString t = stamp();
        QJsonObject reply = okReply(QStringLiteral("update"), dryRun);
        reply[QStringLiteral("topic")] = topic;
        reply[QStringLiteral("revision")] = row.revision + (changed ? 1 : 0);
        reply[QStringLiteral("last_verified")] = t;
        if (dryRun)
            return reply;
        const QJsonObject r = !changed
            ? execAll(db, {{QStringLiteral("UPDATE know_how SET last_verified = ?, retired_at = NULL"
                                           " WHERE topic = ?"), {t, topic}}})
            : execAll(db, {
                {QStringLiteral("INSERT INTO know_how_history (topic, revision, title, body, tags,"
                                " author, replaced_at) VALUES (?, ?, ?, ?, ?, ?, ?)"),
                 {topic, row.revision, row.title, row.body, row.tags, row.author, t}},
                {QStringLiteral("UPDATE know_how SET title = ?, body = ?, tags = ?, author = ?,"
                                " last_verified = ?, revision = ?, retired_at = NULL WHERE topic = ?"),
                 {f.title.value_or(row.title), f.body.value_or(row.body), f.tags.value_or(row.tags),
                  author, t, row.revision + 1, topic}},
                {QStringLiteral("DELETE FROM know_how_history WHERE topic = ? AND revision NOT IN"
                                " (SELECT revision FROM know_how_history WHERE topic = ?"
                                " ORDER BY revision DESC LIMIT %1)").arg(kHistoryKept),
                 {topic, topic}}});
        return r.isEmpty() ? reply : r;
    });
}

QJsonObject KnowHowStore::opRetire(const QJsonObject &args) {
    QString topic;
    if (const QJsonObject r = topicArg(args, &topic); !r.isEmpty())
        return r;
    const bool dryRun = args.value(QStringLiteral("dry_run")).toBool(false);

    return session(dryRun ? Access::Read : Access::Write, refuseNotFound(topic),
                   [&](QSqlDatabase &db) -> QJsonObject {
        Row row;
        if (const QJsonObject r = loadRow(db, topic, &row); !r.isEmpty())
            return r;
        QJsonObject reply = okReply(QStringLiteral("retire"), dryRun);
        reply[QStringLiteral("topic")] = topic;
        // Retiring a retired entry changes nothing, so it moves no revision.
        if (!row.retiredAt.isEmpty()) {
            reply[QStringLiteral("retired_at")] = row.retiredAt;
            return reply;
        }
        const QString t = stamp();
        reply[QStringLiteral("retired_at")] = t;
        if (dryRun)
            return reply;
        const QJsonObject r = execAll(
            db, {{QStringLiteral("UPDATE know_how SET retired_at = ?, revision = revision + 1"
                                 " WHERE topic = ?"), {t, topic}}});
        return r.isEmpty() ? reply : r;
    });
}

QJsonObject KnowHowStore::opPurge(const QJsonObject &args, const QString &author) {
    if (const QJsonObject r = secretRefusal(args, {QStringLiteral("reason")}); !r.isEmpty())
        return r;
    QString topic;
    if (const QJsonObject r = topicArg(args, &topic); !r.isEmpty())
        return r;
    std::optional<qint64> expected, revision;
    if (const QJsonObject r = revisionArg(args, QStringLiteral("expected_revision"), &expected);
        !r.isEmpty())
        return r;
    if (const QJsonObject r = revisionArg(args, QStringLiteral("revision"), &revision); !r.isEmpty())
        return r;
    const QString reason = args.value(QStringLiteral("reason")).toString();
    if (!expected || reason.isEmpty())
        return refuse("missing_field",
                      QStringLiteral("op:\"purge\" needs `expected_revision` and `reason`"));
    if (bytesOf(reason) > kMaxReasonBytes)
        return refuse("bad_args", QStringLiteral("`reason` is at most %1 bytes").arg(kMaxReasonBytes));
    const bool dryRun = args.value(QStringLiteral("dry_run")).toBool(false);

    const Body body = [&](QSqlDatabase &db) -> QJsonObject {
        Row row;
        if (const QJsonObject r = loadRow(db, topic, &row); !r.isEmpty())
            return r;
        if (const QJsonObject r = conflictRefusal(expected, row.revision); !r.isEmpty())
            return r;
        if (revision && *revision == row.revision)
            return refuse("bad_args", QStringLiteral("revision %1 is the current one: op:\"update\" "
                                                     "the text first, then purge that version")
                                          .arg(*revision));
        QJsonObject failed;
        const auto oldRows = scalar(
            db, revision ? QStringLiteral("SELECT count(*) FROM know_how_history"
                                          " WHERE topic = ? AND revision = ?")
                         : QStringLiteral("SELECT count(*) FROM know_how_history WHERE topic = ?"),
            revision ? QVariantList{topic, *revision} : QVariantList{topic}, &failed);
        if (!oldRows)
            return failed;
        if (revision && *oldRows == 0)
            return refuse("not_found", QStringLiteral("topic \"%1\" has no history row at revision "
                                                      "%2").arg(topic).arg(*revision));
        QJsonObject reply = okReply(QStringLiteral("purge"), dryRun);
        reply[QStringLiteral("topic")] = topic;
        reply[QStringLiteral("revision")] = revision ? QJsonValue(*revision) : QJsonValue();
        reply[QStringLiteral("rows_erased")] = *oldRows + (revision ? 0 : 1);
        if (dryRun)
            return reply;
        const QVariant logged = revision ? QVariant(*revision) : QVariant();
        const QJsonObject erased = revision
            ? execAll(db, {{QStringLiteral("DELETE FROM know_how_history WHERE topic = ? AND revision = ?"),
                            {topic, *revision}}})
            : execAll(db, {{QStringLiteral("DELETE FROM know_how_history WHERE topic = ?"), {topic}},
                           {QStringLiteral("DELETE FROM know_how WHERE topic = ?"), {topic}}});
        if (!erased.isEmpty())
            return erased;
        const QJsonObject r = execAll(
            db, {{QStringLiteral("INSERT INTO know_how_purge_log (purged_at, topic, revision, author,"
                                 " reason) VALUES (?, ?, ?, ?, ?)"),
                  {stamp(), topic, logged, author, reason}},
                 {QStringLiteral("DELETE FROM know_how_purge_log WHERE rowid NOT IN"
                                 " (SELECT rowid FROM know_how_purge_log ORDER BY rowid DESC LIMIT %1)")
                      .arg(kPurgeLogKept), {}}});
        return r.isEmpty() ? reply : r;
    };
    return session(dryRun ? Access::Read : Access::Write, refuseNotFound(topic), body,
                   dryRun ? After::Nothing : After::Checkpoint);
}
