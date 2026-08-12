// ANTS-3664 — one finding shape for the doc-lint verbs.
//
// ANTS-3663 (`doc_lint`) composes five checkers into one findings list, and
// before this there was nothing to compose them *into*: six unrelated
// `struct Finding` declarations existed across the tree and no shared
// serialiser (a search for `findingToJson` returned zero). Written before the
// three new checkers (ANTS-3660/3661/3662) rather than after, so they emit this
// natively instead of each inventing a seventh, eighth and ninth shape.
//
// Qt6::Core-only, in ants_core_lib beside markdownscan.h — the same reason:
// unit-testable without RemoteControl / MainWindow. INV-4 pins the include set
// to exactly four QtCore headers; a GUI include here would fail the link, not
// an assertion, because the test bundle links Qt6::Core alone.
//
// NOT a refactor of AuditEngine or DebtSweep. Those serve a different consumer
// with a different surface (severity, confidence, blame, AI triage) and the
// Rule of Three has not fired on them — see the spec's § 5.

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace DocFinding {

// One problem found in one document by one checker. Deliberately small: every
// field below has a named consumer in ANTS-3663, and nothing is here "in case".
struct Finding {
    QString verb;        // producer: "doc_integrity", "doc_citations", …
    QString kind;        // rule slug, unique within `verb`: "dead_anchor", …
    QString file;        // project-relative, always set
    int     line = 0;    // 1-based; 0 means "the document", not "line 0"
    QString message;     // one line, no trailing period, no file:line prefix
    bool    autoFixable = false;   // ANTS-3669 may repair this

    // ANTS-3663 INV-7's tiebreak of last resort: the producer's own 0-based
    // emission index within one run. Set by the producer, NEVER SERIALISED —
    // see toJson. It is here because the six fields above are all properties of
    // the *problem*, so two findings can agree on every one of them (two
    // `broken_link`s on one line naming the same missing target) and a
    // comparator over those six alone is therefore not a total order.
    int     emissionIndex = 0;

    // ANTS-4127 — per-KIND detail, merged into the wire object as it stands.
    // The six fields above are the ones every kind has; this is for the ones a
    // single kind has and no other. `test_surface_absent` carries `invariant`,
    // `surface` and `spec_status` (spec § 2.3), and naming those three here
    // would put three permanently-empty keys on the five verbs that will never
    // set them — the "nothing is here in case" rule above, read forwards.
    //
    // Empty is the default and emits nothing, so no existing finding's wire
    // form moves. A key colliding with one of the six is NOT allowed to
    // overwrite it (toJson enforces): the base shape is the contract ANTS-3663
    // sorts and de-duplicates on, and a producer must not be able to redefine
    // `line` for one kind.
    QJsonObject extra;
};

// Wire form. Keys are snake_case while the members are camelCase (INV-3),
// matching the ScanResult::unterminatedFence <-> unterminated_fence convention
// this lane already uses.
//
// Two rules a naive serialiser gets wrong, both locked by INV-1:
//  * `line` is emitted even when 0. It is NOT omit-when-false: 0 means document
//    scope, so omitting it makes a document-level finding indistinguishable
//    from a malformed one.
//  * `emission_index` is never emitted, by any verb in this family, in any
//    array. It counts emissions within one run, so the same document reviewed
//    with a different `checks[]` set yields different indices for identical
//    findings — an unstable number that means nothing to a consumer.
QJsonObject toJson(const Finding &f);
QJsonArray  toJson(const QList<Finding> &fs);

// counts[verb][kind] -> n, over the WHOLE list it is handed. A consumer that
// caps or filters must call this BEFORE doing so, or its counts describe the
// page rather than the run — the discipline ANTS-3636 INV-42 states for its own
// whole-doc tallies.
QJsonObject countsByVerbAndKind(const QList<Finding> &fs);

}  // namespace DocFinding
