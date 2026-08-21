// ANTS-3833 TU 2/14 — Terminal and window verbs.
#include "remotecontrol.h"
#include "remotecontrol_internal.h"
#include "projectsettings.h"   // ANTS-3771 — the declared id format
#include "findsources.h"
#include "mainwindow.h"
#include "pathvalidation.h"
#include "scrollbackerrors.h"
#include "buildfixhint.h"
#include "passheadingwrite.h"
#include "terminalwidget.h"
#include <QCoreApplication>
#include <QFileInfo>
#include <QSaveFile>
#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QScreen>

using namespace rcdetail;  // ANTS-3833

QJsonDocument RemoteControl::cmdLs() {
    QJsonObject out;
    out["ok"] = true;
    out["tabs"] = m_main->tabListForRemote();
    return QJsonDocument(out);
}

QJsonDocument RemoteControl::cmdSendText(const QJsonObject &req) {
    // Request shape: {"cmd":"send-text","tab":<int>,"text":"<string>",
    //                 "raw":<bool optional>}
    //   - `tab` optional (default: the active tab)
    //   - `text` required; UTF-8 written to the tab's PTY. By default
    //     dangerous C0 control bytes (0x00-0x08, 0x0B-0x1F, 0x7F) are
    //     stripped to prevent local-UID processes from injecting ESC
    //     sequences / bracketed-paste toggles / OSC 52 clipboard
    //     overwrites through the rc socket. See
    //     tests/features/remote_control_opt_in/spec.md.
    //   - `raw`  optional; when `true`, the filter is skipped and
    //     bytes pass through verbatim. Preserves Kitty-compat for
    //     callers that genuinely need raw byte access (terminal test
    //     harnesses, escape-sequence driven plugins).
    QJsonObject out;
    const QJsonValue textVal = req.value("text");
    if (!textVal.isString()) {
        out["ok"] = false;
        out["error"] = QStringLiteral(
            "send-text: missing or non-string \"text\" field");
        return QJsonDocument(out);
    }
    const QString text = textVal.toString();
    // `tab` arrives as a JSON number. toInt() returns 0 for a missing
    // or non-number value, which would silently target tab 0 — use
    // the `isDouble()` check to distinguish "not specified" from
    // "specified as 0" so `--remote-tab 0` stays meaningful.
    const QJsonValue tabVal = req.value("tab");
    TerminalWidget *target = nullptr;
    if (tabVal.isDouble()) {
        const int idx = tabVal.toInt();
        target = m_main->terminalAtTab(idx);
        if (!target) {
            out["ok"] = false;
            out["error"] = QStringLiteral(
                "send-text: no tab at index %1").arg(idx);
            return QJsonDocument(out);
        }
    } else {
        target = m_main->currentTerminal();
        if (!target) {
            out["ok"] = false;
            out["error"] = QStringLiteral(
                "send-text: no active terminal");
            return QJsonDocument(out);
        }
    }
    const bool rawBypass = req.value("raw").toBool(false);
    const QByteArray rawBytes = text.toUtf8();
    int stripped = 0;
    const QByteArray payload = rawBypass
        ? rawBytes
        : RemoteControl::filterControlChars(rawBytes, &stripped);
    target->sendToPty(payload);
    out["ok"] = true;
    out["bytes"] = payload.size();
    if (!rawBypass && stripped > 0) {
        out["stripped"] = stripped;
    }
    return QJsonDocument(out);
}

// filterControlChars is defined inline in remotecontrol.h so feature
// tests can exercise it without pulling the full MainWindow dep chain.

QJsonDocument RemoteControl::cmdSelectWindow(const QJsonObject &req) {
    // Request shape: {"cmd":"select-window","tab":<int>}
    //   - `tab` required. Kitty's rc_protocol uses `--match id:N`;
    //     we use 0-based tab index to stay consistent with the
    //     other ants rc commands and with the `ls` response shape.
    //   - No match → error envelope with out-of-range message; the
    //     tab strip is unchanged.
    QJsonObject out;
    const QJsonValue tabVal = req.value("tab");
    if (!tabVal.isDouble()) {
        out["ok"] = false;
        out["error"] = QStringLiteral(
            "select-window: missing or non-integer \"tab\" field");
        return QJsonDocument(out);
    }
    const int idx = tabVal.toInt();
    if (!m_main->selectTabForRemote(idx)) {
        out["ok"] = false;
        out["error"] = QStringLiteral(
            "select-window: no tab at index %1").arg(idx);
        return QJsonDocument(out);
    }
    out["ok"] = true;
    out["index"] = idx;
    return QJsonDocument(out);
}

QJsonDocument RemoteControl::cmdGetText(const QJsonObject &req) {
    // Request shape: {"cmd":"get-text","tab":<int optional>,"lines":<int optional>}
    //   - `tab`   optional; default = active tab. isDouble() guard
    //     (consistent with send-text / set-title).
    //   - `lines` optional; default 100. Number of trailing lines from
    //     scrollback + screen, joined with `\n`. Negative or zero
    //     falls back to the default (matches the existing
    //     TerminalWidget::recentOutput contract). Capped at 10 000
    //     here so a script that writes `--remote-lines 1000000`
    //     against a million-line scrollback doesn't return a 100 MB
    //     JSON envelope. Beyond 10 000 lines the caller probably
    //     wants the file directly (Ctrl+Shift+P → Export Scrollback)
    //     rather than over the wire.
    QJsonObject out;
    TerminalWidget *target = nullptr;
    const QJsonValue tabVal = req.value("tab");
    if (tabVal.isDouble()) {
        const int idx = tabVal.toInt();
        target = m_main->terminalAtTab(idx);
        if (!target) {
            out["ok"] = false;
            out["error"] = QStringLiteral(
                "get-text: no tab at index %1").arg(idx);
            return QJsonDocument(out);
        }
    } else {
        // ANTS-1392 — when `tab` is omitted, prefer the caller_cwd
        // anchor over the focused tab. terminalForCaller falls back
        // to focusedTerminal() when caller_cwd is empty or no tab
        // matches, preserving the pre-1392 contract.
        const QString callerCwd =
            req.value(QStringLiteral("caller_cwd")).toString();
        target = m_main->terminalForCaller(callerCwd);
        if (!target) {
            out["ok"] = false;
            out["error"] = QStringLiteral("get-text: no active terminal");
            return QJsonDocument(out);
        }
    }

    int lines = 100;
    const QJsonValue linesVal = req.value("lines");
    if (linesVal.isDouble()) {
        const int requested = linesVal.toInt();
        if (requested > 0) lines = std::min(requested, 10000);
    }

    // ANTS-1348 — server-side byte cap. Default 1 MiB matches the MCP
    // bridge's receive budget so the happy path never trips the
    // transport limit. Caller can lower (test harness) or raise (up
    // to the 16 MiB ceiling for non-MCP rc consumers).
    int maxBytes = RemoteControl::kGetTextDefaultBytesCap;
    const QJsonValue maxBytesVal = req.value("max_bytes");
    if (maxBytesVal.isDouble()) {
        const int requested = maxBytesVal.toInt();
        if (requested > 0) maxBytes = requested;
    }

    const QString raw = target->recentOutput(lines);
    const auto trim =
        RemoteControl::trimScrollbackForGetText(raw, maxBytes);

    out["ok"] = true;
    out["text"] = trim.text;
    out["lines"] =
        trim.text.count('\n') + (trim.text.isEmpty() ? 0 : 1);
    out["bytes"] = trim.text.toUtf8().size();
    out["truncated"] = trim.truncated;
    if (trim.truncated) {
        out["bytes_dropped"] = trim.bytesDropped;
        out["lines_dropped"] = trim.linesDropped;
    }
    if (trim.capClamped) out["bytes_cap_clamped"] = true;
    return QJsonDocument(out);
}

