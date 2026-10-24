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
#include "../../_support/roadmapstoreaccess.h"

#include <QDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>
#include <QTemporaryDir>
#include <QVariant>
#include <QVector>

#include <chrono>
#include <future>
#include <memory>
#include <thread>

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
    QSqlQuery q(RoadmapStoreTestAccess::db(s));
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

// ANTS-5473 — the refusal names the slugs that were probably meant. "doom_ants"
// for "doom-ants" was refused with nothing to go on.
TEST(SessionMessage, Ants5473UnknownSlugHasCandidates) {
    Fixture f;
    ASSERT_TRUE(f.init());
    ASSERT_GT(f.addProject(QStringLiteral("doom-ants")), 0);
    ASSERT_GT(f.addProject(QStringLiteral("vestige")), 0);
    ASSERT_GT(f.addProject(QStringLiteral("ants-terminal")), 0);

    const QStringList c = f.store->slugCandidates(QStringLiteral("Doom_Ants"));
    ASSERT_FALSE(c.isEmpty());
    EXPECT_EQ(c.first(), QStringLiteral("doom-ants"));
    EXPECT_FALSE(c.contains(QStringLiteral("vestige")))
        << "a slug sharing nothing with the request is noise, not a candidate";
    EXPECT_TRUE(f.store->slugCandidates(QStringLiteral("zzz")).isEmpty());
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

// ANTS-5591 — the page ceiling is ANTS-5098's 200. A limit of 0 or less asks
// for as many as allowed, so it maps to the ceiling; it was clamped to 1.
TEST(SessionMessage, Ants5591NonPositiveLimitAsksForTheCeiling) {
    EXPECT_EQ(RoadmapStore::inboxPageLimit(0), RoadmapStore::kMailInboxPageMax);
    EXPECT_EQ(RoadmapStore::inboxPageLimit(-1), RoadmapStore::kMailInboxPageMax);
    EXPECT_EQ(RoadmapStore::inboxPageLimit(1), 1);
    EXPECT_EQ(RoadmapStore::inboxPageLimit(20), 20);
    EXPECT_EQ(RoadmapStore::inboxPageLimit(RoadmapStore::kMailInboxPageMax + 1),
              RoadmapStore::kMailInboxPageMax);
}

// ANTS-5601 — one message by id, acked or not, scoped like ack: another
// project's id and an absent id are the same `not_found`.
TEST(SessionMessage, Ants5601MessageForReadsOneMessageScopedToTheInbox) {
    Fixture f;
    ASSERT_TRUE(f.init());
    const qint64 a = f.addProject(QStringLiteral("alpha"));
    const qint64 b = f.addProject(QStringLiteral("bravo"));

    QString code, err;
    qint64 first = 0, second = 0;
    ASSERT_TRUE(f.store->sendMessage(a, QStringLiteral("bravo"),
                                     QStringLiteral("first"), QStringLiteral("s1"),
                                     QString::fromLatin1(kT1), &first, &code, &err));
    ASSERT_TRUE(f.store->sendMessage(a, QStringLiteral("bravo"),
                                     QStringLiteral("second"), QString(),
                                     QString::fromLatin1(kT1), &second, &code, &err));
    bool already = false;
    ASSERT_TRUE(f.store->ackMessage(b, first, QString::fromLatin1(kNew), &already,
                                    &code, &err));

    // The OLDER, acked message: neither newest-first order nor the unacked
    // filter can hand it back by accident.
    RoadmapStore::Message m;
    ASSERT_TRUE(f.store->messageFor(b, first, &m, &code, &err)) << err.toStdString();
    EXPECT_EQ(m.id, first);
    EXPECT_EQ(m.body, QStringLiteral("first"));
    EXPECT_EQ(m.fromSlug, QStringLiteral("alpha"));
    EXPECT_EQ(m.fromSession, QStringLiteral("s1"));
    EXPECT_EQ(m.ackedAt, QString::fromLatin1(kNew));

    EXPECT_FALSE(f.store->messageFor(a, first, &m, &code, &err));
    EXPECT_EQ(code, QStringLiteral("not_found"));
    EXPECT_FALSE(f.store->messageFor(b, 999999, &m, &code, &err));
    EXPECT_EQ(code, QStringLiteral("not_found"));
}

// INV-5 — deregistering KEEPS a project's mail at both ends (ANTS-5366 § 2.2,
// which withdrew the old clear-both-ends rule): the project row stays, so the
// mail it sent and the mail sent to it both still have a row to point at.
TEST(SessionMessage, Inv5DeregisterKeepsMailAtBothEnds) {
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
    ASSERT_TRUE(f.store->deregisterProject(a, QString::fromLatin1(kNew), &counts, &err))
        << err.toStdString();

    EXPECT_EQ(rowCount(*f.store, QStringLiteral("SELECT count(*) FROM message")), 3)
        << "deregistering deleted mail";
    // The mail alpha SENT is still in bravo's inbox, sender intact.
    QVector<RoadmapStore::Message> got;
    ASSERT_TRUE(f.store->inboxFor(b, true, 0, 0, &got, nullptr, &err));
    ASSERT_EQ(got.size(), 2);
    // And the mail alpha RECEIVED is still in its own.
    ASSERT_TRUE(f.store->inboxFor(a, true, 0, 0, &got, nullptr, &err));
    ASSERT_EQ(got.size(), 1);
    EXPECT_EQ(got[0].body, QStringLiteral("b->a"));
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

// ------------------------------------------------- ANTS-5741 broadcast (§ 2.7) --

namespace {

int inboxRows(RoadmapStore &s, qint64 projectId) {
    return rowCount(s, QStringLiteral("SELECT count(*) FROM message WHERE "
                                      "to_project_id = %1").arg(projectId));
}

// Fills `to` to the cap from `from`, so the next copy there is refused.
void fillInbox(RoadmapStore &s, qint64 from, const QString &to) {
    QString code, err;
    qint64 id = 0;
    for (int i = 0; i < RoadmapStore::kMailInboxCap; ++i)
        ASSERT_TRUE(s.sendMessage(from, to, QStringLiteral("fill %1").arg(i),
                                  QString(), QString::fromLatin1(kT1), &id,
                                  &code, &err)) << err.toStdString();
}

}  // namespace

// INV-11 — a broadcast reaches every registered project but the sender, and
// only those. `delta` (deregistered) separates the registered list from every
// row in `project`; `alpha` separates the self-skip from none.
TEST(SessionMessage, Inv11BroadcastReachesRegisteredProjectsButTheSender) {
    Fixture f;
    ASSERT_TRUE(f.init());
    const qint64 a = f.addProject(QStringLiteral("alpha"));
    const qint64 b = f.addProject(QStringLiteral("bravo"));
    const qint64 c = f.addProject(QStringLiteral("charlie"));
    const qint64 d = f.addProject(QStringLiteral("delta"));
    QString code, err;
    RoadmapStore::DeregisterCounts counts;
    ASSERT_TRUE(f.store->deregisterProject(d, QString::fromLatin1(kNew), &counts, &err))
        << err.toStdString();

    QVector<RoadmapStore::BroadcastOutcome> got;
    ASSERT_TRUE(f.store->broadcastMessage(a, QStringLiteral("to all"), QString(),
                                          QString::fromLatin1(kT1), false, false,
                                          &got, &code, &err)) << err.toStdString();
    EXPECT_EQ(inboxRows(*f.store, a), 0) << "the sender received its own broadcast";
    EXPECT_EQ(inboxRows(*f.store, b), 1);
    EXPECT_EQ(inboxRows(*f.store, c), 1);
    EXPECT_EQ(inboxRows(*f.store, d), 0) << "a deregistered project received a copy";
    ASSERT_EQ(got.size(), 2);
    for (const auto &o : got) {
        EXPECT_TRUE(o.code.isEmpty());
        EXPECT_GT(o.messageId, 0);
    }

    ASSERT_TRUE(f.store->broadcastMessage(a, QStringLiteral("to all, me too"), QString(),
                                          QString::fromLatin1(kT1), true, false,
                                          &got, &code, &err)) << err.toStdString();
    EXPECT_EQ(inboxRows(*f.store, a), 1) << "include_self did not reach the sender";
    EXPECT_EQ(inboxRows(*f.store, d), 0);
}

// INV-12 — a full recipient is refused and the rest still receive. charlie's
// row is what separates a per-recipient skip from refusing the whole call.
TEST(SessionMessage, Inv12FullRecipientIsRefusedTheRestReceive) {
    Fixture f;
    ASSERT_TRUE(f.init());
    const qint64 a = f.addProject(QStringLiteral("alpha"));
    const qint64 b = f.addProject(QStringLiteral("bravo"));
    const qint64 c = f.addProject(QStringLiteral("charlie"));
    fillInbox(*f.store, a, QStringLiteral("bravo"));
    const int bravoBefore = inboxRows(*f.store, b);

    QString code, err;
    QVector<RoadmapStore::BroadcastOutcome> got;
    ASSERT_TRUE(f.store->broadcastMessage(a, QStringLiteral("to all"), QString(),
                                          QString::fromLatin1(kT1), false, false,
                                          &got, &code, &err)) << err.toStdString();
    EXPECT_EQ(inboxRows(*f.store, b), bravoBefore);
    EXPECT_EQ(inboxRows(*f.store, c), 1);
    ASSERT_EQ(got.size(), 2);
    EXPECT_EQ(got[0].toSlug, QStringLiteral("bravo"));
    EXPECT_EQ(got[0].code, QStringLiteral("inbox_full"));
    EXPECT_EQ(got[0].messageId, 0);
    EXPECT_EQ(got[1].toSlug, QStringLiteral("charlie"));
    EXPECT_TRUE(got[1].code.isEmpty());
}

// INV-13 — a broadcast dry run writes nothing and reports the same split. The
// full bravo makes the split checkable rather than an echo of the list.
TEST(SessionMessage, Inv13BroadcastDryRunWritesNothingAndReportsTheSplit) {
    Fixture f;
    ASSERT_TRUE(f.init());
    const qint64 a = f.addProject(QStringLiteral("alpha"));
    f.addProject(QStringLiteral("bravo"));
    f.addProject(QStringLiteral("charlie"));
    fillInbox(*f.store, a, QStringLiteral("bravo"));
    const int before = rowCount(*f.store, QStringLiteral("SELECT count(*) FROM message"));

    QString code, err;
    QVector<RoadmapStore::BroadcastOutcome> got;
    ASSERT_TRUE(f.store->broadcastMessage(a, QStringLiteral("to all"), QString(),
                                          QString::fromLatin1(kT1), false, true,
                                          &got, &code, &err)) << err.toStdString();
    EXPECT_EQ(rowCount(*f.store, QStringLiteral("SELECT count(*) FROM message")), before)
        << "a dry run wrote a row";
    ASSERT_EQ(got.size(), 2);
    EXPECT_EQ(got[0].toSlug, QStringLiteral("bravo"));
    EXPECT_EQ(got[0].code, QStringLiteral("inbox_full"));
    EXPECT_EQ(got[1].toSlug, QStringLiteral("charlie"));
    EXPECT_TRUE(got[1].code.isEmpty());
    EXPECT_EQ(got[1].messageId, 0) << "a preview carried a message id";
}

// § 2.7 — an oversized from_session refuses the whole broadcast up front, so
// refused[] never carries bad_args.
TEST(SessionMessage, Ants5741BroadcastValidatesSessionBeforeAnyWrite) {
    Fixture f;
    ASSERT_TRUE(f.init());
    const qint64 a = f.addProject(QStringLiteral("alpha"));
    f.addProject(QStringLiteral("bravo"));

    QString code, err;
    QVector<RoadmapStore::BroadcastOutcome> got;
    const QString longSession(RoadmapStore::kMailSessionMaxBytes + 1, QLatin1Char('s'));
    EXPECT_FALSE(f.store->broadcastMessage(a, QStringLiteral("hi"), longSession,
                                           QString::fromLatin1(kT1), false, false,
                                           &got, &code, &err));
    EXPECT_EQ(code, QStringLiteral("bad_args"));
    EXPECT_EQ(rowCount(*f.store, QStringLiteral("SELECT count(*) FROM message")), 0);
}

// ---------------------------------------------------------- INV-7 (source) --
//
// Every argument cmdSessionMessage reads via req.value()/req.contains() is
// declared in the verb's inputSchema.properties, `caller_cwd` excepted as a
// universal dispatch-layer arg (ANTS-2175 INV-2).
//
// Written against the SOURCE rather than the engine, and that is structural
// rather than lazy: every behavioural test above drives RoadmapStore directly,
// so none of them crosses the dispatcher — and the schema is the dispatcher's
// half of the contract. ANTS-4617 shipped `op` and `confirm` documented in the
// description and declared nowhere, and its own tests could not have caught it
// for exactly this reason.
//
// The keys are SCRAPED from the handler rather than hardcoded, so an op added
// later and left undeclared fails without anyone remembering to extend this.
//
// `static` on both helpers: this file compiles into a shared bundle, so a
// file-scope name here can collide at link time with a sibling's.

#include <QFile>
#include <QRegularExpression>
#include <QSet>

#ifndef ANTS_SRC_DIR
#  error "ANTS_SRC_DIR compile definition required"
#endif

namespace {

static QString readSrc(const char *name) {
    QFile f(QStringLiteral(ANTS_SRC_DIR) + QLatin1Char('/')
            + QString::fromLatin1(name));
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(f.readAll());
}

// Code lines only. Both region guards below assert the ABSENCE of a token, and
// the prose explaining why that token is forbidden names it — so a
// comment-blind match harvests the explanation and reddens correct code. The
// INV-7 scrape above already skips comment lines per line; a region slice needs
// the same treatment, applied to the slice.
static QString codeOnly(const QString &src) {
    QStringList kept;
    const QStringList lines = src.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QString t = line.trimmed();
        if (t.startsWith(QLatin1String("//")))
            continue;
        kept << line;
    }
    return kept.join(QLatin1Char('\n'));
}

// The session_message entry of the tools/list block, bounded by its own
// tools.append(t) so a property declared for a NEIGHBOURING verb cannot be
// mistaken for one of ours.
static QString sessionMessageToolBlock() {
    const QString all = readSrc("claudeintegration.cpp");
    const int start = all.indexOf(QStringLiteral("t[\"name\"] = \"session_message\";"));
    if (start < 0)
        return QString();
    const int end = all.indexOf(QStringLiteral("tools.append(t);"), start);
    if (end < 0)
        return QString();
    return all.mid(start, end - start);
}

}  // namespace

TEST(SessionMessage, Inv7SchemaDeclaresEveryOp) {
    const QString block = sessionMessageToolBlock();
    ASSERT_FALSE(block.isEmpty()) << "session_message tool block not found";

    for (const char *key : {"op", "to", "body", "from_session", "message_id",
                            "include_acked", "limit", "offset", "include_self"}) {
        EXPECT_TRUE(block.contains(QStringLiteral("props[\"%1\"]")
                                       .arg(QString::fromLatin1(key))))
            << "inputSchema.properties is missing \"" << key << "\"";
    }
    // Without the enum values a client offering completions never surfaces the
    // ops at all — the half of ANTS-4621 that a properties check alone misses.
    for (const char *op : {"send", "inbox", "ack"}) {
        EXPECT_TRUE(block.contains(QStringLiteral("e.append(QStringLiteral(\"%1\"))")
                                       .arg(QString::fromLatin1(op))))
            << "op enum is missing \"" << op << "\"";
    }
}

TEST(SessionMessage, Inv7HandlerReadsOnlyDeclaredArgs) {
    const QString handler = readSrc("remotecontrol_session_message.cpp");
    ASSERT_FALSE(handler.isEmpty()) << "handler source not readable";
    const QString block = sessionMessageToolBlock();
    ASSERT_FALSE(block.isEmpty()) << "session_message tool block not found";

    // Comment lines are skipped: this file's own header prose names req.value()
    // while explaining the rule, and a comment-blind scrape would harvest it.
    static const QRegularExpression reRead(
        QStringLiteral("req\\.(?:value|contains)\\(QStringLiteral\\(\"([a-z_]+)\"\\)"));

    QSet<QString> keys;
    const QStringList lines = handler.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        if (line.trimmed().startsWith(QLatin1String("//")))
            continue;
        auto it = reRead.globalMatch(line);
        while (it.hasNext())
            keys.insert(it.next().captured(1));
    }
    ASSERT_FALSE(keys.isEmpty()) << "scraped no req.value() keys — the scrape "
                                    "itself is broken, which would make this "
                                    "test pass vacuously";

    for (const QString &key : keys) {
        if (key == QStringLiteral("caller_cwd"))
            continue;  // ANTS-2175 INV-2 — a universal dispatch arg.
        EXPECT_TRUE(block.contains(QStringLiteral("props[\"%1\"]").arg(key)))
            << "handler reads \"" << key.toStdString()
            << "\" but inputSchema.properties never declares it — a strict "
               "client refuses the call, and ANTS-2175 reports the argument as "
               "ignored on the call it steered";
    }
}

