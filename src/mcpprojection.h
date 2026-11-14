#pragma once

#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>

class QJsonArray;
class QJsonObject;

// ANTS-1720 — `fields=` top-level response projection for high-volume
// MCP read tools. Pure (Qt6::Core only) so the dispatch layer
// (ClaudeIntegration) and the feature test share one implementation —
// mirrors the focusedtest / modelrecommender extraction pattern.
namespace mcp {

// ANTS-4524 — there is NO `fields=` allowlist. Every verb honours it, the
// dispatcher declares the property on every schema, and `isUniversalDispatchArg`
// says so to the undeclared-argument check. The predicate that used to gate it
// (isFieldProjectionTool) is gone: it answered a question whose answer is now
// always yes, while a caller passing `fields=` to any other verb had it dropped
// in silence. projectFields is a pure top-level key filter with a refusal floor
// and a 304 floor, so it is safe on any object envelope.
//
// The verbs whose envelope is compacted when the caller did NOT ask — where
// mcp::terseDefault() applies to an ABSENT `compact` arg.
bool isDefaultCompactTool(const QString &toolName);

// ANTS-5585 — a CALL that is not compacted by default although its verb is.
// changelog_query mode:"lint" answers with `findings`, and its schema says
// the list is always present: an empty one is the answer "clean". Default
// compaction folded it away, so a clean check could not be told from a reply
// that lost the field. An explicit compact:true is still honoured.
bool isDefaultCompactExemptCall(const QString &toolName, const QJsonObject &args);

// An EXPLICIT compact:true / compact:false is honoured on every verb
// (ANTS-4659). Only the default above is per verb: a default nobody asked for
// folded away spec_lint's flag saying a check had never run (ANTS-4673).

// ANTS-2094 — the read verbs whose bodies are large enough to be worth
// spilling to a content-addressed cache file + returning a head+pointer
// envelope (observation masking). A real allowlist, unlike `fields=`: an
// offload replaces the body with a pointer, so it is not something to do to
// every verb by default.
// Write, control-plane, refusal and 304 responses are never offloaded
// (gated by the dispatch site, not here). See docs/specs/ANTS-2094-proactive-server-side-result-offload.md.
bool isOffloadEligible(const QString &toolName);

// ANTS-2218 — read verbs that honour `raw:true`: return file/source bytes
// VERBATIM inside an unforgeable nonce frame instead of the default lossy
// tag-scrub, so an agent can Edit frame-sensitive source faithfully. Opt-in
// per call; the default (scrub) path is unchanged. Gated at the dispatch site;
// kept == the advertised `raw` schema set so the contract has no hidden
// behaviour. See docs/standards/mcp-behavioural-notes.md.
bool isRawEligible(const QString &toolName);

// ANTS-2091 / ANTS-4677 — keys kept regardless of value, because callers
// branch on them: a dropped `ok:false` / `found:false` inverts the result's
// meaning, and a `*_checked:false` (matched by suffix) reports that a check
// DID NOT RUN, so dropping it reads as a clean pass over nothing examined.
// ANTS-4692 — also the floor for the offload path's preserved fields
// (docs/specs/ANTS-2094-proactive-server-side-result-offload.md § 2.3.2), so both paths agree on that set.
bool isProtectedCompactKey(const QString &key);

// Return a compact JSON object carrying only the named top-level fields
// of `responseText`. Contract:
//   - `fields` empty                    -> responseText returned unchanged.
//   - responseText not a JSON object    -> responseText returned unchanged.
//   - field present in source           -> copied verbatim.
//   - field absent / non-string / empty -> omitted, and NAMED in
//                                          `fields_unmatched` (ANTS-4567),
//                                          beside `fields_available`, the
//                                          source's top-level keys.
//   - NO requested field matched        -> `ok` is carried, so a narrowed
//                                          success is distinguishable from a
//                                          call that produced no envelope
//                                          (ANTS-4877). A partial match needs
//                                          no floor: the matched field proves
//                                          the call succeeded.
//   - refusal (`ok:false`)              -> the refusal floor is carried
//                                          verbatim whatever `fields` names.
//   - `warning`, `parseable_bullets`    -> the diagnostic floor: carried
//                                          whenever present, whatever
//                                          `fields` names.
//   - a 304 envelope (`unchanged:true`) -> returned unchanged: there is no
//                                          content to narrow, and narrowing
//                                          one drops the `unchanged` that IS
//                                          the answer (ANTS-4524).
// The etag is NOT auto-preserved: a caller that wants the etag for a
// follow-up ANTS-1499 304 round-trip lists "etag" in `fields`. The
// dispatch computes the etag on the unfiltered body before projecting,
// so the etag a narrowed call returns equals a full call's etag.
QString projectFields(const QString &responseText, const QJsonArray &fields);

// ANTS-2081 + ANTS-2086 — for a successful read response, append
// presentation-only nudges: `next_call_hint` (reuse the issued etag to
// 304 a repeat read) and `leaner_call_hint` (the cheaper mode on this
// verb). Pure so the dispatch layer and the feature test share one
// implementation (mirrors projectFields). Lives here rather than inline
// in ClaudeIntegration so the per-verb tool-name literals don't displace
// the source-string-match anchors the WiringContract tests use against
// claudeintegration.cpp. ANTS-2180 — the `next_call_hint` fires for ANY
// body size (a 304 saves the full body next time even on a tiny
// read_region/file_outline slice); only `leaner_call_hint` keeps the
// 4 KiB worthwhile-body gate. Returns `responseText` unchanged on: a 304
// (`etagUnchanged`), a fields=-narrowed call, a refusal (`ok:false`), an
// unparseable body, or when neither nudge applies.
QString appendReadHints(const QString &toolName, const QJsonObject &args,
                        const QString &responseText, bool etagUnchanged);

// ANTS-3550 — session/process "already-taught" latch for the presentation-only
// advisory hints (`next_call_hint` / `leaner_call_hint`). Each is educational
// the first time and pure recurring overhead thereafter; once a (tool, hint-
// kind) pair has been emitted this process, appendReadHints omits it for the
// rest of the process. Keyed per (tool, hint-kind) so a distinct verb's own
// leaner tip is never hidden — only exact repeats are dropped. Process-scoped:
// the one Ants server is shared across subagents, so the latch spans them
// (intended — same rationale as ANTS-3546). Module default OFF so direct /
// library use is unaffected (byte-identical to pre-3550; mirrors
// setTerseDefault); the application enables it via the claude.mcp_hint_latch
// config key (default true). Atomic enable flag + mutex-guarded taught-set;
// resetHintLatch() clears the set (test isolation / a future per-session hook).
// See tests/features/mcp_projection/spec.md.
void setHintLatchEnabled(bool enabled);
bool hintLatchEnabled();
void resetHintLatch();

// ANTS-2091 — compact-envelope transform. Recursively drops dead-weight
// fields the model never reads (JSON null, false, empty string, empty
// array, empty object) from a read response. It runs on `compact:true`,
// and BY DEFAULT on a verb whose compaction-table row sets defaultCompact
// while terseDefault() is on (see mcpprojection.cpp's table).
// Contract:
//   - responseText not a JSON object  -> returned unchanged.
//   - isProtectedCompactKey's keys are kept at EVERY nesting level
//     regardless of value — the fields callers branch on, where a dropped
//     false inverts the meaning (ANTS-4673, ANTS-4956).
//   - recurses into nested objects and array elements, so per-bullet /
//     per-match empties (lanes:[], kind:"") are pruned too.
//   - absent ⟺ default for every other key: a caller reading a dropped
//     field as its zero-value sees no behaviour change.
QString compactEnvelope(const QString &responseText);

// ANTS-2090 — pack each eligible top-level array-of-objects field of a
// response envelope into a columnar {__cols__, __rows__} form: one header
// row of (lexicographically sorted) column names + one value-row per
// element, with `null` where an element lacks a column. This drops the
// per-row key repetition that dominates list-shaped replies
// (roadmap_query bullets[], workspace_search matches[], file_outline
// symbols[], …). Pure, Qt6::Core only. Opt-in: the dispatcher calls it
// only when the caller passed encoding:"tabular", because it changes a
// field's SHAPE and the caller must know how to decode it — never a
// session default. Self-guarding PER ARRAY (no tool-name predicate):
// returns `responseText` unchanged on a non-object body, a refusal
// (`ok:false`), and leaves any array untouched unless it has ≥2 elements
// that are all objects AND the columnar form is strictly smaller in
// compact UTF-8 (so it can never cost bytes). v1 carries nested
// object/array cell values verbatim (no recursion). See
// docs/specs/ANTS-2090-tabular-columnar-encoding-homogeneous.md.
QString tabularize(const QString &responseText);

// ANTS-2085 — session/user "terse" default for read responses. When true,
// the dispatcher applies compactEnvelope() to a read
// response from a verb whose compaction-table row sets defaultCompact, even
// when the caller did NOT pass compact:true — so a user can
// set "keep Ants MCP replies lean" ONCE (the Settings toggle / the
// claude.mcp_terse_responses config key) instead of repeating compact:true
// on every call (generalises ANTS-2082 across the whole read surface).
// Precedence: an explicit per-call compact (true OR false) always wins;
// this only fills the default when the arg is absent. A false a caller
// branches on survives only through isProtectedCompactKey, so a new flag of
// that kind belongs there. Process-global + atomic: read on every dispatch,
// written from MainWindow (config load + external reload) and the Settings
// Apply. Cache-free — no per-call config I/O. The module default is false;
// claude.mcp_terse_responses (default true) turns it on at startup.
void setTerseDefault(bool terse);
bool terseDefault();

// Return, sorted, the keys in `args` that the verb does not declare: neither
// in its inputSchema property set (`known`), nor one of the args every verb
// honours (caller_cwd, encoding, fields, compact), nor in `honoured`.
// ANTS-4659 — the dispatcher refuses a call that has any (`bad_args`). Until
// then it ran the call and named them in an `ignored_args` advisory
// (ANTS-2175), hence the name. An empty `known` reports every non-universal
// key.
// ANTS-4578 — `honoured` names the dispatch-layer args this verb acts on
// (offload / etag_match), which the dispatcher knows and this pure helper
// cannot.
QStringList ignoredArgs(const QJsonObject &args, const QSet<QString> &known,
                        const QSet<QString> &honoured);

// ANTS-6325 — Claude Code sends an argument the wire schema leaves out
// (ANTS-5725) as text, so `timeout_sec:20` arrives as "20". Returns `args` with
// each string converted to the type `properties` declares for it: integer,
// number, boolean, array or object. Text that does not parse as that type is
// left for the verb to judge, as is every other argument.
QJsonObject coerceArgTypes(const QJsonObject &args, const QJsonObject &properties);

// ANTS-3391 — true when the roadmap bullet object `bullet` contains `needle`
// as a case-insensitive substring of its headline, headline_full, or body
// field (the three text surfaces roadmap_query emits). Powers the
// roadmap_query `query` keyword filter. Pure (mirrors ignoredArgs) so the
// cmdRoadmapQuery list path and the feature test share one matcher. Empty
// needle → true (an empty substring is contained in everything; the caller
// gates on non-empty so the filter is a no-op, not a match-all-then-drop).
// ANTS-4367 — how `query` is matched. Substring is the default and the
// historical behaviour; the other two exist because a short query had NO
// narrowing knob at all, and the damage from that is reordering rather than
// padding — genuine hits get pushed off page one by substring noise while
// `truncated:true` gives no clue the answer is in the tail.
enum class QueryMode { Substring, WholeWord, Regex };

// ANTS-4610 — the needle-against-haystack half, lifted out of
// bulletMatchesQuery so mode:section_index can narrow by section name using
// the SAME matcher rather than a second one that would drift from it. `hay`
// is whatever text surface the caller is filtering; the mode semantics
// (case-insensitivity, the word boundary, fail-closed on a bad pattern) are
// identical wherever it is called.
bool textMatchesQuery(const QString &hay, const QString &needle,
                      QueryMode mode = QueryMode::Substring);

bool bulletMatchesQuery(const QJsonObject &bullet, const QString &needle,
                        QueryMode mode = QueryMode::Substring);

// ANTS-5104 — a query compiled once: the lowercased needle and, for the two
// narrowing modes, the pattern. roadmap_query filters every bullet with one
// query, and the two functions above rebuilt it per call; they remain as
// one-shot wrappers over this.
class QueryMatcher {
public:
    QueryMatcher(const QString &needle, QueryMode mode);
    bool matches(const QString &hay) const;
    bool matchesBullet(const QJsonObject &bullet) const;
    // ANTS-5257 — the bullet keys matchesBullet() searches, echoed beside
    // `query` so a caller can see what a zero-hit search covered.
    static QStringList bulletFields();

private:
    QueryMode          m_mode;
    QString            m_needleLower;
    QRegularExpression m_re;
};

}  // namespace mcp
