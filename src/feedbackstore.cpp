// ANTS-4513 — the feedback corpus in the roadmap store. See feedbackstore.h.
#include "feedbackstore.h"

#include "feedbackfile.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace FeedbackStore {

namespace {

bool g_forceExportFail = false;

QString nowUtc() {
    return QDateTime::currentDateTimeUtc().toString(
        QStringLiteral("yyyy-MM-dd'T'HH:mm:ss'Z'"));
}

QString sha256Hex(const QByteArray &bytes) {
    return QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

Result refusal(const QString &code, const QString &message) {
    Result r;
    r.code    = code;
    r.message = message;
    return r;
}

// Rolls back whatever begin() opened, on every return that is not a commit.
Result abort(RoadmapStore &store, Result r) {
    store.rollback();
    return r;
}

Result storeFailed(const QString &err) {
    return refusal(QStringLiteral("store_failed"),
                   QStringLiteral("the roadmap store refused: %1").arg(err));
}

// § 4.3 — the file's bytes as text, or the refusal. Read binary: a text-mode
// read drops '\r', and INV-1 needs the bytes back exactly.
Result readForImport(const QString &path, QByteArray *bytes, QString *text) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return refusal(QStringLiteral("read_failed"),
                       QStringLiteral("cannot open \"%1\"").arg(path));
    *bytes = f.readAll();
    *text  = QString::fromUtf8(*bytes);
    // Invalid UTF-8 and a leading byte-order mark both fail this round trip;
    // QString::fromUtf8 drops the mark.
    if (text->toUtf8() != *bytes)
        return refusal(QStringLiteral("not_utf8"),
                       QStringLiteral("\"%1\" is not plain UTF-8 (invalid bytes "
                                      "or a byte-order mark); it was not "
                                      "imported").arg(path));
    if (FeedbackFile::markerVersion(*text) < 2)
        return refusal(QStringLiteral("not_v2"),
                       QStringLiteral("\"%1\" has no `<!-- ants-mcp-feedback: 2 "
                                      "-->` marker; only v2 files import")
                           .arg(path));
    return {};
}

// § 4.3 — import inside the caller's open transaction.
Result importRow(RoadmapStore &store, const QString &key, const QString &path,
                 RoadmapStore::FeedbackReporter *row) {
    QByteArray bytes;
    QString text;
    if (Result r = readForImport(path, &bytes, &text); !r.code.isEmpty())
        return r;
    QString err;
    const auto id = store.insertFeedbackReporter(key, text, sha256Hex(bytes),
                                                 nowUtc(), rowsFor(text), &err);
    if (!id) return storeFailed(err);
    row->reporterId   = *id;
    row->exportPath   = key;
    row->content      = text;
    row->revision     = 1;
    row->exportSha256 = sha256Hex(bytes);
    return {};
}

// § 4.4's file half: owner-write back first (QSaveFile refuses a read-only
// target), the bytes through QSaveFile, then 0444.
bool writeExport(const QString &path, const QString &text) {
    const bool existed = QFileInfo::exists(path);
    const QFileDevice::Permissions before =
        existed ? QFile::permissions(path) : QFileDevice::Permissions();
    if (existed) QFile::setPermissions(path, before | QFileDevice::WriteOwner);
    const QByteArray utf8 = text.toUtf8();
    QSaveFile sf(path);
    bool ok = sf.open(QIODevice::WriteOnly) && sf.write(utf8) == utf8.size();
    if (ok && g_forceExportFail) {
        sf.cancelWriting();
        ok = false;
    }
    if (ok) ok = sf.commit();
    if (!ok) {
        if (existed) QFile::setPermissions(path, before);
        return false;
    }
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::ReadGroup
                                    | QFileDevice::ReadOther);
    return true;
}

bool fileDrifted(const QString &path, const QString &exportSha) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return true;
    return sha256Hex(f.readAll()) != exportSha;
}

}  // namespace

QString exportKey(const QString &filePath) {
    const QFileInfo fi(filePath);
    const QString dir = fi.absolutePath();
    const QString canon = QFileInfo(dir).canonicalFilePath();
    return (canon.isEmpty() ? dir : canon) + QLatin1Char('/') + fi.fileName();
}

QVector<RoadmapStore::FeedbackFindingRow> rowsFor(const QString &content) {
    const FeedbackFile::ParseResult pr = FeedbackFile::parse(content);
    QVector<RoadmapStore::FeedbackFindingRow> rows;
    rows.reserve(pr.findings.size());
    for (const FeedbackFile::FindingRecord &f : pr.findings) {
        RoadmapStore::FeedbackFindingRow r;
        r.ordinal      = int(rows.size());
        r.heading      = f.heading;
        r.headingLine  = f.line;
        r.entryHeading = f.entryHeading;
        r.filedOn      = f.filedOn;
        r.triage       = f.triage;
        r.ids          = f.ids;
        rows.append(r);
    }
    return rows;
}

Result load(RoadmapStore &store, const QString &filePath) {
    const QString key = exportKey(filePath);
    QString err;
    auto row = store.feedbackReporter(key, &err);
    if (!row && !err.isEmpty()) return storeFailed(err);
    Result out;
    if (!row) {
        if (!QFileInfo::exists(filePath)) return out;   // found:false
        if (!store.begin(&err)) return storeFailed(err);
        // Re-read under the lock: another connection may have imported it
        // since (INV-8).
        row = store.feedbackReporter(key, &err);
        if (!row && !err.isEmpty()) return abort(store, storeFailed(err));
        if (!row) {
            RoadmapStore::FeedbackReporter fresh;
            if (Result r = importRow(store, key, filePath, &fresh); !r.code.isEmpty())
                return abort(store, r);
            row = fresh;
            out.imported = true;
        }
        if (!store.commit(&err)) return abort(store, storeFailed(err));
    }
    out.found   = true;
    out.content = row->content;
    out.reporterId = row->reporterId;
    out.drift   = QFileInfo::exists(filePath)
                  && fileDrifted(filePath, row->exportSha256);
    return out;
}

