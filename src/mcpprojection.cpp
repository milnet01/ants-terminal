#include "mcpprojection.h"

#include <atomic>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

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

bool isFieldProjectionTool(const QString &toolName) {
    return toolName == QStringLiteral("roadmap_query")
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
        || toolName == QStringLiteral("model_switch_stats"); // ANTS-1735
}

// ANTS-2094 — offload-eligible read verbs (see header). A separate set from
// isFieldProjectionTool: adds the large-body verbs that take no `fields=`.
bool isOffloadEligible(const QString &toolName) {
    return toolName == QStringLiteral("get_scrollback")
        || toolName == QStringLiteral("get_text")
        || toolName == QStringLiteral("read_log")
        || toolName == QStringLiteral("read_region")
        || toolName == QStringLiteral("workspace_search")
        || toolName == QStringLiteral("codebase_index")
        || toolName == QStringLiteral("docs_index")
        || toolName == QStringLiteral("find_sources")
        || toolName == QStringLiteral("roadmap_query")
        // ANTS-2093 — project_query: a large snippet result spills like any
        // other read body (INV-9), re-read verbatim via read_spill.
        || toolName == QStringLiteral("project_query");
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
        return QStringLiteral(
            "pass max_match_bytes= to clip long match lines, or a "
            "narrower lane=/glob= to cut the result set");
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
    // etag-reuse nudge — any body size (ANTS-2180).
    if (env.contains(QStringLiteral("etag")) &&
        !args.contains(QStringLiteral("etag_match")) &&
        !env.contains(QStringLiteral("next_call_hint"))) {
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
        if (!lean.isEmpty()) {
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

}  // namespace mcp
