// ANTS-4622 TU 17/17 — the `session_message` handler: the cross-session mailbox's verb.
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

#include "resolvedroot.h"
#include "roadmapstore.h"

#include <QDateTime>
#include <QDir>
#include <QJsonArray>

namespace {

QJsonDocument refuse(const char *code, const QString &message) {
    QJsonObject e;
    e[QStringLiteral("ok")]    = false;
    e[QStringLiteral("code")]  = QString::fromLatin1(code);
    e[QStringLiteral("error")] = message;
    return QJsonDocument(e);
}

QJsonObject messageToJson(const RoadmapStore::Message &m) {
    QJsonObject o;
    o[QStringLiteral("message_id")]   = double(m.id);
    o[QStringLiteral("from")]         = m.fromSlug;
    o[QStringLiteral("from_session")] = m.fromSession;
    o[QStringLiteral("created_at")]   = m.createdAt;
    o[QStringLiteral("body")]         = m.body;
    // Absent rather than null when unread: `acked_at` present IS the read
    // state, so a null would make a client test the value instead of the key.
    if (!m.ackedAt.isEmpty())
        o[QStringLiteral("acked_at")] = m.ackedAt;
    return o;
}

}  // namespace

QJsonDocument RemoteControl::cmdSessionMessage(const QJsonObject &req) {
    // caller_cwd absent is the dispatcher's refusal (CallerCwdContract::
    // Required, ANTS-1404), so the only case here is a path that does not
    // resolve to a directory.
    const QString callerRaw = req.value(QStringLiteral("caller_cwd")).toString();
    const ants::ResolvedRoot rr = ants::resolveCallerCwdRoot(m_main, callerRaw);
    if (rr.source == ants::ResolvedRoot::Source::Unresolvable)
        return refuse("no_project",
                      QStringLiteral("session_message: caller_cwd \"%1\" does "
                                     "not resolve to a directory").arg(callerRaw));

    const QString op = req.value(QStringLiteral("op")).toString(
        QStringLiteral("inbox"));
    if (op != QStringLiteral("send") && op != QStringLiteral("inbox")
        && op != QStringLiteral("ack"))
        return refuse("bad_args",
                      QStringLiteral("session_message: unknown op \"%1\" — "
                                     "expected send, inbox or ack").arg(op));

    RoadmapStore store(RoadmapStore::defaultPath());
    QString err;
    if (!store.open(&err))
        return refuse("io_error",
                      QStringLiteral("session_message: cannot open the roadmap "
                                     "store: %1").arg(err));

    // § 2.3 — all three ops require the CALLER's project to be registered.
    // `from_project_id` is NOT NULL and inbox/ack resolve the caller the same
    // way, so this is one condition with one code. Deliberately NOT
    // registerProject(): that is upsert-shaped and would silently register any
    // root handed to it, which is the machine-global hazard ANTS-4600 guards.
    const auto self = store.projectIdForRoot(rr.cwd, &err);
    if (!self)
        return refuse("no_project",
                      QStringLiteral("session_message: \"%1\" is not registered "
                                     "in the roadmap store, so it has no "
                                     "mailbox. Run roadmap_migrate first.")
                          .arg(rr.cwd));

    // Read once, so every stamp this call writes agrees.
    const QString now = QDateTime::currentDateTimeUtc()
                            .toString(QStringLiteral("yyyy-MM-ddTHH:mm:ssZ"));

    // § 2.6 — the prune fires on ops that ALREADY write, never on a read and
    // never on a dry run. A dry_run send is an op too, and INV-10 forbids a
    // preview from writing anything.
    const bool dryRun = req.value(QStringLiteral("dry_run")).toBool(false);
    const auto pruneIfWriting = [&] {
        if (op == QStringLiteral("inbox") || dryRun)
            return;
        const QString cutoff =
            QDateTime::currentDateTimeUtc()
                .addDays(-RoadmapStore::kMailAckedTtlDays)
                .toString(QStringLiteral("yyyy-MM-ddTHH:mm:ssZ"));
        QString ignored;
        store.pruneAckedMail(*self, cutoff, &ignored);
    };

    QJsonObject out;
    out[QStringLiteral("ok")] = true;
    out[QStringLiteral("op")] = op;

    if (op == QStringLiteral("send")) {
        const QString to   = req.value(QStringLiteral("to")).toString();
        const QString body = req.value(QStringLiteral("body")).toString();
        const QString from = req.value(QStringLiteral("from_session")).toString();
        if (to.isEmpty())
            return refuse("bad_args",
                          QStringLiteral("session_message: op:\"send\" needs a "
                                         "`to` export_slug"));

        if (dryRun) {
            // ANTS-4463 — a preview carries NO past-tense field: `would_send`
            // rather than `message_id`, so a caller branching on either cannot
            // read a preview as a completed write.
            //
            // It runs the same two validations the real call runs — resolve the
            // recipient, then check the body against the byte cap — and stops
            // before the insert. Nothing here writes, and § 2.6's prune is
            // skipped for the same reason (see pruneIfWriting above), so a
            // preview cannot destroy aged mail to measure itself.
            if (!store.projectIdForSlug(to))
                return refuse("unknown_project",
                              QStringLiteral("no project is registered under "
                                             "export_slug \"%1\", so mail to it "
                                             "could never be delivered").arg(to));
            const int bodyBytes = body.toUtf8().size();
            if (bodyBytes == 0 || bodyBytes > RoadmapStore::kMailBodyMaxBytes)
                return refuse("bad_args",
                              QStringLiteral("message body is %1 bytes; the cap "
                                             "is %2").arg(bodyBytes)
                                  .arg(RoadmapStore::kMailBodyMaxBytes));
            out[QStringLiteral("dry_run")]    = true;
            out[QStringLiteral("would_send")] = true;
            out[QStringLiteral("to")]         = to;
            return QJsonDocument(out);
        }

        qint64 id = 0;
        QString code;
        if (!store.sendMessage(*self, to, body, from, now, &id, &code, &err))
            return refuse(code.isEmpty() ? "io_error" : code.toLatin1().constData(),
                          QStringLiteral("session_message: %1").arg(err));
        pruneIfWriting();
        out[QStringLiteral("message_id")] = double(id);
        out[QStringLiteral("to")]         = to;
        out[QStringLiteral("created_at")] = now;
        return QJsonDocument(out);
    }

    if (op == QStringLiteral("ack")) {
        if (!req.contains(QStringLiteral("message_id")))
            return refuse("bad_args",
                          QStringLiteral("session_message: op:\"ack\" needs a "
                                         "`message_id`"));
        const qint64 id = qint64(req.value(QStringLiteral("message_id")).toDouble());
        bool already = false;
        QString code;
        if (!store.ackMessage(*self, id, now, &already, &code, &err))
            return refuse(code.isEmpty() ? "io_error" : code.toLatin1().constData(),
                          QStringLiteral("session_message: %1").arg(err));
        pruneIfWriting();
        out[QStringLiteral("message_id")]    = double(id);
        out[QStringLiteral("already_acked")] = already;
        if (!already)
            out[QStringLiteral("acked_at")] = now;
        return QJsonDocument(out);
    }

    // op == "inbox"
    const bool includeAcked =
        req.value(QStringLiteral("include_acked")).toBool(false);
    const int limit  = req.contains(QStringLiteral("limit"))
                           ? req.value(QStringLiteral("limit")).toInt(20) : 20;
    const int offset = req.value(QStringLiteral("offset")).toInt(0);

    QVector<RoadmapStore::Message> rows;
    int unacked = 0;
    if (!store.inboxFor(*self, includeAcked, limit, offset, &rows, &unacked, &err))
        return refuse("io_error", QStringLiteral("session_message: %1").arg(err));

    QJsonArray arr;
    for (const auto &m : rows)
        arr.append(messageToJson(m));
    out[QStringLiteral("messages")]      = arr;
    out[QStringLiteral("unacked_count")] = unacked;
    out[QStringLiteral("count")]         = arr.size();
    return QJsonDocument(out);
}