void RemoteControl::enrichLikelyFixes(QJsonArray &errors,
                                      const QString &root) const {
    // ANTS-3374 — stitch the diagnose→fix loop: on an undeclared-symbol
    // diagnostic, resolve the declaring header and attach a `likely_fix`
    // add_include hint. Dedups by symbol (cascades name the same symbol
    // repeatedly) and caps distinct header lookups so a wall of errors
    // can't fan out into an unbounded SymbolQuery tree-walk.
    if (root.isEmpty() || errors.isEmpty()) return;
    constexpr int kMaxLookups = 25;
    QHash<QString, QString> headerBySym;  // symbol → header ("" = miss)
    int lookups = 0;
    for (int i = 0; i < errors.size(); ++i) {
        QJsonObject e = errors.at(i).toObject();
        const QString sym =
            BuildFixHint::undeclaredSymbol(e.value("message").toString());
        if (sym.isEmpty()) continue;
        QString header;
        if (const auto it = headerBySym.constFind(sym);
            it != headerBySym.constEnd()) {
            header = it.value();
        } else if (lookups < kMaxLookups) {
            header = BuildFixHint::resolveHeader(root, sym);
            headerBySym.insert(sym, header);
            ++lookups;
        } else {
            continue;  // lookup budget spent — leave the rest un-enriched
        }
        if (header.isEmpty()) continue;
        QJsonObject lf;
        lf["add_include"] = header;
        lf["defines"]     = sym;
        const QString at = e.value("file").toString();
        if (!at.isEmpty()) lf["at"] = at;
        e["likely_fix"] = lf;
        errors.replace(i, e);
    }
}

QJsonDocument RemoteControl::cmdRecentErrors(const QJsonObject &req) {
    // ANTS-1301 — scan the focused terminal's recent scrollback for
    // structured errors. Terminal resolution mirrors cmdGetText
    // (ANTS-1392): explicit `tab` int → terminalAtTab; else caller_cwd
    // → terminalForCaller → focused fallback. MCP-only.
    QJsonObject out;
    TerminalWidget *target = nullptr;
    const QJsonValue tabVal = req.value(QStringLiteral("tab"));
    if (tabVal.isDouble()) {
        target = m_main->terminalAtTab(tabVal.toInt());
    } else {
        target = m_main->terminalForCaller(
            req.value(QStringLiteral("caller_cwd")).toString());
    }
    if (!target) {
        out["ok"]    = false;
        out["error"] = QStringLiteral("recent_errors: no terminal to read");
        out["code"]  = QStringLiteral("no_window");
        return QJsonDocument(out);
    }

    int lines = 500;
    const QJsonValue linesVal = req.value(QStringLiteral("lines"));
    if (linesVal.isDouble()) {
        const int requested = linesVal.toInt();
        if (requested > 0) lines = std::min(requested, 10000);
    }

    ScrollbackErrors::Options opts;
    const QJsonValue mr = req.value(QStringLiteral("max_results"));
    if (mr.isDouble()) opts.maxResults = mr.toInt();  // lib clamps ≤0 / >1000

    const ScrollbackErrors::Result res =
        ScrollbackErrors::parse(target->recentOutput(lines), opts);

    QJsonArray errors;
    for (const ScrollbackErrors::ErrorEntry &e : res.errors) {
        QJsonObject o;
        o["category"] = e.category;
        if (!e.file.isEmpty()) o["file"]   = e.file;
        if (e.line   > 0)      o["line"]   = e.line;
        if (e.column > 0)      o["column"] = e.column;
        o["message"] = e.message;
        o["text"]    = e.text;
        errors.append(o);
    }

    // ANTS-3374 — best-effort add_include hint on undeclared-symbol errors.
    enrichLikelyFixes(errors, resolveRootCanonical(m_main, req));

    out["ok"]            = true;
    out["errors"]        = errors;
    out["errors_count"]  = res.errorsTotal;
    out["lines_scanned"] = res.linesScanned;
    out["truncated"]     = res.truncated;
    return QJsonDocument(out);
}

// ANTS-1312 — last_selection. Return the focused (or routed) terminal's
// current selection text so Claude can pull the highlighted error /
// trace without walking the scrollback. Sole source of truth is
// TerminalWidget::selectedText() — no reimplementation.
QJsonDocument RemoteControl::cmdLastSelection(const QJsonObject &req) {
    QJsonObject out;
    TerminalWidget *target = nullptr;
    const QJsonValue tabVal = req.value(QStringLiteral("tab"));
    if (tabVal.isDouble()) {
        target = m_main->terminalAtTab(tabVal.toInt());
    } else {
        // ANTS-1392 — caller_cwd anchors to caller's tab; falls back
        // to focused tab when absent or no match.
        target = m_main->terminalForCaller(
            req.value(QStringLiteral("caller_cwd")).toString());
    }
    if (!target) {
        out[QStringLiteral("ok")]    = false;
        out[QStringLiteral("error")] = QStringLiteral(
            "last_selection: no terminal to read");
        out[QStringLiteral("code")]  = QStringLiteral("no_window");
        return QJsonDocument(out);
    }

    const QString text = target->selectedText();
    const bool hasSelection = !text.isEmpty();

    out[QStringLiteral("ok")]            = true;
    out[QStringLiteral("has_selection")] = hasSelection;
    out[QStringLiteral("text")]          = text;
    out[QStringLiteral("length")]        = text.size();
    out[QStringLiteral("bytes")]         = text.toUtf8().size();
    return QJsonDocument(out);
}

// ANTS-1636 — find_sources. Project-scoped topic-to-files discovery.
QJsonDocument RemoteControl::cmdFindSources(const QJsonObject &req) {
    QJsonObject out;
    // ANTS-3415 — accept `symbol` as an alias for `topic` (fills in only
    // when `topic` is absent), mirroring the file_outline path/file_path
    // and workspace_search query/pattern idioms. `topic` stays canonical.
    QString topic = req.value(QStringLiteral("topic")).toString().trimmed();
    if (topic.isEmpty())
        topic = req.value(QStringLiteral("symbol")).toString().trimmed();
    if (topic.isEmpty()) {
        out[QStringLiteral("ok")]    = false;
        out[QStringLiteral("error")] = QStringLiteral(
            "find_sources: missing or empty \"topic\" (alias: \"symbol\")");
        out[QStringLiteral("code")]  = QStringLiteral("bad_args");
        return QJsonDocument(out);
    }

    // Project root from caller_cwd (Required contract — dispatcher
    // refuses the empty case upstream). We still resolve to a
    // canonical path here so the underlying file walk can join paths
    // safely.
    const QString callerCwd =
        req.value(QStringLiteral("caller_cwd")).toString();
    const QString root = QFileInfo(callerCwd).canonicalFilePath();
    if (root.isEmpty() || !QFileInfo(root).isDir()) {
        out[QStringLiteral("ok")]    = false;
        out[QStringLiteral("error")] = QStringLiteral(
            "find_sources: caller_cwd does not canonicalise to a directory");
        out[QStringLiteral("code")]  = QStringLiteral("bad_path");
        return QJsonDocument(out);
    }

    FindSources::Options opts;
    const QJsonValue maxVal = req.value(QStringLiteral("max_results"));
    if (maxVal.isDouble()) {
        const int requested = maxVal.toInt();
        if (requested > 0) opts.maxResults = requested;
    }
    const FindSources::Result res =
        FindSources::findSources(topic, root, opts);

    QJsonArray files;
    for (const FindSources::FileHit &h : res.files) {
        QJsonObject f;
        f[QStringLiteral("path")]  = h.path;
        f[QStringLiteral("score")] = h.score;
        f[QStringLiteral("role")]  = h.role;
        QJsonArray ev;
        for (const QString &e : h.evidence) ev.append(e);
        f[QStringLiteral("evidence")] = ev;
        files.append(f);
    }
    QJsonArray unmatched;
    for (const QString &t : res.unmatchedTerms) unmatched.append(t);

    out[QStringLiteral("ok")]              = true;
    out[QStringLiteral("files")]           = files;
    out[QStringLiteral("files_count")]     = static_cast<int>(res.files.size());
    out[QStringLiteral("unmatched_terms")] = unmatched;
    out[QStringLiteral("files_scanned")]   = res.filesScanned;
    out[QStringLiteral("truncated")]       = res.truncated;
    // ANTS-3435 — an empty result must not read as a genuine "no such code".
    // find_sources ranks by FILENAME + keyword frequency, so a topic that
    // leads with a bare symbol name (e.g. "FooBar does X and Y") often
    // matches nothing here even though the symbol exists. Redirect the caller
    // to the exact-match verbs rather than letting them conclude absence.
    // ANTS-3489 — distinguish an EMPTY CANDIDATE SET (files_scanned:0 — no
    // C/C++ source under the resolved roots) from "scanned but nothing
    // scored". The former means find_sources is the wrong tool for this
    // project (a non-C/C++ layout, or code under an undeclared root), not
    // that the code is absent — so the hint names that cause explicitly.
    if (files.isEmpty() && res.filesScanned == 0) {
        out[QStringLiteral("hint")] = QStringLiteral(
            "scanned 0 files: no C/C++ source found under the project's "
            "source roots. find_sources ranks C/C++ only — for a Python or "
            "other-language project use codebase_index / workspace_search. "
            "If this IS a C/C++ project laid out beyond src/ + tests/, "
            "declare its source_roots in .ants/project.json "
            "(project_settings op:init) so the walk can reach it.");
    } else if (files.isEmpty()) {
        out[QStringLiteral("hint")] = QStringLiteral(
            "no files matched by filename/keyword ranking — for a specific "
            "symbol, try workspace_search (exact string/regex), "
            "find_definition (where it's defined), or find_caller (call "
            "sites); find_sources is best for a topical/prose description");
    }
    return QJsonDocument(out);
}

