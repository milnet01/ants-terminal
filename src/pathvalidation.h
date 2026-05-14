// ANTS-1295 — central cwd-anchor validator for path-accepting tools.
// Every MCP/IPC handler that consumes a path-typed argument routes
// through PathValidation::validatePath before any filesystem call,
// so the "is this path inside the focused project root" check lives
// in one place. Spec: docs/specs/ANTS-1295.md.

#ifndef ANTS_PATHVALIDATION_H
#define ANTS_PATHVALIDATION_H

#include <QJsonObject>
#include <QString>

namespace PathValidation {

// Result of a single validation call.
//   bad=true   → caller returns `err` as the response envelope.
//   bad=false  → argvForm is the safe-for-argv form of the path
//                (NFC-normalised; `./` prefix when starts with `-`).
//                resolved is the canonical-filesystem form if the
//                path exists, or an empty string if it does not.
//                Callers that require existence check resolved.isEmpty().
//   Empty rawPath returns bad=false with everything empty; caller
//   decides whether absence of a path is itself an error.
struct Check {
    bool        bad = false;
    QJsonObject err;
    QString     argvForm;
    QString     resolved;
};

// Validate `rawPath` against `rootCanonical` (the canonicalised
// focused-project root). On reject, `err` carries the envelope
// {ok:false, error:"<tool>: \"<paramName>\" <reason>", code:"bad_path"}.
//
// Reject reasons:
//   - rawPath contains a control char (U+0000..U+001F) or backslash.
//   - resolved path is outside rootCanonical (canonical anchor for
//     existing paths, lexical fallback for non-existent ones).
Check validatePath(const QString &rawPath,
                   const QString &rootCanonical,
                   const QString &toolName,
                   const QString &paramName = QStringLiteral("path"));

}  // namespace PathValidation

#endif  // ANTS_PATHVALIDATION_H