// INV-10 and INV-4 are covered by INSPECTION, not behaviour, and the reason is
// the same one ANTS-3855 gives for cmdRoadmapMigrate: the handler opens
// RoadmapStore::defaultPath(), which is the developer's REAL machine-global
// store. A test that drove it would write live data — which is how ANTS-3856's
// fixture row got there. So the properties are asserted against the source.

// INV-10 — a dry_run send writes nothing and claims nothing in the past tense.
//
// The strong half is the SECOND assertion: the preview must not reach the
// writer at all. A preview that called sendMessage() and rolled back would
// still satisfy "no row survives" while having taken a write lock on a store
// other sessions share.
TEST(SessionMessage, Inv10DryRunNeitherWritesNorClaimsPastTense) {
    const QString h = readSrc("remotecontrol_session_message.cpp");
    ASSERT_FALSE(h.isEmpty());

    // ANTS-5741 — the single-slug send and the broadcast each have a preview:
    // the broadcast's dry-run branch and sendPreview(). Both are checked.
    const auto checkPreview = [](const QString &branch) {
        EXPECT_TRUE(branch.contains(QStringLiteral("would_send")))
            << "ANTS-4463: a preview carries a would-be field, never message_id";
        EXPECT_FALSE(branch.contains(QStringLiteral("message_id")))
            << "ANTS-4463: a past-tense field on a preview lets a caller read it "
               "as a completed write";
        EXPECT_FALSE(branch.contains(QStringLiteral("sendMessage(")))
            << "the preview must stop SHORT of the writer, not write and roll back";
    };
    const auto upToReturn = [&h](int at) {
        const int end = h.indexOf(QStringLiteral("return QJsonDocument(c.out);"), at);
        return end > at ? codeOnly(h.mid(at, end - at)) : QString();
    };
    const int broadcastAt = h.indexOf(QStringLiteral("if (c.dryRun) {"));
    ASSERT_GE(broadcastAt, 0) << "the broadcast's dry-run branch moved";
    checkPreview(upToReturn(broadcastAt));
    const int previewAt = h.indexOf(QStringLiteral("QJsonDocument sendPreview("));
    ASSERT_GE(previewAt, 0) << "sendPreview moved";
    checkPreview(upToReturn(previewAt));

    // The single-slug send reaches the preview BEFORE the writer.
    const int sendAt = h.indexOf(QStringLiteral("QJsonDocument opSend("));
    ASSERT_GE(sendAt, 0);
    const QString send = codeOnly(h.mid(sendAt));
    const int toPreview = send.indexOf(QStringLiteral("return sendPreview("));
    ASSERT_GE(toPreview, 0);
    EXPECT_LT(toPreview, send.indexOf(QStringLiteral("sendMessage(")));

    // § 2.6 — the prune is a write, so a dry run must not trigger it either.
    // Against an empty inbox a pruning preview is indistinguishable from a
    // clean one, which is why this is asserted here rather than by fixture.
    const int pruneAt = h.indexOf(QStringLiteral("void pruneAfterWrite() const {"));
    ASSERT_GE(pruneAt, 0);
    const QString guard = codeOnly(h.mid(pruneAt, 220));
    EXPECT_TRUE(guard.contains(QStringLiteral("dryRun")))
        << "pruneAfterWrite must exclude a dry run — a preview that prunes "
           "destroys aged mail to measure itself";
}