QJsonDocument RemoteControl::cmdSetTitle(const QJsonObject &req) {
    // Request shape: {"cmd":"set-title","tab":<int optional>,"title":"<string>"}
    //   - `tab` optional; default = active tab. `isDouble()` guard to
    //     keep `--remote-tab 0` distinct from "tab omitted" — same
    //     pattern as send-text.
    //   - `title` required (must be a string). Empty string clears the
    //     pin and lets the auto-title path resume — useful for
    //     scripts that want to "reset to default" without restarting
    //     the tab.
    QJsonObject out;
    const QJsonValue titleVal = req.value("title");
    if (!titleVal.isString()) {
        out["ok"] = false;
        out["error"] = QStringLiteral(
            "set-title: missing or non-string \"title\" field");
        return QJsonDocument(out);
    }
    const QString title = titleVal.toString();

    int idx;
    const QJsonValue tabVal = req.value("tab");
    if (tabVal.isDouble()) {
        idx = tabVal.toInt();
    } else {
        // No explicit tab → resolve the active one. We need an index
        // (not just a TerminalWidget*) because `setTabTitleForRemote`
        // operates by index. Look up via currentIndex() rather than
        // walking all tabs.
        idx = m_main->currentTabIndexForRemote();
    }

    if (!m_main->setTabTitleForRemote(idx, title)) {
        out["ok"] = false;
        out["error"] = QStringLiteral(
            "set-title: no tab at index %1").arg(idx);
        return QJsonDocument(out);
    }
    out["ok"] = true;
    out["index"] = idx;
    return QJsonDocument(out);
}

QJsonDocument RemoteControl::cmdLaunch(const QJsonObject &req) {
    // Request shape: {"cmd":"launch","cwd":"<path optional>","command":"<string required>",
    //                 "raw":<bool optional>}
    //
    // `launch` differs from `new-tab` in two ways:
    //   1. `command` is REQUIRED — the whole point of launch is to
    //      spawn something, so we reject the no-command call up front
    //      rather than silently behaving like new-tab.
    //   2. We auto-append `\n` if the command doesn't already end in
    //      one — matches user intent ("launch this command" implies
    //      "and run it"). new-tab leaves command untouched because
    //      it's the lower-level building block; launch is the sugar
    //      that "just works" for the common case.
    //
    // 0.7.52 (2026-04-27 indie-review HIGH) — `command` is routed
    // through filterControlChars by default, identical to send-text.
    // Without this, a same-UID attacker reaching the rc socket gets
    // ESC-sequence / bracketed-paste / OSC 52 injection via launch
    // even though send-text was hardened against it. The `raw: true`
    // opt-out matches send-text's escape hatch for callers (test
    // harnesses, plugins) who need raw byte access.
    QJsonObject out;
    const QJsonValue commandVal = req.value("command");
    if (!commandVal.isString() || commandVal.toString().isEmpty()) {
        out["ok"] = false;
        out["error"] = QStringLiteral(
            "launch: missing or empty \"command\" field "
            "(use new-tab if you want a bare shell)");
        return QJsonDocument(out);
    }
    QString command = commandVal.toString();
    if (!command.endsWith('\n')) command += '\n';

    const bool rawBypass = req.value("raw").toBool(false);
    int stripped = 0;
    const QByteArray rawBytes = command.toUtf8();
    const QByteArray payload = rawBypass
        ? rawBytes
        : RemoteControl::filterControlChars(rawBytes, &stripped);
    const QString filteredCommand = QString::fromUtf8(payload);

    const QString cwd = req.value("cwd").toString();
    // ANTS-1347 — `cwd` hygiene + anchor.
    //
    // Byte hygiene (always): the shared cwdHasBadByte helper rejects
    // C0 (U+0000..U+001F), backslash, and C1 (U+0080..U+009F). The
    // C1 leg is the path-side counterpart to ANTS-1335's byte-strip
    // on text payloads — same threat (rc/MCP seam delivering
    // untrusted bytes), different semantics (reject-not-strip for
    // paths, where silent mutation would mislead the caller).
    //
    // Anchor (default-on): non-empty cwd routes through
    // PathValidation::validatePath against the focused project root,
    // matching every other path-typed rc/MCP verb post-ANTS-1295.
    // The optional `allow_outside_root: true` opt-out skips the
    // anchor while keeping byte hygiene — for callers (Lua plugins,
    // ants @ launch CLI) that legitimately need to chdir outside any
    // project root.
    if (!cwd.isEmpty()) {
        if (RemoteControl::cwdHasBadByte(cwd)) {
            QJsonObject errOut;
            errOut["ok"] = false;
            errOut["error"] = QStringLiteral(
                "launch: cwd contains control or backslash characters");
            errOut["code"] = QStringLiteral("bad_cwd");
            return QJsonDocument(errOut);
        }
        const bool allowOutside =
            req.value("allow_outside_root").toBool(false);
        if (!allowOutside) {
            const QString root = resolveRootCanonical(m_main);
            if (root.isEmpty()) {
                QJsonObject errOut;
                errOut["ok"] = false;
                errOut["error"] = QStringLiteral(
                    "launch: no focused project root (set "
                    "allow_outside_root:true to chdir outside any project)");
                errOut["code"] = QStringLiteral("no_project");
                return QJsonDocument(errOut);
            }
            const auto check = PathValidation::validatePath(
                cwd, root, QStringLiteral("launch"),
                QStringLiteral("cwd"));
            if (check.bad) return QJsonDocument(check.err);
        }
    }
    const int idx = m_main->newTabForRemote(cwd, filteredCommand);
    out["ok"] = true;
    out["index"] = idx;
    if (!rawBypass && stripped > 0) out["stripped"] = stripped;
    return QJsonDocument(out);
}

QJsonDocument RemoteControl::cmdNewTab(const QJsonObject &req) {
    // Request shape: {"cmd":"new-tab","cwd":"<path>","command":"<string>",
    //                 "raw":<bool optional>}
    //   - `cwd` optional; empty/absent → inherit cwd from the focused
    //     terminal (same default as the menu-driven newTab() slot)
    //   - `command` optional; when present, written to the new tab's
    //     shell after a 200 ms settle (matches onSshConnect's timing).
    //     Caller is responsible for the trailing newline — matches
    //     `send-text` semantics so the two commands behave
    //     consistently with shell pipes.
    //   - `raw` optional; default false. When true, skips C0 filter
    //     (matches send-text). Otherwise `command` is filtered
    //     identically to send-text — see cmdLaunch for rationale.
    //
    // 0.7.52 (2026-04-27 indie-review HIGH) — `command` is routed
    // through filterControlChars by default, identical to send-text.
    QJsonObject out;
    const QString cwd     = req.value("cwd").toString();
    const QString command = req.value("command").toString();
    const bool rawBypass  = req.value("raw").toBool(false);

    QString filteredCommand = command;
    int stripped = 0;
    if (!command.isEmpty() && !rawBypass) {
        const QByteArray payload =
            RemoteControl::filterControlChars(command.toUtf8(), &stripped);
        filteredCommand = QString::fromUtf8(payload);
    }

    // ANTS-1347 — `cwd` hygiene + anchor. See cmdLaunch for the
    // full rationale; this verb mirrors the same flow.
    if (!cwd.isEmpty()) {
        if (RemoteControl::cwdHasBadByte(cwd)) {
            QJsonObject errOut;
            errOut["ok"] = false;
            errOut["error"] = QStringLiteral(
                "new-tab: cwd contains control or backslash characters");
            errOut["code"] = QStringLiteral("bad_cwd");
            return QJsonDocument(errOut);
        }
        const bool allowOutside =
            req.value("allow_outside_root").toBool(false);
        if (!allowOutside) {
            const QString root = resolveRootCanonical(m_main);
            if (root.isEmpty()) {
                QJsonObject errOut;
                errOut["ok"] = false;
                errOut["error"] = QStringLiteral(
                    "new-tab: no focused project root (set "
                    "allow_outside_root:true to chdir outside any project)");
                errOut["code"] = QStringLiteral("no_project");
                return QJsonDocument(errOut);
            }
            const auto check = PathValidation::validatePath(
                cwd, root, QStringLiteral("new-tab"),
                QStringLiteral("cwd"));
            if (check.bad) return QJsonDocument(check.err);
        }
    }
    const int idx = m_main->newTabForRemote(cwd, filteredCommand);
    out["ok"] = true;
    out["index"] = idx;
    if (!rawBypass && stripped > 0) out["stripped"] = stripped;
    return QJsonDocument(out);
}

