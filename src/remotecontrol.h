#pragma once

#include <QObject>
#include <QString>
#include <QSet>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "auditengine.h"  // ANTS-1254 — AuditSummary value member below
#include "docintegrity.h"  // ANTS-3601 — DocIntegrity::Finding in helper sig
#include "doccitations.h"  // ANTS-3636 — DocCitations::Options in helper sig
#include "docsymbols.h"    // ANTS-3661 — DocSymbols::Symbol in helper sig
                           // (pulls docfinding.h for DocFinding::Finding)
#include "docdedup.h"      // ANTS-3660 — DocDedup::Result in helper sig
#include "changelogquery.h"  // ANTS-3533 — changelog_query parse-cache member
#include "coldeyesengine.h"  // ANTS-1319 — cold-eyes partition cache
#include "roadmapindex.h"  // ANTS-1287 — heading-index cache members
#include "roadmapparse.h"  // ANTS-3793 — BulletRecord, roadmapBullets()'s return

#include <QDate>       // ANTS-4501 — buildRoadmapReportEnvelope's window args
#include <functional>  // ANTS-3543 — downshiftMatches lean-projector callback
#include <memory>
#include <optional>   // ANTS-4501 — scope:"all" is a nullopt project id

class QLocalServer;
class QLocalSocket;
class QWidget;                 // ANTS-2049 — e2eResolveTarget return type
class MainWindow;
class ClaudeIntegration;
namespace VerifyTrust { class Client; }
// ANTS-3793 § 4 — the read seam reaches this header by forward declaration and
// NOT by #include: roadmapstore.h includes <QSqlDatabase>, and ants_core_lib
// links ants_roadmapstore_lib PRIVATE precisely so Qt6::Sql stops at core. A
// consumer of this header (mainwindow.cpp, claudeintegration.cpp, the test
// bundles) has no Sql include path, so including it here would break them all.
class RoadmapStore;
// ANTS-3863 — RoadmapText joins ReadError here, and by declaration for the same
// reason: the three seam wrappers take it by reference, so no definition is
// needed to declare them.
namespace RoadmapSource { enum class ReadError; class RoadmapText; }

// Remote-control server for Ants Terminal. Kitty-style JSON envelopes
// over a Unix domain socket — unlocks scripting, IDE integration, CI.
// See ROADMAP.md § 0.8.0 > 🎨 Features — multiplexing for the full
// command list; this first slice implements only `ls`, with the socket
// + envelope + client infrastructure in place for the next commands
// (`send-text`, `set-title`, `select-window`, `get-text`, `new-tab`,
// `launch`) to land one-by-one.
//
// Protocol: one JSON object per line (LF-terminated). Request shape:
//   {"cmd": "<name>", ...args}
// Response shape on success:
//   {"ok": true, ...result-fields}
// Response shape on error:
//   {"ok": false, "error": "<message>"}
//
// Socket path resolution (in order):
//   1. `$ANTS_REMOTE_SOCKET` env var — explicit override, used by
//      client + server together for multi-instance scenarios
//   2. `$XDG_RUNTIME_DIR/ants-terminal.sock` — XDG standard dir,
//      user-scoped, survives tmp-cleaner sweeps
//   3. `/tmp/ants-terminal-<uid>.sock` — fallback when XDG runtime
//      dir is unset (very unusual on modern Linux, but keeps the
//      fallback deterministic instead of failing silently)
//
// Server-side: if `listen()` fails because the path is already in use
// (another Ants instance owns it), we log and give up — remote-control
// is optional, we don't want to take the main window down with us.
// A future enhancement could fall back to a per-PID path; for now
// single-instance-per-user is the documented behaviour, and
// multi-instance users set the env var explicitly.
class RemoteControl : public QObject {
    Q_OBJECT

public:
    explicit RemoteControl(MainWindow *main, QObject *parent = nullptr);
    ~RemoteControl() override;

    // Start listening. Returns true on success; false if another
    // instance already owns the socket. Either way, MainWindow
    // construction continues (remote-control is non-critical).
    bool start();

    // Default socket path — see header doc for resolution order.
    static QString defaultSocketPath();

    // ANTS-1337 Phase 2 — install the verify-changes trust client.
    // Called from MainWindow construction with a chrome-layer
    // VerifyTrustModalClient. RemoteControl takes ownership. nullptr
    // (default) means cmdVerifyChanges runs without a trust gate,
    // preserving pre-ANTS-1337 behaviour — used by tests / headless
    // CI / ANTS_VERIFY_TRUST_AUTOTRUST=1.
    void setVerifyTrustClient(std::unique_ptr<VerifyTrust::Client> c);
    VerifyTrust::Client *verifyTrustClient() const {
        return m_verifyTrustClient.get();
    }

    // ANTS-2049 — E2E mode gate. Set true only by MainWindow when the
    // `--e2e` CLI flag was passed; the sole enabler of the inject verbs
    // (inject-key / inject-click / resize-window / grab-image) in dispatch().
    // Never set true from config/env — synthetic input is arbitrary control.
    void setE2eMode(bool on) { m_e2eMode = on; }
    bool e2eMode() const { return m_e2eMode; }

    // Client entry point — connects, sends one JSON request, reads
    // one JSON response, writes it to stdout. Called from main.cpp
    // when `--remote <cmd>` is passed. Returns process exit code
    // (0 on success, 1 on connect/parse error, 2 on server error
    // response).
    //
    // `command` is the raw command name (e.g. `"ls"`); `args` is an
    // already-constructed JSON object that will be merged into the
    // envelope under the `cmd` field at runtime.
    static int runClient(const QString &command,
                         const QJsonObject &args,
                         const QString &socketPath);

    // Strips C0 control bytes (0x00..0x1F minus HT/LF/CR), DEL (0x7F),
    // and the UTF-8 encoding of C1 control codepoints U+0080..U+009F
    // (encoded as 0xC2 0x80 .. 0xC2 0x9F) from an rc-socket payload
    // before it reaches the PTY. Both classes — 7-bit ESC-led and
    // 8-bit C1-led — would otherwise drive vtparser into CSI/OSC/DCS/
    // APC states (cursor reprogramming, OSC 52 clipboard overwrites,
    // bracketed-paste toggle, Sixel/APC image injection) from
    // untrusted same-UID input. ANTS-1335 closed the C1 byte vector
    // (the C0 strip alone was added by ANTS-0.7.52).
    //
    // Preserves HT/LF/CR (regular PTY keystrokes), all printable
    // ASCII, all multi-byte UTF-8 not in the C1 range, and any
    // malformed pre-existing input — the filter does not synthesise
    // meaning from invalid bytes.
    //
    // Returns the filtered payload. `out_stripped`, if non-null, is
    // set to the number of bytes removed (a C1 strip increments by 2
    // since the U+0080..U+009F UTF-8 sequence is two bytes; a C0 or
    // DEL strip increments by 1).
    //
    // The `send-text` request JSON may carry `"raw": true` to bypass
    // this filter; see tests/features/remote_control_opt_in/spec.md.
    //
    // Defined inline so feature tests can exercise it without pulling
    // in the full MainWindow dep chain.
    static inline QByteArray filterControlChars(const QByteArray &in,
                                                int *out_stripped = nullptr) {
        QByteArray out;
        out.reserve(in.size());
        int removed = 0;
        const int n = in.size();
        for (int i = 0; i < n; ++i) {
            const unsigned char b = static_cast<unsigned char>(in[i]);

            // ANTS-1335 — C1 8-bit form: 0xC2 followed by 0x80..0x9F
            // is the UTF-8 encoding of U+0080..U+009F (CSI / OSC /
            // DCS / APC / PM / SOS introducers). Strip both bytes
            // atomically. No other valid UTF-8 sequence can encode
            // U+0080..U+009F, so this match is unambiguous; bare
            // 0xC2 at end-of-input or followed by an out-of-range
            // byte passes through (don't synthesise meaning from
            // partial/invalid UTF-8).
            if (b == 0xC2 && i + 1 < n) {
                const unsigned char b1 =
                    static_cast<unsigned char>(in[i + 1]);
                if (b1 >= 0x80 && b1 <= 0x9F) {
                    removed += 2;
                    ++i;  // skip both bytes
                    continue;
                }
            }

            const bool isAllowedWhitespace =
                (b == 0x09 || b == 0x0A || b == 0x0D);
            const bool isC0Bad = (b < 0x20) && !isAllowedWhitespace;
            const bool isDel = (b == 0x7F);
            if (isC0Bad || isDel) {
                ++removed;
                continue;
            }
            out.append(static_cast<char>(b));
        }
        if (out_stripped) *out_stripped = removed;
        return out;
    }

    // ANTS-1348 — server-side byte cap on `get-text` responses.
    //
    // Default cap matches the MCP bridge's 1 MiB receive budget so the
    // happy path never trips the transport limit; over-ceiling values
    // (e.g. a curious test passing 256 MiB) silently clamp at 16 MiB
    // and surface via `capClamped` in the result.
    static constexpr int kGetTextDefaultBytesCap  = 1 * 1024 * 1024;   // 1 MiB
    static constexpr int kGetTextMaxBytesCeiling  = 16 * 1024 * 1024;  // 16 MiB

    // Result of `trimScrollbackForGetText`. `text` is the (possibly
    // truncation-prefixed) string to put in the response; the other
    // fields surface as response-envelope flags.
    struct GetTextTrim {
        QString text;
        qint64  bytesDropped = 0;
        qint64  linesDropped = 0;
        bool    truncated = false;
        bool    capClamped = false;
    };

