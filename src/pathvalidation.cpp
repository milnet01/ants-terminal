// ANTS-1295 — central cwd-anchor validator. See header for contract.
// The body is the production-tested logic previously sitting at file
// scope in remotecontrol.cpp (ANTS-1250); promoted here so every
// path-accepting MCP/IPC handler calls a single chokepoint.

#include "pathvalidation.h"

#include <QChar>
#include <QDir>
#include <QFileInfo>
#include <QLatin1Char>
#include <QStringLiteral>

namespace PathValidation {

namespace {

bool hasControlOrBackslash(const QString &s) {
    for (QChar c : s) {
        if (c.unicode() < 0x20 || c == QLatin1Char('\\')) return true;
    }
    return false;
}

QJsonObject makeErr(const QString &toolName, const QString &paramName,
                    const QString &reason) {
    QJsonObject o;
    o[QStringLiteral("ok")]    = false;
    o[QStringLiteral("error")] =
        QStringLiteral("%1: \"%2\" %3").arg(toolName, paramName, reason);
    o[QStringLiteral("code")]  = QStringLiteral("bad_path");
    return o;
}

}  // namespace

Check validatePath(const QString &rawPath,
                   const QString &rootCanonical,
                   const QString &toolName,
                   const QString &paramName,
                   bool allowOutsideRoot) {
    Check pc;
    if (rawPath.isEmpty()) return pc;

    const QString nfc = rawPath.normalized(QString::NormalizationForm_C);
    if (hasControlOrBackslash(nfc)) {
        pc.bad = true;
        pc.err = makeErr(toolName, paramName,
            QStringLiteral("contains control or backslash characters"));
        return pc;
    }

    QString joined;
    if (QFileInfo(nfc).isAbsolute()) {
        joined = nfc;
    } else {
        joined = QDir(rootCanonical).filePath(nfc);
    }
    const QString resolved = QFileInfo(joined).canonicalFilePath();

    // ANTS-1455 — allowOutsideRoot=true skips the project-root anchor
    // check but still applies the NFC + control-char + backslash +
    // canonicalisation pipeline above. `resolved` is still set when
    // the path canonicalises so callers can sanity-check existence.
    if (!allowOutsideRoot) {
        // If the path canonicalises, the canonical form is the source of
        // truth — symlink escapes from inside the tree to outside get
        // caught. If it doesn't canonicalise (e.g. a git pathspec for a
        // deleted file), apply the lexical fallback so non-existent
        // escapes still reject.
        if (!resolved.isEmpty()) {
            if (!(resolved == rootCanonical ||
                  resolved.startsWith(rootCanonical + QLatin1Char('/')))) {
                pc.bad = true;
                pc.err = makeErr(toolName, paramName,
                    QStringLiteral("escapes project root"));
                return pc;
            }
            pc.resolved = resolved;
        } else {
            const QString cleaned = QDir::cleanPath(joined);
            if (!(cleaned == rootCanonical ||
                  cleaned.startsWith(rootCanonical + QLatin1Char('/')))) {
                pc.bad = true;
                pc.err = makeErr(toolName, paramName,
                    QStringLiteral("escapes project root"));
                return pc;
            }
        }
    } else {
        // allowOutsideRoot=true: caller accepts paths anywhere
        // readable. Still surface the canonical form when available.
        if (!resolved.isEmpty()) pc.resolved = resolved;
    }

    // ANTS-1250-INV-5: prefix `./` when leading char is `-` so the
    // value cannot be misread as a CLI flag when passed positionally.
    QString form = nfc;
    if (form.startsWith(QLatin1Char('-'))) {
        form = QStringLiteral("./") + form;
    }
    pc.argvForm = form;
    return pc;
}

}  // namespace PathValidation
