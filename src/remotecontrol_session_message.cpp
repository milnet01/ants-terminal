// ANTS-4622 TU 18/25 — the `session_message` handler: the cross-session mailbox's verb.
// Contract: docs/specs/ANTS-4622-cross-session-mailbox.md § 2.3.
//
// Thin by design, like its roadmap_migrate sibling. Everything that touches a
// store is RoadmapStore's (send/inbox/ack/prune), which `test_core` links
// without dragging RemoteControl and MainWindow in behind it. What lives here
// is the part that cannot: resolving caller_cwd to a project, reading the clock
// once, and shaping the envelope.
//
// Every argument this file reads via req.value() MUST be declared in the verb's
// inputSchema.properties in claudeintegration.cpp — INV-7, and ANTS-4621 is why
// it is an invariant rather than a habit. The schema sets
// additionalProperties:false, so an undeclared argument is refused by a strict
// client before the handler runs; and on the permissive path ANTS-2175 builds
// its `ignored_args` advisory from those same properties, so an undeclared
// argument is reported as ignored on the very call it just steered. That
// shipped once, on roadmap_migrate, and was found only by calling the verb.

#include "remotecontrol.h"

#include "gitorigin.h"         // ANTS-6720 — op:"projects"' github_repo
#include "projectsettings.h"   // ANTS-5667 — repoRootOf
#include "resolvedroot.h"
#include "roadmapstore.h"

#include <cmath>
#include <QDateTime>
#include <QDir>
#include <QJsonArray>

#include <optional>

// ANTS-5699 — see the header.
QJsonObject RemoteControl::mailBodyFields(const QString &body, bool wholeBody) {
    QString subject;
    const QStringList lines = body.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        subject = line.trimmed();
        if (!subject.isEmpty()) break;
    }
    const bool clipped = subject.size() > RoadmapStore::kMailPreviewChars;
    if (wholeBody || (!clipped && subject == body.trimmed()))
        return QJsonObject{{QStringLiteral("body"), body}};
    if (clipped)
        subject = subject.left(RoadmapStore::kMailPreviewChars) + QChar(0x2026);
    return QJsonObject{{QStringLiteral("subject"), subject},
                       {QStringLiteral("body_bytes"), body.toUtf8().size()}};
}

namespace {

QJsonDocument refuse(const char *code, const QString &message) {
    QJsonObject e;
    e[QStringLiteral("ok")]    = false;
    e[QStringLiteral("code")]  = QString::fromLatin1(code);
    e[QStringLiteral("error")] = message;
    return QJsonDocument(e);
}

// ANTS-5473 — an unknown recipient refusal names the slugs probably meant,
// so a hyphen-for-underscore slip is fixable without guessing.
QJsonDocument refuseUnknownProject(const RoadmapStore &store, const QString &to,
                                   const QString &message) {
    QJsonObject e = refuse("unknown_project", message).object();
    e[QStringLiteral("candidates")] =
        QJsonArray::fromStringList(store.slugCandidates(to));
    return QJsonDocument(e);
}

QJsonObject messageToJson(const RoadmapStore::Message &m, bool wholeBody) {
    QJsonObject o = RemoteControl::mailBodyFields(m.body, wholeBody);
    o[QStringLiteral("message_id")]   = double(m.id);
    o[QStringLiteral("from")]         = m.fromSlug;
    o[QStringLiteral("from_session")] = m.fromSession;
    o[QStringLiteral("created_at")]   = m.createdAt;
    // Absent rather than null when unread: `acked_at` present IS the read
    // state, so a null would make a client test the value instead of the key.
    if (!m.ackedAt.isEmpty())
        o[QStringLiteral("acked_at")] = m.ackedAt;
    return o;
}


QJsonDocument refuseStore(const QString &code, const QString &err) {
    return refuse(code.isEmpty() ? "io_error" : code.toLatin1().constData(),
                  QStringLiteral("session_message: %1").arg(err));
}

// What every op shares, read once by cmdSessionMessage.
struct MailCall {
    RoadmapStore &store;
    qint64 self;             // the caller's project
    const QJsonObject &req;
    QString now;             // read once, so every stamp this call writes agrees
    bool dryRun;
    QJsonObject out;         // {ok, op}; each op adds its fields

