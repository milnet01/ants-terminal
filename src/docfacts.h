// ANTS-5506 — doc_facts: five document checks skills otherwise do by hand.
// Qt6::Core only, in ants_core_lib beside markdownscan.h / docfinding.h, so it
// is unit-testable without RemoteControl / MainWindow.
//
// ONE checker with five kinds, not five checkers (spec § 2.6): one `checks[]`
// name, one eligibility row, one ran-flag. It is NATIVE — it takes the shared
// read's text — so doc_lint opens nothing extra for it.
//
//   count_mismatch      a "The three steps:" lead-in over a list of another size
//   invariant_duplicate an INV-N bullet defined twice under an Invariants heading
//   leaked_markup       a tool-call tag left in prose
//   version_drift       a version claim that disagrees with the project version
//   verb_arg_unknown    an MCP call example passing a key the verb does not take
//
// Every finding is autoFixable:false. This engine reports and never edits —
// which is also why it shares nothing with rcScrubLeakedToolXml, a RemoteControl
// helper that strips a different tag set and that ants_core_lib cannot link.
//
// See docs/specs/ANTS-3663.md § 2.6.

#pragma once

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>

#include "docfinding.h"

namespace DocFacts {

// Both inputs are INJECTED, never resolved here: the verb reads the project's
// version and builds the argument map from the live schema. Empty switches that
// kind off; the caller reports it as version_unavailable / schema_unavailable.
struct Options {
    QString                       projectVersion;
    QHash<QString, QSet<QString>> verbArgs;  // verb -> accepted top-level keys
};

struct Result {
    QList<DocFinding::Finding> findings;  // ascending line, then kind row order
    // The three denominators. Without them a clean run and one that found
    // nothing to check are the same envelope.
    int countClaimsChecked   = 0;
    int versionClaimsChecked = 0;
    int verbCallsChecked     = 0;
};

Result check(const QString &text, const QString &relPath, const Options &opts);

}  // namespace DocFacts
