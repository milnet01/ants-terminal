// ANTS-3636 / ANTS-3654 — the fixture and response accessors shared by the
// DocCitations::check test files.
//
// Hoisted out of test_doc_citations.cpp when ANTS-3654 added a second file
// against the same engine. Shared rather than copied because `Fixture::root`
// is CANONICAL on purpose (see below): a second copy that dropped the
// canonicalisation would not fail loudly — it would make every in-root
// citation look like a root escape, and the anchor tests would then pass or
// fail for a reason that has nothing to do with anchors.
//
// Everything here is `inline`: both TUs compile into the same test bundle.

#pragma once

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

namespace doccit_test {

// A throwaway project root. `root` is canonical because gate G compares
// canonical paths, and /tmp is a symlink on some distributions — an
// uncanonicalised root would make every in-root citation look like an escape.
struct Fixture {
    QTemporaryDir dir;
    QString root;

    Fixture() : root(QFileInfo(dir.path()).canonicalFilePath()) {}

    QString write(const QString &rel, const QByteArray &content) const {
        const QString abs = root + QLatin1Char('/') + rel;
        QDir().mkpath(QFileInfo(abs).path());
        QFile f(abs);
        if (!f.open(QIODevice::WriteOnly)) return QString();
        f.write(content);
        f.close();
        return abs;
    }

    QString doc(const QByteArray &content) const { return write(QStringLiteral("doc.md"), content); }
};

inline QJsonArray cites(const QJsonObject &r) {
    return r.value(QStringLiteral("citations")).toArray();
}
inline QJsonArray unparsed(const QJsonObject &r) {
    return r.value(QStringLiteral("unparsed")).toArray();
}

inline QJsonObject cite(const QJsonObject &r, int i) { return cites(r).at(i).toObject(); }

inline QString status(const QJsonObject &r, int i) {
    return cite(r, i).value(QStringLiteral("status")).toString();
}

// Failure detail: the whole envelope, so a red line says what actually came
// back rather than only which predicate failed.
inline QString render(const QJsonObject &r) {
    return QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Compact));
}

inline QStringList textOf(const QJsonObject &c) {
    QStringList out;
    for (const QJsonValue &v : c.value(QStringLiteral("text")).toArray()) out << v.toString();
    return out;
}

}  // namespace doccit_test
