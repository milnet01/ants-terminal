// ANTS-5734 — see queries.h.

#include "trace/queries.h"

#include "trace/languages.h"

#include <tree_sitter/api.h>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>

namespace trace {

namespace {

struct Compiled {
    QByteArray hash;                       // of the text compiled
    std::shared_ptr<const TSQuery> query;
    QString error;
};

// A user file as last read: re-read only when its mtime or size moves.
struct UserFile {
    qint64 mtimeMs = -1;
    qint64 size = -1;
    QByteArray text;
};

QMutex g_mutex;

// Key: language id. ANTS-6504 — one entry per language: a new text replaces
// the last, whose query is freed once no caller's Query holds it. Heap-held
// and never destroyed, so the current queries stay reachable at exit: a static
// table is torn down before LeakSanitizer looks, which orphans every TSQuery
// in it. Not freed at exit either, since a worker thread may still be
// matching with one.
QHash<QString, Compiled> &compiled() {
    static auto *table = new QHash<QString, Compiled>;
    return *table;
}

QHash<QString, UserFile> g_userFiles;     // key: absolute path

QString limitsOf(const QByteArray &text) {
    static const QByteArray tag = ";; limits:";
    for (const QByteArray &line : text.split('\n')) {
        const QByteArray t = line.trimmed();
        if (t.startsWith(tag)) return QString::fromUtf8(t.mid(tag.size()).trimmed());
    }
    return {};
}

const char *queryErrorName(TSQueryError e) {
    switch (e) {
    case TSQueryErrorSyntax:    return "syntax error";
    case TSQueryErrorNodeType:  return "unknown node type";
    case TSQueryErrorField:     return "unknown field";
    case TSQueryErrorCapture:   return "unknown capture";
    case TSQueryErrorStructure: return "impossible pattern";
    case TSQueryErrorLanguage:  return "language mismatch";
    case TSQueryErrorNone:      break;
    }
    return "error";
}

// The user copy's text when one exists, re-read on an mtime or size change.
// Caller holds g_mutex.
bool readUserCopy(const QString &path, QByteArray *text) {
    const QFileInfo fi(path);
    if (!fi.isFile()) {
        g_userFiles.remove(path);
        return false;
    }
    UserFile &u = g_userFiles[path];
    const qint64 mtime = fi.lastModified().toMSecsSinceEpoch();
    if (u.mtimeMs != mtime || u.size != fi.size()) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return false;
        u.text = f.readAll();
        u.mtimeMs = mtime;
        u.size = fi.size();
    }
    *text = u.text;
    return true;
}

}  // namespace

QString userQueryDir() {
    QString base = qEnvironmentVariable("XDG_DATA_HOME");
    if (base.isEmpty()) base = QDir::homePath() + QStringLiteral("/.local/share");
    return base + QStringLiteral("/ants-terminal/trace-queries");
}

QByteArray builtinQueryText(const QString &langId) {
    QFile f(QStringLiteral(":/trace-queries/%1.scm").arg(langId));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

Query queryFor(const QString &langId) {
    Query out;
    const TSLanguage *lang = languageFor(langId);
    if (!lang) {
        out.error = QStringLiteral("no grammar");
        return out;
    }
    QMutexLocker lock(&g_mutex);
    QByteArray text;
    const QString userPath = userQueryDir() + QLatin1Char('/') + langId +
                             QStringLiteral(".scm");
    out.userCopy = readUserCopy(userPath, &text);
    if (!out.userCopy) text = builtinQueryText(langId);
    if (text.isEmpty()) {
        out.error = QStringLiteral("no query file");
        return out;
    }
    out.hash = QCryptographicHash::hash(text, QCryptographicHash::Sha1).toHex();
    out.limits = limitsOf(text);
    Compiled &c = compiled()[langId];
    if (c.hash != out.hash) {
        c = Compiled{out.hash, {}, {}};
        uint32_t errOffset = 0;
        TSQueryError err = TSQueryErrorNone;
        TSQuery *q = ts_query_new(lang, text.constData(), uint32_t(text.size()),
                                  &errOffset, &err);
        if (q) c.query.reset(q, [](const TSQuery *p) { ts_query_delete(const_cast<TSQuery *>(p)); });
        if (!c.query) {
            const int line = int(text.left(errOffset).count('\n')) + 1;
            c.error = QStringLiteral("%1 query %2 at line %3%4")
                          .arg(langId, QString::fromLatin1(queryErrorName(err)))
                          .arg(line)
                          .arg(out.userCopy ? QStringLiteral(" of ") + userPath
                                            : QString());
        }
    }
    out.query = c.query;
    out.error = c.error;
    return out;
}

}  // namespace trace