    // § 2.6 — the prune fires on ops that ALREADY write, never on a read and
    // never on a dry run. A dry_run send is an op too, and INV-10 forbids a
    // preview from writing anything.
    void pruneAfterWrite() const {
        if (dryRun)
            return;
        const QString cutoff =
            QDateTime::currentDateTimeUtc()
                .addDays(-RoadmapStore::kMailAckedTtlDays)
                .toString(QStringLiteral("yyyy-MM-ddTHH:mm:ssZ"));
        QString ignored;
        store.pruneAckedMail(self, cutoff, &ignored);
    }
};

// ANTS-5741 (§ 2.7) — `to:"*"` addresses every registered project. The store
// owns the walk, the cap split and the dry-run split; this only shapes the
// reply.
QJsonDocument sendBroadcast(MailCall &c, const QString &body, const QString &from) {
    const bool includeSelf = c.req.value(QStringLiteral("include_self")).toBool(false);
    QVector<RoadmapStore::BroadcastOutcome> outcomes;
    QString code, err;
    if (!c.store.broadcastMessage(c.self, body, from, c.now, includeSelf, c.dryRun,
                                  &outcomes, &code, &err))
        return refuseStore(code, err);
    QJsonArray went, refused;
    for (const auto &o : outcomes) {
        if (!o.code.isEmpty()) {
            QJsonObject r;
            r[QStringLiteral("to")]   = o.toSlug;
            r[QStringLiteral("code")] = o.code;
            refused.append(r);
        } else if (!c.dryRun) {
            QJsonObject s;
            s[QStringLiteral("to")]         = o.toSlug;
            s[QStringLiteral("message_id")] = double(o.messageId);
            went.append(s);
        } else {
            went.append(o.toSlug);
        }
    }
    c.out[QStringLiteral("to")] = QStringLiteral("*");
    if (c.dryRun) {
        c.out[QStringLiteral("dry_run")]       = true;
        c.out[QStringLiteral("would_send_to")] = went;
        c.out[QStringLiteral("would_refuse")]  = refused;
        return QJsonDocument(c.out);
    }
    c.pruneAfterWrite();
    c.out[QStringLiteral("sent")]       = went;
    c.out[QStringLiteral("refused")]    = refused;
    c.out[QStringLiteral("created_at")] = c.now;
    return QJsonDocument(c.out);
}

// ANTS-4463 — a preview carries NO past-tense field: `would_send` rather than
// `message_id`, so a caller branching on either cannot read a preview as a
// completed write.
//
// It runs the same two validations the real call runs — resolve the
// recipient, then RoadmapStore::mailArgsError() — and stops before the
// insert. Nothing here writes, and § 2.6's prune is skipped for the same
// reason, so a preview cannot destroy aged mail to measure itself.
QJsonDocument sendPreview(MailCall &c, const QString &to, const QString &body,
                          const QString &from, bool dedupe) {
    const auto toId = c.store.projectIdForSlug(to);
    if (!toId)
        return refuseUnknownProject(c.store, to,
                      QStringLiteral("no project is registered under "
                                     "export_slug \"%1\", so mail to it "
                                     "could never be delivered").arg(to));
    if (const QString bad = RoadmapStore::mailArgsError(body, from); !bad.isEmpty())
        return refuse("bad_args", bad);
    // ANTS-6720 — the read the real send makes, without its transaction.
    qint64 duplicate = 0;
    if (dedupe) {
        QString err;
        const auto dup = c.store.unackedDuplicate(c.self, *toId, body, &err);
        if (!dup)
            return refuseStore(QString(), err);
        duplicate = *dup;
    }
    c.out[QStringLiteral("dry_run")]    = true;
    c.out[QStringLiteral("would_send")] = duplicate == 0;
    if (duplicate != 0)
        c.out[QStringLiteral("duplicate_of")] = double(duplicate);
    c.out[QStringLiteral("to")]         = to;
    return QJsonDocument(c.out);
}

QJsonDocument opSend(MailCall &c) {
    const QString to   = c.req.value(QStringLiteral("to")).toString();
    const QString body = c.req.value(QStringLiteral("body")).toString();
    const QString from = c.req.value(QStringLiteral("from_session")).toString();
    if (to.isEmpty())
        return refuse("bad_args",
                      QStringLiteral("session_message: op:\"send\" needs a "
                                     "`to` export_slug"));
    const bool dedupe = c.req.value(QStringLiteral("dedupe")).toBool(false);
    if (to == QLatin1String("*")) {
        if (dedupe)
            return refuse("bad_args",
                          QStringLiteral("session_message: `dedupe` applies to one "
                                         "recipient, not to:\"*\""));
        return sendBroadcast(c, body, from);
    }
    if (c.dryRun)
        return sendPreview(c, to, body, from, dedupe);

    qint64 id = 0;
    bool deduplicated = false;
    QString code, err;
    const bool sent =
        dedupe ? c.store.sendMessageDeduped(c.self, to, body, from, c.now, &id,
                                            &deduplicated, &code, &err)
               : c.store.sendMessage(c.self, to, body, from, c.now, &id, &code, &err);
    if (!sent) {
        if (code == QLatin1String("unknown_project"))
            return refuseUnknownProject(c.store, to,
                                        QStringLiteral("session_message: %1").arg(err));
        return refuseStore(code, err);
    }
    c.out[QStringLiteral("message_id")] = double(id);
    c.out[QStringLiteral("to")]         = to;
    if (deduplicated) {
        // Nothing was written, so there is no new created_at and no prune.
        c.out[QStringLiteral("deduplicated")] = true;
        return QJsonDocument(c.out);
    }
    c.pruneAfterWrite();
    c.out[QStringLiteral("created_at")] = c.now;
    return QJsonDocument(c.out);
}

// ANTS-6720 — every registered project, so a caller outside Claude (a stats
// server) can map a GitHub repo to the export_slug `to` takes. A read.
QJsonDocument opProjects(MailCall &c) {
    QString err;
    const QVector<RoadmapStore::ProjectRow> rows = c.store.listProjects(&err);
    if (!err.isEmpty())
        return refuseStore(QString(), err);
    QJsonArray arr;
    for (const auto &p : rows) {
        QJsonObject o;
        o[QStringLiteral("export_slug")] = p.exportSlug;
        o[QStringLiteral("name")]        = p.name;
        o[QStringLiteral("root")]        = p.root;
        // Absent rather than null when the origin is not on GitHub.
        if (const QString gh = GitOrigin::githubRepo(p.root); !gh.isEmpty())
            o[QStringLiteral("github_repo")] = gh;
        // ANTS-6726 — every GitHub remote, so a fork whose origin is the
        // upstream still matches. Absent when there is none.
        if (const QStringList all = GitOrigin::githubRepos(p.root); !all.isEmpty())
            o[QStringLiteral("github_repos")] = QJsonArray::fromStringList(all);
        arr.append(o);
    }
    c.out[QStringLiteral("projects")] = arr;
    c.out[QStringLiteral("count")]    = arr.size();
    return QJsonDocument(c.out);
}

// ANTS-5098 — range-checked before the cast. A fractional, negative or
// out-of-range double cast to qint64 is undefined behaviour or names a
// different row. Shared by ack and by inbox's single-message read. 0 when
// absent; nullopt when present and bad.
std::optional<qint64> messageIdArg(const QJsonObject &req) {
    if (!req.contains(QStringLiteral("message_id")))
        return 0;
    const double raw = req.value(QStringLiteral("message_id")).toDouble(-1);
    if (!(raw >= 1.0 && raw <= 9007199254740992.0) || raw != std::floor(raw))
        return std::nullopt;
    return qint64(raw);
}

QJsonDocument opAck(MailCall &c, qint64 id) {
    if (id == 0)
        return refuse("bad_args",
                      QStringLiteral("session_message: op:\"ack\" needs a "
                                     "`message_id`"));
    bool already = false;
    QString code, err;
    if (!c.store.ackMessage(c.self, id, c.now, &already, &code, &err))
        return refuseStore(code, err);
    c.pruneAfterWrite();
    c.out[QStringLiteral("message_id")]    = double(id);
    c.out[QStringLiteral("already_acked")] = already;
    if (!already)
        c.out[QStringLiteral("acked_at")] = c.now;
    return QJsonDocument(c.out);
}

// `id` 0 reads a page of the inbox; otherwise that one message.
QJsonDocument opInbox(MailCall &c, qint64 id) {
    QVector<RoadmapStore::Message> rows;
    int unacked = 0;
    QString err;
    if (id != 0) {
        // ANTS-5601 — one message by id, acked or not, so a note citing
        // "mailbox 295" costs one read rather than a paged scan.
        RoadmapStore::Message m;
        QString code;
        if (!c.store.messageFor(c.self, id, &m, &code, &err))
            return refuseStore(code, err);
        rows.append(m);
        if (!c.store.inboxFor(c.self, false, 0, 0, nullptr, &unacked, &err))
            return refuseStore(QString(), err);
    } else {
        const bool includeAcked =
            c.req.value(QStringLiteral("include_acked")).toBool(false);
        // ANTS-5098 bounds the page; ANTS-5591 maps 0 or less to that bound.
        const int limit = RoadmapStore::inboxPageLimit(
            c.req.value(QStringLiteral("limit")).toInt(20));
        const int offset = qMax(0, c.req.value(QStringLiteral("offset")).toInt(0));
        if (!c.store.inboxFor(c.self, includeAcked, limit, offset, &rows, &unacked, &err))
            return refuseStore(QString(), err);
    }

    // ANTS-5699 — a page lists subject lines; one message by id, or
    // include_body, returns bodies.
    const bool wholeBody =
        id != 0 || c.req.value(QStringLiteral("include_body")).toBool(false);
    QJsonArray arr;
    int omitted = 0;
    for (const auto &m : rows) {
        const QJsonObject row = messageToJson(m, wholeBody);
        if (!row.contains(QStringLiteral("body"))) ++omitted;
        arr.append(row);
    }
    c.out[QStringLiteral("messages")]      = arr;
    c.out[QStringLiteral("unacked_count")] = unacked;
    c.out[QStringLiteral("count")]         = arr.size();
    if (omitted > 0) {
        c.out[QStringLiteral("bodies_omitted")] = omitted;
        c.out[QStringLiteral("bodies_hint")] = QStringLiteral(
            "a row with `subject` has more text: pass include_body:true for "
            "every body, or message_id for one");
    }
    return QJsonDocument(c.out);
}

// ANTS-5255 — the mail this project sent. A read, so it never prunes. `to` is an
// optional filter; an unknown slug refuses, because an empty list would read as
// "nothing was sent there".
QJsonDocument opOutbox(MailCall &c) {
    const QString to = c.req.value(QStringLiteral("to")).toString();
    qint64 toProject = 0;
    if (!to.isEmpty()) {
        const auto id = c.store.projectIdForSlug(to);
        if (!id)
            return refuseUnknownProject(c.store, to,
                          QStringLiteral("no project is registered under "
                                         "export_slug \"%1\"").arg(to));
        toProject = *id;
    }
    const int limit = RoadmapStore::inboxPageLimit(
        c.req.value(QStringLiteral("limit")).toInt(20));
    const int offset = qMax(0, c.req.value(QStringLiteral("offset")).toInt(0));

    QVector<RoadmapStore::SentMessage> rows;
    int unacked = 0;
    QString err;
    if (!c.store.outboxFor(c.self, toProject, limit, offset, &rows, &unacked, &err))
        return refuseStore(QString(), err);

    QJsonArray arr;
    for (const auto &m : rows) {
        QJsonObject o;
        o[QStringLiteral("message_id")] = double(m.id);
        o[QStringLiteral("to")]         = m.toSlug;
        o[QStringLiteral("created_at")] = m.createdAt;
        o[QStringLiteral("preview")]    = m.preview;
        if (!m.ackedAt.isEmpty())
            o[QStringLiteral("acked_at")] = m.ackedAt;
        arr.append(o);
    }
    c.out[QStringLiteral("messages")]      = arr;
    c.out[QStringLiteral("unacked_count")] = unacked;
    c.out[QStringLiteral("count")]         = arr.size();
    return QJsonDocument(c.out);
}

}  // namespace

