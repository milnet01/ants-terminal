// ANTS-1352 — server-side `indie_review_dispatch` orchestrator.
//
// Architecture: a synchronous facade on top of an asynchronous
// QNetworkAccessManager. dispatchLanes() spins a local QEventLoop;
// the QNAM signals (finished/errorOccurred) feed it. Each finished
// reply pops the next-in-queue and starts it; the loop quits when
// the queue and the in-flight set are both empty.
//
// Threading (ANTS-2104): MCP QLocalSockets live on the MAIN thread,
// so a synchronous call here would spin the local QEventLoop ON the
// main thread — reentrantly delivering socket read-notifications and
// freeing a live MCP socket mid-sweep (the ANTS-2103 use-after-free
// class). The caller (mainwindow.cpp cmdIndieReviewDispatch) therefore
// runs dispatchLanes inside a QThread::create worker: the nam/loop are
// locals so they construct on the worker, and QThread::wait() joins
// without pumping events. Mirrors the AuditRunner/audit_run precedent.

#include "indiereviewdispatcher.h"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QQueue>
#include <QSaveFile>
#include <QUrl>

#include <atomic>

namespace IndieReviewDispatcher {

namespace {

// ANTS-1352 INV-9 — process-global in-flight counter. Atomic so
// inFlightCountForTest() is race-free across the dispatch thread
// and the probing test thread.
std::atomic<int> g_inFlightCount{0};

// ANTS-1352 INV-21 — redact-then-truncate. Order matters; see
// docs/specs/ANTS-1352.md § 3.5.
QString redactAndTruncate(QByteArray body, const QString &apiKey,
                          int maxBytes) {
    QString text = QString::fromUtf8(body);
    if (!apiKey.isEmpty()) {
        text.replace(apiKey, QStringLiteral("<redacted>"));
    }
    // ANTS-2119 — enforce the byte budget on the UTF-8 encoding, not on
    // QString::truncate's UTF-16 code-unit count (which under-counts multibyte
    // content, letting the reply exceed maxBytes). Trim any partial trailing
    // UTF-8 sequence so the re-decode stays clean (no U+FFFD tail).
    QByteArray utf8 = text.toUtf8();
    if (utf8.size() > maxBytes) {
        utf8.truncate(maxBytes);
        while (!utf8.isEmpty() &&
               (static_cast<unsigned char>(utf8.back()) & 0xC0) == 0x80) {
            utf8.chop(1);  // drop UTF-8 continuation bytes
        }
        if (!utf8.isEmpty() &&
            (static_cast<unsigned char>(utf8.back()) & 0x80)) {
            utf8.chop(1);  // drop the now-dangling lead byte
        }
        text = QString::fromUtf8(utf8);
        text += QStringLiteral("\n…<truncated>");
    }
    return text;
}

// ANTS-1352 INV-11 — usage-token precedence: OpenAI > Anthropic > 0.
void extractUsage(const QJsonObject &usage,
                  qint64 *inputTokens, qint64 *outputTokens) {
    if (usage.contains(QStringLiteral("prompt_tokens"))
        || usage.contains(QStringLiteral("completion_tokens"))) {
        *inputTokens  = usage.value(QStringLiteral("prompt_tokens"))
                            .toVariant().toLongLong();
        *outputTokens = usage.value(QStringLiteral("completion_tokens"))
                            .toVariant().toLongLong();
        return;
    }
    if (usage.contains(QStringLiteral("input_tokens"))
        || usage.contains(QStringLiteral("output_tokens"))) {
        *inputTokens  = usage.value(QStringLiteral("input_tokens"))
                            .toVariant().toLongLong();
        *outputTokens = usage.value(QStringLiteral("output_tokens"))
                            .toVariant().toLongLong();
        return;
    }
    *inputTokens = 0;
    *outputTokens = 0;
}

// ANTS-1352 INV-16 — append /v1/chat/completions if absent.
// Mirrors auditdialog.cpp:4673.
QUrl resolveEndpoint(const QString &raw) {
    QUrl u(raw);
    if (!u.path().contains(QStringLiteral("chat/completions"))) {
        const QString p = u.path();
        u.setPath((p.endsWith(QChar('/')) ? p : p + QChar('/'))
                  + QStringLiteral("v1/chat/completions"));
    }
    return u;
}

QByteArray buildRequestBody(const LaneRequest &lr,
                            const DispatchRequest &dr) {
    QJsonArray messages;
    messages.append(QJsonObject{
        {QStringLiteral("role"),    QStringLiteral("system")},
        {QStringLiteral("content"), dr.systemPrompt},
    });
    messages.append(QJsonObject{
        {QStringLiteral("role"),    QStringLiteral("user")},
        {QStringLiteral("content"), lr.brief},
    });
    QJsonObject body;
    body[QStringLiteral("model")]       = dr.model;
    body[QStringLiteral("max_tokens")]  = dr.maxTokens;
    body[QStringLiteral("temperature")] = 0.2;
    body[QStringLiteral("messages")]    = messages;
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

// Extract assistant text from an OpenAI Chat Completions response.
QString extractAssistantText(const QJsonObject &root) {
    const QJsonArray choices = root.value(QStringLiteral("choices"))
                                   .toArray();
    if (choices.isEmpty()) return QString();
    return choices.first().toObject()
                  .value(QStringLiteral("message")).toObject()
                  .value(QStringLiteral("content")).toString();
}

}  // namespace

int inFlightCountForTest() {
    return g_inFlightCount.load(std::memory_order_relaxed);
}

DispatchResult dispatchLanes(const DispatchRequest &req) {
    DispatchResult r;
    r.resolvedModel = req.model;

    QElapsedTimer total;
    total.start();

    // ANTS-1352 INV-15 — endpoint scheme check.
    const QUrl endpointUrl = resolveEndpoint(req.endpoint);
    const QString scheme = endpointUrl.scheme().toLower();
    if (scheme != QStringLiteral("http")
        && scheme != QStringLiteral("https")) {
        r.ok    = false;
        r.code  = QStringLiteral("bad_args");
        r.error = QStringLiteral(
            "indie_review_dispatch: endpoint scheme \"%1\" must be "
            "http or https").arg(scheme);
        return r;
    }

    if (req.lanes.isEmpty()) {
        // INV-18 — empty lanes is permitted by the spec (caller
        // says "all"); the MCP handler resolves "all" before
        // reaching here. An empty list at this point is a logic
        // error in the caller — flag.
        r.ok    = false;
        r.code  = QStringLiteral("no_lanes");
        r.error = QStringLiteral(
            "indie_review_dispatch: dispatcher received empty lane "
            "list (caller must resolve before invocation)");
        return r;
    }

    // ANTS-2119 — lane names become <reports_dir>/<name>.md; reject any name
    // that is not a single, safe path component so a crafted lane can't escape
    // reports_dir via a directory separator or '..' (path traversal). Defence
    // in depth: the MCP caller resolves lanes from the subsystem partition, but
    // validate at the path-construction boundary regardless.
    for (const LaneRequest &lr : req.lanes) {
        const QString &n = lr.name;
        if (n.isEmpty() || n == QLatin1String(".") || n == QLatin1String("..")
            || n.contains(QLatin1Char('/')) || n.contains(QLatin1Char('\\'))) {
            r.ok    = false;
            r.code  = QStringLiteral("bad_lane");
            r.error = QStringLiteral(
                "indie_review_dispatch: lane name \"%1\" must be a single "
                "path component (no directory separators or '..')").arg(n);
            return r;
        }
    }

    // INV-19 — ensure reports_dir exists.
    const QString absReportsDir = req.projectRoot + QChar('/')
                                  + req.reportsDir;
    {
        QDir d(absReportsDir);
        if (!d.exists() && !d.mkpath(QStringLiteral("."))) {
            r.ok    = false;
            r.code  = QStringLiteral("mkdir_failed");
            r.error = QStringLiteral(
                "indie_review_dispatch: cannot create reports_dir "
                "\"%1\"").arg(req.reportsDir);
            return r;
        }
    }

    // INV-7 — refuse if any planned lane output already exists.
    QStringList collisions;
    for (const LaneRequest &lr : req.lanes) {
        const QString abs = absReportsDir + QChar('/') + lr.name
                            + QStringLiteral(".md");
        if (QFileInfo::exists(abs)) collisions << lr.name;
    }
    if (!collisions.isEmpty()) {
        r.ok    = false;
        r.code  = QStringLiteral("plan_exists");
        r.error = QStringLiteral(
            "indie_review_dispatch: lane output(s) already exist "
            "under reports_dir: %1 (delete or pick a fresh dir)")
                .arg(collisions.join(QStringLiteral(", ")));
        return r;
    }

    // Per-lane LaneResult skeletons + queue.
    QHash<QNetworkReply *, int> replyToIdx;
    QHash<QNetworkReply *, QElapsedTimer> replyTimer;
    r.reports.reserve(req.lanes.size());
    QQueue<int> queue;
    for (int i = 0; i < req.lanes.size(); ++i) {
        LaneResult lr;
        lr.name = req.lanes[i].name;
        r.reports.append(lr);

        // INV-14 — pre-flight prompt-size budget. Rough 4 chars/token.
        const qint64 promptBytes = req.lanes[i].brief.toUtf8().size();
        const qint64 budgetBytes = qint64(175000 - req.maxTokens) * 4;
        if (promptBytes > budgetBytes) {
            r.reports[i].status = QStringLiteral("prompt_too_large");
            r.reports[i].error  = QStringLiteral(
                "brief is %1 bytes; budget is %2 bytes "
                "(175000 - max_tokens, 4 chars/token)")
                    .arg(promptBytes).arg(budgetBytes);
            continue;
        }
        queue.enqueue(i);
    }

    if (queue.isEmpty()) {
        // Everything was prompt_too_large; nothing to dispatch.
        r.totalElapsedMs = total.elapsed();
        return r;
    }

    QNetworkAccessManager nam;
    QEventLoop loop;

    auto startReply = [&](int idx) {
        const LaneRequest &lr = req.lanes[idx];
        QNetworkRequest nreq(endpointUrl);
        nreq.setHeader(QNetworkRequest::ContentTypeHeader,
                       QStringLiteral("application/json"));
        if (!req.apiKey.isEmpty()) {
            // INV-5 — apiKey only appears here (Authorization
            // header). No qDebug / qInfo / qWarning sees it.
            nreq.setRawHeader("Authorization",
                              ("Bearer " + req.apiKey).toUtf8());
        }
        // INV-8 — per-lane timeout.
        nreq.setTransferTimeout(req.perLaneTimeoutMs);
        const QByteArray body = buildRequestBody(lr, req);
        QNetworkReply *reply = nam.post(nreq, body);
        replyToIdx[reply] = idx;
        QElapsedTimer t;
        t.start();
        replyTimer[reply] = t;
        g_inFlightCount.fetch_add(1, std::memory_order_relaxed);
    };

    int active = 0;
    const int cap = qBound(1, req.concurrency, 8);
    auto pumpQueue = [&]() {
        while (active < cap && !queue.isEmpty()) {
            startReply(queue.dequeue());
            ++active;
        }
    };

    auto onReplyFinished = [&](QNetworkReply *reply) {
        const int idx = replyToIdx.take(reply);
        const qint64 elapsedMs = replyTimer.take(reply).elapsed();
        g_inFlightCount.fetch_sub(1, std::memory_order_relaxed);
        --active;

        LaneResult &lr = r.reports[idx];
        lr.elapsedMs = elapsedMs;

        const QNetworkReply::NetworkError err = reply->error();
        const int httpStatus = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QByteArray body = reply->readAll();
        reply->deleteLater();

        if (err == QNetworkReply::OperationCanceledError) {
            // setTransferTimeout fires this.
            lr.status = QStringLiteral("timeout");
            lr.error  = QStringLiteral("transfer timeout after %1 ms")
                            .arg(req.perLaneTimeoutMs);
        } else if (err != QNetworkReply::NoError && httpStatus == 0) {
            lr.status = QStringLiteral("network_error");
            lr.error  = reply->errorString();
        } else if (httpStatus >= 400) {
            lr.status = QStringLiteral("http_error");
            lr.error  = QStringLiteral("HTTP %1: %2")
                            .arg(httpStatus)
                            .arg(redactAndTruncate(
                                body, req.apiKey, 4 * 1024));
        } else {
            // Parse the response.
            const QJsonDocument doc = QJsonDocument::fromJson(body);
            if (!doc.isObject()) {
                lr.status = QStringLiteral("http_error");
                lr.error  = QStringLiteral(
                    "response not a JSON object: %1")
                        .arg(redactAndTruncate(
                            body, req.apiKey, 1024));
            } else {
                const QJsonObject root = doc.object();
                const QString assistantText = extractAssistantText(root);
                if (assistantText.isEmpty()) {
                    lr.status = QStringLiteral("http_error");
                    lr.error  = QStringLiteral(
                        "no choices[0].message.content in response");
                } else {
                    extractUsage(root.value(QStringLiteral("usage"))
                                     .toObject(),
                                 &lr.inputTokens, &lr.outputTokens);
                    // INV-6 — atomic write via QSaveFile.
                    const QString outPath = absReportsDir + QChar('/')
                                            + lr.name
                                            + QStringLiteral(".md");
                    QSaveFile f(outPath);
                    if (!f.open(QIODevice::WriteOnly
                                | QIODevice::Truncate)) {
                        lr.status = QStringLiteral("write_failed");
                        lr.error  = QStringLiteral(
                            "open(%1): %2").arg(outPath, f.errorString());
                    } else {
                        const QByteArray utf8 = assistantText.toUtf8();
                        if (f.write(utf8) != utf8.size()
                            || !f.commit()) {
                            lr.status = QStringLiteral("write_failed");
                            lr.error  = QStringLiteral(
                                "write/commit failed: %1")
                                    .arg(redactAndTruncate(
                                        assistantText.toUtf8(),
                                        req.apiKey, 4 * 1024));
                        } else {
                            lr.status = QStringLiteral("ok");
                            lr.path   = req.reportsDir + QChar('/')
                                        + lr.name
                                        + QStringLiteral(".md");
                            lr.bytes  = utf8.size();
                        }
                    }
                }
            }
        }

        // Pump the next-in-queue, or quit if drained.
        pumpQueue();
        if (active == 0 && queue.isEmpty()) loop.quit();
    };

    QObject::connect(&nam, &QNetworkAccessManager::finished,
                     &loop, onReplyFinished);

    pumpQueue();
    if (active == 0) {
        // Nothing dispatched (all prompt_too_large pre-flight; we
        // already early-returned above for empty queue — defensive).
        r.totalElapsedMs = total.elapsed();
        return r;
    }
    loop.exec();

    r.totalElapsedMs = total.elapsed();
    return r;
}

}  // namespace IndieReviewDispatcher
