// ANTS-3761 — RFC 8785 (JSON Canonicalization Scheme) serialisation.
// Spec: docs/specs/ANTS-3761-roadmap-export-format.md § 2.2
//
// This exists because QJsonDocument::toJson(Compact) is NOT JCS, and § 2.2
// records exactly how far off it is: measured against the RFC's own material,
// all six published vector files come back byte-identical but Appendix B's
// number table scores 21 of 24. Every miss is ECMAScript's fixed-versus-
// exponential boundary (0.000001, which Qt writes as 1e-06) and its unpadded
// exponent. So the divergence is numbers alone — which is why this is ~120
// lines rather than a vendored library.
#pragma once

#include <QByteArray>
#include <QJsonValue>
#include <QString>

namespace JsonCanonical {

// Serialise one JSON value per RFC 8785. Returns false with `error` set when
// the value cannot be canonicalised — ANTS-3761 § 2.4: the export aborts and
// reports the offending row, it never emits a partial file and never
// substitutes a replacement character.
//
// Two failure modes, both reachable here: a lone surrogate (RFC § 3.2.2.2
// requires terminating with an error, and SQLite TEXT does not validate
// encoding) and a non-finite number (JSON cannot represent one). Duplicate
// object keys — the RFC's third — cannot occur: QJsonObject cannot hold them.
bool serialise(const QJsonValue &value, QByteArray *out, QString *error = nullptr);

// ECMAScript Number::toString, which RFC 8785 § 3.2.2.3 adopts wholesale.
// Exported because it is the one place Qt diverges, so INV-19 tests it against
// Appendix B directly rather than only through whole documents.
QByteArray numberToString(double d);

}  // namespace JsonCanonical
