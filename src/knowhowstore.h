// ANTS-6310 — the machine-global know-how store: lasting how-to facts any
// registered project writes once and any session finds.
// Contract: docs/specs/ANTS-6310-know-how-store.md.
//
// Its own file, knowhow.sqlite, and not a table in roadmap.sqlite: a schema
// bump there would lock every older running terminal out of its roadmap (§ 8).
//
// Every op, refusal and reply object lives here, so test_core can drive them
// without RemoteControl. The class holds no connection between calls: each op
// opens the file and closes it again, so the verb has no running state.
#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>

#include <functional>

class QSqlDatabase;

class KnowHowStore {
public:
    static constexpr int kSchemaVersion = 1;
    static constexpr int kStaleAfterDays = 90;
    static constexpr int kHistoryKept = 20;
    static constexpr int kPurgeLogKept = 1000;
    static constexpr int kMaxTopics = 1000;
    static constexpr int kMaxTopicBytes = 64;
    static constexpr int kMaxTitleBytes = 120;
    static constexpr int kMaxBodyBytes = 16 * 1024;
    static constexpr int kMaxTags = 8;
    static constexpr int kMaxTagBytes = 32;
    static constexpr int kMaxReasonBytes = 200;
    static constexpr int kSnippetChars = 120;

    using Clock = std::function<QDateTime()>;

    // `clock` lets a test step time forward; empty means the real clock.
    explicit KnowHowStore(QString path, Clock clock = {});

    static QString defaultPath();
    static bool isOp(const QString &op);
    // A write needs a registered caller, whose slug the handler passes to run().
    static bool isWriteOp(const QString &op);

    // `op` must satisfy isOp(). Returns {ok:true, op, ...} or a refusal.
    QJsonObject run(const QString &op, const QJsonObject &args, const QString &author);

private:
    enum class Access { Read, Write };
    enum class After { Nothing, Checkpoint };
    using Body = std::function<QJsonObject(QSqlDatabase &)>;

    QJsonObject opFind(const QJsonObject &args);
    QJsonObject opGet(const QJsonObject &args);
    QJsonObject opList(const QJsonObject &args);
    QJsonObject opAdd(const QJsonObject &args, const QString &author);
    QJsonObject opUpdate(const QJsonObject &args, const QString &author);
    QJsonObject opRetire(const QJsonObject &args);
    QJsonObject opPurge(const QJsonObject &args, const QString &author);

    // Runs `body` in one transaction. `whenEmpty` answers for a store with no
    // schema yet; an empty object there means create it. Commits only a Write
    // whose body answered ok, and removes a file this call created and did not
    // commit a row to.
    QJsonObject session(Access access, const QJsonObject &whenEmpty, const Body &body,
                        After after = After::Nothing);
    QJsonObject sessionOn(QSqlDatabase &db, Access access, const QJsonObject &whenEmpty,
                          const Body &body, After after);

    QDateTime now() const;
    QString stamp() const;
    int ageDays(const QString &lastVerified) const;

    QString m_path;
    Clock m_clock;
};