// ANTS-1117 v1: tab-list — richer per-tab snapshot than `ls`.
QJsonDocument RemoteControl::cmdTabList() {
    QJsonObject out;
    out["ok"] = true;
    out["tabs"] = m_main->tabsAsJson();
    return QJsonDocument(out);
}

// ============================================================================
// ANTS-2049 — e2e harness inject verbs (socket-only, gated on m_e2eMode).
// All run on the GUI thread (dispatch() is called from the QLocalServer
// readyRead handler, which the GUI thread owns), so posting synthetic events
// to widgets is main-thread-safe using raw QKeyEvent / QMouseEvent —
// the shipped binary gains no Qt Test-module link (INV-8).
// ============================================================================

// Shared refusal envelope for the e2e verbs. Mirrors the {ok:false, code,
// error} shape the harness dispatches on (spec §2.3).
static QJsonDocument e2eRefusal(const QString &verb, const QString &code,
                                const QString &msg) {
    QJsonObject o;
    o["ok"]    = false;
    o["code"]  = code;
    o["error"] = verb + QStringLiteral(": ") + msg;
    return QJsonDocument(o);
}

// Offscreen-safe target resolver (spec §2.3 / INV-4). Order: (1) an active
// modal dialog; else (2) the first visible non-main top-level; else (3) the
// main window. Never QApplication::activeWindow() (unreliable offscreen / on
// some WMs — the codebase avoids it deliberately). With a non-empty
// objectName, descend into the resolved top-level via findChild (nullptr →
// the caller emits widget_not_found).
QWidget *RemoteControl::e2eResolveTarget(const QString &objectName) const {
    QWidget *top = QApplication::activeModalWidget();
    if (!top) {
        const auto tops = QApplication::topLevelWidgets();
        for (QWidget *w : tops) {
            if (w == static_cast<QWidget *>(m_main)) continue;
            if (!w->isVisible()) continue;
            top = w;
            break;
        }
    }
    if (!top) top = m_main;
    if (objectName.isEmpty()) return top;
    return top->findChild<QWidget *>(objectName);
}

QJsonDocument RemoteControl::cmdInjectKey(const QJsonObject &req) {
    const QString widget = req.value(QStringLiteral("widget")).toString();
    QWidget *target = e2eResolveTarget(widget);
    if (!widget.isEmpty() && !target)
        return e2eRefusal(QStringLiteral("inject-key"),
                          QStringLiteral("widget_not_found"),
                          QStringLiteral("no widget named \"%1\"").arg(widget));
    if (!target)
        return e2eRefusal(QStringLiteral("inject-key"),
                          QStringLiteral("bad_args"),
                          QStringLiteral("no live top-level window"));

    // Post to the focus widget (falling back to the target itself) — the
    // robust, accelerator-independent path the smoke suite relies on
    // (a printable string reaching the focused grid's keyPressEvent → PTY).
    QWidget *dest = target->focusWidget();
    if (!dest) dest = target;

    // key: an integer Qt::Key value, or a small set of named keys.
    int key = 0;
    const QJsonValue kv = req.value(QStringLiteral("key"));
    if (kv.isDouble()) {
        key = kv.toInt();
    } else {
        static const QHash<QString, int> named = {
            {QStringLiteral("Return"),    Qt::Key_Return},
            {QStringLiteral("Enter"),     Qt::Key_Enter},
            {QStringLiteral("Escape"),    Qt::Key_Escape},
            {QStringLiteral("Tab"),       Qt::Key_Tab},
            {QStringLiteral("Backspace"), Qt::Key_Backspace},
            {QStringLiteral("Space"),     Qt::Key_Space},
            {QStringLiteral("Up"),        Qt::Key_Up},
            {QStringLiteral("Down"),      Qt::Key_Down},
            {QStringLiteral("Left"),      Qt::Key_Left},
            {QStringLiteral("Right"),     Qt::Key_Right},
        };
        key = named.value(kv.toString(), 0);
    }
    const QString text = req.value(QStringLiteral("text")).toString();
    if (key == 0 && text.isEmpty())
        return e2eRefusal(QStringLiteral("inject-key"),
                          QStringLiteral("bad_args"),
                          QStringLiteral("need a \"key\" or \"text\""));

    Qt::KeyboardModifiers mods = Qt::NoModifier;
    const QJsonArray modArr = req.value(QStringLiteral("modifiers")).toArray();
    for (const QJsonValue &m : modArr) {
        const QString ms = m.toString().toLower();
        if (ms == QLatin1String("ctrl") || ms == QLatin1String("control"))
            mods |= Qt::ControlModifier;
        else if (ms == QLatin1String("shift"))
            mods |= Qt::ShiftModifier;
        else if (ms == QLatin1String("alt"))
            mods |= Qt::AltModifier;
        else if (ms == QLatin1String("meta"))
            mods |= Qt::MetaModifier;
    }

    QCoreApplication::postEvent(
        dest, new QKeyEvent(QEvent::KeyPress, key, mods, text));
    QCoreApplication::postEvent(
        dest, new QKeyEvent(QEvent::KeyRelease, key, mods, text));
    QJsonObject o;
    o["ok"] = true;
    return QJsonDocument(o);
}

QJsonDocument RemoteControl::cmdInjectClick(const QJsonObject &req) {
    const QString widget = req.value(QStringLiteral("widget")).toString();
    QWidget *target = e2eResolveTarget(widget);
    if (!widget.isEmpty() && !target)
        return e2eRefusal(QStringLiteral("inject-click"),
                          QStringLiteral("widget_not_found"),
                          QStringLiteral("no widget named \"%1\"").arg(widget));
    if (!target)
        return e2eRefusal(QStringLiteral("inject-click"),
                          QStringLiteral("bad_args"),
                          QStringLiteral("no live top-level window"));

    QPoint pt;
    if (req.contains(QStringLiteral("x")) && req.contains(QStringLiteral("y")))
        pt = QPoint(req.value(QStringLiteral("x")).toInt(),
                    req.value(QStringLiteral("y")).toInt());
    else
        pt = target->rect().center();

    Qt::MouseButton btn = Qt::LeftButton;
    const QString bs = req.value(QStringLiteral("button")).toString().toLower();
    if (bs == QLatin1String("right"))       btn = Qt::RightButton;
    else if (bs == QLatin1String("middle")) btn = Qt::MiddleButton;

    const QPointF local(pt);
    const QPointF global = target->mapToGlobal(pt);
    QCoreApplication::postEvent(
        target, new QMouseEvent(QEvent::MouseButtonPress, local, global,
                                btn, btn, Qt::NoModifier));
    QCoreApplication::postEvent(
        target, new QMouseEvent(QEvent::MouseButtonRelease, local, global,
                                btn, Qt::NoButton, Qt::NoModifier));
    QJsonObject o;
    o["ok"] = true;
    return QJsonDocument(o);
}