    // Trim a scrollback string to fit `maxBytes` of UTF-8. Drops from
    // the *head* (oldest output) so the newest tail always survives;
    // snaps the cut to the next '\n' so the result never starts with
    // a partial line. When truncation happens, the returned text is
    // prefixed with a `<truncated N bytes / M lines>\n` sentinel so
    // line-based consumers see an unambiguous boundary marker.
    //
    // `maxBytes <= 0` falls back to the 1 MiB default. Values above
    // the 16 MiB ceiling are silently clamped (and `capClamped` is set
    // in the result). The `\n` snap is safe across multi-byte UTF-8
    // because 0x0A is never a continuation byte in any valid UTF-8
    // sequence.
    //
    // Defined inline so feature tests can exercise it without pulling
    // in the full MainWindow dep chain. See ANTS-1348 spec.
    static inline GetTextTrim trimScrollbackForGetText(
            const QString &raw, int maxBytes) {
        GetTextTrim r;

        int cap = maxBytes;
        if (cap > kGetTextMaxBytesCeiling) {
            cap = kGetTextMaxBytesCeiling;
            r.capClamped = true;
        }
        if (cap <= 0) cap = kGetTextDefaultBytesCap;

        const QByteArray utf8 = raw.toUtf8();
        if (utf8.size() <= cap) {
            r.text = raw;
            return r;
        }

        const int cut = static_cast<int>(utf8.size()) - cap;
        int snap = utf8.indexOf('\n', cut);
        if (snap < 0) snap = cut;       // no newline found; hard cut
        else          ++snap;            // skip the newline itself

        const QByteArray kept = utf8.mid(snap);
        r.bytesDropped = snap;
        r.linesDropped = utf8.left(snap).count('\n');
        r.text = QStringLiteral("<truncated %1 bytes / %2 lines>\n")
                     .arg(r.bytesDropped).arg(r.linesDropped)
                 + QString::fromUtf8(kept);
        r.truncated = true;
        return r;
    }

    // ANTS-1293 — server-side byte cap for structured read-tool responses
    // whose payload is a JSON array (file_outline `symbols[]`,
    // workspace_search `matches[]`). Count caps (max_symbols / max_results)
    // bound the *number* of items; this bounds the *total size* so a few
    // pathologically long items can't blow the transport budget. Mirrors
    // the get_text cap (ANTS-1348) but trims the array from the TAIL so the
    // leading items (earliest symbols / first matches) always survive.
    //
    // Defined inline so feature tests can exercise it without the full
    // MainWindow dep chain.
    static constexpr int kReadToolDefaultBytesCap = 512 * 1024;      // 512 KiB
    static constexpr int kReadToolMaxBytesCeiling = 4 * 1024 * 1024; // 4 MiB

    struct ArrayByteCap {
        int  itemsDropped = 0;
        bool truncated    = false;
        bool capClamped   = false;
    };

    // Trim `env[arrayField]` from the tail until the compact serialization
    // of `env` fits within `maxBytes`. Mutates env in place; on trim sets
    // env["truncated"]=true and env[droppedField]=<dropped count>.
    // `maxBytes <= 0` → default cap; `> ceiling` → clamp + capClamped.
    static inline ArrayByteCap capJsonArrayToBytes(
            QJsonObject &env, const QString &arrayField,
            const QString &droppedField, int maxBytes) {
        ArrayByteCap r;
        int cap = maxBytes;
        if (cap > kReadToolMaxBytesCeiling) {
            cap = kReadToolMaxBytesCeiling;
            r.capClamped = true;
        }
        if (cap <= 0) cap = kReadToolDefaultBytesCap;

        // Fast path: whole envelope already fits.
        if (QJsonDocument(env).toJson(QJsonDocument::Compact).size() <= cap) {
            return r;
        }

        const QJsonArray items = env.value(arrayField).toArray();
        // Base envelope size with an empty array stand-in for the field AND
        // the metadata fields we will add on trim reserved at worst-case
        // size, so the final serialized env (incl. truncated + dropped
        // count) stays within `cap`.
        QJsonObject base = env;
        base[arrayField]   = QJsonArray();
        base["truncated"]  = true;
        base[droppedField] = items.size();
        int running =
            QJsonDocument(base).toJson(QJsonDocument::Compact).size();

        QJsonArray kept;
        for (const QJsonValue &v : items) {
            // Standalone element size: `[X]` minus the two bracket bytes.
            const int itemSize =
                QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact)
                    .size() - 2;
            const int delta = itemSize + (kept.isEmpty() ? 0 : 1);  // comma
            if (running + delta > cap) break;
            running += delta;
            kept.append(v);
        }

