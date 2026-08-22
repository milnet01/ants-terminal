// Feature-conformance test for ANTS-4622 — the cross-session mailbox.
// Contract: tests/features/session_message/spec.md, which points at
// docs/specs/ANTS-4622-cross-session-mailbox.md.
//
// Every invariant here drives RoadmapStore directly against a store inside a
// QTemporaryDir. NEVER default-construct RoadmapStore in a test: the default
// path resolves to RoadmapStore::defaultPath() under XDG_DATA_HOME, which is
// the developer's REAL machine-global store.
//
// Timestamps are passed in rather than read from the clock, which is the
// store's existing convention (appendHistory takes `changedAt`). INV-8 is the
// reason it matters here: it needs rows on each side of a 30-day TTL, and a
// store that stamped internally could not be handed one.

#include <gtest/gtest.h>

#include "roadmapstore.h"

#include <QDir>
#include <QSqlQuery>
#include <QString>
#include <QTemporaryDir>
#include <QVariant>
#include <QVector>

#include <memory>

namespace {

// A registered project needs a root that EXISTS: registerProject() canonicalises
// it (INV-8 there) and refuses a path that cannot be resolved.
struct Fixture {
    QTemporaryDir dir;
    std::unique_ptr<RoadmapStore> store;

    bool init() {
        if (!dir.isValid())
            return false;
        store = std::make_unique<RoadmapStore>(
            dir.filePath(QStringLiteral("mail.sqlite")));
        QString err;
        return store->open(&err);
    }

    qint64 addProject(const QString &slug) {
        const QString root = dir.filePath(slug);
        if (!QDir().mkpath(root))
            return 0;
        QString err;
        const auto pk = store->registerProject(root, slug, slug, &err);
        return pk ? *pk : 0;
    }
};

constexpr const char *kT1  = "2026-01-01T00:00:00Z";
constexpr const char *kOld = "2026-01-01T00:00:00Z";  // well before the cutoff
constexpr const char *kNew = "2026-06-01T00:00:00Z";  // well after it
constexpr const char *kCut = "2026-03-01T00:00:00Z";

int rowCount(RoadmapStore &s, const QString &sql) {
    QSqlQuery q(s.db());
    if (!q.exec(sql) || !q.next())
        return -1;
    return q.value(0).toInt();
}

}  // namespace

// INV-1 — a message is addressed to a PROJECT. A well-formed but unregistered
// slug refuses `unknown_project` and writes nothing.
//
// The slug is syntactically valid, so the export_slug CHECK cannot be what
// rejects it; only the resolution step can.
TEST(SessionMessage, Inv1UnknownRecipientRefusesAndWritesNothing) {
    Fixture f;
    ASSERT_TRUE(f.init());
    const qint64 a = f.addProject(QStringLiteral("alpha"));
    ASSERT_GT(a, 0);

    QString code, err;
    qint64 id = 0;
    EXPECT_FALSE(f.store->sendMessage(a, QStringLiteral("never-registered"),
                                      QStringLiteral("hello"), QString(),
                                      QString::fromLatin1(kT1), &id, &code, &err));
    EXPECT_EQ(code, QStringLiteral("unknown_project"));
    EXPECT_EQ(rowCount(*f.store, QStringLiteral("SELECT count(*) FROM message")), 0);
}

// INV-2 — inbox returns only the calling project's mail. Both messages are
// unacked and recent, so neither the ack filter nor retention can account for
// the exclusion; only the recipient predicate can.
TEST(SessionMessage, Inv2InboxIsScopedToTheCallingProject) {
    Fixture f;
    ASSERT_TRUE(f.init());
    const qint64 a = f.addProject(QStringLiteral("alpha"));
    const qint64 b = f.addProject(QStringLiteral("bravo"));
    ASSERT_GT(a, 0);
    ASSERT_GT(b, 0);

    QString code, err;
    qint64 id = 0;
    ASSERT_TRUE(f.store->sendMessage(a, QStringLiteral("bravo"),
                                     QStringLiteral("for bravo"), QString(),
                                     QString::fromLatin1(kT1), &id, &code, &err)) << err.toStdString();
    ASSERT_TRUE(f.store->sendMessage(b, QStringLiteral("alpha"),
                                     QStringLiteral("for alpha"), QString(),
                                     QString::fromLatin1(kT1), &id, &code, &err)) << err.toStdString();

    QVector<RoadmapStore::Message> got;
    int unacked = 0;
    ASSERT_TRUE(f.store->inboxFor(b, false, 0, 0, &got, &unacked, &err)) << err.toStdString();
    ASSERT_EQ(got.size(), 1);
    EXPECT_EQ(got[0].body, QStringLiteral("for bravo"));
    EXPECT_EQ(got[0].fromSlug, QStringLiteral("alpha"));
    EXPECT_EQ(unacked, 1);

    ASSERT_TRUE(f.store->inboxFor(a, false, 0, 0, &got, &unacked, &err)) << err.toStdString();
    ASSERT_EQ(got.size(), 1);
    EXPECT_EQ(got[0].body, QStringLiteral("for alpha"));
}

