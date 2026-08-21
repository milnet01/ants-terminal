#include "mcpprojection.h"
#include <QRegularExpression>

#include <atomic>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QMutex>

namespace mcp {

// ANTS-2085 — see header. Read on every dispatch, written from the GUI
// thread (config load / Settings Apply); atomic so the cross-thread
// publish is a plain relaxed flag flip, no lock.
namespace {
std::atomic<bool> g_terseDefault{false};
}  // namespace

void setTerseDefault(bool terse) {
    g_terseDefault.store(terse, std::memory_order_relaxed);
}

bool terseDefault() {
    return g_terseDefault.load(std::memory_order_relaxed);
}

// ANTS-3550 — the advisory-hint "already-taught" latch (see header). The enable
// flag is published cross-thread like g_terseDefault (GUI writes, dispatch
// reads) so it's atomic; the taught-set is mutated on the dispatch path and
// guarded by a mutex (a request may run concurrently with a sibling subagent's).
namespace {
std::atomic<bool> g_hintLatchEnabled{false};  // module default OFF (mirror terse)
QMutex g_hintLatchMutex;
QSet<QString> g_taughtHints;

// Test-and-set: true the FIRST time (tool, field) is seen while the latch is
// enabled, marking it taught; false on every repeat. Latch disabled → always
// true and records nothing, so appendReadHints emits exactly as pre-ANTS-3550.
bool claimHint(const QString &tool, const QString &field) {
    if (!g_hintLatchEnabled.load(std::memory_order_relaxed)) return true;
    const QString key = tool + QChar(u'\x1f') + field;  // unit-separator delim
    QMutexLocker lock(&g_hintLatchMutex);
    if (g_taughtHints.contains(key)) return false;
    g_taughtHints.insert(key);
    return true;
}
}  // namespace

void setHintLatchEnabled(bool enabled) {
    g_hintLatchEnabled.store(enabled, std::memory_order_relaxed);
}

bool hintLatchEnabled() {
    return g_hintLatchEnabled.load(std::memory_order_relaxed);
}

void resetHintLatch() {
    QMutexLocker lock(&g_hintLatchMutex);
    g_taughtHints.clear();
}

bool isFieldProjectionTool(const QString &toolName) {
    return toolName == QStringLiteral("roadmap_query")
        || toolName == QStringLiteral("changelog_query")   // ANTS-3533
        || toolName == QStringLiteral("project_layout")
        || toolName == QStringLiteral("file_outline")
        || toolName == QStringLiteral("get_environment")
        || toolName == QStringLiteral("tab_list")
        || toolName == QStringLiteral("subsystem")
        || toolName == QStringLiteral("git_state")
        || toolName == QStringLiteral("read_log")            // ANTS-1855
        || toolName == QStringLiteral("read_region")         // ANTS-2021
        || toolName == QStringLiteral("codebase_index")      // ANTS-1637
        || toolName == QStringLiteral("docs_index")          // ANTS-2139
        || toolName == QStringLiteral("co_change_family")    // ANTS-3368
        || toolName == QStringLiteral("model_switch_stats")  // ANTS-1735
        // ANTS-4523 — the documented first call and the largest response in
        // the session, so the verb a caller is most likely to narrow. It was
        // absent by omission rather than by decision: the sibling test's
        // negative list names session_brief and current_state and never named
        // this one. `fields=` narrows the PAYLOAD, not the work — the eager
        // codebase_index refresh (ANTS-2140) still runs.
        || toolName == QStringLiteral("session_orient");
}

// ANTS-2094 — offload-eligible read verbs (see header). A separate set from
// isFieldProjectionTool: adds the large-body verbs that take no `fields=`.
bool isOffloadEligible(const QString &toolName) {
    return toolName == QStringLiteral("get_scrollback")
        || toolName == QStringLiteral("get_text")
        || toolName == QStringLiteral("read_log")
        || toolName == QStringLiteral("read_region")
        // ANTS-2219 — read_regions: a 64-slice batch can be large; spill it
        // like any other read body, re-read verbatim via read_spill.
        || toolName == QStringLiteral("read_regions")
        || toolName == QStringLiteral("workspace_search")
        || toolName == QStringLiteral("codebase_index")
        || toolName == QStringLiteral("docs_index")
        || toolName == QStringLiteral("find_sources")
        // ANTS-3345 — debt_sweep_scan: a release-sized diff yields 1000+
        // findings (~130k chars on one line) that bypass the token cap.
        // Pagination (limit/offset) bounds the default page; spilling the
        // page like any other read body keeps even a full page off the wire.
        || toolName == QStringLiteral("debt_sweep_scan")
        || toolName == QStringLiteral("roadmap_query")
        || toolName == QStringLiteral("changelog_query")   // ANTS-3533
        // ANTS-2093 — project_query: a large snippet result spills like any
        // other read body (INV-9), re-read verbatim via read_spill.
        || toolName == QStringLiteral("project_query");
}

// ANTS-2218 — read verbs that honour `raw:true` (see header). The content
// reads an agent edits from: a file slice (read_region / read_regions) or a
// code search (workspace_search). Kept == the advertised `raw` schema set so
// the contract has no hidden behaviour.
bool isRawEligible(const QString &toolName) {
    return toolName == QStringLiteral("read_region")
        || toolName == QStringLiteral("read_regions")
        || toolName == QStringLiteral("workspace_search")
        // ANTS-4365 — file_outline returns `header_doc` (and `signature`) as
        // file bytes, so it is a content read by exactly the test the other
        // three pass. Without the escape, a Markdown file whose header IS an
        // HTML comment — every `*_Ants_MCP_Feedback.md`, whose version marker
        // lives there — could not report its own first line truthfully, and a
        // caller building an Edit from `header_doc` wrote the mangled
        // spelling back and broke the marker the feedback verbs key on. That
        // is the hazard raw:true was added to read_region for.
        || toolName == QStringLiteral("file_outline");
}

QString projectFields(const QString &responseText, const QJsonArray &fields) {
    if (fields.isEmpty()) return responseText;

    QJsonParseError err{};
    const QJsonDocument doc =
        QJsonDocument::fromJson(responseText.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return responseText;

    const QJsonObject src = doc.object();
    QJsonObject out;
    for (const QJsonValue &f : fields) {
        if (!f.isString()) continue;
        const QString name = f.toString();
        if (!name.isEmpty() && src.contains(name))
            out.insert(name, src.value(name));
    }
    // ANTS-2112 — never blank a refusal. A fields=-narrowed read that hits a
    // rate-limit / validation refusal carries its error in ok/code/error/
    // retry_after_ms — none of which the caller's fields= would name — so the
    // projection above yields `{}` and the model never sees the error or the
    // retry hint. When the envelope is a refusal (ok present and false), carry
    // the refusal floor verbatim regardless of fields=. Mirrors
    // compactEnvelope's protected-key floor (isProtectedCompactKey); the
    // sibling appendReadHints already bails on !ok. Successful (ok:true) reads
    // are untouched — a narrowed success stays exactly as requested.
    if (src.contains(QStringLiteral("ok")) &&
        !src.value(QStringLiteral("ok")).toBool()) {
        for (const QString &key : {QStringLiteral("ok"), QStringLiteral("code"),
                                   QStringLiteral("error"),
                                   QStringLiteral("retry_after_ms")}) {
            if (src.contains(key)) out.insert(key, src.value(key));
        }
    }
    return QString::fromUtf8(
        QJsonDocument(out).toJson(QJsonDocument::Compact));
}

namespace {

// ANTS-2086 — name the cheaper mode available on `tool`, given the args
// the caller actually sent (so the nudge isn't shown when they already
// use the lean path). Empty string = no leaner mode to suggest.
QString leanerModeHintFor(const QString &tool, const QJsonObject &args) {
    if (tool == QStringLiteral("roadmap_query")) {
        // ANTS-4382 — say nothing on a TARGETED fetch. On
        // `{ids:[…], include_body:true}` the hint below recommended
        // headline_only / status / section_index: all three discard the
        // bodies, which is the one thing the call explicitly opted into, and
        // two of them cannot combine with `ids` at all (bad_mode_combo). So on
        // that shape it partly recommended a refusal. Cosmetic on its own —
        // filed because a hint that contradicts the call's stated intent
        // trains a caller to stop reading hints, which costs more where they
        // are right. The nudge stays valuable on an untargeted list query,
        // where a caller may not know the lean modes exist.
        if (args.contains(QStringLiteral("id")) ||
            args.contains(QStringLiteral("ids")) ||
            args.value(QStringLiteral("include_body")).toBool()) {
            return QString();
        }
        const QString mode = args.value(QStringLiteral("mode")).toString();
        if (mode.isEmpty() || mode == QStringLiteral("bullets")) {
            if (args.value(QStringLiteral("status")).toString()
                    != QStringLiteral("active"))
                return QStringLiteral(
                    "pass mode=\"headline_only\" (id+status+headline) or "
                    "status=\"active\" / mode=\"section_index\" for a "
                    "much smaller payload");
            return QStringLiteral(
                "pass mode=\"headline_only\" for a ~10x smaller payload");
        }
        return QString();
    }
    if (tool == QStringLiteral("workspace_search"))
        // ANTS-3548 — long lines are clipped by default now (512 B), so
        // the remaining levers are a narrower search or a leaner row
        // shape; don't nudge max_match_bytes (already default-ON).
        return QStringLiteral(
            "pass a narrower lane=/glob= to cut the result set, or "
            "headline_only=/count_only=/files_only= for a leaner shape");
    if (tool == QStringLiteral("file_outline") &&
        !args.contains(QStringLiteral("filter")))
        return QStringLiteral(
            "pass filter=<substr> to scan only matching symbols");
    // Generic fallback: any field-projection tool the caller isn't yet
    // narrowing (ANTS-1720 fields=).
    if (isFieldProjectionTool(tool) &&
        !args.contains(QStringLiteral("fields")))
        return QStringLiteral(
            "pass fields=[…] to return only the fields you need");
    return QString();
}

}  // namespace

QString appendReadHints(const QString &tool, const QJsonObject &args,
                        const QString &responseText, bool etagUnchanged) {
    // ANTS-2086 — the leaner-mode nudge only pays on a sizeable body.
    // ANTS-2180 — the etag-reuse nudge does NOT share that gate: a 304 on the
    // next call saves the FULL body regardless of how small THIS slice was,
    // and the highest-churn re-read targets (read_region / file_outline
    // symbol slices across an edit loop) are usually < 4 KiB. So parse every
    // non-304, non-fields, ok:true body and decide the two nudges
    // independently.
    constexpr int kLeanerThresholdBytes = 4096;  // leaner-nudge gate only
    if (etagUnchanged) return responseText;
    if (args.contains(QStringLiteral("fields"))) return responseText;
    const QByteArray utf8 = responseText.toUtf8();
    QJsonParseError pe{};
    const QJsonDocument d = QJsonDocument::fromJson(utf8, &pe);
    if (pe.error != QJsonParseError::NoError || !d.isObject())
        return responseText;
    QJsonObject env = d.object();
    if (!env.value(QStringLiteral("ok")).toBool()) return responseText;
    bool changed = false;
    // etag-reuse nudge — any body size (ANTS-2180). ANTS-3550 — claimHint
    // last (test-and-set) so it only marks taught when we'd actually emit.
    if (env.contains(QStringLiteral("etag")) &&
        !args.contains(QStringLiteral("etag_match")) &&
        !env.contains(QStringLiteral("next_call_hint")) &&
        claimHint(tool, QStringLiteral("next_call_hint"))) {
        env[QStringLiteral("next_call_hint")] = QStringLiteral(
            "pass etag_match=\"%1\" next call to skip an unchanged "
            "re-read (304 Not Modified)")
                .arg(env.value(QStringLiteral("etag")).toString());
        changed = true;
    }
    // leaner-mode nudge — only worth surfacing on a worthwhile body.
    if (utf8.size() >= kLeanerThresholdBytes &&
        !env.contains(QStringLiteral("leaner_call_hint"))) {
        const QString lean = leanerModeHintFor(tool, args);
        // ANTS-3550 — claimHint after !lean.isEmpty() so an absent leaner tip
        // never burns the latch slot; only a real emission marks it taught.
        if (!lean.isEmpty() &&
            claimHint(tool, QStringLiteral("leaner_call_hint"))) {
            env[QStringLiteral("leaner_call_hint")] = lean;
            changed = true;
        }
    }
    if (!changed) return responseText;
    return QString::fromUtf8(
        QJsonDocument(env).toJson(QJsonDocument::Compact));
}

namespace {

// ANTS-2091 — keys kept at every level regardless of value. These are the
// fields callers branch on; a dropped `ok:false` / `found:false` would
// silently invert the result's meaning.
bool isProtectedCompactKey(const QString &key) {
    return key == QStringLiteral("ok")
        || key == QStringLiteral("code")
        || key == QStringLiteral("error")
        || key == QStringLiteral("etag")
        || key == QStringLiteral("found")
        || key == QStringLiteral("unchanged");
}

// A value is "dead weight" when it equals its zero/default form: null,
// false, "", [], {}. Numbers (including 0) are kept — a 0 count is often
// load-bearing (flipped_count:0, files:0) and cheap.
bool isCompactDroppable(const QJsonValue &v) {
    switch (v.type()) {
    case QJsonValue::Null:   return true;
    case QJsonValue::Bool:   return v.toBool() == false;
    case QJsonValue::String: return v.toString().isEmpty();
    case QJsonValue::Array:  return v.toArray().isEmpty();
    case QJsonValue::Object: return v.toObject().isEmpty();
    default:                 return false;  // Double / Undefined
    }
}

QJsonValue compactValue(const QJsonValue &v);

QJsonObject compactObject(const QJsonObject &in) {
    QJsonObject out;
    for (auto it = in.begin(); it != in.end(); ++it) {
        const QString key = it.key();
        if (isProtectedCompactKey(key)) {
            out.insert(key, it.value());  // verbatim, never recursed/pruned
            continue;
        }
        const QJsonValue cv = compactValue(it.value());
        // Re-test after recursion: an object/array that became empty by
        // pruning its children is itself dead weight.
        if (!isCompactDroppable(cv)) out.insert(key, cv);
    }
    return out;
}

QJsonValue compactValue(const QJsonValue &v) {
    if (v.isObject()) return compactObject(v.toObject());
    if (v.isArray()) {
        QJsonArray out;
        const QJsonArray in = v.toArray();
        for (const QJsonValue &e : in) {
            // Recurse into element objects; scalars pass through (array
            // membership is itself meaningful — don't drop a `false`
            // element from a list of booleans).
            out.append(e.isObject() ? QJsonValue(compactObject(e.toObject()))
                                    : e);
        }
        return out;
    }
    return v;
}

}  // namespace

QString compactEnvelope(const QString &responseText) {
    QJsonParseError err{};
    const QJsonDocument doc =
        QJsonDocument::fromJson(responseText.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return responseText;
    return QString::fromUtf8(
        QJsonDocument(compactObject(doc.object()))
            .toJson(QJsonDocument::Compact));
}

QString tabularize(const QString &responseText) {
    QJsonParseError err{};
    const QJsonDocument doc =
        QJsonDocument::fromJson(responseText.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return responseText;  // INV-6 — non-object floor

    const QJsonObject src = doc.object();
    // INV-6 — never restructure a refusal envelope (ok present and false).
    if (src.contains(QStringLiteral("ok")) &&
        !src.value(QStringLiteral("ok")).toBool())
        return responseText;

    QJsonObject out = src;
    bool changed = false;
    for (auto it = src.constBegin(); it != src.constEnd(); ++it) {
        if (!it.value().isArray()) continue;
        const QJsonArray arr = it.value().toArray();
        // INV-2 — eligibility: ≥2 elements, every element a JSON object.
        if (arr.size() < 2) continue;
        bool allObjects = true;
        for (const QJsonValue &e : arr) {
            if (!e.isObject()) { allObjects = false; break; }
        }
        if (!allObjects) continue;

        // INV-7 / §2.3 step 2 — column set = union of element keys,
        // drained into a LEXICOGRAPHICALLY sorted list (a single
        // QJsonObject's keys are already sorted in Qt6, but the union
        // across elements only inherits that if the accumulator sorts).
        QSet<QString> colSet;
        for (const QJsonValue &e : arr) {
            const QJsonObject o = e.toObject();
            for (auto ko = o.constBegin(); ko != o.constEnd(); ++ko)
                colSet.insert(ko.key());
        }
        QStringList cols(colSet.constBegin(), colSet.constEnd());
        cols.sort();

        QJsonArray colsJson;
        for (const QString &c : cols) colsJson.append(c);

        // INV-3 — one row per element, value at its column index, `null`
        // where the element lacks that key (missing-key ⟺ explicit-null).
        // INV-5 — nested object/array cell values are carried verbatim.
        QJsonArray rows;
        for (const QJsonValue &e : arr) {
            const QJsonObject o = e.toObject();
            QJsonArray row;
            for (const QString &c : cols)
                row.append(o.contains(c) ? o.value(c) : QJsonValue());
            rows.append(row);
        }

        QJsonObject tab;
        tab.insert(QStringLiteral("__cols__"), colsJson);
        tab.insert(QStringLiteral("__rows__"), rows);

        // INV-4 — never costs bytes: emit the columnar form only when it
        // is strictly smaller (compact UTF-8) than the original array.
        const int origBytes =
            QJsonDocument(arr).toJson(QJsonDocument::Compact).size();
        const int tabBytes =
            QJsonDocument(tab).toJson(QJsonDocument::Compact).size();
        if (tabBytes < origBytes) {
            out.insert(it.key(), tab);
            changed = true;
        }
    }
    // INV-1 — inert when nothing eligible: byte-identical passthrough.
    if (!changed) return responseText;
    return QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Compact));
}

namespace {

// ANTS-2175 — args the dispatch layer (claudeintegration.cpp) consumes for
// EVERY verb, independent of the per-verb inputSchema: the caller_cwd anchor
// (ANTS-1520), the ETag short-circuit (ANTS-1499), the fields= projection
// (ANTS-1720) + compact flag (ANTS-2091/ANTS-2085), the result-offload flag
// (ANTS-2094), and the tabular encoding selector (ANTS-2090). A verb that
// doesn't redeclare these in its own schema still accepts them, so they must
// never be reported as ignored. Keep in sync with the dispatch
// post-processing chain.
bool isUniversalDispatchArg(const QString &key) {
    return key == QStringLiteral("caller_cwd")
        || key == QStringLiteral("etag_match")
        || key == QStringLiteral("fields")
        || key == QStringLiteral("compact")
        || key == QStringLiteral("offload")
        || key == QStringLiteral("encoding");
}

}  // namespace

QStringList ignoredArgs(const QJsonObject &args, const QSet<QString> &known) {
    QStringList out;
    for (auto it = args.constBegin(); it != args.constEnd(); ++it) {
        const QString &key = it.key();
        if (known.contains(key) || isUniversalDispatchArg(key))
            continue;
        out.append(key);
    }
    out.sort();  // deterministic order for callers and the feature test
    return out;
}

QString withIgnoredArgs(const QString &responseJson, const QStringList &ignored) {
    if (ignored.isEmpty()) return responseJson;
    QJsonParseError perr{};
    const QJsonDocument doc =
        QJsonDocument::fromJson(responseJson.toUtf8(), &perr);
    // A body this cannot parse is returned verbatim rather than replaced: the
    // advisory is worth less than the response it would destroy.
    if (perr.error != QJsonParseError::NoError || !doc.isObject())
        return responseJson;
    QJsonObject env = doc.object();
    env[QStringLiteral("ignored_args")] = QJsonArray::fromStringList(ignored);
    return QString::fromUtf8(
        QJsonDocument(env).toJson(QJsonDocument::Compact));
}

bool bulletMatchesQuery(const QJsonObject &bullet, const QString &needle,
                        QueryMode mode) {
    // headline + headline_full + body — the text surfaces the list emits.
    // headline_full is present only when a long headline was capped; absent
    // keys stringify to "" and drop out harmlessly. body is present at the
    // pre-pagination filter point (rcStripBodyFields runs post-slice), so
    // the match works even when include_body is false.
    return textMatchesQuery(
        bullet.value(QStringLiteral("headline")).toString() + QChar('\n') +
            bullet.value(QStringLiteral("headline_full")).toString() +
            QChar('\n') + bullet.value(QStringLiteral("body")).toString(),
        needle, mode);
}

bool textMatchesQuery(const QString &hayRaw, const QString &needle,
                      QueryMode mode) {
    const QString needleLower = needle.toLower();
    const QString hay = hayRaw.toLower();
    if (mode == QueryMode::Substring) return hay.contains(needleLower);

    // ANTS-4367 — the two narrowing modes. Both are case-insensitive, like
    // the default: what they narrow is the BOUNDARY (or the pattern), never
    // the casing, and a caller reaching for whole_word to find "CI" still
    // wants the bullet that writes it "ci".
    //
    // A hyphenated occurrence (`CI-parity`) matches under WholeWord, because
    // `-` is a word boundary. That is correct and is the one case where the
    // rule looks wrong at a glance — the noise this exists to remove is
    // "decision" / "efficiency" / "precision" / "facing", where the term sits
    // INSIDE a word.
    const QString pattern =
        (mode == QueryMode::WholeWord)
            ? QStringLiteral("\\b") + QRegularExpression::escape(needle) +
                  QStringLiteral("\\b")
            : needle;
    QRegularExpression re(pattern,
                          QRegularExpression::CaseInsensitiveOption);
    // An invalid pattern matches NOTHING rather than everything: a typo that
    // returned every bullet would read as "the roadmap is entirely about X",
    // which is the more expensive wrong answer. The caller-facing refusal for
    // a malformed pattern is the verb's job, not this predicate's.
    if (!re.isValid()) return false;
    return re.match(hay).hasMatch();
}

}  // namespace mcp