        r.itemsDropped = items.size() - kept.size();
        if (r.itemsDropped > 0) {
            r.truncated       = true;
            env[arrayField]   = kept;
            env["truncated"]  = true;
            env[droppedField] = r.itemsDropped;
        }
        return r;
    }

    // ANTS-3543 — workspace_search auto-downshift. Applies the byte-cap to
    // out["matches"]; iff the cap dropped rows AND the caller isn't already
    // lean, re-projects the FULL pre-cap matches via `leanProjector`
    // (the handler passes rcApplyHeadlineOnly) and re-caps, so a scanning
    // caller keeps every file:line instead of losing the tail. Sets
    // downshifted / headline_only / results_dropped / bytes_cap_clamped and an
    // HONEST truncated. `scanTruncated` is out["truncated"] BEFORE this call
    // (its SCAN-cutoff meaning, set at the handler's out["truncated"]=truncated
    // — capJsonArrayToBytes overwrites truncated to true on a trim and clears
    // nothing on a fit, so the honest value is recomputed here). The projector
    // is a callback (not the TU-local rcApplyHeadlineOnly) purely so a pure
    // test can drive this socket-free, mirroring capJsonArrayToBytes.
    static inline void downshiftMatches(
            QJsonObject &out, bool alreadyLean, bool scanTruncated, int maxBytes,
            const std::function<void(QJsonArray &)> &leanProjector) {
        const QJsonArray fullMatches = out.value("matches").toArray();  // pre-cap
        auto cap = capJsonArrayToBytes(out, QStringLiteral("matches"),
                                       QStringLiteral("results_dropped"), maxBytes);
        const bool capClamped = cap.capClamped;
        if (out.contains("results_dropped") && !alreadyLean) {
            QJsonArray lean = fullMatches;
            leanProjector(lean);                 // handler passes rcApplyHeadlineOnly
            out["matches"] = lean;
            out.remove("results_dropped");       // the fat-cap drop no longer applies
            out["downshifted"]  = true;
            out["headline_only"] = true;         // reflect the shape now emitted
            capJsonArrayToBytes(out, QStringLiteral("matches"),
                                QStringLiteral("results_dropped"), maxBytes);
            // (same maxBytes ⇒ capClamped is unchanged from the first cap.)
            // HONEST truncated: scan cutoff OR the lean re-cap still dropped.
            // Without this the stale fat-cap truncated:true would falsely signal
            // "incomplete" on a fully-returned lean list — the exact signal
            // ANTS-3543 § 1 exists to prevent.
            out["truncated"] = scanTruncated || out.contains("results_dropped");
        }
        if (capClamped) out["bytes_cap_clamped"] = true;   // preserve ANTS-1293 echo
    }

    // ANTS-2220 — the function/method enclosing `line`, given a file_outline
    // `symbols[]` array (each {line, name}). Nearest-preceding symbol by
    // start line; empty when `line` precedes the first symbol (e.g. a match
    // up in the includes). Used by cmdWorkspaceSearch's `enclosing_symbol`
    // annotation. Defined inline (like capJsonArrayToBytes above) so the
    // feature test can exercise the heuristic without the MainWindow chain.
    static inline QString enclosingSymbolForLine(const QJsonArray &symbols,
                                                 int line) {
        QString best;
        int bestLine = -1;  // greatest start line <= `line`
        for (const QJsonValue &sv : symbols) {
            const QJsonObject so = sv.toObject();
            const int sl = so.value(QStringLiteral("line")).toInt();
            if (sl <= line && sl > bestLine) {
                bestLine = sl;
                best     = so.value(QStringLiteral("name")).toString();
            }
        }
        return best;
    }

    // ANTS-1347 — path-side byte hygiene for the `cwd` field on
    // `launch` and `new-tab`. Reject-not-strip semantics: silently
    // mutating a path would change its identity and mislead the
    // caller; reject the request instead and let the caller fix it.
    //
    // Rejects:
    //   - C0 controls U+0000..U+001F (all of them — HT/LF/CR are
    //     never legitimate in a path argument; the filterControlChars
    //     allowlist for those bytes applies only to text payloads).
    //   - Backslash U+005C (Windows-path-confusion vector — every
    //     Ants path is forward-slash-separated, no exceptions).
    //   - C1 controls U+0080..U+009F (path-side counterpart to
    //     ANTS-1335's byte-strip on text payloads).
    //
    // NFC-normalises the input before scanning so a decomposed-form
    // path (e.g. "café" as 'c' 'a' 'f' 'e' U+0301) doesn't sneak
    // around the U+0080..U+009F check via the combining-acute byte.
    //
    // Defined inline so feature tests can exercise it without
    // pulling in the full MainWindow dep chain.
    static inline bool cwdHasBadByte(const QString &raw) {
        const QString nfc = raw.normalized(QString::NormalizationForm_C);
        for (QChar c : nfc) {
            const ushort u = c.unicode();
            if (u < 0x20) return true;                  // C0
            if (u == 0x5C) return true;                 // backslash
            if (u >= 0x80 && u <= 0x9F) return true;    // C1
        }
        return false;
    }

    // ANTS-1244: read-only verbs promoted to public so the MCP server
    // in ClaudeIntegration can delegate to them without duplicating
    // bodies. Provider lambdas in MainWindow::setupClaudeMcpProviders
    // call these on the existing m_remoteControl instance, sharing
    // the roadmap-query cache (INV-7 in the spec).
    //
    // ANTS-1247: cmdRoadmapQuery accepts an optional `status` filter
    // in `req`. Zero-arg-equivalent (empty req) is back-compat with
    // ANTS-1244 callers — returns the full unfiltered array.
    QJsonDocument cmdRoadmapQuery(const QJsonObject &req = {});
    // ANTS-1922 — pure work-bundle builder for roadmap_query
    // mode:"bundles". Static + public so the feature test drives it
    // directly with hand-authored fixtures (the bundles branch calls it
    // on m_roadmapCacheBullets). softCapBytes bounds the emitted
    // bundles[]; truncation drops whole trailing bundles. See
    // docs/specs/ANTS-1922.md.
    // ANTS-4501 — roadmap_query mode:"report". Its own builder, on the
    // buildRoadmapBundlesEnvelope precedent directly below: cmdRoadmapQuery is
    // already past 2000 lines. Returns {} on a store error, with `error` set.
    // Reads only — INV-10.
    static QJsonObject buildRoadmapReportEnvelope(
        RoadmapStore &store, std::optional<qint64> projectId, const QDate &today,
        const QDate &since, const QDate &until, QString *error);

    static QJsonObject buildRoadmapBundlesEnvelope(
        const QJsonArray &cacheBullets, int softCapBytes);
    // ANTS-1922 — test-only soft-cap override for the bundles branch
    // (0 = use PaginationEngine::kSoftCapBytes). The pure builder above
    // takes the cap directly; this lets a live-verb path force truncation.
    void setBundleSoftCapOverride(int bytes) { m_bundleSoftCapOverride = bytes; }
    // ANTS-1583 — roadmap_branch_drift: compare ROADMAP ✅ entries'
    // cited commit SHAs against HEAD's reachable history. Reuses
    // findRoadmapUnder + collectGitSnapshot + runGit. See
    // docs/specs/ANTS-1583.md.
    QJsonDocument cmdRoadmapBranchDrift(const QJsonObject &req);
    QJsonDocument cmdTabList();
    QJsonDocument cmdGetText(const QJsonObject &req);

    // ANTS-2049 — e2e harness inject verbs. Socket-dispatch-only (hyphenated
    // on-wire keys inject-key / inject-click / resize-window / grab-image),
    // gated behind m_e2eMode (set only via setE2eMode on the --e2e launch
    // path). Deliberately NOT MCP-registered, so unreachable through the
    // agent's MCP toolset against the user's live instance (security property,
    // spec §5). e2eResolveTarget is the shared offscreen-safe widget resolver
    // (never QApplication::activeWindow()). See docs/specs/ANTS-2049.md.
    QWidget *e2eResolveTarget(const QString &objectName) const;
    QJsonDocument cmdInjectKey(const QJsonObject &req);
    QJsonDocument cmdInjectClick(const QJsonObject &req);
    QJsonDocument cmdResizeWindow(const QJsonObject &req);
    QJsonDocument cmdGrabImage(const QJsonObject &req);

    // ANTS-1301: recent_errors — scan the focused terminal's recent
    // scrollback with a per-language regex bank (ScrollbackErrors) and
    // return structured errors[]. Resolves the terminal exactly as
    // cmdGetText. MCP-only (no IPC dispatch verb, like get_scrollback).
    // See docs/specs/ANTS-1301.md.
    QJsonDocument cmdRecentErrors(const QJsonObject &req);

    // ANTS-3374: attach a `likely_fix` add_include hint to every entry in
    // `errors` whose message names an undeclared symbol that resolves to a
    // project header (BuildFixHint). Dedups by symbol and bounds the number
    // of distinct header lookups. Best-effort — an empty `root` is a no-op.
    // Shared by cmdRecentErrors + cmdBuildStatus. See
    // tests/features/mcp_likely_fix/spec.md.
    void enrichLikelyFixes(QJsonArray &errors, const QString &root) const;

    // ANTS-1312: last_selection — return the focused (or routed)
    // terminal's current selection text. Delegates to
    // TerminalWidget::selectedText(). Tab routing mirrors cmdGetText /
    // cmdRecentErrors: explicit `tab` int wins, else caller_cwd →
    // terminalForCaller → focused fallback. MCP-only.
    QJsonDocument cmdLastSelection(const QJsonObject &req);

    // ANTS-1636: find_sources — topic-to-files discovery. Project-
    // scoped read (Required caller_cwd). Delegates to
    // FindSources::findSources after resolving the project root.
    QJsonDocument cmdFindSources(const QJsonObject &req);

    // ANTS-1248: ripgrep wrapper. Public for the same reason as the
    // ANTS-1244 trio — MCP server lambda in MainWindow delegates here
    // so the body is reused across IPC + MCP transports. Argv-only
    // QProcess::start (no shell), 2 s hard-kill via constant
    // kWorkspaceSearchHardKillMs + 200 ms grace, server-clamped to
    // 500 results. See docs/specs/ANTS-1248.md.
    QJsonDocument cmdWorkspaceSearch(const QJsonObject &req);

    // ANTS-3368 — co_change_family: every edit site of one settings field,
    // grouped by file. Shares this TU's rcRunRg() call site; the matching
    // rule itself is the pure seam in cochangefamily.h.
    QJsonDocument cmdCoChangeFamily(const QJsonObject &req);

    // ANTS-3716: cited_by — given the anchors a review loop changed, which
    // documents cite which of them. One rg run per anchor so attribution is by
    // construction; the caller records the verdicts. Beside cmdWorkspaceSearch
    // in remotecontrol_workspace.cpp, whose rg runner it shares.
    // See docs/specs/ANTS-3716-cited-by-sweep.md.
    QJsonDocument cmdCitedBy(const QJsonObject &req);

    // ANTS-1249: file outline (regex scanner over a file, returns
    // header_doc + symbols[] for cpp / py / md / unknown). Shares
    // the pathInRepoRoot helper with cmdWorkspaceSearch.
    // See docs/specs/ANTS-1249.md.
    QJsonDocument cmdFileOutline(const QJsonObject &req);
    // ANTS-3745 — build_target_for: which build target owns a source, plus
    // the `cmake --build --target` and `ctest -R` lines that answer follows
    // from. Static, from CMakeLists.txt; engine in buildtargets.cpp.
    QJsonDocument cmdBuildTargetFor(const QJsonObject &req);
    // ANTS-4398 — mutation_probe: mutate, run, restore.
    QJsonDocument cmdMutationProbe(const QJsonObject &req);
    // ANTS-1855 — read_log: filter a log file (Ants debug log by
    // default, or a caller_cwd-relative path) via ReadLog::filter.
    QJsonDocument cmdReadLog(const QJsonObject &req);
    // ANTS-2021 — read_region: return a line range or a named symbol's
    // body from a caller_cwd-relative project file via ReadRegion::extract.
    QJsonDocument cmdReadRegion(const QJsonObject &req);
    // ANTS-2219 — read_regions: batched multi-selector read. Each item is a
    // {path, symbol|start_line/end_line|section, etag_match?}; returns one
    // slice envelope per item (per-item etag → individual 304) under a shared
    // max_bytes budget. The read-side mirror of apply_edits' batched writes.
    QJsonDocument cmdReadRegions(const QJsonObject &req);
    // ANTS-2022 — apply_edits: apply N {path, old, new} edits across M
    // project files in one call, atomic per file (ApplyEdits::applyToContent
    // + QSaveFile), with per-edit skipped[] accounting.
    QJsonDocument cmdApplyEdits(const QJsonObject &req);
    // ANTS-2094 — read_spill: re-read a result spilled by the offload path,
    // by its content-addressed handle, byte-paged. Not project-scoped
    // (global content-addressed cache), so caller_cwd is Optional. Resolves
    // ONLY under the spill dir via mcp::readSpill.
    QJsonDocument cmdReadSpill(const QJsonObject &req);
    // ANTS-1637 — codebase_index: serve a pre-computed project structural map
    // (symbol / lane / file_path / summary) via CodebaseIndex::serve.
    QJsonDocument cmdCodebaseIndex(const QJsonObject &req);
    // ANTS-2139 — docs_index: serve a pre-computed project documentation map
    // (summary / topic / doc_path / id) via DocsIndex::serve.
    QJsonDocument cmdDocsIndex(const QJsonObject &req);
    // ANTS-3601 — doc_integrity: deterministic dead-anchor / broken-link / TOC
    // checks over a project-relative doc set (DocIntegrity::check).
    QJsonDocument cmdDocIntegrity(const QJsonObject &req);
    // Pure helpers extracted for headless testing (the handler needs a live
    // MainWindow). `docIntegrityEnumerate` maps a validated `path` → the sorted
    // project-relative doc set (dir → recursive *.md; file → one; empty → the
    // docsDirDefault walk; non-existent → empty). `docIntegrityBuildResponse`
    // filters findings + counts by `kinds` (empty = all).
    static QStringList docIntegrityEnumerate(const QString &rootCanonical,
                                             const QString &rawPath,
                                             const QString &docsDirDefault);
    // ANTS-4106 — the `paths:[…]` form: the de-duplicated, sorted union of
    // each entry's enumeration. An empty list yields an empty set (the caller
    // falls back to the single-`path` walk), so "no paths given" and "paths
    // matched nothing" stay distinguishable at the call site.
    static QStringList docIntegrityEnumerateMany(const QString &rootCanonical,
                                                 const QStringList &rawPaths,
                                                 const QString &docsDirDefault);
    static QJsonObject docIntegrityBuildResponse(
        const QList<DocIntegrity::Finding> &findings,
        const QSet<QString> &kinds, const QStringList &checkedDocs);
    // ANTS-3737 — content fingerprint of the checked doc set, emitted as
    // `docs_digest` by doc_integrity / doc_symbols / spec_lint. These three
    // report FINDINGS rather than content, so without it the central ETag
    // (a sha256 of the response body) cannot see a doc edit that changes no
    // finding — and `etag_match` returns a false 304 on the post-fix re-check.
    // Public-static for the same headless-test reason as the pair above.
    static QString docSetDigest(const QString &rootCanonical,
                                const QStringList &checkedDocs);
    // ANTS-3661 — doc_symbols: resolve the identifiers a doc asserts something
    // about (DocSymbols::scan). Reuses docIntegrityEnumerate for the walk
    // rather than cloning it — one *.md enumeration, not two that can drift.
    QJsonDocument cmdDocSymbols(const QJsonObject &req);
    // Pure, for the same headless-test reason as the pair above. `truncated`
    // is the run-wide OR; counts are occurrence counts across every document.
    static QJsonObject docSymbolsBuildResponse(
        const QVector<DocSymbols::Symbol> &symbols,
        const QList<DocFinding::Finding> &findings, bool truncated,
        const QStringList &checkedDocs);
    // ANTS-3662 — spec_lint: the greppable half of the spec-format contract
    // (SpecLint::check). Walks `specs_dir`, not `docs_dir` — a README has no
    // invariants to check — but reuses docIntegrityEnumerate for the walk, for
    // the same reason doc_symbols does.
    QJsonDocument cmdSpecLint(const QJsonObject &req);
    // Pure, for the same headless-test reason as the pairs above.
    // `sectionsChecked` is ALWAYS emitted: it is the one field distinguishing
    // "every required section is there" from "nobody checked", and those are
    // the two states this verb ships between (spec § 2.1).
    //
    // ANTS-4127 — `surfacesResolved` / `surfacesChecked` are emitted under the
    // same rule and are not defaulted here: a caller that forgets to pass them
    // should fail to compile rather than ship a silent zero, which is exactly
    // the "nobody checked, reported as clean" state the pair exists to prevent.
    static QJsonObject specLintBuildResponse(
        const QList<DocFinding::Finding> &findings, bool sectionsChecked,
        const QJsonObject &lineCounts, bool truncated,
        const QStringList &checkedDocs, int surfacesResolved,
        bool surfacesChecked,
        // ANTS-4373/4390 — the standard path actually consulted, empty when
        // none resolved. Reported so a caller is told what to fix rather than
        // re-deriving it from a bare boolean.
        const QString &sectionsSource = QString());
    // ANTS-4108 — spec_conformance: RUN the patterns a spec prescribes against
    // the expectation table beside them (SpecConformance::run). Reads ONE
    // document, so `path` is required — there is no tree walk to default to.
    QJsonDocument cmdSpecConformance(const QJsonObject &req);
    // Pure, for the same headless-test reason as the pairs above, and because
    // this verb's ETag 304 is HANDLER-LOCAL rather than central: the envelope
    // carries a measured-microsecond `observations[]` row per case, so the
    // dispatcher's hash-the-whole-response etag would differ on every run and
    // never match. The engine hashes the envelope minus `observations` (spec
    // INV-9) and this compares it. `relPath` replaces the engine's absolute
    // `path` — the envelope crosses the wire. A refusal passes through
    // untouched: it carries no etag, and 304-ing one would report "unchanged"
    // for a call that never ran.
    static QJsonObject specConformanceBuildResponse(const QJsonObject &engineOut,
                                                    const QString &relPath,
                                                    const QString &etagMatch);
    // ANTS-3660 — doc_dedup: near-duplicate passages across a doc set
    // (DocDedup::Accumulator). Reuses docIntegrityEnumerate for the walk like
    // its two siblings. The ONE verb in this family whose engine is
    // corpus-scoped: it accumulates every document then scores once, so there
    // is no per-document result to build a response from.
    QJsonDocument cmdDocDedup(const QJsonObject &req);
    // Pure, for the same headless-test reason as the pairs above. `pairs[]` and
    // `clusters[]` live only in this envelope — DocFinding::Finding has no
    // second-location field, so a findings row cannot carry both ends of a
    // pair, and ANTS-3663 hoists both arrays whole.
    static QJsonObject docDedupBuildResponse(const DocDedup::Result &result,
                                             const QStringList &checkedDocs);
    // ANTS-3661 § 2.4 — the registry-sourced half of DocSymbols'
    // `excludedNames`. Injected because ants_core_lib is Qt6::Core-only and the
    // library dependency runs core → claude: RemoteControl cannot see
    // ClaudeIntegration, so MainWindow (which sees both) installs this from its
    // constructor, immediately after `m_remoteControl` is allocated.
    //
    // ANTS-3688 — that position is load-bearing and is asserted by
    // `tests/features/doc_symbols_verb/`. This setter takes the object eagerly,
    // unlike the rcDelegate registrations, so installing it from
    // setupClaudeMcpProviders() (which runs far earlier, from
    // setupStatusBarChrome()) silently did nothing. An absent provider is NOT a
    // benign "noisier run": it drops every MCP verb name from the exclusion
    // set, and the verb whose purpose is a short unresolved list then reports
    // ~74 of them as findings on every document that names one.
    void setMcpVerbVocabularyProvider(std::function<QStringList()> p) {
        m_mcpVerbVocabularyProvider = std::move(p);
    }
    // ANTS-3636 — doc_citations: resolve a doc's path:line citations against the
    // files and return the cited line text (DocCitations::check).
    QJsonDocument cmdDocCitations(const QJsonObject &req);
    // Pure statics, for the same reason as the pair above: the handler needs a
    // live MainWindow and these must run in a unit test that has none.
    // `docCitationsValidate` takes rootCanonical rather than resolving it (that
    // resolution is what needs the MainWindow) and returns a refusal envelope,
    // or an empty object for input it accepts. It never decodes the doc:
    // undecodable-as-UTF-8 is the same read_failed to the caller but is raised
    // by DocCitations::check, which is about to read the file anyway.
    // `docCitationsClampOptions` needs no root at all — request args in,
    // Options out, coercing out-of-domain values rather than refusing.
    static QJsonObject docCitationsValidate(const QString &rootCanonical,
                                            const QJsonObject &req);
    static DocCitations::Options docCitationsClampOptions(const QJsonObject &req);

    // ANTS-4728 — the markdown basename supplement, factored out of the
    // handler so it can be driven without a live MainWindow (the handler needs
    // one; a directory walk does not). Appends `basename -> relpath` into
    // `index` and returns the number of files scanned. Deliberately APPENDS:
    // a basename that ends up with two paths takes the engine's existing
    // `ambiguous` arm rather than being resolved here by a guess.
    static int docCitationsMdScan(const QString &rootCanonical,
                                  QHash<QString, QStringList> *index);
    // ANTS-2161 — project_settings: detect a misplaced layout + create/update
    // .ants/project.json (ops detect / init / set) via ProjectSettings.
    QJsonDocument cmdProjectSettings(const QJsonObject &req);

    // ANTS-1961 — feedback_query: read the un-triaged tail of a
    // *_Ants_MCP_Feedback.md file (delegates to FeedbackFile::parse).
    // ANTS-1962 — feedback_log: append a contributor finding block or a
    // maintainer tracking block (delegates to FeedbackFile renderers).
    // Both are m_main-independent (absolute path used as-is; relative
    // path resolved off caller_cwd canonicalisation).
    QJsonDocument cmdFeedbackQuery(const QJsonObject &req);
    QJsonDocument cmdFeedbackLog(const QJsonObject &req);
    // ANTS-1963 — spec_log: flip Status / append a cold-eyes loop entry /
    // append an INV bullet in docs/specs|phases/<id>.md (delegates to the
    // pure SpecLog transforms). m_main-independent (caller_cwd canonical
    // + filesystem only, mirroring changelog_log).
    QJsonDocument cmdSpecLog(const QJsonObject &req);
    // ANTS-2129 — audit_falsepos_log: append a confirmed false positive to
    // <root>/.ants_review_falsepos.jsonl (delegates to the pure
    // ants::falsepos::appendEntry, which does the atomic O_APPEND). Resolves
    // the root via resolveCallerCwdRoot (m_main-dependent for the tab
    // fallback, like the other root-resolving verbs).
    QJsonDocument cmdAuditFalseposLog(const QJsonObject &req);
    // ANTS-1713 — audit_dismiss: record a learned-false-positive verdict
    // into <root>/.audit_cache/learned-fp.jsonl (delegates to the pure
    // ants::auditfp::appendEntry). Deferred v2 of ANTS-1708, which shipped
    // the ledger + GUI recording path but no MCP write side. Takes either a
    // `fingerprint` or (`file`, `message`) the server hashes via
    // computeFingerprint. Distinct from cmdAuditFalseposLog: that writes the
    // PROSE ledger the review skills read; this writes the FINGERPRINT
    // ledger both audit_run and the Audit dialog filter on.
    QJsonDocument cmdAuditDismiss(const QJsonObject &req);

    // ANTS-1303: find_definition / find_caller — tree-wide regex
    // symbol scanner. Delegate to SymbolQuery; resolve the root via
    // resolveRootCanonical, validate `symbol` (→ bad_args). Public so
    // the MCP provider lambdas in MainWindow delegate here.
    // See docs/specs/ANTS-1303.md.
    QJsonDocument cmdFindDefinition(const QJsonObject &req);
    QJsonDocument cmdFindCaller(const QJsonObject &req);

    // ANTS-1305: similar_code — tree-wide shape matcher. Reuses the
    // FileOutline extractor + ranks signatures by token-set Jaccard
    // similarity to a free-text `shape` query. Delegate to SimilarCode;
    // resolve the root via resolveRootCanonical, validate `shape`
    // (→ bad_args). Public so the MCP provider lambda in MainWindow
    // delegates here. See docs/specs/ANTS-1305.md.
    QJsonDocument cmdSimilarCode(const QJsonObject &req);

    // ANTS-1250: git_state — single tool, dispatches on `op` field
    // (status / log / diff). Wraps gitwrap.cpp's shell-less QProcess
    // helper. Argv-only, --separator + ./ prefix on -leading paths,
    // strict regex on the diff range (rejects leading -). Public so
    // the MCP provider lambda in MainWindow delegates here.
    // See docs/specs/ANTS-1250.md.
    QJsonDocument cmdGitState(const QJsonObject &req);

    // ANTS-1251: subsystem — single tool, dispatches on `op` field
    // (map / files / recent_changes). Parses the project's CLAUDE.md
    // Module map, returns per-lane file lists, and (via cmdGitState
    // composition) per-lane git history. Public so the MCP provider
    // lambda in MainWindow delegates here.
    // See docs/specs/ANTS-1251.md.
    QJsonDocument cmdSubsystem(const QJsonObject &req);

    // ANTS-1254: last_audit_summary — opens latest .audit_cache/audit-*.sarif
    // and returns compact summary (counts + top_findings). Single-entry
    // mtime-keyed cache; SARIF parsing delegated to
    // AuditEngine::summariseSarif. See docs/specs/ANTS-1254.md.
    QJsonDocument cmdLastAuditSummary(const QJsonObject &req);

    // ANTS-1569: current_state — one-call session-start state recovery.
    // Aggregates cmdRoadmapQuery (active filter) + cmdGitState(status)
    // + cmdLastAuditSummary + .claude/workflow.md best-effort parse +
    // docs/specs/<active-id>.md probe into a single envelope. Pure
    // composer — no new file reads or cache layer beyond what the
    // upstream verbs already do. MCP-only (mirrors last_audit_summary;
    // no IPC dispatch branch). See docs/specs/ANTS-1569.md.
    QJsonDocument cmdCurrentState(const QJsonObject &req);
    // ANTS-1735: model_switch_stats — read-only aggregation of the model-switch
    // effectiveness ledger, scoped to the caller's project. MCP-only (mirrors
    // current_state; no IPC dispatch branch). See docs/specs/ANTS-1735.md §2.5.
    QJsonDocument cmdModelSwitchStats(const QJsonObject &req);
    // ANTS-1724: compact session-state envelope for fresh-session orientation.
    QJsonDocument cmdSessionBrief(const QJsonObject &req);
    // ANTS-1883: bundle of current_state + project_layout +
    // roadmap_query mode:section_index status:active in one
    // envelope under a single ETag — for fresh-session orientation.
    QJsonDocument cmdSessionOrient(const QJsonObject &req);
    // ANTS-1723: per-project, per-skill step/phase store for superpowers skills.
    QJsonDocument cmdWorkflowState(const QJsonObject &req);

    // ANTS-1306: task_priors — given a free-text task description,
    // return ranked context buckets (matching specs, ROADMAP cards,
    // recent commits touching named paths, related ADRs) in one call.
    // Pure composer over cmdRoadmapQuery + cmdGitState + the spec
    // parser; no new lib, no cache, MCP-only. See docs/specs/ANTS-1306.md.
    QJsonDocument cmdTaskPriors(const QJsonObject &req);

    // ANTS-1307: project_conventions — return the curated subset of
    // project conventions relevant to a task_type (feature/bugfix/
    // refactor/docs/test), each {rule, source} with a source-existence
    // check. Static table, MCP-only. See docs/specs/ANTS-1307.md.
    QJsonDocument cmdProjectConventions(const QJsonObject &req);

    // ANTS-1302: focused_test — run only the ctest subset touching the
    // changed files. Resolves changed_files (or auto-derives from git
    // status) to ctest -R patterns via tests/coverage-map.json (or a
    // heuristic, or full-suite fallback), runs ctest, and returns the
    // test_results (ANTS-1300) envelope shape plus selection metadata.
    // Re-entrancy-gated, Expensive, MCP-only. See docs/specs/ANTS-1302.md.
    QJsonDocument cmdFocusedTest(const QJsonObject &req);

    // ANTS-1309: spec_query — parse a single docs/specs/<id>.md file
    // and return {title, status, kind, invariants[], path}. Token-
    // frugal alternative to a full Read of a spec when the caller
    // only wants the invariant list. MCP-only; no IPC dispatch.
    QJsonDocument cmdSpecQuery(const QJsonObject &req);

    // ANTS-1308: invariant_check — given a `files[]` array (project-
    // relative paths), scan docs/specs/*.md for specs that mention
    // any of those files and return their parsed invariant lists.
    // Shares the spec parser with cmdSpecQuery. MCP-only.
    QJsonDocument cmdInvariantCheck(const QJsonObject &req);

    // ANTS-1299: build_status — record/read the most recent build's
    // {exit_code, errors:[], warnings_count} summary at
    // <root>/.audit_cache/build.json. op ∈ {record, read} (default
    // read). MCP-only. See docs/specs/ANTS-1299.md.
    QJsonDocument cmdBuildStatus(const QJsonObject &req);

    // ANTS-1300: test_results — record/read the most recent ctest run's
    // {passed, failed, skipped, total, failing_tests:[…]} summary at
    // <root>/.audit_cache/tests.json. op ∈ {record, read} (default
    // read); op=read also accepts detail=<name> to return one
    // failing test's full excerpt. MCP-only.
    QJsonDocument cmdTestResults(const QJsonObject &req);

    // ANTS-1112 — five `indie_review_*` MCP tools. All resolve the
    // active project via the focused TerminalWidget's shellCwd
    // (matches git_state / subsystem / last_audit_summary). Pure
    // delegation to IndieReviewEngine + (for cmdIndieReviewFoldIn)
    // RoadmapFoldIn helpers. See docs/specs/ANTS-1112.md.
    QJsonDocument cmdIndieReviewPartition(const QJsonObject &req);
    QJsonDocument cmdIndieReviewBrief(const QJsonObject &req);
    QJsonDocument cmdIndieReviewCorroborate(const QJsonObject &req);
    QJsonDocument cmdIndieReviewSynthesisPrompt(const QJsonObject &req);
    QJsonDocument cmdIndieReviewFoldIn(const QJsonObject &req);

    // ANTS-1352 — server-side dispatch orchestrator. Fires N parallel
    // HTTP POSTs to Config::aiEndpoint, saves each response under
    // reports_dir. See docs/specs/ANTS-1352.md.
    QJsonDocument cmdIndieReviewDispatch(const QJsonObject &req);

    // ANTS-1279 — single-call dispatch manifest for a Claude-Code-driven
    // /indie-review sweep: derivePartition + per-lane brief manifest +
    // suggested_merges + per-lane report paths + next-steps, so the parent
    // fires one Agent per lane without first calling partition + N briefs.
    // Collection reuses indie_review_corroborate + indie_review_fold_in.
    // See docs/specs/ANTS-1279.md.
    QJsonDocument cmdIndieReviewOrchestrate(const QJsonObject &req);

    // ANTS-1113 — four `debt_sweep_*` MCP tools. Same project-path
    // resolution as `indie_review_*`. Pure delegation to
    // DebtSweepEngine + (for cmdDebtSweepDefer) RoadmapFoldIn helpers.
    // See docs/specs/ANTS-1113.md.
    QJsonDocument cmdDebtSweepScan(const QJsonObject &req);
    QJsonDocument cmdDebtSweepApplyFix(const QJsonObject &req);
    QJsonDocument cmdDebtSweepDefer(const QJsonObject &req);
    QJsonDocument cmdDebtSweepTriagePrompt(const QJsonObject &req);

    // ANTS-1289 — verify_changes. Drives the project's build → tests →
    // lint gates and returns structured pass/fail. Pure delegation to
    // VerifyEngine. See docs/specs/ANTS-1289.md.
    QJsonDocument cmdVerifyChanges(const QJsonObject &req);

    // ANTS-1290 — plan_template. Emits an Ants-conventional
    // implementation-plan skeleton with project conventions
    // pre-baked. Pure delegation to PlanTemplateEngine. See
    // docs/specs/ANTS-1290.md.
    QJsonDocument cmdPlanTemplate(const QJsonObject &req);

    // ANTS-1284 — token_usage. Reads ClaudeIntegration's
    // TokenUsageEngine::Tracker and returns the per-tool dispatch
    // report. See docs/specs/ANTS-1284.md.
    //
    // ANTS-1422 pull 3 — `explicitCi` is the canonical path. The
    // MCP lambda in mainwindow.cpp passes the captured
    // `m_claudeIntegration` directly. The pull-1/2 fallback through
    // `m_main->claudeIntegration()` was deleted: it returned null
    // on a live build (no static-analysis explanation, bug class
    // unreproducible after the bypass), and the only call site
    // (the lambda) always supplies `explicitCi`.
    QJsonDocument cmdTokenUsage(const QJsonObject &req,
                                ClaudeIntegration *ci);

    // ANTS-1319 — four `cold_eyes_*` MCP tools. Mirror to indie_review
    // / debt_sweep fold-in pattern. Pure delegation to ColdEyesEngine
    // + (for cmdColdEyesFoldIn) RoadmapFoldIn helpers. See
    // docs/specs/ANTS-1319.md.
    QJsonDocument cmdColdEyesPartition(const QJsonObject &req);
    QJsonDocument cmdColdEyesBrief(const QJsonObject &req);
    QJsonDocument cmdColdEyesCrossDocDiff(const QJsonObject &req);
    QJsonDocument cmdColdEyesFoldIn(const QJsonObject &req);
    // ANTS-1413 — single-doc cross-consistency brief (no partition).
    QJsonDocument cmdColdEyesSingleDoc(const QJsonObject &req);
    // ANTS-1414 — lane-source-agnostic cross-doc-diff alias.
    QJsonDocument cmdCrossDocDiff(const QJsonObject &req);

    // ANTS-1283 — session_memory KV. Per-cwd key-value persistence to
    // ~/.cache/ants-terminal/mcp-state/<cwd-hash>.json. Pure
    // delegation to SessionMemoryEngine::execute. See
    // docs/specs/ANTS-1283.md.
    QJsonDocument cmdSessionMemory(const QJsonObject &req);

    // ANTS-1430 — project_layout read verb. Scan-if-stale wrapper
    // over ProjectLayoutEngine + SessionMemoryEngine (well-known
    // key `project_layout`). Required contract; gateErrorEnvelope
    // on caller_cwd mismatch. See docs/specs/ANTS-1430.md.
    QJsonDocument cmdProjectLayout(const QJsonObject &req);

    // ANTS-1424 — roadmap_log. Append a new bullet to ROADMAP.md.
    // Mutates project state (counter + markdown). Required contract
    // gates absent caller_cwd; PathValidation anchors paths to the
    // caller's project root. See docs/specs/ANTS-1424.md.
    //
    // ANTS-1428 — adapter mode adds `op:"flip"` for GFM-format
    // roadmaps; cmdRoadmapLog routes to cmdRoadmapLogFlip on that
    // value. Default `op:"append"` preserves ANTS-1424 behaviour
    // byte-for-byte. See docs/specs/ANTS-1428.md § Tier 2.
    QJsonDocument cmdRoadmapLog(const QJsonObject &req);

    // ANTS-3855 — roadmap_migrate: load one project's markdown roadmap into
    // the roadmap store. The ONLY production entry point into the migration
    // engine. This handler resolves the caller's root, stamps the clock once
    // and names RoadmapStore::defaultPath(); everything that happens to a
    // store is RoadmapMigrateVerb::run(), which takes the path as a parameter
    // so a test can drive it against a QTemporaryDir instead of the user's
    // real store. See docs/specs/ANTS-3855-roadmap-migrate-verb.md § 2.1.1.
    QJsonDocument cmdRoadmapMigrate(const QJsonObject &req);
    // ANTS-4622 — the cross-session mailbox (ops send / inbox / ack).
    QJsonDocument cmdSessionMessage(const QJsonObject &req);

    // ANTS-1548 — changelog_log: token-frugal Keep-a-Changelog writer.
    // op:"add" renders a bullet under a category in `## [Unreleased]`;
    // op:"add_from_roadmap" reuses a ROADMAP bullet's prose verbatim by
    // id. caller_cwd-required (write op); atomic via QSaveFile.
    // Contract: tests/features/changelog_log_writer/spec.md.
    QJsonDocument cmdChangelogLog(const QJsonObject &req);

    // ANTS-3533 — changelog_query: read-only structured reader over
    // CHANGELOG.md (mirror of cmdRoadmapQuery). Modes entries /
    // version_index / headline_only; version/category/query/id/ids
    // filters; mtime+TTL parse cache. See docs/specs/ANTS-3533.md.
    QJsonDocument cmdChangelogQuery(const QJsonObject &req);

    // ANTS-2044 — op:"add_batch": N entries, one read + one atomic
    // QSaveFile commit (parity with roadmap_log op:append_batch). Each
    // entry resolves by auto-detected mode — `summary` → add,
    // `id`-only → add_from_roadmap; per-entry failures land in
    // skipped[] while the rest apply. m_main-independent.
    QJsonDocument cmdChangelogLogAddBatch(const QJsonObject &req);

    // ANTS-1346 test-only inspectors for the section-cache LRU.
    int sectionCacheSizeForTest() const {
        return m_roadmapSectionCache.size();
    }
    QStringList sectionLruForTest() const {
        return QStringList(m_roadmapSectionLru.cbegin(),
                           m_roadmapSectionLru.cend());
    }

    // ANTS-1359 — test-only entry point + cache inspectors. Bypasses
    // the MainWindow / RcGate path so tests can drive cmdVerifyChanges
    // against a synthetic project root inside a QTemporaryDir without
    // standing up a real MainWindow. See docs/specs/ANTS-1359.md § 3.
    QJsonDocument cmdVerifyChangesWithRoot(const QString &root,
                                           const QJsonObject &req);
    int verifyCacheSizeForTest() const { return m_verifyCache.size(); }
    QStringList verifyCacheLruForTest() const {
        return QStringList(m_verifyCacheLru.cbegin(),
                           m_verifyCacheLru.cend());
    }
    void putVerifyCacheForTest(const QString &key,
                               const QJsonObject &response);
    QJsonObject tryGetVerifyCacheForTest(const QString &key) const;
    void clearVerifyCacheForTest() {
        m_verifyCache.clear();
        m_verifyCacheLru.clear();
    }
    bool verifyInFlightForTest() const { return m_verifyInFlight; }
    void setVerifyInFlightForTest(bool v) { m_verifyInFlight = v; }

    // ANTS-1433 — test-only seam for the two-stage QSaveFile commit in
    // cmdRoadmapLog. setForceCounterCommitFailForTest(true) forces the
    // .roadmap-counter commit to fail so the ROADMAP.md rollback path
    // can be asserted; cmdRoadmapLogAppendForTest drives the append
    // path against a synthetic caller_cwd without a MainWindow. See
    // tests/features/mcp_roadmap_log_atomicity/spec.md.
    static void setForceCounterCommitFailForTest(bool on);
    QJsonDocument cmdRoadmapLogAppendForTest(const QJsonObject &req);
    // ANTS-1962 / ANTS-1963 — test-only QSaveFile commit-failure seams,
    // mirroring setForceCounterCommitFailForTest. Force the atomic write
    // in cmdFeedbackLog / cmdSpecLog to fail so the atomicity tests can
    // assert the original file is left byte-for-byte untouched. See
    // tests/features/mcp_feedback_log/spec.md + mcp_spec_log/spec.md.
    static void setForceFeedbackWriteFailForTest(bool on);
    static void setForceSpecWriteFailForTest(bool on);
    // ANTS-1717/1793 — drive the flip/annotate path (note append +
    // status flip) against a synthetic caller_cwd without a MainWindow.
    // Flip is m_main-independent. See
    // tests/features/roadmap_log_annotate/spec.md.
    QJsonDocument cmdRoadmapLogFlipForTest(const QJsonObject &req);
    // ANTS-3406 — drive the amend_body path (exact single-line body
    // patch) against a synthetic caller_cwd without a MainWindow.
    // m_main-independent. See tests/features/roadmap_log_amend_body/spec.md.
    QJsonDocument cmdRoadmapLogAmendBodyForTest(const QJsonObject &req);
    QJsonDocument cmdRoadmapLogAmendHeadlineForTest(const QJsonObject &req);
    // ANTS-4667 — op:"amend_field": store-only, so the seam is the
    // section ops' shape (no MainWindow, no counter).
    QJsonDocument cmdRoadmapLogAmendFieldForTest(const QJsonObject &req);
    // ANTS-1690 — drive the batch-flip path against a synthetic
    // caller_cwd without a MainWindow. See
    // tests/features/roadmap_log_flip_batch/spec.md.
    QJsonDocument cmdRoadmapLogFlipBatchForTest(const QJsonObject &req);
    // ANTS-1878 — drive the create_section path against a synthetic
    // caller_cwd without a MainWindow. m_main-independent.
    QJsonDocument cmdRoadmapLogCreateSectionForTest(const QJsonObject &req);
    // ANTS-1879 — drive the append_batch path against a synthetic
    // caller_cwd without a MainWindow. m_main-independent (counter +
    // markdown only).
    QJsonDocument cmdRoadmapLogAppendBatchForTest(const QJsonObject &req);
    // ANTS-1691 — drive the bundle_row path against a synthetic
    // caller_cwd without a MainWindow. m_main-independent (filesystem
    // only). See tests/features/roadmap_log_bundle_row/spec.md.
    QJsonDocument cmdRoadmapLogBundleRowForTest(const QJsonObject &req);
    // ANTS-4070 — drive rotate_minor / retitle_section against a synthetic
    // caller_cwd without a MainWindow. Store-only ops, so m_main-independent.
    // See tests/features/roadmap_rotate_minor/spec.md.
    QJsonDocument cmdRoadmapLogRotateMinorForTest(const QJsonObject &req);
    QJsonDocument cmdRoadmapLogRetitleSectionForTest(const QJsonObject &req);
    // ANTS-4501 § 2.3 — drive the git backfill against a synthetic caller_cwd
    // without a MainWindow. See tests/features/roadmap_backfill_dates/spec.md.
    QJsonDocument cmdRoadmapLogBackfillDatesForTest(const QJsonObject &req);
    // ANTS-4585 phase 2 — drive the trailer repair against a synthetic
    // caller_cwd. See tests/features/roadmap_repair_trailers/spec.md.
    QJsonDocument cmdRoadmapLogRepairTrailersForTest(const QJsonObject &req);

    // ANTS-4614 — render: publish the store to ROADMAP.md with no semantic
    // change, so a migration's normalisation can be landed on demand instead
    // of waiting for the next unrelated write. Store-only, m_main-independent.
    // See tests/features/roadmap_write_half/spec.md.
    QJsonDocument cmdRoadmapLogRenderForTest(const QJsonObject &req);
    // ANTS-3822 § 2.3.2 — lower the history cap this instance opens its store
    // with, so a test can reach the cap without 250 MiB of real history. Must be
    // called BEFORE the first verb, since the store is opened lazily and cached.
    // See tests/features/roadmap_write_history/spec.md.
    // Defined in remotecontrol_roadmap_query.cpp, not inline: it discards the
    // cached store, and ~unique_ptr<RoadmapStore> needs the complete type, which
    // this header does not have (see m_roadmapStore's comment).
    void setRoadmapHistoryCapForTest(qint64 bytes);