// The single-slug preview checks its arguments with the store's own
// mailArgsError(), so it refuses what the real send refuses. A hand copy in
// the handler checked the body alone and passed an over-cap from_session.
TEST(SessionMessage, DryRunSendSharesTheStoresArgumentCheck) {
    const QString h = codeOnly(readSrc("remotecontrol_session_message.cpp"));
    ASSERT_FALSE(h.isEmpty());
    EXPECT_TRUE(h.contains(QStringLiteral("RoadmapStore::mailArgsError(")))
        << "the preview must call the store's argument check";
    EXPECT_FALSE(h.contains(QStringLiteral("kMailBodyMaxBytes")))
        << "the handler keeps its own copy of the body cap check";

    const QString longSession(RoadmapStore::kMailSessionMaxBytes + 1, QLatin1Char('s'));
    EXPECT_FALSE(RoadmapStore::mailArgsError(QStringLiteral("hi"), longSession).isEmpty());
    EXPECT_FALSE(RoadmapStore::mailArgsError(QString(), QString()).isEmpty());
    EXPECT_TRUE(RoadmapStore::mailArgsError(QStringLiteral("hi"), QString()).isEmpty());
}

// ANTS-5594 — a caller_cwd inside a registered project (its src/, say) finds
// that project's mailbox. The walk is status_bar_mail_chip INV-1's; this pins
// that the handler uses it rather than the exact-root lookup. ANTS-5667 maps
// the folder through repoRootOf first; its behaviour is ProjectRepoRoot's.
TEST(SessionMessage, Ants5594SubdirectoryCallerFindsItsProject) {
    const QString h = codeOnly(readSrc("remotecontrol_session_message.cpp"));
    ASSERT_FALSE(h.isEmpty());
    EXPECT_TRUE(h.contains(QStringLiteral(
        "store.projectIdContaining(ProjectSettings::repoRootOf(cwd)")))
        << "the caller's project must be found by walking up from caller_cwd";
    EXPECT_FALSE(h.contains(QStringLiteral("projectIdForRoot(")))
        << "an exact-root lookup refuses no_project from a subdirectory";
}