// INV-3 — confirmation is two states in one nullable column, and ack is
// idempotent: the FIRST ack is the fact, so a second leaves the stamp alone.
TEST(SessionMessage, Inv3AckIsTwoStatesAndIdempotent) {
    Fixture f;
    ASSERT_TRUE(f.init());
    const qint64 a = f.addProject(QStringLiteral("alpha"));
    const qint64 b = f.addProject(QStringLiteral("bravo"));

    QString code, err;
    qint64 id = 0;
    ASSERT_TRUE(f.store->sendMessage(a, QStringLiteral("bravo"),
                                     QStringLiteral("ping"), QString(),
                                     QString::fromLatin1(kT1), &id, &code, &err)) << err.toStdString();

    QVector<RoadmapStore::Message> got;
    ASSERT_TRUE(f.store->inboxFor(b, true, 0, 0, &got, nullptr, &err));
    ASSERT_EQ(got.size(), 1);
    EXPECT_TRUE(got[0].ackedAt.isEmpty()) << "unread is acked_at IS NULL";

    bool already = true;
    ASSERT_TRUE(f.store->ackMessage(b, id, QString::fromLatin1(kNew), &already,
                                    &code, &err)) << err.toStdString();
    EXPECT_FALSE(already);

    ASSERT_TRUE(f.store->inboxFor(b, true, 0, 0, &got, nullptr, &err));
    ASSERT_EQ(got.size(), 1);
    const QString firstStamp = got[0].ackedAt;
    EXPECT_EQ(firstStamp, QString::fromLatin1(kNew));

    // Second ack with a DIFFERENT stamp: succeeds, reports already, and must
    // not move the timestamp.
    already = false;
    ASSERT_TRUE(f.store->ackMessage(b, id, QStringLiteral("2026-12-25T00:00:00Z"),
                                    &already, &code, &err)) << err.toStdString();
    EXPECT_TRUE(already);
    ASSERT_TRUE(f.store->inboxFor(b, true, 0, 0, &got, nullptr, &err));
    EXPECT_EQ(got[0].ackedAt, firstStamp) << "the first ack is the fact";
}

// A message addressed elsewhere answers `not_found`, the same code an absent id
// gets, so a probe cannot use the refusal to learn that an id exists.
TEST(SessionMessage, AckOnAnotherProjectsMessageIsNotFound) {
    Fixture f;
    ASSERT_TRUE(f.init());
    const qint64 a = f.addProject(QStringLiteral("alpha"));
    const qint64 b = f.addProject(QStringLiteral("bravo"));

    QString code, err;
    qint64 id = 0;
    ASSERT_TRUE(f.store->sendMessage(a, QStringLiteral("bravo"),
                                     QStringLiteral("ping"), QString(),
                                     QString::fromLatin1(kT1), &id, &code, &err));

    bool already = false;
    EXPECT_FALSE(f.store->ackMessage(a, id, QString::fromLatin1(kNew), &already,
                                     &code, &err));
    EXPECT_EQ(code, QStringLiteral("not_found"));

    // And the same code for an id that does not exist at all — the two are
    // deliberately indistinguishable.
    EXPECT_FALSE(f.store->ackMessage(b, 999999, QString::fromLatin1(kNew),
                                     &already, &code, &err));
    EXPECT_EQ(code, QStringLiteral("not_found"));
}