Result write(RoadmapStore &store, const QString &filePath,
             const QString &createContent, bool dryRun, const Edit &edit) {
    const QString key = exportKey(filePath);
    QString err;
    // INV-7 — the lock comes before the read, so two writers never edit the
    // same old text.
    if (!store.begin(&err)) return storeFailed(err);
    auto found = store.feedbackReporter(key, &err);
    if (!found && !err.isEmpty()) return abort(store, storeFailed(err));

    Result out;
    const bool fileExists = QFileInfo::exists(filePath);
    RoadmapStore::FeedbackReporter row;   // reporterId 0 = not yet inserted
    if (found) {
        row = *found;
        // A missing file is not drift: the write below recreates it.
        if (fileExists && fileDrifted(filePath, row.exportSha256))
            return abort(store, refusal(
                QStringLiteral("export_drift"),
                QStringLiteral("\"%1\" was edited outside the feedback verbs, so "
                               "it no longer matches the store. Nothing was "
                               "written. feedback_log op:\"reimport\" with "
                               "confirm:true keeps the file's version; deleting "
                               "the file keeps the store's.").arg(filePath)));
    } else if (fileExists) {
        if (Result r = importRow(store, key, filePath, &row); !r.code.isEmpty())
            return abort(store, r);
        out.imported = true;
    } else if (!createContent.isEmpty()) {
        row.content = createContent;
        out.created = true;
    } else {
        return abort(store, refusal(QStringLiteral("not_found"),
                                    QStringLiteral("\"%1\" does not exist")
                                        .arg(filePath)));
    }
    out.found = true;

    QString text;
    if (!edit(row.content, out.created, &text)) {
        Result r = refusal(QStringLiteral("edit_refused"), QString());
        return abort(store, r);
    }
    out.content = text;
    if (text == row.content || dryRun) {   // today's no-op skip; a preview
        out.changed = text != row.content;
        store.rollback();
        return out;
    }
    out.changed = true;

    const auto rows = rowsFor(text);
    const QString now = nowUtc();
    if (row.reporterId == 0) {
        const auto id = store.insertFeedbackReporter(key, text, QString(), now,
                                                     rows, &err);
        if (!id) return abort(store, storeFailed(err));
        row.reporterId = *id;
    } else if (!store.updateFeedbackContent(row.reporterId, text, now, rows, &err)) {
        return abort(store, storeFailed(err));
    }
    // Inside the transaction, so no second writer renders between this
    // commit and this file write.
    if (!writeExport(filePath, text))
        return abort(store, refusal(QStringLiteral("write_failed"),
                                    QStringLiteral("atomic write of \"%1\" failed")
                                        .arg(filePath)));
    if (!store.setFeedbackExportSha(row.reporterId, sha256Hex(text.toUtf8()), &err))
        return abort(store, storeFailed(err));
    if (!store.commit(&err))
        // § 6 — the file now holds text the store does not; the next write
        // sees drift and reimport recovers it.
        return abort(store, storeFailed(err));
    return out;
}

Result reimport(RoadmapStore &store, const QString &filePath, bool confirm) {
    const QString key = exportKey(filePath);
    if (!QFileInfo::exists(filePath))
        return refusal(QStringLiteral("not_found"),
                       QStringLiteral("\"%1\" does not exist; there is nothing "
                                      "to reimport").arg(filePath));
    QString err;
    if (!store.begin(&err)) return storeFailed(err);
    auto found = store.feedbackReporter(key, &err);
    if (!found && !err.isEmpty()) return abort(store, storeFailed(err));
    Result out;
    out.found = true;
    if (!found) {   // never imported: a plain first touch
        RoadmapStore::FeedbackReporter row;
        if (Result r = importRow(store, key, filePath, &row); !r.code.isEmpty())
            return abort(store, r);
        if (!store.commit(&err)) return abort(store, storeFailed(err));
        out.imported = out.changed = true;
        out.content  = row.content;
        return out;
    }
    QByteArray bytes;
    QString text;
    if (Result r = readForImport(filePath, &bytes, &text); !r.code.isEmpty())
        return abort(store, r);
    const QString sha = sha256Hex(bytes);
    if (sha == found->exportSha256) {   // no drift: nothing to settle
        out.content = found->content;
        store.rollback();
        return out;
    }
    if (!confirm) {
        Result r = refusal(QStringLiteral("confirm_required"),
                           QStringLiteral("reimport replaces the store's text "
                                          "with the file's; pass confirm:true"));
        r.storeBytes = found->content.toUtf8().size();
        r.fileBytes  = bytes.size();
        return abort(store, r);
    }
    if (!store.updateFeedbackContent(found->reporterId, text, nowUtc(),
                                     rowsFor(text), &err)
        || !store.setFeedbackExportSha(found->reporterId, sha, &err)
        || !store.commit(&err))
        return abort(store, storeFailed(err));
    out.changed = true;
    out.content = text;
    return out;
}

void setForceExportFailForTest(bool on) { g_forceExportFail = on; }

}  // namespace FeedbackStore