private slots:
    void onNewConnection();

private:
    // ANTS-3661 — see setMcpVerbVocabularyProvider.
    std::function<QStringList()> m_mcpVerbVocabularyProvider;

    QJsonDocument dispatch(const QJsonObject &req);
    // ANTS-1428 — adapter-mode write path for GFM-format roadmaps.
    // Dispatched from cmdRoadmapLog when req["op"] == "flip". See
    // docs/specs/ANTS-1428.md § Tier 2.
    QJsonDocument cmdRoadmapLogFlip(const QJsonObject &req);
    // ANTS-3406 — amend_body: patch a bullet's continuation body prose
    // in place (exact single-line old_text→new_text). Dispatched from
    // cmdRoadmapLog when req["op"] == "amend_body". Standalone (not
    // threaded into cmdRoadmapLogFlip); reuses the shared walk/scrub
    // helpers. See tests/features/roadmap_log_amend_body/spec.md.
    // ANTS-4372 — `headlineMode` retargets the same locate / format-gate /
    // write machinery at the bullet's HEADLINE line instead of its body span.
    // A shared parameter rather than a sibling handler: the locate (id >
    // anchor > headline), the fenced-block refusal, the pass-headings gate,
    // the unique-match guard and the atomic write are identical, and a second
    // copy of ~400 lines is how the two drift.
    QJsonDocument cmdRoadmapLogAmendField(const QJsonObject &req);
    QJsonDocument cmdRoadmapLogAmendBody(const QJsonObject &req,
                                         bool headlineMode = false);
    // ANTS-1690 — batch flip: flip N bullets to one to_status in a
    // single read + single QSaveFile commit. Dispatched from
    // cmdRoadmapLog when req["op"] == "flip_batch".
    QJsonDocument cmdRoadmapLogFlipBatch(const QJsonObject &req);
    // ANTS-1433 — append path, split out of cmdRoadmapLog so the
    // mcp_roadmap_log_atomicity test can drive it without the m_main
    // guard. cmdRoadmapLog keeps op-dispatch + the m_main guard, then
    // delegates here for op:"append".
    QJsonDocument cmdRoadmapLogAppend(const QJsonObject &req);
    // ANTS-1878 — create_section: splice a new ## / ### heading after
    // an existing section. No counter touch; single atomic write.
    QJsonDocument cmdRoadmapLogCreateSection(const QJsonObject &req);
    // ANTS-1879 — append_batch: append N bullets to one section in a
    // single read + single atomic commit. Mirrors flip_batch's
    // partial-apply contract (ok:true even when all-skipped); shared
    // bullet-formatting helper extracted from cmdRoadmapLogAppend.
    QJsonDocument cmdRoadmapLogAppendBatch(const QJsonObject &req);
    // ANTS-1691 — bundle_row: append a row to the Markdown table under a
    // named section (the "## 📊 Bundle progress" cross-session table),
    // pipe/newline-escaping each cell; find-or-create the table. No
    // counter touch; single atomic write.
    QJsonDocument cmdRoadmapLogBundleRow(const QJsonObject &req);
    // ANTS-4070 — rotate_minor: move a closed minor's sections (and their
    // descendants) to docs/roadmap/<M>.<N>.md, re-slugging them the way the
    // importer will. retitle_section: change a section's heading, recomputing
    // its slug. Both are store-only — they refuse op_unsupported on a project
    // the migration has not taken over. See
    // docs/specs/ANTS-4070-rotation-and-section-title.md.
    QJsonDocument cmdRoadmapLogRotateMinor(const QJsonObject &req);
    QJsonDocument cmdRoadmapLogRetitleSection(const QJsonObject &req);
    // ANTS-4501 § 2.3 — backfill_dates: walk this project's git history over
    // its roadmap files and fill `created` / `shipped` for the rows that
    // predate § 2.2's forward stamping. One-off and opt-in; never a side
    // effect of a read, because INV-10 forbids the report from writing at all.
    // Store-only, like the two above: `project_not_registered` on a project the
    // migration has not taken over. See docs/specs/ANTS-4501-roadmap-report.md.
    QJsonDocument cmdRoadmapLogBackfillDates(const QJsonObject &req);
    // ANTS-4585 phase 2 — repair_trailers: re-parse each item's surviving
    // prose and extend the trailer columns migration cut short. Guarded by
    // strict prefix, so it can only ever lengthen a value with more of the
    // author's own adjacent text. Store-only, like the ops above.
    // See tests/features/roadmap_repair_trailers/spec.md.
    QJsonDocument cmdRoadmapLogRepairTrailers(const QJsonObject &req);

    // ANTS-4614 — render: the shared write sequence with a mutate that does
    // nothing, so the canonical file can be published without inventing a
    // semantic write to trigger it. Body in remotecontrol_roadmap_publish.cpp.
    QJsonDocument cmdRoadmapLogRender(const QJsonObject &req);
    // ANTS-1879 INV-10 — shared bullet-formatting helper extracted from
    // cmdRoadmapLogAppend's :3293-3344 block so cmdRoadmapLogAppendBatch
    // can format each bullet through the same code path. scrubbedNames
    // is an out-param fed by rcScrubLeakedToolXml.
    QString formatRoadmapBullet(const QJsonObject &bulletReq,
                                const QString    &idStr,
                                const QString    &statusEmoji,
                                QStringList      &scrubbedNames,
                                int              *unnamedRemovals = nullptr);
    QJsonDocument cmdLs();
    QJsonDocument cmdSendText(const QJsonObject &req);
    QJsonDocument cmdNewTab(const QJsonObject &req);
    QJsonDocument cmdSelectWindow(const QJsonObject &req);
    QJsonDocument cmdSetTitle(const QJsonObject &req);
    QJsonDocument cmdLaunch(const QJsonObject &req);

    // ANTS-3793 § 2.1 — the owner wrapper. Returns the records for
    // `projectRoot`: from the store when that project is migrated, from
    // `markdown` when it is not. The three-outcome branch, the process-owned
    // RoadmapStore and § 2.2's stat-and-open all live here, once, rather than
    // at each read site.
    //
    // `*why` is written on EVERY path, success included, and a caller must
    // branch on it before trusting the result: a refusal returns an empty
    // vector, which is otherwise indistinguishable from a roadmap with no
    // bullets. rcRoadmapSourceRefused() maps it to the refusal envelope.
    //
    // An EMPTY `projectRoot` takes the markdown path with `*why == None`. That
    // is § 2.2 rule 1 one step earlier, not a silent fallback: the seam refuses
    // a root it cannot resolve, so a caller that could not name a project must
    // not reach it. Every roadmap verb resolves one from `caller_cwd`; the
    // focused-tab back-compat path has none.
    //
    // ANTS-3863 — `text` is a provider rather than the text: the dispatch reads
    // only the detector's window through it, and full() is reached ONLY on the
    // unmigrated fall-through below, which is the one case that was always
    // going to need the whole file.
    QVector<RoadmapParse::BulletRecord>
    roadmapBullets(const QString &projectRoot, RoadmapSource::RoadmapText &text,
                   bool includeArchive, RoadmapSource::ReadError *why,
                   QString *error) const;

    // ANTS-3793 § 2.2 — the dispatch alone: true iff the store serves this
    // project, with NO records materialised. One site needs it, and needs it in
    // this shape. roadmap_query's `section=` path slices the section's text and
    // parses the slice; a store holds records and not lines, so that path has
    // to know which backend answers BEFORE it decides how to read — and
    // answering by calling roadmapBullets() would read the whole project, which
    // is exactly what ANTS-1287 INV-9 keeps section mode away from.
    bool roadmapStoreServes(const QString &projectRoot,
                            RoadmapSource::RoadmapText &text,
                            RoadmapSource::ReadError *why,
                            QString *error) const;

    // ANTS-3809 § 2.1 — the write-side companion to roadmapBullets(). Hands
    // back the process-owned store rather than records, because the eight
    // roadmap_log ops mutate rows and then let RoadmapWrite::commitAndRender()
    // re-render the file.
    //
    // It is storeFor() plus migratedProject() and nothing else — the same two
    // calls the read seam makes, the same three outcomes, the same
    // Access::Interactive connection. A second dispatch rule here would be a
    // second answer to "is this project migrated", and the read and write
    // halves of one verb call must not be able to disagree.
    //
    // nullopt with `*why == None` means NOT migrated: splice markdown exactly
    // as today. nullopt with `*why != None` is a refusal and never a fallback.
    struct RoadmapWriteTarget {
        RoadmapStore *store = nullptr;
        qint64 projectId = 0;
    };
    std::optional<RoadmapWriteTarget>
    roadmapWriteTarget(const QString &projectRoot,
                       RoadmapSource::RoadmapText &text,
                       RoadmapSource::ReadError *why, QString *error) const;

    // ANTS-4070 — the prologue rotate_minor and retitle_section share:
    // canonicalise caller_cwd, find the live roadmap, read it, resolve the
    // store. Returns nullopt with *refusal set on every failure — including
    // `op_unsupported` on a project the migration has NOT taken over, which is
    // the one outcome the seam cannot express: it returns nullopt with
    // ReadError::None and the existing ops fall through to their markdown
    // path. These two have none, so the fall-through would reach nothing.
    std::optional<RoadmapWriteTarget>
    roadmapSectionOpTarget(const QJsonObject &req, QString *projectRoot,
                           QString *roadmapPath, QJsonDocument *refusal) const;

    // § 2.2 rules 1 and 2 (absent → nullptr, no error; present but unopenable
    // → nullptr with *why set), plus the connection caching the p95 budget
    // needs. Both readers above go through it.
    RoadmapStore *roadmapStoreOrNull(RoadmapSource::ReadError *why,
                                     QString *error) const;

    // ANTS-3802 — the cross-repo id resolver shared by feedback_query and
    // feedback_log's compact_resolved. ANTS-3793 made it a member: the sibling
    // roadmaps it opens may themselves be migrated, so it reads through
    // roadmapBullets() and therefore needs the owner's store connection.
    // Returns id → {status, resolved_from, shipped_date?}.
    QHash<QString, QJsonObject> rlResolveForeignFeedbackIds(
        const QString &feedbackFilePath,
        const QString &callerRoadmapPath,
        const QSet<QString> &callerPrefixes,
        const QStringList &wantedIds,
        const QSet<QString> &alreadyResolved) const;

    // § 2.2's last rule — ONE long-lived Access::Interactive connection for
    // RoadmapStore::defaultPath(), opened on the first dispatch that finds a
    // store file and never per call: opening SQLite, applying pragmas and
    // warming the page cache is the dominant term in § 4's p95 budget. The
    // MARKER is still resolved per call (inside the seam), so a project
    // migrated mid-session is picked up. Held by unique_ptr to a
    // forward-declared type — ~RemoteControl() is defined in the .cpp, which is
    // where the complete type is.
    mutable std::unique_ptr<RoadmapStore> m_roadmapStore;

    // ANTS-3822 § 2.3.2 — the history cap this instance opens its store with.
    // Production always uses the default; a test lowers it so the cap is
    // reachable at all.
    //
    // Without this the cap can only be set BELOW the verb (RoadmapStore's
    // constructor) while `history_note` is only built ABOVE it (the handler's
    // envelope), so the invariant guarding INV-14's "fails AND reports" could be
    // made neither red nor green — the only honest route being 250 MiB of real
    // history, which ANTS-3756's INV-14 already refuses for its own legs.
    // -1 means "not overridden — use RoadmapStore::kDefaultHistoryCapBytes".
    // A sentinel rather than the constant itself because RoadmapStore is only
    // forward-declared here (see m_roadmapStore's comment above), so its static
    // cannot be named in this header.
    qint64 m_roadmapHistoryCap = -1;

    // Cached parse of `m_main->roadmapPathForRemote()` content. Refreshed
    // on `roadmap-query` when EITHER the mtime advances OR the wall-clock
    // age of the cache exceeds `kRoadmapCacheTtlMs`. INV-10 contract is
    // both bounds ANDed: "≤ 100 ms cache lifetime" plus mtime detection
    // for the rare edit-then-re-edit-within-the-same-tick case.
    // ANTS-1123 indie-review F1 fold-in.
    mutable QString m_roadmapCachePath;
    mutable qint64 m_roadmapCacheMtimeMs = 0;
    mutable qint64 m_roadmapCacheStampMs = 0;  // epoch ms of last refresh
    mutable QJsonArray m_roadmapCacheBullets;
    // ANTS-1646 — duplicate-ID descriptor cache, recomputed every time
    // m_roadmapCacheBullets refills. Emitted on the envelope as
    // `duplicate_ids` only when non-empty so a clean roadmap stays at
    // its existing envelope shape.
    mutable QJsonArray m_roadmapCacheDuplicateIds;
    // ANTS-4402 — which backend answered the last refill, and the two
    // high-water marks that say whether ROADMAP.md holds ids the store has
    // never seen. Cached beside the bullets they describe: the id scan is one
    // regex pass over the roadmap, so it is paid once per mtime change rather
    // than per query, and it is reset in lockstep with m_roadmapCacheBullets
    // (mcp-caches.md — same key, never a second shadowing cache).
    mutable QString m_roadmapCacheSource;
    mutable qint64 m_roadmapCacheFileMaxId = 0;
    mutable qint64 m_roadmapCacheStoreHighWater = 0;
    // ANTS-3533 — changelog_query parse cache (mtime+TTL, same shape as
    // the roadmap cache; single-slot + path-keyed so it never shadows
    // across projects — docs/standards/mcp-caches.md). Holds the parsed
    // CHANGELOG plus the roadmap prefix P sniffed once per parse.
    mutable QString m_changelogCachePath;
    mutable qint64 m_changelogCacheMtimeMs = 0;
    mutable qint64 m_changelogCacheStampMs = 0;
    mutable ChangelogQuery::ParseResult m_changelogCache;
    mutable QString m_changelogCachePrefix;
    // ANTS-1922 — test-only soft-cap override for bundles mode (0 =
    // default PaginationEngine::kSoftCapBytes). Set via
    // setBundleSoftCapOverride; never written on the live path.
    int m_bundleSoftCapOverride = 0;
    // ANTS-1287 — heading index + section-slice bullet cache. Shares
    // (path, mtime, stamp) keys with the bullets cache; cleared on
    // mtime advance or TTL expiry. See docs/specs/ANTS-1287.md § 2.3.
    mutable QVector<RoadmapIndex::Section> m_roadmapIndex;
    mutable QHash<QString, QJsonArray>     m_roadmapSectionCache;
    // ANTS-1696 — per-section shape hint for non-bullet sections.
    // When parseBullets returns zero entries for a section but the
    // slice has table / prose content, callers want to know to use
    // Read instead of falling back blindly. Cached alongside the
    // bullets so the helper runs once per slug. Stored value is
    // {shape:"table"|"prose"|"empty", non_bullet_lines:N}; emit-
    // time skips when shape == "empty" (matches INV-3 contract).
    mutable QHash<QString, QJsonObject>    m_roadmapSectionShape;
    // ANTS-1907 — per-section ETag cache. Derived from the section's
    // sliced byte content (SHA-256 prefix) so two sections with
    // identical bodies hash identically and an edit to one section
    // never invalidates another. Lives in lockstep with the section
    // bullets cache: keys cleared together on mtime advance / TTL
    // expiry, LRU evicts the same slugs. Used by both the section=
    // mode (emit + accept section_etag_match for 304 short-circuit)
    // and the section_index mode (emit per-section section_etag so
    // a multi-section caller can ask "did any of these change?").
    mutable QHash<QString, QString>        m_roadmapSectionEtags;
    // ANTS-1346 — MRU-front list bounding the section cache at 64
    // slugs. Hit-path bumps the slug to the front; insert-path evicts
    // the tail when size > kRoadmapSectionCacheCap. Cleared together
    // with the cache on the mtime-stale wipe path.
    mutable QList<QString>                 m_roadmapSectionLru;
    static constexpr int    kRoadmapSectionCacheCap = 64;
    static constexpr qint64 kRoadmapCacheTtlMs = 100;
    // ANTS-1429 — minimum file size (bytes) above which an empty
    // parseBullets result is treated as `unrecognised_format`
    // rather than the legitimate "no work pending" envelope. 1 KB
    // is conservative: a structured stub (intro paragraph +
    // headings) lands ~600 B; real roadmaps clear by ≥ 100×.
    static constexpr qint64 kRoadmapMinParseableSize = 1024;

    // ANTS-1319 — mtime-cached partition result (INV-12). Single-entry
    // cache keyed on (path, scope, stamp). TTL = 5 s, picked to cover
    // the typical N-lane parallel dispatch within Phase 2 of /cold-eyes
    // without holding stale state past a doc edit cycle.
    mutable QString m_coldEyesCachePath;
    mutable qint64  m_coldEyesCacheStampMs = 0;
    mutable ColdEyesEngine::Scope m_coldEyesCacheScope = ColdEyesEngine::Scope::Default;
    mutable ColdEyesEngine::PartitionResult m_coldEyesCache;
    static constexpr qint64 kColdEyesCacheTtlMs = 5000;

    // ANTS-1359 — session-scoped verify_changes build-cache. Entries
    // keyed on (projectRoot, git HEAD, git status SHA, trust outcome,
    // autotrust env, canonicalised options) — see docs/specs/ANTS-1359.md
    // § 2.3. Excludes bad_config / none / verify_untrusted / non-git /
    // not-naturally-completed gates / mid-run snapshot drift per § 2.5.
    struct VerifyChangesCacheEntry {
        qint64       stampMs = 0;
        QString      key;
        QJsonObject  response;
    };
    mutable QHash<QString, VerifyChangesCacheEntry> m_verifyCache;
    mutable QList<QString>                          m_verifyCacheLru;
    mutable bool m_verifyInFlight = false;
    // ANTS-1302 — focused_test re-entrancy gate (mirrors m_verifyInFlight).
    mutable bool m_focusedTestInFlight = false;
    static constexpr int    kVerifyCacheCap   = 8;
    static constexpr qint64 kVerifyCacheTtlMs = 300 * 1000;   // 5 min
    QJsonDocument cmdVerifyChangesImpl(const QString &root,
                                        const QJsonObject &req);

    QLocalServer *m_server = nullptr;
    MainWindow *m_main;  // non-owning; MainWindow owns us via QObject parent
    bool m_e2eMode = false;  // ANTS-2049 — inject-verb gate (see setE2eMode)

    // ANTS-1337 — verify_changes content-trust gate. nullptr by
    // default; MainWindow constructs the chrome-layer modal client
    // and installs it via setVerifyTrustClient.
    std::unique_ptr<VerifyTrust::Client> m_verifyTrustClient;

    // ANTS-1254 — single-entry summary cache. Keyed on
    // (path, mtime, topN, floor) per spec INV-2.
    mutable QString  m_auditSummaryPath;
    mutable qint64   m_auditSummaryMtimeMs    = 0;
    mutable AuditEngine::AuditSummary m_auditSummaryCache;
    mutable int      m_auditSummaryCachedTopN = -1;
    mutable QString  m_auditSummaryCachedFloor;

    // ANTS-3444a — project roots whose find_sources candidate files have
    // already been page-cache-warmed this session (see cmdSessionOrient).
    // Accessed only on the MCP request thread, so it needs no lock.
    QSet<QString> m_prewarmedRoots;
};