QJsonDocument RemoteControl::cmdResizeWindow(const QJsonObject &req) {
    // resize-window ignores `widget` — it acts on the resolved top-level
    // window. Reply echoes the post-clamp size (INV-1: socket-observable).
    QWidget *target = e2eResolveTarget(QString());
    if (!target)
        return e2eRefusal(QStringLiteral("resize-window"),
                          QStringLiteral("bad_args"),
                          QStringLiteral("no live top-level window"));
    QWidget *win = target->window();

    int w = req.value(QStringLiteral("w")).toInt(0);
    int h = req.value(QStringLiteral("h")).toInt(0);
    if (w <= 0 || h <= 0)
        return e2eRefusal(QStringLiteral("resize-window"),
                          QStringLiteral("bad_args"),
                          QStringLiteral("\"w\" and \"h\" must be positive"));

    // Clamp to [minimum, screen-available].
    const QSize minSz =
        win->minimumSizeHint().expandedTo(win->minimumSize());
    if (minSz.width()  > 0) w = std::max(w, minSz.width());
    if (minSz.height() > 0) h = std::max(h, minSz.height());
    if (QScreen *scr = win->screen()) {
        const QRect avail = scr->availableGeometry();
        if (avail.width()  > 0) w = std::min(w, avail.width());
        if (avail.height() > 0) h = std::min(h, avail.height());
    }
    win->resize(w, h);

    QJsonObject o;
    o["ok"] = true;
    o["w"]  = win->width();
    o["h"]  = win->height();
    return QJsonDocument(o);
}

QJsonDocument RemoteControl::cmdGrabImage(const QJsonObject &req) {
    // Guard order (spec §2.3 / INV-5): (1) empty path → bad_args;
    // (2) unset artifact dir → bad_args (clearer than validatePath's
    // fail-closed empty-root message); (3) path escaping the artifact root
    // → bad_path via PathValidation.
    const QString path = req.value(QStringLiteral("path")).toString();
    if (path.isEmpty())
        return e2eRefusal(QStringLiteral("grab-image"),
                          QStringLiteral("bad_args"),
                          QStringLiteral("\"path\" required"));
    const QByteArray dirEnv = qgetenv("ANTS_E2E_ARTIFACT_DIR");
    if (dirEnv.isEmpty())
        return e2eRefusal(QStringLiteral("grab-image"),
                          QStringLiteral("bad_args"),
                          QStringLiteral("ANTS_E2E_ARTIFACT_DIR unset"));
    const QString root =
        QFileInfo(QString::fromLocal8Bit(dirEnv)).canonicalFilePath();
    const auto chk = PathValidation::validatePath(
        path, root, QStringLiteral("grab-image"), QStringLiteral("path"));
    if (chk.bad) return QJsonDocument(chk.err);

    const QString widget = req.value(QStringLiteral("widget")).toString();
    QWidget *target = e2eResolveTarget(widget);
    if (!widget.isEmpty() && !target)
        return e2eRefusal(QStringLiteral("grab-image"),
                          QStringLiteral("widget_not_found"),
                          QStringLiteral("no widget named \"%1\"").arg(widget));
    if (!target)
        return e2eRefusal(QStringLiteral("grab-image"),
                          QStringLiteral("bad_args"),
                          QStringLiteral("no live top-level window"));

    const QString out = chk.argvForm;
    if (!target->grab().save(out, "PNG"))
        return e2eRefusal(QStringLiteral("grab-image"),
                          QStringLiteral("bad_args"),
                          QStringLiteral("PNG save failed"));
    QJsonObject o;
    o["ok"]   = true;
    o["path"] = out;
    return QJsonDocument(o);
}

// ANTS-1932 — to_status synonym resolver. Maps the natural English words
// a caller reaches for first (done/wip/todo/maybe …) onto the canonical
// roadmap_log status. Extracted (ANTS-2126) so the GFM flip path and the
// pass-headings flip path share one map instead of duplicating it.
QString rcdetail::rlCanonicalToStatus(const QString &toStatus) {
    const QString lo = toStatus.toLower();
    if (lo == QLatin1String("done")     || lo == QLatin1String("complete") ||
        lo == QLatin1String("completed"))
        return QStringLiteral("shipped");
    if (lo == QLatin1String("wip")      || lo == QLatin1String("in_progress"))
        return QStringLiteral("in-progress");
    if (lo == QLatin1String("todo")     || lo == QLatin1String("open"))
        return QStringLiteral("planned");
    if (lo == QLatin1String("maybe")    || lo == QLatin1String("idea"))
        return QStringLiteral("considered");
    return toStatus;
}

// ============================ ANTS-2126 ============================
// Pass-headings (`#### Pass N.M`) write handlers. The GFM/ants-v1 write
// paths route here (instead of returning the ANTS-2031 format_mismatch
// refusal) when the target roadmap is pass-headings. File-static free
// functions: each is pure given (req, roadmapPath, markdown) — invoked
// from inside the member handlers at the format-detection gate, so they
// need no test seam of their own (the existing *ForTest seams reach them
// through the gate). The splice/flip primitives live in
// passheadingwrite.{h,cpp}. See docs/specs/ANTS-2126.md.