// INV-4 — session_orient reports the INBOX, never the outbox, and emits no
// block at all when there is nothing unacked.
TEST(SessionMessage, Inv4OrientReportsTheInboxOnly) {
    const QString st = readSrc("remotecontrol_state.cpp");
    ASSERT_FALSE(st.isEmpty());

    const int at = st.indexOf(QStringLiteral("mail_pending (ANTS-4622"));
    ASSERT_GE(at, 0) << "no mail_pending block in session_orient";
    const QString block = codeOnly(st.mid(at, 2200));

    // The summary reader is scoped to the caller's own inbox; nothing in the
    // block reads mail this project SENT.
    EXPECT_TRUE(block.contains(QStringLiteral("mailSummaryFor")))
        << "the block must use the bounded summary, not load message bodies "
           "on a call that runs at every session start";
    // Emitted only when non-zero — a present-and-zero block would churn the
    // orient ETag for the 30 days acked rows linger (ANTS-3631's shape).
    EXPECT_TRUE(block.contains(QStringLiteral("unacked > 0")))
        << "the block must be omitted when nothing is unacked";
    // projectIdForRoot, never registerProject: a READ must not register the
    // caller into a machine-global store.
    EXPECT_TRUE(block.contains(QStringLiteral("projectIdForRoot")));
    EXPECT_FALSE(block.contains(QStringLiteral("registerProject")))
        << "an orientation READ must never register a project";
}

