#pragma once

// quotation_check (ANTS-5502) — the seam below the resolved project root.
// Contract: docs/specs/ANTS-5502-quotation-check.md. Its own translation unit
// so a test links it without RemoteControl (docs/standards/mcp-tools.md
// § Tests).

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <vector>

namespace QuotationCheck {

// quotation-check.sh at ~/.claude 60fd0be, echoed in the envelope.
inline constexpr const char *kNormaliser = "quotation-check.sh 60fd0be";

inline constexpr int kMaxItems = 500;
inline constexpr qsizetype kMaxTextBytes = 8 * 1024;
inline constexpr qsizetype kMaxSubjectBytes = 16 * 1024 * 1024;
inline constexpr int kNearestBytes = 200;

// A normalised text, with the offset in the original bytes each output
// byte came from, so a miss can name the line its nearest text is on.
struct Normalised {
    QByteArray text;
    std::vector<qsizetype> source;
};

// § 2.2 steps 1-5, in the script's order.
Normalised normalise(const QByteArray &raw);

// The quotation side: normalise, then trim leading and trailing spaces.
QByteArray normaliseQuotation(const QByteArray &raw);

// The `allowed` glob grammar of § 2.3: `**`, `*`, `?`, all else literal.
bool globMatch(const QString &glob, const QString &path);

// Runs the whole call below the root. Returns either the § 2.4 envelope
// with `results` untrimmed, or a refusal {ok:false, code:"bad_args", error}.
// The handler applies max_bytes to `results`.
QJsonObject run(const QString &root, const QJsonObject &args);

} // namespace QuotationCheck
