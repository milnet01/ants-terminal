#pragma once

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

// Allowlist: the 11 read tools that accept `fields=` (grew past the original
// 7 via ANTS-1855 read_log, ANTS-2021 read_region, ANTS-1637 codebase_index,
// ANTS-1735 model_switch_stats). Mostly overlaps
// ClaudeIntegration::isEtagSupportedTool, but `read_log` is projection-only
// (NOT etag-able) — so this is no longer a strict subset of the etag set.
// Chosen for payload size (see docs/specs/ANTS-1720 / ROADMAP).
bool isFieldProjectionTool(const QString &toolName);

// ANTS-2094 — the read verbs whose bodies are large enough to be worth
// spilling to a content-addressed cache file + returning a head+pointer
// envelope (observation masking). Deliberately a SEPARATE set from
// isFieldProjectionTool: it adds the large-body verbs that take no
// `fields=` (get_scrollback / get_text / workspace_search / find_sources).
// Write, control-plane, refusal and 304 responses are never offloaded
// (gated by the dispatch site, not here). See docs/specs/ANTS-2094.md.
bool isOffloadEligible(const QString &toolName);

// ANTS-2218 — read verbs that honour `raw:true`: return file/source bytes
// VERBATIM inside an unforgeable nonce frame instead of the default lossy
// tag-scrub, so an agent can Edit frame-sensitive source faithfully. Opt-in
// per call; the default (scrub) path is unchanged. Gated at the dispatch site;
// kept == the advertised `raw` schema set so the contract has no hidden
// behaviour. See docs/standards/mcp-behavioural-notes.md.
bool isRawEligible(const QString &toolName);

// Return a compact JSON object carrying only the named top-level fields
// of `responseText`. Contract:
//   - `fields` empty                    -> responseText returned unchanged.
//   - responseText not a JSON object    -> responseText returned unchanged.
//   - field present in source           -> copied verbatim.
//   - field absent / non-string / empty -> omitted (never an error).
//   - all fields unknown                -> "{}".
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
// array, empty object) from a read response, opt-in via `compact:true`.
// Contract:
//   - responseText not a JSON object  -> returned unchanged.
//   - protected keys (ok, code, error, etag, found, unchanged) are kept
//     at EVERY nesting level regardless of value — they are the fields
//     callers branch on (a dropped `ok:false` / `found:false` would
//     invert meaning).
//   - recurses into nested objects and array elements, so per-bullet /
//     per-match empties (lanes:[], kind:"") are pruned too.
//   - absent ⟺ default: a caller reading a dropped field as its
//     zero-value sees no behaviour change. The opt-in is the safety
//     valve — a caller that needs to distinguish "" / [] / null / false
//     from absent (e.g. git_state's null binary-file markers) omits it.
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
// docs/specs/ANTS-2090.md.
QString tabularize(const QString &responseText);

// ANTS-2085 — session/user "terse" default for read responses. When true,
// the dispatcher applies compactEnvelope() to a field-projection read
// response even when the caller did NOT pass compact:true — so a user can
// set "keep Ants MCP replies lean" ONCE (the Settings toggle / the
// claude.mcp_terse_responses config key) instead of repeating compact:true
// on every call (generalises ANTS-2082 across the whole read surface).
// Precedence: an explicit per-call compact (true OR false) always wins;
// this only fills the default when the arg is absent. Safe because
// compactEnvelope is absent-⟺-default by design (see its contract above),
// so a session opting into terse IS the one opt-in the empty-vs-absent
// hazard needs. Process-global + atomic: read on every dispatch, written
// from MainWindow (config load + external reload) and the Settings Apply.
// Cache-free — no per-call config I/O. Default false (full back-compat).
void setTerseDefault(bool terse);
bool terseDefault();

// ANTS-2175 — return, sorted, the keys in `args` that the verb does not
// recognise: neither in its declared inputSchema property set (`known`) nor
// a universal dispatch-layer arg accepted by every verb (caller_cwd /
// etag_match / fields / compact / offload). Lets the dispatcher echo a
// non-fatal `ignored_args` advisory on the success envelope so a typo'd or
// stale param surfaces on the FIRST call instead of being silently dropped —
// the trigger was a `query=` passed to roadmap_query (which has no such
// param), which returned the full unfiltered roadmap and looked like a
// working search. Pure (mirrors projectFields) so the dispatch layer and the
// feature test share one implementation. `known` is empty -> every non-
// universal key is reported (matches a verb that declares no properties).
QStringList ignoredArgs(const QJsonObject &args, const QSet<QString> &known);

// ANTS-4525 — attach the `ignored_args` advisory to a response body, REFUSALS
// INCLUDED. Returns `responseJson` unchanged when `ignored` is empty or the
// body is not a JSON object.
//
// The advisory used to be suppressed on `ok:false`, on the reasoning that the
// caller already reads the error. That is backwards for the case it matters
// most in: a caller holding a wrong mental model of a verb passes wrong
// ARGUMENTS and gets a refusal, so the reply that would correct the model is
// suppressed precisely because it refused. Measured — a DOOM session called
// read_log {max_commits:3, body:true} believing it read git log; both args are
// unknown to the verb, nothing said so, and the reply was `not_found` on the
// Ants debug-log path, which explains the file rather than the misconception.
//
// Suppression stays defensible for a refusal CAUSED by the unknown args, where
// the error says everything; it is wrong for a refusal that happened for an
// unrelated reason and left the wrong args unmentioned. Distinguishing the two
// costs more than the harmless duplication, so the advisory is attached to
// both. It only ADDS a key, so the ANTS-2112 refusal floor
// (ok/code/error/retry_after_ms) is untouched by construction — a caller who
// cannot see the error is worse off than one who cannot see the advisory.
QString withIgnoredArgs(const QString &responseJson, const QStringList &ignored);

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

}  // namespace mcp
