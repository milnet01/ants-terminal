// ANTS-1677 mainwindow piece 5/8 — the MCP providers
#include "mainwindow.h"
#include "mainwindow_internal.h"
#include "claudesetup.h"        // ANTS-5236 — refreshStatusHookScript
#include "mcpdsocket.h"         // ANTS-5236 — reapStaleTerminalSockets
#include "mcpprojection.h"   // ANTS-2085 — mcp::setTerseDefault
#include "mcpspill.h"        // ANTS-2094 — mcp::setOffloadConfig / spillSweep
#include "mcptoolregistry.h"   // ANTS-4932 § 2.3
#include "secureio.h"          // ANTS-4456 — ensurePrivateDir (0700)
#include "terminalwidget.h"
#include "guithread.h"       // ANTS-4682 — ants::onGuiThread in inline verbs
#include "remotecontrol.h"
#include "rootprovider.h"      // ANTS-4932 § 2.4
#include "claudeintegration.h"
#include "mcporientation.h"  // ANTS-1897 — SessionStart hook installer.
#include <QJsonObject>
#include <QApplication>
#include <QMessageBox>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonValue>
#include <algorithm>

using namespace mainwindowdetail;

// ANTS-1146 — MCP-provider plumbing for ClaudeIntegration.
// Provides the scrollback / cwd / lastCommand / git-status /
// environment lookups MCP needs from MainWindow's tab/terminal
// state, then starts the hook server. Split out from
// setupClaudeIntegration because it isn't status-bar chrome.
void MainWindow::setupClaudeMcpProviders() {
    // Read at call time: m_remoteControl is built later in the constructor.
    const mcp::RemoteControlGetter rcGetter = [this] { return m_remoteControl; };
    // ANTS-2085 — publish the terse-by-default preference to the MCP
    // dispatcher before any provider can serve. Default true (token-saving
    // on out of the box); the Settings Apply path and onConfigFileChanged
    // (external edits) re-publish it.
    mcp::setTerseDefault(m_config.claudeMcpTerseResponses());
    // ANTS-3550 — publish the advisory-hint latch (default ON): the
    // next_call_hint / leaner_call_hint nudges are taught once per process
    // then suppressed. Same publish sites as terse (load + external reload).
    mcp::setHintLatchEnabled(m_config.claudeMcpHintLatch());
    // ANTS-2094 — publish the result-offload config (default ON since the
    // 2026-06-25 fast-follow) and run a one-shot session-start sweep of
    // stale (>24 h) spill files.
    mcp::setOffloadConfig(m_config.claudeMcpOffloadLargeResults(),
                          m_config.claudeMcpOffloadThresholdBytes(),
                          m_config.claudeMcpOffloadHeadBytes());
    mcp::spillSweep();
    // ANTS-1322: reap stale MCP sockets from previously-crashed
    // ants-terminal instances; the picker would otherwise pick a stale one
    // and fail to connect. ANTS-5236 — both the private runtime directory
    // and the legacy /tmp names, with ANTS-5080's owner check.
    mcpd::reapStaleTerminalSockets(
        static_cast<pid_t>(QApplication::applicationPid()));

    // ANTS-1901 — master MCP gate. Seed the dispatcher's live bit, then
    // bind the socket + export ANTS_MCP_SOCKET only when enabled. When
    // off: no socket binds, no mcp-<pid> socket file, no env
    // export (the orientation script self-silences on the missing var),
    // and any stale hook is removed below. Turning the switch ON takes
    // effect on the next launch (the socket binds here); turning it OFF
    // is honoured immediately by the dispatcher guard (ANTS-1901 § 2.4).
    const bool mcpOn = m_config.claudeMcpEnabled();
    m_claudeIntegration->setMcpEnabled(mcpOn);
    if (mcpOn) {
        // ANTS-5236 — the private runtime directory; empty when it is not
        // usable, and then nothing binds and nothing is exported.
        const QString mcpSocket = privateSocketPath(
            QStringLiteral("mcp-") + QString::number(QApplication::applicationPid()));
        if (!mcpSocket.isEmpty()) {
            m_claudeIntegration->startMcpServer(mcpSocket);

            // ANTS-1897 INV-14 — export the MCP socket path into the parent
            // process env so every PTY spawned after this point (via the
            // non-flatpak `environ`-copy loop at ptyhandler.cpp:171)
            // inherits ANTS_MCP_SOCKET. The orientation prelude script
            // gates on this var being set + the socket file existing. The
            // ordering is correct because setupStatusBarChrome() (which
            // calls this) runs at L609 of the MainWindow ctor, BEFORE the
            // first newTab() at L612 spawns a PTY. Verified via grep.
            qputenv("ANTS_MCP_SOCKET", mcpSocket.toLocal8Bit());
        }
    }

    // ANTS-1897 / ANTS-1901 — install the SessionStart hook only when the
    // master MCP gate AND the per-feature toggle are both on (default ON);
    // otherwise remove any stale Ants entry.
    if (mcpOn && m_config.claudeMcpOrientationEnabled()) {
        auto orient = ants::mcp_orientation::install();
        if (!orient.warning.isEmpty()) {
            qWarning().noquote() << "[mcp-orientation]" << orient.warning;
        }
        // INV-8 — first-run nudge latch. The visible signal to the
        // user is the prelude appearing at their next Claude session
        // start; latch the "nudge shown" flag so a future UI
        // enrichment (full QMessageBox or non-blocking toast) only
        // fires the once.
        if (orient.ok && !m_config.claudeMcpOrientationNudgeShown()) {
            m_config.setClaudeMcpOrientationNudgeShown(true);
        }
    } else {
        // User opted out — make sure no stale Ants entry remains.
        ants::mcp_orientation::uninstall();
    }
    // ANTS-1253: 12 tool handlers registered on the single-registry
    // ClaudeIntegration::registerToolProvider surface. Each handler
    // takes the JSON-RPC `arguments` object (extracts what it needs,
    // ignoring the rest) and returns the tool's response as a JSON
    // string. `get_session_info` is intentionally not registered —
    // it reads ClaudeIntegration's own state and is dispatched
    // inline (see claudeintegration.cpp processTools).
    // Indie-review-2026-05-14 lane-5 HI-3: every other envelope in the
    // codebase carries a `code` field so callers can dispatch on it
    // programmatically. This was the one outlier.
    // ANTS-1301 — recent_errors. Scans the focused terminal's recent
    // scrollback for structured errors (compiler/lint/lua/test/python).
    // TabSpecific like get_text/get_scrollback; delegates to
    // RemoteControl::cmdRecentErrors. See docs/specs/ANTS-1301.md.
    m_claudeIntegration->registerToolProvider("recent_errors",
        ClaudeIntegration::CallerCwdContract::TabSpecific,
        mcp::rcDelegate(rcGetter, &RemoteControl::cmdRecentErrors));

    // ANTS-1312 — last_selection. Returns the focused (or routed) tab's
    // current selection text so Claude can pull the highlighted error /
    // trace / snippet without walking the scrollback to re-find it.
    // TabSpecific; delegates to RemoteControl::cmdLastSelection.
    m_claudeIntegration->registerToolProvider("last_selection",
        ClaudeIntegration::CallerCwdContract::TabSpecific,
        mcp::rcDelegate(rcGetter, &RemoteControl::cmdLastSelection));

    m_claudeIntegration->registerToolProvider("get_scrollback",
        ClaudeIntegration::CallerCwdContract::TabSpecific,
        [this](const QJsonObject &args) -> QString {
            const int lines = args.value("lines").toInt(50);
            // ANTS-1392 — caller_cwd routes to the caller's tab when
            // present; falls back to focusedTerminal() otherwise.
            const QString callerCwd =
                args.value("caller_cwd").toString();
            auto *t = terminalForCaller(callerCwd);
            if (!t) return {};
            // ANTS-5219 — at most RemoteControl::kGetTextMaxLines lines,
            // trimmed to get_text's byte cap. docs/specs/ANTS-5219-scrollback-line-cap.md.
            const int available = t->grid()->scrollbackSize() + t->grid()->rows();
            // ANTS-1500 — since_cursor incremental-fetch mode. Cursor
            // encodes the grid's monotonic scrollbackPushed counter at
            // the time of issue. On a follow-up call the server
            // computes the delta and only emits new content, or flips
            // cursor_stale:true when the gap exceeds ring capacity
            // (terminal restart, ring wrap). Absent since_cursor →
            // legacy raw-text response (current contract).
            const QString sinceStr =
                args.value(QStringLiteral("since_cursor")).toString();
            // ANTS-5169 — secrets are redacted before the trim, so the
            // byte cap bounds what is sent.
            const bool redact = Config().claudeMcpRedactSecrets();
            if (sinceStr.isEmpty()) {
                const auto req = RemoteControl::capScrollbackRequest(lines, available, 0);
                const auto clean = RemoteControl::redactForClaude(
                    t->recentOutput(req.lines), redact);
                const auto trim = RemoteControl::trimScrollbackForGetText(
                    clean.text, RemoteControl::kGetTextDefaultBytesCap);
                QString head;
                if (clean.redacted > 0)
                    head = QStringLiteral("<redacted %1 secrets>\n").arg(clean.redacted);
                if (req.linesCapped > 0)
                    head += QStringLiteral("<capped at %1 of %2 requested lines>\n")
                                .arg(req.lines).arg(lines);
                return head + trim.text;
            }
            const uint64_t currentPushed =
                t->grid()->scrollbackPushed();
            const int  ringCap = t->grid()->maxScrollback();
            const int  screenRows = t->grid()->rows();
            QJsonObject env;
            env[QStringLiteral("ok")]     = true;
            env[QStringLiteral("cursor")] =
                QString::number(currentPushed);
            // The since_cursor reply reports a cut through truncated /
            // lines_dropped / bytes_dropped, never with the cap line: its
            // requested count is one the caller never sent.
            auto setContent = [&](int requestedLines) {
                const auto req =
                    RemoteControl::capScrollbackRequest(requestedLines, available, 0);
                const auto clean = RemoteControl::redactForClaude(
                    t->recentOutput(req.lines), redact);
                const auto trim = RemoteControl::trimScrollbackForGetText(
                    clean.text, RemoteControl::kGetTextDefaultBytesCap);
                env[QStringLiteral("content")] = trim.text;
                if (clean.redacted > 0)
                    env[QStringLiteral("redacted")] = clean.redacted;
                const bool truncated = req.linesCapped > 0 || trim.truncated;
                env[QStringLiteral("truncated")] = truncated;
                if (truncated) {
                    env[QStringLiteral("lines_dropped")] =
                        static_cast<qint64>(req.linesCapped) + trim.linesDropped;
                    env[QStringLiteral("bytes_dropped")] = trim.bytesDropped;
                }
            };
            bool parsedOk = false;
            const uint64_t since =
                sinceStr.toULongLong(&parsedOk);
            auto emitFullWindow = [&](const QString &reason) {
                env[QStringLiteral("cursor_stale")] = true;
                env[QStringLiteral("stale_reason")] = reason;
                setContent(lines);
                return QString::fromUtf8(QJsonDocument(env)
                    .toJson(QJsonDocument::Compact));
            };
            if (!parsedOk) {
                return emitFullWindow(QStringLiteral("malformed_cursor"));
            }
            if (since > currentPushed) {
                // Counter went backwards — terminal restart or unrelated
                // session. Stale fallback returns the current window.
                return emitFullWindow(
                    QStringLiteral("counter_regressed"));
            }
            const uint64_t added = currentPushed - since;
            if (added > static_cast<uint64_t>(ringCap)) {
                // Lines lost beyond what the ring can replay.
                return emitFullWindow(QStringLiteral("ring_wrapped"));
            }
            // Up-to-date case: emit the delta lines + current screen so
            // the caller sees both newly-scrolled content and the live
            // viewport. content == "" only when nothing happened AND
            // the screen is empty.
            const int deltaPlusScreen =
                static_cast<int>(added) + screenRows;
            env[QStringLiteral("cursor_stale")] = false;
            setContent(deltaPlusScreen);
            return QString::fromUtf8(QJsonDocument(env)
                .toJson(QJsonDocument::Compact));
        });
    m_claudeIntegration->registerToolProvider("get_cwd",
        ClaudeIntegration::CallerCwdContract::TabSpecific,
        [this](const QJsonObject &args) -> QString {
            // ANTS-1391: get_cwd's contract is "the terminal's cwd". The
            // caller passes caller_cwd so we resolve the right tab in a
            // multi-Ants-tab setup; empty caller_cwd → focused tab.
            // ANTS-1749 — route through terminalForCaller (like the
            // sibling tab-specific tools) so we only ever return an OPEN
            // tab's actual shellCwd(). Pre-fix this canonicalised and
            // echoed back ANY existing path the caller supplied — an
            // info-disclosure smell (confirms arbitrary path existence +
            // leaks the symlink-resolved form) that the ANTS-1295/-1392
            // tab-membership contract is meant to prevent.
            const QString callerCwd =
                args.value(QStringLiteral("caller_cwd")).toString();
            if (auto *t = terminalForCaller(callerCwd)) {
                const QString cwd = t->shellCwd();
                if (!cwd.isEmpty()) return cwd;
            }
            return QDir::currentPath();
        });
    m_claudeIntegration->registerToolProvider("get_last_command",
        ClaudeIntegration::CallerCwdContract::TabSpecific,
        [this](const QJsonObject &args) -> QString {
            // ANTS-1392 — caller_cwd routes to the caller's tab.
            const QString callerCwd =
                args.value("caller_cwd").toString();
            auto *t = terminalForCaller(callerCwd);
            const int   exitCode = t ? t->lastExitCode()      : 0;
            const QString output = t ? t->lastCommandOutput() : QString();
            // ANTS-1503 — mode:"summary" envelope cuts the typical
            // 2-4 KiB body to a few hundred bytes when the caller
            // only needs exit + tail + duration. Default stays
            // "full" for back-compat with the original {exit_code,
            // output, failed} shape.
            QString mode = args.value("mode").toString().trimmed().toLower();
            if (mode.isEmpty()) mode = QStringLiteral("full");
            QJsonObject info;
            info["exit_code"] = exitCode;
            info["failed"]    = (exitCode != 0);
            if (mode == QStringLiteral("summary")) {
                const QStringList lines = output.split(QLatin1Char('\n'));
                info["line_count"] = lines.size();
                const int tailFrom = std::max<int>(0, lines.size() - 20);
                QJsonArray tail;
                for (int i = tailFrom; i < lines.size(); ++i) tail.append(lines.at(i));
                info["last_20"]    = tail;
                // Duration from the most recent completed OSC 133 region.
                qint64 ms = 0;
                if (t && t->grid()) {
                    const auto &regs = t->grid()->promptRegions();
                    for (auto it = regs.rbegin(); it != regs.rend(); ++it) {
                        if (it->commandEndMs > 0 && it->commandStartMs > 0) {
                            ms = it->commandEndMs - it->commandStartMs;
                            break;
                        }
                    }
                }
                info["ms"]   = static_cast<double>(ms);
                info["mode"] = QStringLiteral("summary");
            } else {
                info["output"] = output;
                info["mode"]   = QStringLiteral("full");
            }
            return QString::fromUtf8(
                QJsonDocument(info).toJson(QJsonDocument::Compact));
        });
    m_claudeIntegration->registerToolProvider("get_environment",
        ClaudeIntegration::CallerCwdContract::TabSpecific,
        [this](const QJsonObject &args) -> QString {
            // ANTS-1392 — caller_cwd routes to the caller's tab.
            const QString callerCwd =
                args.value("caller_cwd").toString();
            auto *t = terminalForCaller(callerCwd);
            if (!t) return {};
            pid_t pid = t->shellPid();
            if (pid <= 0) return {};
            QFile envFile(QString("/proc/%1/environ").arg(pid));
            if (!envFile.open(QIODevice::ReadOnly)) return {};
            QByteArray raw = envFile.readAll();
            QStringList vars = QString::fromUtf8(raw).split('\0', Qt::SkipEmptyParts);
            QStringList filtered;
            QStringList keys = {"PATH", "VIRTUAL_ENV", "CONDA_DEFAULT_ENV", "NODE_ENV",
                               "SHELL", "EDITOR", "LANG", "HOME", "USER", "TERM", "COLORTERM"};
            for (const QString &v : vars) {
                for (const QString &k : keys) {
                    if (v.startsWith(k + "=")) { filtered << v; break; }
                }
            }
            return filtered.join("\n");
        });
    // ANTS-4682 — STAYS. Reads live tab state, which is GUI-owned. Touches
    // no project file, so it is not part of the § 5 concurrency hazard.
    m_claudeIntegration->registerToolProvider("tab_list",
        ClaudeIntegration::CallerCwdContract::ProcessGlobal,
        [this](const QJsonObject &) -> QString {
            if (!m_remoteControl) return QString::fromUtf8(kRcUnavailable);
            return QString::fromUtf8(
                m_remoteControl->cmdTabList().toJson(QJsonDocument::Compact));
        });
    m_claudeIntegration->registerToolProvider("get_text",
        ClaudeIntegration::CallerCwdContract::TabSpecific,
        [this](const QJsonObject &args) -> QString {
            if (!m_remoteControl) return QString::fromUtf8(kRcUnavailable);
            // ANTS-1244 INV-9 — tab=0 is a valid index distinct from
            // "tab omitted"; gate via isDouble(). Same shape for lines
            // (matches the IPC verb at remotecontrol.cpp:347).
            QJsonObject req;
            if (args.value("tab").isDouble())   req["tab"]   = args.value("tab").toInt();
            if (args.value("lines").isDouble()) req["lines"] = args.value("lines").toInt();
            // ANTS-1392 — forward caller_cwd so cmdGetText's
            // tab-resolution falls through to terminalForCaller when
            // `tab` is omitted.
            const QJsonValue cwdVal = args.value("caller_cwd");
            if (cwdVal.isString() && !cwdVal.toString().isEmpty())
                req["caller_cwd"] = cwdVal.toString();
            return QString::fromUtf8(
                m_remoteControl->cmdGetText(req).toJson(QJsonDocument::Compact));
        });

    // ANTS-1284 — token_usage. ANTS-1422 pull 3: explicit
    // ClaudeIntegration* is now the canonical (only) path. The
    // m_main->claudeIntegration() fallback was retired — it
    // returned null on a live build with no static-analysis
    // explanation, and the only call site (this lambda) always
    // supplies the pointer.
    // ANTS-4682 — STAYS. ClaudeIntegration counters + MainWindow savings,
    // both GUI-owned; no project file. The marshal inside cmdTokenUsage now
    // refuses rather than defaulting (ANTS-4684).
    m_claudeIntegration->registerToolProvider("token_usage",
        ClaudeIntegration::CallerCwdContract::ProcessGlobal,
        [this](const QJsonObject &args) -> QString {
            if (!m_remoteControl) return QString::fromUtf8(kRcUnavailable);
            return QString::fromUtf8(
                m_remoteControl->cmdTokenUsage(args, m_claudeIntegration)
                    .toJson(QJsonDocument::Compact));
        });

    // ANTS-4932 § 2.3 — every verb that reads no tab or terminal state is
    // registered by the shared list, the one ants-mcpd also runs.
    mcp::RegistryHost host;
    host.ci    = m_claudeIntegration;
    host.roots = m_rootProvider.get();
    // ANTS-4682 — the three Config reads are GUI-thread-owned; project_query
    // runs on the dispatch worker, so they are marshalled.
    host.projectQueryConfig = [this]() -> std::optional<mcp::ProjectQueryConfig> {
        return ants::onGuiThread([this]() {
            return mcp::ProjectQueryConfig{
                m_config.claudeMcpProjectQueryEnabled(),
                m_config.claudeMcpProjectQueryTimeoutMs(),
                m_config.claudeMcpProjectQueryResultCapBytes()};
        });
    };
    mcp::registerProjectScopedVerbs(*m_claudeIntegration, rcGetter, host);

    // ANTS-5236 § 2.3 — an installed forwarder from before the move only
    // knows the /tmp name; bring it up to date before any hook can fire.
    ants::claude_setup::refreshStatusHookScript();

    // Start hook server. ANTS-5236 — export the path it bound, as
    // ANTS_MCP_SOCKET is exported, so each terminal's tabs name its socket.
    // Cleared on failure: a terminal started from an Ants tab inherits its
    // parent's value, which would send this terminal's hooks to the parent.
    if (m_claudeIntegration->startHookServer())
        qputenv("ANTS_CLAUDE_HOOK_SOCKET",
                ClaudeIntegration::defaultHookSocketPath().toLocal8Bit());
    else
        qunsetenv("ANTS_CLAUDE_HOOK_SOCKET");
}
