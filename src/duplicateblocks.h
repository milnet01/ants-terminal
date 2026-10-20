#pragma once

// duplicate_blocks (ANTS-5785) — code written out more than once across the
// given files, including copies that have drifted apart, as a shortlist for
// coding.md § 1.3. It grades nothing. Contract:
// tests/features/mcp_duplicate_blocks/spec.md. Its own translation unit so a
// test links it without RemoteControl (docs/standards/mcp-tools.md § Tests).
//
// Not built on DocDedup: its passages are bags of word shingles, which keep
// no line order, so they cannot give a site's first and last line or the
// lines a drifted copy changed. This matches lines in order instead.

#include <QJsonObject>
#include <QString>

namespace DuplicateBlocks {

// The whole call below the resolved project root: the envelope, or a
// refusal {ok:false, code, error}.
QJsonObject run(const QString &rootCanonical, const QJsonObject &args);

} // namespace DuplicateBlocks