// INV-5 — deregistering removes a project's mail from BOTH ends and no
// further. The surviving row is what distinguishes a scoped delete from a
// table truncate.
TEST(SessionMessage, Inv5DeregisterClearsBothEndsAndSparesSiblings) {
    Fixture f;
    ASSERT_TRUE(f.init());
    const qint64 a = f.addProject(QStringLiteral("alpha"));
    const qint64 b = f.addProject(QStringLiteral("bravo"));
    const qint64 c = f.addProject(QStringLiteral("charlie"));

    QString code, err;
    qint64 id = 0;
    ASSERT_TRUE(f.store->sendMessage(a, QStringLiteral("bravo"), QStringLiteral("a->b"),
                                     QString(), QString::fromLatin1(kT1), &id, &code, &err));
    ASSERT_TRUE(f.store->sendMessage(b, QStringLiteral("alpha"), QStringLiteral("b->a"),
                                     QString(), QString::fromLatin1(kT1), &id, &code, &err));
    ASSERT_TRUE(f.store->sendMessage(c, QStringLiteral("bravo"), QStringLiteral("c->b"),
                                     QString(), QString::fromLatin1(kT1), &id, &code, &err));
    ASSERT_EQ(rowCount(*f.store, QStringLiteral("SELECT count(*) FROM message")), 3);

    RoadmapStore::DeregisterCounts counts;
    ASSERT_TRUE(f.store->deregisterProject(a, &counts, &err)) << err.toStdString();

    // Both of alpha's rows go — the one it SENT and the one it RECEIVED.
    EXPECT_EQ(counts.messages, 2);
    // charlie's message to bravo is untouched: this is a scoped delete.
    EXPECT_EQ(rowCount(*f.store, QStringLiteral("SELECT count(*) FROM message")), 1);
    QVector<RoadmapStore::Message> got;
    ASSERT_TRUE(f.store->inboxFor(b, true, 0, 0, &got, nullptr, &err));
    ASSERT_EQ(got.size(), 1);
    EXPECT_EQ(got[0].body, QStringLiteral("c->b"));
}

// INV-8 — the prune removes acked mail past the TTL and unacked mail at NO
// age. Both legs are required and neither alone is sufficient: the surviving
// row alone passes against a prune that never runs, and the deleted row alone
// passes against one that ignores acked_at and filters on age.
TEST(SessionMessage, Inv8PruneSparesUnackedAtAnyAge) {
    Fixture f;
    ASSERT_TRUE(f.init());
    const qint64 a = f.addProject(QStringLiteral("alpha"));
    const qint64 b = f.addProject(QStringLiteral("bravo"));

    QString code, err;
    qint64 stale = 0, unread = 0;
    ASSERT_TRUE(f.store->sendMessage(a, QStringLiteral("bravo"), QStringLiteral("acked+old"),
                                     QString(), QString::fromLatin1(kOld), &stale, &code, &err));
    ASSERT_TRUE(f.store->sendMessage(a, QStringLiteral("bravo"), QStringLiteral("unacked+old"),
                                     QString(), QString::fromLatin1(kOld), &unread, &code, &err));

    bool already = false;
    ASSERT_TRUE(f.store->ackMessage(b, stale, QString::fromLatin1(kOld), &already,
                                    &code, &err)) << err.toStdString();

    const int pruned = f.store->pruneAckedMail(b, QString::fromLatin1(kCut), &err);
    EXPECT_EQ(pruned, 1) << err.toStdString();

    QVector<RoadmapStore::Message> got;
    ASSERT_TRUE(f.store->inboxFor(b, true, 0, 0, &got, nullptr, &err));
    ASSERT_EQ(got.size(), 1);
    EXPECT_EQ(got[0].body, QStringLiteral("unacked+old"))
        << "unacked mail is spared at any age; an age filter alone would take it";
}

// The prune is scoped to the calling project's own INBOX. Scoped to
// from_project_id it would prune rows in OTHER projects' inboxes and never its
// own — the reading under which INV-8 passes without exercising anything.
TEST(SessionMessage, PruneIsScopedToTheCallersOwnInbox) {
    Fixture f;
    ASSERT_TRUE(f.init());
    const qint64 a = f.addProject(QStringLiteral("alpha"));
    const qint64 b = f.addProject(QStringLiteral("bravo"));

    QString code, err;
    qint64 id = 0;
    ASSERT_TRUE(f.store->sendMessage(a, QStringLiteral("bravo"), QStringLiteral("a->b"),
                                     QString(), QString::fromLatin1(kOld), &id, &code, &err));
    bool already = false;
    ASSERT_TRUE(f.store->ackMessage(b, id, QString::fromLatin1(kOld), &already, &code, &err));

    // alpha SENT that message. Pruning as alpha must not reach bravo's inbox.
    EXPECT_EQ(f.store->pruneAckedMail(a, QString::fromLatin1(kCut), &err), 0) << err.toStdString();
    EXPECT_EQ(rowCount(*f.store, QStringLiteral("SELECT count(*) FROM message")), 1);

    // bravo owns the inbox, so bravo's prune takes it.
    EXPECT_EQ(f.store->pruneAckedMail(b, QString::fromLatin1(kCut), &err), 1) << err.toStdString();
    EXPECT_EQ(rowCount(*f.store, QStringLiteral("SELECT count(*) FROM message")), 0);
}

