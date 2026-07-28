// ANTS-3664 — see docfinding.h for the contract and the reasoning.

#include "docfinding.h"

namespace DocFinding {

QJsonObject toJson(const Finding &f) {
    QJsonObject o;
    o[QStringLiteral("verb")]    = f.verb;
    o[QStringLiteral("kind")]    = f.kind;
    o[QStringLiteral("file")]    = f.file;
    // Unconditional: 0 is document scope, a value, not an absence (INV-1).
    o[QStringLiteral("line")]    = f.line;
    o[QStringLiteral("message")] = f.message;
    // Omit-when-false, per the ANTS-3636 INV-28 convention. A consumer reads an
    // absent key as false; ANTS-3663 § 2.2 states that on the reading side.
    if (f.autoFixable)
        o[QStringLiteral("auto_fixable")] = true;
    // f.emissionIndex is deliberately absent — see the header. This is the line
    // an "obvious" serialiser adds, which is why INV-1 asserts against it.
    return o;
}

QJsonArray toJson(const QList<Finding> &fs) {
    QJsonArray a;
    for (const Finding &f : fs)
        a.append(toJson(f));
    return a;
}

QJsonObject countsByVerbAndKind(const QList<Finding> &fs) {
    QJsonObject out;
    for (const Finding &f : fs) {
        QJsonObject byKind = out.value(f.verb).toObject();
        byKind[f.kind] = byKind.value(f.kind).toInt() + 1;
        out[f.verb] = byKind;
    }
    return out;
}

}  // namespace DocFinding
