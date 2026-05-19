#pragma once

#include <QObject>
#include <QString>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "auditengine.h"  // ANTS-1254 — AuditSummary value member below
#include "coldeyesengine.h"  // ANTS-1319 — cold-eyes partition cache
#include "roadmapindex.h"  // ANTS-1287 — heading-index cache members

#include <memory>

class QLocalServer;
class QLocalSocket;
class MainWindow;
class ClaudeIntegration;
namespace VerifyTrust { class Client; }

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
    // ANTS-1583 — roadmap_branch_drift: compare ROADMAP ✅ entries'
    // cited commit SHAs against HEAD's reachable history. Reuses
    // findRoadmapUnder + collectGitSnapshot + runGit. See
    // docs/specs/ANTS-1583.md.
    QJsonDocument cmdRoadmapBranchDrift(const QJsonObject &req);
    QJsonDocument cmdTabList();
    QJsonDocument cmdGetText(const QJsonObject &req);

    // ANTS-1248: ripgrep wrapper. Public for the same reason as the
    // ANTS-1244 trio — MCP server lambda in MainWindow delegates here
    // so the body is reused across IPC + MCP transports. Argv-only
    // QProcess::start (no shell), 2 s hard-kill via constant
    // kWorkspaceSearchHardKillMs + 200 ms grace, server-clamped to
    // 500 results. See docs/specs/ANTS-1248.md.
    QJsonDocument cmdWorkspaceSearch(const QJsonObject &req);

    // ANTS-1249: file outline (regex scanner over a file, returns
    // header_doc + symbols[] for cpp / py / md / unknown). Shares
    // the pathInRepoRoot helper with cmdWorkspaceSearch.
    // See docs/specs/ANTS-1249.md.
    QJsonDocument cmdFileOutline(const QJsonObject &req);

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

private slots:
    void onNewConnection();

private:
    QJsonDocument dispatch(const QJsonObject &req);
    // ANTS-1428 — adapter-mode write path for GFM-format roadmaps.
    // Dispatched from cmdRoadmapLog when req["op"] == "flip". See
    // docs/specs/ANTS-1428.md § Tier 2.
    QJsonDocument cmdRoadmapLogFlip(const QJsonObject &req);
    QJsonDocument cmdLs();
    QJsonDocument cmdSendText(const QJsonObject &req);
    QJsonDocument cmdNewTab(const QJsonObject &req);
    QJsonDocument cmdSelectWindow(const QJsonObject &req);
    QJsonDocument cmdSetTitle(const QJsonObject &req);
    QJsonDocument cmdLaunch(const QJsonObject &req);

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
    // ANTS-1287 — heading index + section-slice bullet cache. Shares
    // (path, mtime, stamp) keys with the bullets cache; cleared on
    // mtime advance or TTL expiry. See docs/specs/ANTS-1287.md § 2.3.
    mutable QVector<RoadmapIndex::Section> m_roadmapIndex;
    mutable QHash<QString, QJsonArray>     m_roadmapSectionCache;
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
    static constexpr int    kVerifyCacheCap   = 8;
    static constexpr qint64 kVerifyCacheTtlMs = 300 * 1000;   // 5 min
    QJsonDocument cmdVerifyChangesImpl(const QString &root,
                                        const QJsonObject &req);

    QLocalServer *m_server = nullptr;
    MainWindow *m_main;  // non-owning; MainWindow owns us via QObject parent

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
};