// ANTS-5499 — a send racing another connection's write waits for it instead
// of failing "database is locked". The other connection holds the write lock,
// writes, and commits after the send has started. A send that reads before it
// claims the write lock holds a snapshot that commit makes stale, and SQLite
// refuses that upgrade at once, without honouring busy_timeout.
TEST(SessionMessage, Ants5499SendWaitsOutAConcurrentWriter) {
    Fixture f;
    ASSERT_TRUE(f.init());
    const qint64 a = f.addProject(QStringLiteral("alpha"));
    ASSERT_GT(a, 0);
    ASSERT_GT(f.addProject(QStringLiteral("beta")), 0);
    {
        QSqlQuery q(RoadmapStoreTestAccess::db(*f.store));
        ASSERT_TRUE(q.exec(QStringLiteral("CREATE TABLE race_probe (v INTEGER)")));
    }

    const QString path = f.dir.filePath(QStringLiteral("mail.sqlite"));
    std::promise<void> locked;
    auto lockedFuture = locked.get_future();
    std::thread writer([&path, &locked] {
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                        QStringLiteral("ants5499_writer"));
            db.setDatabaseName(path);
            if (db.open()) {
                QSqlQuery q(db);
                q.exec(QStringLiteral("BEGIN IMMEDIATE"));
                q.exec(QStringLiteral("INSERT INTO race_probe VALUES (1)"));
                locked.set_value();
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                q.exec(QStringLiteral("COMMIT"));
            } else {
                locked.set_value();
            }
            db.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("ants5499_writer"));
    });
    lockedFuture.wait();

    QString code, err;
    qint64 id = 0;
    const bool sent = f.store->sendMessage(a, QStringLiteral("beta"),
                                           QStringLiteral("hello"), QString(),
                                           QString::fromLatin1(kT1), &id, &code, &err);
    writer.join();
    EXPECT_TRUE(sent) << code.toStdString() << ": " << err.toStdString();
    EXPECT_EQ(rowCount(*f.store, QStringLiteral("SELECT count(*) FROM message")), 1);
}