QJsonDocument RemoteControl::cmdSessionMessage(const QJsonObject &req) {
    // caller_cwd absent is the dispatcher's refusal (CallerCwdContract::
    // Required, ANTS-1404), so the only case here is a path that does not
    // resolve to a directory.
    QJsonObject refusal;
    const QString cwd =
        ants::requiredCallerCwd(m_roots, req, QStringLiteral("session_message"), &refusal);
    if (cwd.isEmpty())
        return QJsonDocument(refusal);

    const QString op = req.value(QStringLiteral("op")).toString(
        QStringLiteral("inbox"));
    if (op != QStringLiteral("send") && op != QStringLiteral("inbox")
        && op != QStringLiteral("outbox") && op != QStringLiteral("ack")
        && op != QStringLiteral("projects"))
        return refuse("bad_args",
                      QStringLiteral("session_message: unknown op \"%1\" — "
                                     "expected send, inbox, outbox, ack or "
                                     "projects").arg(op));

    RoadmapStore store(RoadmapStore::defaultPath());
    QString err;
    if (!store.open(&err))
        return refuse("io_error",
                      QStringLiteral("session_message: cannot open the roadmap "
                                     "store: %1").arg(err));

    // § 2.3 — every op requires the CALLER's project to be registered.
    // `from_project_id` is NOT NULL and inbox/ack resolve the caller the same
    // way, so this is one condition with one code. Deliberately NOT
    // registerProject(): that is upsert-shaped and would silently register any
    // root handed to it, which is the machine-global hazard ANTS-4600 guards.
    // ANTS-5594 — the nearest registered root at or above caller_cwd, so a
    // session in a project's subdirectory still reaches that project's mail.
    // ANTS-5667 — an outer folder declaring `repo_root` asks for its clone.
    const auto self =
        store.projectIdContaining(ProjectSettings::repoRootOf(cwd), &err);
    if (!self)
        return refuse("no_project",
                      QStringLiteral("session_message: \"%1\" is not inside a "
                                     "project registered in the roadmap store, "
                                     "so it has no mailbox. Run roadmap_migrate "
                                     "first.")
                          .arg(cwd));

    MailCall c{store, *self, req,
               QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ssZ")),
               req.value(QStringLiteral("dry_run")).toBool(false),
               QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("op"), op}}};
    if (op == QStringLiteral("send"))
        return opSend(c);
    if (op == QStringLiteral("outbox"))
        return opOutbox(c);
    if (op == QStringLiteral("projects"))
        return opProjects(c);
    const std::optional<qint64> id = messageIdArg(req);
    if (!id)
        return refuse("bad_args",
                      QStringLiteral("session_message: `message_id` must be a "
                                     "positive integer"));
    return op == QStringLiteral("ack") ? opAck(c, *id) : opInbox(c, *id);
}