// INV-9 — a full inbox refuses rather than dropping the oldest. A silently
// dropped message is indistinguishable from one that was never sent.
TEST(SessionMessage, Inv9FullInboxRefusesRatherThanDropping) {
    Fixture f;
    ASSERT_TRUE(f.init());
    const qint64 a = f.addProject(QStringLiteral("alpha"));
    const qint64 b = f.addProject(QStringLiteral("bravo"));

    QString code, err;
    qint64 id = 0;
    for (int i = 0; i < RoadmapStore::kMailInboxCap; ++i) {
        ASSERT_TRUE(f.store->sendMessage(a, QStringLiteral("bravo"),
                                         QStringLiteral("msg %1").arg(i), QString(),
                                         QString::fromLatin1(kT1), &id, &code, &err))
            << "failed at " << i << ": " << err.toStdString();
    }
    const int before = rowCount(*f.store, QStringLiteral("SELECT count(*) FROM message"));
    ASSERT_EQ(before, RoadmapStore::kMailInboxCap);

    EXPECT_FALSE(f.store->sendMessage(a, QStringLiteral("bravo"),
                                      QStringLiteral("one too many"), QString(),
                                      QString::fromLatin1(kT1), &id, &code, &err));
    EXPECT_EQ(code, QStringLiteral("inbox_full"));
    EXPECT_EQ(rowCount(*f.store, QStringLiteral("SELECT count(*) FROM message")), before)
        << "the refusal must not have dropped anything to make room";

    // Acking frees a slot: the cap counts UNACKED rows (§ 2.6).
    QVector<RoadmapStore::Message> got;
    ASSERT_TRUE(f.store->inboxFor(b, false, 1, 0, &got, nullptr, &err));
    ASSERT_EQ(got.size(), 1);
    bool already = false;
    ASSERT_TRUE(f.store->ackMessage(b, got[0].id, QString::fromLatin1(kNew),
                                    &already, &code, &err));
    EXPECT_TRUE(f.store->sendMessage(a, QStringLiteral("bravo"),
                                     QStringLiteral("now there is room"), QString(),
                                     QString::fromLatin1(kT1), &id, &code, &err)) << err.toStdString();
}

// § 2.2 — the body cap is BYTE-valued. SQLite's length() counts characters on
// a TEXT value, so a character-valued cap would admit up to 4x the budget.
TEST(SessionMessage, BodyCapIsBytesNotCharacters) {
    Fixture f;
    ASSERT_TRUE(f.init());
    const qint64 a = f.addProject(QStringLiteral("alpha"));
    f.addProject(QStringLiteral("bravo"));

    // 2000 three-byte characters = 6000 bytes. Under a CHARACTER cap of 4096
    // this passes; under a BYTE cap it must be refused.
    const QString multibyte(2000, QChar(0x65E5));
    ASSERT_EQ(multibyte.toUtf8().size(), 6000);

    QString code, err;
    qint64 id = 0;
    EXPECT_FALSE(f.store->sendMessage(a, QStringLiteral("bravo"), multibyte,
                                      QString(), QString::fromLatin1(kT1),
                                      &id, &code, &err));
    EXPECT_EQ(code, QStringLiteral("bad_args"));

    // 1000 of the same characters is 3000 bytes and is accepted.
    const QString ok(1000, QChar(0x65E5));
    ASSERT_EQ(ok.toUtf8().size(), 3000);
    EXPECT_TRUE(f.store->sendMessage(a, QStringLiteral("bravo"), ok, QString(),
                                     QString::fromLatin1(kT1), &id, &code, &err)) << err.toStdString();
}