// Atomic ROADMAP.md write (QSaveFile). No counter side-effect (INV-10).
static bool rcAtomicWriteRoadmap(const QString &path, const QString &content) {
    QSaveFile rw(path);
    if (!rw.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    const QByteArray utf8 = content.toUtf8();
    return rw.write(utf8) == utf8.size() && rw.commit();
}

// Validate + render one pass bullet (shared by single + batch append).
struct PassAppendItem {
    bool    ok = false;
    QString code;     // refusal code iff !ok
    QString error;    // message iff !ok
    QString block;    // rendered `#### Pass …` block iff ok
    QString synthId;  // reader-synthesised id iff ok
};
// ANTS-4354 — `fallbackPass` is the CALL-level `pass`, used when a bullet
// carries none. On the single-append path the bullet object IS the request,
// so nothing changed there. On `append_batch` the bullet is one `bullets[]`
// item with no `pass` slot, and this function was handed it alone — so every
// bullet refused bad_args "pass is required", and supplying the top-level
// `pass` did not help because nothing read it. The refusal was byte-identical
// either way, which is the shape that reads as "I passed it wrong" and
// invites retries that cannot work. So append_batch could write NONE on a
// pass-headings roadmap, not merely all-under-one-heading as the schema
// suggested.
//
// Both halves of the reporter's first option ship: a per-bullet `pass`
// (mirroring how `stable_id` is already per-bullet under
// id_strategy:"stable_prefix"), AND the call-level one as the fallback, so a
// batch of N passes can name N designators or share one.
static PassAppendItem rcRenderPassBullet(const QJsonObject &b,
                                         const QString &fallbackPass = {}) {
    PassAppendItem it;
    const QString status   = b.value(QStringLiteral("status")).toString();
    const QString headline = b.value(QStringLiteral("headline")).toString();
    const QString perBullet = b.value(QStringLiteral("pass")).toString();
    const QString pass = perBullet.isEmpty() ? fallbackPass : perBullet;
    QString body           = b.value(QStringLiteral("body")).toString();
    // INV-3 — status / pass / headline are required; bad_args is the
    // documented missing/ill-shaped-arg code (ANTS-2128 keeps the GFM
    // append path's undocumented missing_field out of this new site).
    if (status.isEmpty()) {
        it.code = QStringLiteral("bad_args");
        it.error = QStringLiteral("status is required"); return it;
    }
    const QString keyword = PassHeadingWrite::passStatusKeyword(status);
    if (keyword.isEmpty()) {
        it.code = QStringLiteral("bad_status");
        it.error = QStringLiteral("unknown status \"%1\" — expected "
            "planned / in-progress / shipped / considered").arg(status);
        return it;
    }
    if (pass.isEmpty()) {
        it.code = QStringLiteral("bad_args");
        it.error = QStringLiteral("pass is required on a pass-headings "
            "roadmap (e.g. \"43.5\" or \"43.5.B\") — set it per bullet, or "
            "once at the call to share one designator across the batch");
        return it;
    }
    if (!PassHeadingWrite::isValidPassDesignator(pass)) {
        it.code = QStringLiteral("bad_args");
        it.error = QStringLiteral("pass \"%1\" is malformed — expected "
            "^\\d+\\.\\d+(?:\\.[A-Za-z][A-Za-z0-9]*)?$").arg(pass);
        return it;
    }
    if (headline.trimmed().isEmpty()) {
        it.code = QStringLiteral("bad_args");
        it.error = QStringLiteral("headline is required"); return it;
    }
    QStringList scrubbed;
    rcScrubLeakedToolXml(body, scrubbed);
    it.block   = PassHeadingWrite::formatPassBlock(pass, headline, keyword, body);
    it.synthId = PassHeadingWrite::passIdFromDesignator(pass);
    it.ok = true;
    return it;
}

// Locate `section` in a pass-headings roadmap; emit the GFM-parity
// bad_case (case-sensitive slug) / bad_section refusal when absent.
// Returns nullptr + fills *refusal on miss; the section pointer on hit.
static const RoadmapIndex::Section *rcPassFindSection(
        const QVector<RoadmapIndex::Section> &index,
        const QString &section, QJsonDocument *refusal) {
    const auto *sec = RoadmapIndex::findBySlug(index, section);
    if (sec) return sec;
    // Sanitise the echoed slug (≤ 64 B + control-char filter), same
    // hygiene as the GFM cmdRoadmapLogAppend bad_section path — never
    // reflect arbitrary caller bytes through the response.
    QString verbatim = section;
    if (verbatim.size() > 64) verbatim.truncate(64);
    for (int i = 0; i < verbatim.size(); ++i) {
        if (verbatim.at(i).unicode() < 0x20) verbatim[i] = QChar('?');
    }
    const QString sectionCi = section.toLower();
    for (const auto &s : index) {
        if (s.slug.toLower() == sectionCi && s.slug != section) {
            QJsonObject e;
            e["ok"]             = false;
            e["code"]           = QStringLiteral("bad_case");
            e["error"]          = QStringLiteral("roadmap_log: section "
                "slug case mismatch: \"%1\" — did you mean \"%2\"?")
                    .arg(verbatim, s.slug);
            e["canonical_slug"] = s.slug;
            e["format"]         = QStringLiteral("pass-headings");
            *refusal = QJsonDocument(e);
            return nullptr;
        }
    }
    QJsonObject e;
    e["ok"]     = false;
    e["code"]   = QStringLiteral("bad_section");
    e["error"]  = QStringLiteral("roadmap_log: unknown section slug "
        "\"%1\"").arg(verbatim);
    e["format"] = QStringLiteral("pass-headings");
    *refusal = QJsonDocument(e);
    return nullptr;
}

// ANTS-4117 — does this roadmap separate its pass blocks with a `---` rule?
// RetroDB's ~200 passes each end in one, so a block appended without it does
// not close and the next append reads as part of it; that session abandoned
// the verb and hand-edited three passes. Detect rather than assume: a `---`
// whose next non-blank line is a `#### Pass` heading is a block separator and
// nothing else, so a file that does not use them (Ants' own fixtures, and the
// shape ANTS-2126 § 2.2 renders) sees no change at all.
static bool rcPassBlocksUseSeparator(const QString &markdown) {
    const QStringList lines = markdown.split(QChar('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        if (lines.at(i).trimmed() != QStringLiteral("---")) continue;
        for (int j = i + 1; j < lines.size(); ++j) {
            const QString t = lines.at(j).trimmed();
            if (t.isEmpty()) continue;
            if (t.startsWith(QStringLiteral("#### Pass "))) return true;
            break;  // this `---` is some other rule — keep scanning
        }
    }
    return false;
}

// Splice `block` (already rendered, no surrounding blanks) at the end of
// the section body, managing one leading/trailing blank line for layout.
// Returns the updated body; *headingIdx0 ← 0-based line of the first
// rendered `####` heading.
static QString rcSplicePassBlock(const QString &markdown,
                                 const RoadmapIndex::Section &sec,
                                 const QString &block, int *headingIdx0) {
    QStringList lines = markdown.split(QChar('\n'));
    const int insertAt = sec.lineEnd;  // 0-indexed, exclusive
    QStringList toInsert;
    bool leadingBlank = false;
    if (insertAt > 0 && insertAt <= lines.size() &&
        !lines.value(insertAt - 1).trimmed().isEmpty()) {
        toInsert << QString();
        leadingBlank = true;
    }
    toInsert += block.split(QChar('\n'));
    if (insertAt < lines.size() &&
        !lines.value(insertAt).trimmed().isEmpty()) {
        toInsert << QString();
    }
    for (int k = toInsert.size() - 1; k >= 0; --k)
        lines.insert(insertAt, toInsert.at(k));
    if (headingIdx0) *headingIdx0 = insertAt + (leadingBlank ? 1 : 0);
    return lines.join(QChar('\n'));
}

// ANTS-4357 — the pass-headings block has no slot for kind / source / lanes /
// layman (ANTS-2126 § 2.2), so those fields are correctly DROPPED on this
// dialect. What was wrong is that the drop was silent: four supplied fields
// went nowhere with ok:true and nothing in the envelope, so a silent drop and
// a faithful write were indistinguishable and the only way to learn was to
// re-read the file. That matters because roadmap-format.md § 3.5 makes `Kind:`
// and `Layman:` REQUIRED parts of a bullet — an author conforming to the
// standard supplies them, the writer drops them, and both the author and every
// later reader believe the roadmap conforms.
//
// Echo, do not refuse: dropping them IS correct here, and refusing would make
// a batch across mixed projects unwritable.
QJsonArray rcPassIgnoredFields(const QJsonObject &req) {
    QJsonArray out;
    for (const char *k : {"kind", "source", "lanes", "layman", "evidence"}) {
        const QJsonValue v = req.value(QLatin1String(k));
        const bool present = !v.isUndefined() && !v.isNull() &&
                             !(v.isString() && v.toString().isEmpty()) &&
                             !(v.isArray() && v.toArray().isEmpty());
        if (present) out.append(QLatin1String(k));
    }
    return out;
}

QJsonDocument rcdetail::cmdRoadmapLogPassAppend(
        const QJsonObject &req, const QString &roadmapPath,
        const QString &markdown) {
    auto err = [](const QString &code, const QString &message) {
        QJsonObject e;
        e["ok"]     = false;
        e["code"]   = code;
        e["error"]  = QStringLiteral("roadmap_log op:\"append\": %1").arg(message);
        e["format"] = QStringLiteral("pass-headings");
        return QJsonDocument(e);
    };
    const QString section = req.value(QStringLiteral("section")).toString();
    const PassAppendItem it = rcRenderPassBullet(req);
    if (!it.ok) return err(it.code, it.error);

    const auto index = RoadmapIndex::buildIndex(markdown);
    QJsonDocument refusal;
    const auto *sec = rcPassFindSection(index, section, &refusal);
    if (!sec) return refusal;

    // ANTS-4117 — close the block the way this file closes its others.
    QString block = it.block;
    if (rcPassBlocksUseSeparator(markdown))
        block += QStringLiteral("\n\n---");

    int headingIdx0 = 0;
    const QString updated =
        rcSplicePassBlock(markdown, *sec, block, &headingIdx0);

    if (req.value(QStringLiteral("dry_run")).toBool()) {
        QJsonObject out;
        out["ok"]      = true;
        out["dry_run"] = true;
        out["id"]      = it.synthId;
        // ANTS-4116 — echo the roadmap file actually resolved, not a canonical
        // display name. RetroDB's roadmap is lowercase `roadmap.md`, so
        // echoing "ROADMAP.md" named a file that does not exist in that repo:
        // on a case-sensitive filesystem that reads as "the verb is about to
        // create a second, wrong roadmap", and it cost them the verb.
        out["file"]    = QFileInfo(roadmapPath).fileName();
        out["line"]    = headingIdx0 + 1;
        out["bullet"]  = block;
        out["bytes"]   = static_cast<qint64>(block.toUtf8().size());
        out["format"]  = QStringLiteral("pass-headings");
        const QJsonArray ignored = rcPassIgnoredFields(req);
        if (!ignored.isEmpty()) out["ignored_fields"] = ignored;  // ANTS-4357
        return QJsonDocument(out);
    }
    if (!rcAtomicWriteRoadmap(roadmapPath, updated))
        return err(QStringLiteral("roadmap_write_failed"),
            QStringLiteral("atomic write of \"%1\" failed").arg(roadmapPath));

    QJsonObject out;
    out["ok"]            = true;
    out["id"]            = it.synthId;
    out["file"]          = QFileInfo(roadmapPath).fileName();   // ANTS-4116
    out["line"]          = headingIdx0 + 1;
    out["format"]        = QStringLiteral("pass-headings");
    // ANTS-4117 — echo what was actually rendered, not only its size. The
    // pass-headings shape is fixed by ANTS-2126 § 2.2 (canonical Status
    // keyword, body verbatim, no kind/source/lanes/layman slot) and differs
    // from some projects' house style; a caller who did not dry_run first had
    // no way to see that until they re-read the file.
    out["bullet"]        = block;
    out["bytes_written"] = static_cast<qint64>(block.toUtf8().size());
    const QJsonArray ignored = rcPassIgnoredFields(req);
    if (!ignored.isEmpty()) out["ignored_fields"] = ignored;  // ANTS-4357
    return QJsonDocument(out);
}

QJsonDocument rcdetail::cmdRoadmapLogPassAppendBatch(
        const QJsonObject &req, const QString &roadmapPath,
        const QString &markdown) {
    const QString section = req.value(QStringLiteral("section")).toString();
    const QJsonArray bullets = req.value(QStringLiteral("bullets")).toArray();

    const auto index = RoadmapIndex::buildIndex(markdown);
    QJsonDocument refusal;
    const auto *sec = rcPassFindSection(index, section, &refusal);
    if (!sec) return refusal;

    QJsonArray applied, skipped;
    QStringList renderedBlocks;
    const bool useSeparator = rcPassBlocksUseSeparator(markdown);  // ANTS-4117
    for (int i = 0; i < bullets.size(); ++i) {
        const PassAppendItem it = rcRenderPassBullet(
            bullets.at(i).toObject(),
            req.value(QStringLiteral("pass")).toString());   // ANTS-4354
        if (!it.ok) {
            QJsonObject s;
            s["bullet_index"] = i;
            s["code"]         = it.code;
            s["error"]        = it.error;
            skipped.append(s);
            continue;
        }
        const QString block = useSeparator
            ? it.block + QStringLiteral("\n\n---") : it.block;
        renderedBlocks << block;
        QJsonObject a;
        a["bullet_index"] = i;
        a["id"]           = it.synthId;
        a["bullet"]       = block;   // ANTS-4117
        applied.append(a);
    }

    auto envelope = [&](qint64 bytesWritten) {
        QJsonObject out;
        out["ok"]            = true;
        out["op"]            = QStringLiteral("append_batch");
        out["format"]        = QStringLiteral("pass-headings");
        out["file"]          = QFileInfo(roadmapPath).fileName();   // ANTS-4116
        out["applied"]       = applied;
        out["applied_count"] = applied.size();
        out["skipped"]       = skipped;
        out["skipped_count"] = skipped.size();
        if (bytesWritten >= 0) out["bytes_written"] = bytesWritten;
        return QJsonDocument(out);
    };

    // INV-14 — an all-invalid batch leaves the file untouched.
    if (renderedBlocks.isEmpty()) return envelope(-1);

    const QString combined = renderedBlocks.join(QStringLiteral("\n\n"));
    int headingIdx0 = 0;
    const QString updated =
        rcSplicePassBlock(markdown, *sec, combined, &headingIdx0);

    if (req.value(QStringLiteral("dry_run")).toBool()) {
        QJsonObject out = envelope(-1).object();
        out["dry_run"] = true;
        return QJsonDocument(out);
    }
    if (!rcAtomicWriteRoadmap(roadmapPath, updated)) {
        QJsonObject e;
        e["ok"]     = false;
        e["code"]   = QStringLiteral("roadmap_write_failed");
        e["error"]  = QStringLiteral("roadmap_log op:\"append_batch\": "
            "atomic write of \"%1\" failed").arg(roadmapPath);
        e["format"] = QStringLiteral("pass-headings");
        return QJsonDocument(e);
    }
    return envelope(static_cast<qint64>(combined.toUtf8().size()));
}

// Serves op:"flip" AND op:"annotate" (the member gate routes both here,
// reading op from req — mirrors cmdRoadmapLogFlip).
QJsonDocument rcdetail::cmdRoadmapLogPassFlip(
        const QJsonObject &req, const QString &roadmapPath,
        const QString &markdown) {
    const bool annotateMode =
        req.value(QStringLiteral("op")).toString() ==
            QStringLiteral("annotate");
    auto err = [&](const QString &code, const QString &message) {
        QJsonObject e;
        e["ok"]     = false;
        e["code"]   = code;
        e["error"]  = QStringLiteral("roadmap_log op:\"%1\": %2")
            .arg(annotateMode ? QStringLiteral("annotate")
                              : QStringLiteral("flip"), message);
        e["format"] = QStringLiteral("pass-headings");
        return QJsonDocument(e);
    };
    const QString locId = req.value(QStringLiteral("id")).toString();
    const QString locHeadline =
        req.value(QStringLiteral("headline")).toString();
    // INV-3 — a locator is required; a pass is addressed by its
    // synthesised `PASS-N-M` id or its heading tail.
    if (locId.isEmpty() && locHeadline.isEmpty())
        return err(QStringLiteral("bad_args"),
            QStringLiteral("needs a locator — `id` (PASS-N-M) or `headline`"));

    PassHeadingWrite::WriteResult r;
    QString toKeyword;
    if (annotateMode) {
        QString note = req.value(QStringLiteral("note")).toString();
        QStringList scrubbed;
        rcScrubLeakedToolXml(note, scrubbed);
        // INV-8 — empty note → bad_args (deliberately the documented code,
        // diverging from the GFM annotate guard's missing_field; ANTS-2128).
        if (note.isEmpty())
            return err(QStringLiteral("bad_args"),
                QStringLiteral("a non-empty `note` is required"));
        r = PassHeadingWrite::annotatePass(markdown, locId, locHeadline, note);
    } else {
        const QString toStatus =
            req.value(QStringLiteral("to_status")).toString();
        if (toStatus.isEmpty())
            return err(QStringLiteral("bad_args"),
                QStringLiteral("to_status is required"));
        toKeyword = PassHeadingWrite::passStatusKeyword(
            rlCanonicalToStatus(toStatus));
        if (toKeyword.isEmpty())
            return err(QStringLiteral("bad_status"),
                QStringLiteral("unknown to_status \"%1\"").arg(toStatus));
        r = PassHeadingWrite::flipPassStatus(markdown, locId, locHeadline,
                                             toKeyword);
    }
    if (!r.ok)
        return err(r.code, QStringLiteral("no pass matched the locator"));

    // ANTS-2136 — dry_run preview: the locator resolved and the would-be
    // markdown is computed; return the preview (located id, target line,
    // would-be bytes) WITHOUT writing ROADMAP.md. A dry_run that returns
    // ok:true proves the locator resolves on a pass-headings roadmap —
    // the exact verification gap RetroDB flagged for flip/annotate.
    if (req.value(QStringLiteral("dry_run")).toBool()) {
        QJsonObject out;
        out["ok"]      = true;
        out["op"]      = annotateMode ? QStringLiteral("annotate")
                                       : QStringLiteral("flip");
        out["dry_run"] = true;
        out["id"]      = r.matchedId;
        out["file"]    = QFileInfo(roadmapPath).fileName();   // ANTS-4116
        out["line"]    = r.headingLine + 1;
        out["format"]  = QStringLiteral("pass-headings");
        out["bytes"]   = static_cast<qint64>(r.markdown.toUtf8().size());
        if (annotateMode) {
            out["note_appended"] = true;
            out["note_line"]     = r.changedLine + 1;
        }
        return QJsonDocument(out);
    }

    const qint64 sizeBefore = QFileInfo(roadmapPath).size();   // ANTS-3702
    if (!rcAtomicWriteRoadmap(roadmapPath, r.markdown))
        return err(QStringLiteral("roadmap_write_failed"),
            QStringLiteral("atomic write of \"%1\" failed").arg(roadmapPath));

    QJsonObject out;
    out["ok"]            = true;
    out["op"]            = annotateMode ? QStringLiteral("annotate")
                                        : QStringLiteral("flip");
    out["id"]            = r.matchedId;
    out["file"]          = QFileInfo(roadmapPath).fileName();   // ANTS-4116
    out["line"]          = r.headingLine + 1;
    out["format"]        = QStringLiteral("pass-headings");
    rcSetWriteBytes(out, sizeBefore,
                    static_cast<qint64>(r.markdown.toUtf8().size()));
    if (annotateMode) {
        out["note_appended"] = true;
        out["note_line"]     = r.changedLine + 1;
    } else {
        out["to_status"] = toKeyword;
    }
    return QJsonDocument(out);
}

QJsonDocument rcdetail::cmdRoadmapLogPassFlipBatch(
        const QJsonObject &req, const QString &roadmapPath,
        const QString &markdown) {
    const QString canon =
        rlCanonicalToStatus(req.value(QStringLiteral("to_status")).toString());
    const QString keyword = PassHeadingWrite::passStatusKeyword(canon);
    // to_status is validated canonical at the gate, so keyword is non-empty.
    const QJsonArray locators = req.value(QStringLiteral("locators")).toArray();

    QString md = markdown;
    QJsonArray flipped, skipped;
    for (int i = 0; i < locators.size(); ++i) {
        const QJsonObject loc = locators.at(i).toObject();
        const QString locId = loc.value(QStringLiteral("id")).toString();
        const QString locHeadline =
            loc.value(QStringLiteral("headline")).toString();
        if (locId.isEmpty() && locHeadline.isEmpty()) {
            QJsonObject s;
            s["locator_index"] = i;
            s["code"]          = QStringLiteral("missing_field");
            s["error"]         = QStringLiteral("locator needs one of "
                "id / headline");
            skipped.append(s);
            continue;
        }
        PassHeadingWrite::WriteResult r =
            PassHeadingWrite::flipPassStatus(md, locId, locHeadline, keyword);
        if (!r.ok) {
            QJsonObject s;
            s["locator_index"] = i;
            s["code"]          = r.code;
            s["error"]         = QStringLiteral("locator matched no pass");
            skipped.append(s);
            continue;
        }
        md = r.markdown;
        QString note = loc.value(QStringLiteral("note")).toString();
        if (!note.isEmpty()) {
            QStringList sc;
            rcScrubLeakedToolXml(note, sc);
            PassHeadingWrite::WriteResult an =
                PassHeadingWrite::annotatePass(md, locId, locHeadline, note);
            if (an.ok) md = an.markdown;
        }
        QJsonObject f;
        f["locator_index"] = i;
        f["id"]            = r.matchedId;
        flipped.append(f);
    }

    // ANTS-3702 — `before` < 0 means nothing was written (INV-14).
    auto envelope = [&](qint64 before, qint64 after) {
        QJsonObject out;
        out["ok"]            = true;
        out["op"]            = QStringLiteral("flip_batch");
        out["format"]        = QStringLiteral("pass-headings");
        out["file"]          = QFileInfo(roadmapPath).fileName();   // ANTS-4116
        out["flipped"]       = flipped;
        out["flipped_count"] = flipped.size();
        out["skipped"]       = skipped;
        out["skipped_count"] = skipped.size();
        if (before >= 0) rcSetWriteBytes(out, before, after);
        return QJsonDocument(out);
    };

    // INV-14 — nothing applied → file untouched.
    if (flipped.isEmpty()) return envelope(-1, 0);
    const qint64 sizeBefore = QFileInfo(roadmapPath).size();   // ANTS-3702
    if (!rcAtomicWriteRoadmap(roadmapPath, md)) {
        QJsonObject e;
        e["ok"]     = false;
        e["code"]   = QStringLiteral("roadmap_write_failed");
        e["error"]  = QStringLiteral("roadmap_log op:\"flip_batch\": "
            "atomic write of \"%1\" failed").arg(roadmapPath);
        e["format"] = QStringLiteral("pass-headings");
        return QJsonDocument(e);
    }
    return envelope(sizeBefore, static_cast<qint64>(md.toUtf8().size()));
}
// ========================== end ANTS-2126 ==========================

// ANTS-1922 — id-ordering for bundles mode. Compares the integer
// suffix of an ANTS-NNNN id ascending (so ANTS-999 precedes ANTS-1000,
// which a plain string sort inverts); falls back to a lexicographic
// compare on the full id for any non-conforming id or an equal suffix.
// One rule, three call-sites (items[] sort, bundle size-tie-break,
// bundle_label lowest-id fallback) so the envelope is byte-stable.
bool rcdetail::rcRoadmapIdLess(const QString &a, const QString &b) {
    auto suffix = [](const QString &id, bool *ok) -> qlonglong {
        const int dash = id.lastIndexOf(QLatin1Char('-'));
        if (dash < 0 || dash + 1 >= id.size()) { *ok = false; return 0; }
        return id.mid(dash + 1).toLongLong(ok);
    };
    bool okA = false, okB = false;
    const qlonglong na = suffix(a, &okA);
    const qlonglong nb = suffix(b, &okB);
    if (okA && okB) {
        if (na != nb) return na < nb;
        return a < b;   // equal numeric suffix → lexicographic tie-break
    }
    return a < b;       // non-conforming id → lexicographic
}

// ANTS-1922 — scan a (≤2000-char cached) bullet body for the FIRST
// line carrying a directional gate/blocker marker; return it trimmed
// then capped to ≤160 chars (empty = no marker). A "line" is the text
// between `\n` separators. Match is a case-folded substring test per
// line; the `until` rule additionally requires `lands` or `ships` on
// the same line, in any order. `blocks ` is deliberately NOT a marker
// (an item that blocks others is itself actionable, and the bare verb
// false-fires on prose like "blocks the cursor"). Only `blocked by` and
// the `until`+(`lands`|`ships`) rule are INV-locked (INV-5); the rest
// are best-effort. The 2000-char cap is a hard character cut, so a
// marker past it — or on the line straddling it — is missed (acceptable
// for v1: gate notes sit near the bullet head by convention).
QString rcdetail::rcExtractGateNote(const QString &body) {
    static const QStringList markers = {
        QStringLiteral("blocked by"), QStringLiteral("gated"),
        QStringLiteral("depends on"), QStringLiteral("waiting on"),
        QStringLiteral("parked"),     QStringLiteral("superseded"),
        // ANTS-3389 — the imperative prose form ("Wait for P10 to close"),
        // which no -ing/participle marker above catches.
        QStringLiteral("wait for "),
    };
    const QStringList lines = body.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QString lower = line.toLower();
        bool hit = false;
        for (const QString &m : markers) {
            if (lower.contains(m)) { hit = true; break; }
        }
        if (!hit && lower.contains(QStringLiteral("until ")) &&
            (lower.contains(QStringLiteral("lands")) ||
             lower.contains(QStringLiteral("ships")))) {
            hit = true;
        }
        if (hit) {
            QString note = line.trimmed();
            if (note.size() > 160) note.truncate(160);
            return note;
        }
    }
    return QString();
}

// ANTS-3793 § 2.1 — the owner wrapper. See remotecontrol.h for the contract;
// the two rules discharged here are § 2.2's first and second, in that order.
QVector<RoadmapParse::BulletRecord>
RemoteControl::roadmapBullets(const QString &projectRoot,
                              RoadmapSource::RoadmapText &text,
                              bool includeArchive,
                              RoadmapSource::ReadError *why,
                              QString *error) const {
    if (why)
        *why = RoadmapSource::ReadError::None;
    if (error)
        error->clear();

    // A caller with no project root cannot ask the marker anything —
    // migratedProject() refuses a root it cannot canonicalise, and passing ""
    // would turn every focused-tab call into a refusal.
    if (!projectRoot.isEmpty()) {
        // The local rather than `why` directly: a caller that passed nullptr
        // would otherwise turn rule 2's refusal into a fall-through to
        // markdown — the silent fallback INV-1 forbids, arriving through an
        // omitted out-param.
        RoadmapSource::ReadError openWhy = RoadmapSource::ReadError::None;
        RoadmapStore *store = roadmapStoreOrNull(&openWhy, error);
        if (openWhy != RoadmapSource::ReadError::None) {
            if (why)
                *why = openWhy;
            return {};
        }
        if (store) {
            RoadmapSource::ReadError seamWhy = RoadmapSource::ReadError::None;
            auto records = RoadmapSource::bulletsFor(
                *store, projectRoot, text, includeArchive,
                &seamWhy, error,
                ProjectSettings::idFormatFor(projectRoot));   // ANTS-3771
            if (why)
                *why = seamWhy;
            if (records)
                return *records;
            // nullopt with a reason is a refusal and never a fallback (INV-1);
            // nullopt with None means "not migrated", which falls through.
            if (seamWhy != RoadmapSource::ReadError::None)
                return {};
        }
    }
    // ANTS-3863 — the line that makes the laziness pay. It is reached only when
    // the project is NOT migrated, which is the one case where the whole file
    // was always going to be needed.
    // ANTS-3771 — the same declaration the seam above was given, so the two
    // outcomes of this function cannot resolve one bullet to two ids (INV-13).
    return RoadmapParse::parseBullets(
        text.full(), ProjectSettings::idFormatFor(projectRoot));
}

