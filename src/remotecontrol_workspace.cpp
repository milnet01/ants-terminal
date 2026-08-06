// ANTS-3833 TU 6/12 — Workspace and code index verbs.
#include "remotecontrol.h"
#include "remotecontrol_internal.h"
#include "fileoutline.h"
#include "readlog.h"
#include "readregion.h"
#include "mcpspill.h"        // ANTS-2094 — read_spill
#include "applyedits.h"
#include "codebaseindex.h"
#include "claudeintegration.h"
#include "mainwindow.h"
#include "pathvalidation.h"
#include "falseposledger.h"
#include "resolvedroot.h"
#include "terminalwidget.h"
#include "debuglog.h"
#include "secureio.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QCryptographicHash>

using namespace rcdetail;  // ANTS-3833

QJsonDocument RemoteControl::cmdWorkspaceSearch(const QJsonObject &req) {
    QElapsedTimer wall;
    wall.start();

    // ANTS-1248-INV-1: empty/missing pattern → bad_pattern, no fork.
    // ANTS-2041 — accept `query` as an alias for `pattern` (the natural
    // synonym a caller reaches for beside the sibling read verbs
    // roadmap_query / spec_query). `pattern` stays the source of truth;
    // `query` only fills in when `pattern` is absent/empty.
    QString pattern = req.value("pattern").toString();
    if (pattern.isEmpty()) {
        pattern = req.value(QStringLiteral("query")).toString();
    }
    if (pattern.isEmpty()) {
        QJsonObject e = wsErr("bad_pattern",
            QStringLiteral("workspace-search: missing or empty \"pattern\""));
        e["hint"] = QStringLiteral(
            "pass the search string as `pattern` (alias: `query`).");
        return QJsonDocument(e);
    }

    // Server resolves the project root from QCoreApplication::applicationDirPath()
    // is wrong — that's the build dir. The remote-control + MCP path
    // semantically targets the *focused tab's CWD*, but ripgrep's
    // working dir for the search is determined by `lane`. Default
    // (`lane=""`) is the focused tab's shellCwd; explicit `lane` is
    // resolved relative to that root and must canonicalise back inside.
    // ANTS-1391: prefer the caller_cwd-rooted project when present so a
    // Claude session in project B searches project B, not whichever tab
    // is focused in Ants.
    // ANTS-1390: `~global` / `~claude-config` sentinel routes to
    // ~/.claude/ so global-config edits (skills, agents, the global
    // CLAUDE.md) can be searched without a project root.
    const QString callerRaw =
        req.value(QStringLiteral("caller_cwd")).toString();
    QString rootCwd;
    const QString sentinelRoot =
        ants::expandGlobalConfigSentinel(callerRaw);
    if (!sentinelRoot.isEmpty()) {
        rootCwd = sentinelRoot;
    } else if (!callerRaw.isEmpty()) {
        rootCwd = callerRaw;
    } else if (auto *t = m_main->currentTerminal()) {
        rootCwd = t->shellCwd();
    }
    if (rootCwd.isEmpty()) rootCwd = QDir::currentPath();
    const QFileInfo rootInfo(rootCwd);
    const QString rootCanonical = rootInfo.canonicalFilePath();
    if (rootCanonical.isEmpty()) {
        // ANTS-1295: unified to bad_path with the rest of the
        // anchor-failure envelope; bad_lane is retired.
        return QJsonDocument(wsErr("bad_path",
            QStringLiteral("workspace-search: project root \"%1\" does not exist").arg(rootCwd)));
    }

    // ANTS-1248-INV-2 / ANTS-1295: lane validation through the central
    // PathValidation chokepoint. The historical contract requires the
    // lane to exist on disk (ripgrep's cwd), so reject when
    // check.resolved is empty even though validatePath would otherwise
    // accept via the lexical-fallback branch.
    const QString laneRaw = req.value("lane").toString();
    QString laneAbs = rootCanonical;
    if (!laneRaw.isEmpty()) {
        const auto check = PathValidation::validatePath(
            laneRaw, rootCanonical,
            QStringLiteral("workspace-search"),
            QStringLiteral("lane"));
        if (check.bad) return QJsonDocument(check.err);
        if (check.resolved.isEmpty()) {
            return QJsonDocument(wsErr("bad_path",
                QStringLiteral("workspace-search: \"lane\" does not exist")));
        }
        laneAbs = check.resolved;
    }

    // ANTS-1248-INV-9: glob validation — NFC normalise, 256-byte cap,
    // reject `..` substrings.
    QString glob = req.value("glob").toString().normalized(QString::NormalizationForm_C);
    if (!glob.isEmpty()) {
        if (glob.toUtf8().size() > kWorkspaceSearchGlobBytesCap) {
            return QJsonDocument(wsErr("bad_glob",
                QStringLiteral("workspace-search: \"glob\" exceeds 256 bytes")));
        }
        if (glob.contains(QStringLiteral(".."))) {
            return QJsonDocument(wsErr("bad_glob",
                QStringLiteral("workspace-search: \"glob\" contains \"..\" segments")));
        }
        // ANTS-1274: reject a leading '!'. ripgrep treats a !-prefixed
        // --glob as a NEGATION whose precedence is ABOVE .gitignore, so
        // "!.git" / "!node_modules/**" resurrects trees the ignore files
        // excluded. workspace-search only ever wants inclusion globs; the
        // negation operator has no legitimate use here and is the one
        // gitignore-glob shape that can un-exclude an ignored directory.
        if (glob.startsWith(QChar('!'))) {
            return QJsonDocument(wsErr("bad_glob",
                QStringLiteral("workspace-search: \"glob\" must not start with \"!\" "
                               "(negation) — use \"exclude_glob\" instead")));
        }
    }

    // ANTS-3704 — the positive-only `glob` above has no way to express
    // "everywhere EXCEPT here", so a doc-heavy repo whose hit list is dominated
    // by prose forced callers out to `Bash rg`, which is what this verb exists
    // to replace. `exclude_glob` is that expression, kept as a separate arg so
    // `glob` stays unambiguously positive and the ANTS-1274 guard above can
    // stand unchanged.
    //
    // On ANTS-1274's stated worry — that a `!`-prefixed --glob outranks
    // .gitignore and "resurrects trees the ignore files excluded" — measured on
    // ripgrep 15.2.0 against a seeded repo with `node_modules/` gitignored:
    // `--glob '!node_modules'`, `'!node_modules/**'` and `'!.git'` each left the
    // ignored tree unsearched, and a POSITIVE `--glob '*.txt'` did not surface
    // it either. The premise does not hold on the shipped rg, so exclusion here
    // is subtractive only. The guard stays anyway: it costs nothing and now
    // names the alternative rather than only what is forbidden.
    QStringList excludeGlobs;
    {
        const QJsonValue ex = req.value("exclude_glob");
        QStringList raw;
        if (ex.isString()) raw << ex.toString();
        else if (ex.isArray()) for (const QJsonValue &v : ex.toArray())
            if (v.isString()) raw << v.toString();

        for (QString g : raw) {
            g = g.normalized(QString::NormalizationForm_C);
            if (g.isEmpty()) continue;
            // A leading '!' here would double-negate into an INCLUSION, so it is
            // refused rather than stripped — silently rewriting a caller's
            // pattern into its opposite is the worse failure.
            if (g.startsWith(QChar('!'))) {
                return QJsonDocument(wsErr("bad_glob",
                    QStringLiteral("workspace-search: \"exclude_glob\" entries are already "
                                   "negations — drop the leading \"!\"")));
            }
            if (g.toUtf8().size() > kWorkspaceSearchGlobBytesCap) {
                return QJsonDocument(wsErr("bad_glob",
                    QStringLiteral("workspace-search: \"exclude_glob\" entry exceeds 256 bytes")));
            }
            if (g.contains(QStringLiteral(".."))) {
                return QJsonDocument(wsErr("bad_glob",
                    QStringLiteral("workspace-search: \"exclude_glob\" entry contains "
                                   "\"..\" segments")));
            }
            excludeGlobs << g;
        }
    }

    // ANTS-1248-INV-4: server-side max_results clamp at 500.
    int maxResults = 50;
    const QJsonValue maxVal = req.value("max_results");
    if (maxVal.isDouble()) {
        const int requested = maxVal.toInt();
        if (requested > 0) {
            maxResults = std::min(requested, kWorkspaceSearchMaxResultsCap);
        }
    }

    int context = 0;
    const QJsonValue ctxVal = req.value("context");
    if (ctxVal.isDouble()) {
        const int requested = ctxVal.toInt();
        if (requested > 0) context = std::min(requested, 10);
    }

    const bool isRegex = req.value("regex").toBool(false);
    const QString caseMode = req.value("case").toString(QStringLiteral("smart"));

    // ANTS-1876 / ANTS-3548 — per-match text clip (INV-1). The clip is
    // now default-ON (token-saver default per the project's "savers
    // default ON, keep an off switch" rule): an absent arg → the
    // kDefaultMaxMatchBytes (512) clip. An explicit value in [50, 10000]
    // overrides; an explicit `<= 0` opts OUT (0 = no clip, the off
    // switch); an out-of-range *positive* value clamps into [50, 10000]
    // so a mistyped small/large value still clips predictably rather
    // than silently disabling the saver. A non-numeric value leaves the
    // 512 default (so the only opt-out is a numeric `<= 0`).
    int maxMatchBytes = kDefaultMaxMatchBytes;
    const QJsonValue mmbVal = req.value(QStringLiteral("max_match_bytes"));
    if (mmbVal.isDouble()) {
        const int requested = mmbVal.toInt();
        if (requested <= 0) {
            maxMatchBytes = 0;          // ANTS-3548 explicit opt-out
        } else if (requested < 50) {
            maxMatchBytes = 50;         // clamp low
        } else if (requested > 10000) {
            maxMatchBytes = 10000;      // clamp high
        } else {
            maxMatchBytes = requested;
        }
    }
    // ANTS-1876 — headline_only projection. Default false; activates
    // the per-match {file, line, headline} shape.
    const bool headlineOnly =
        req.value(QStringLiteral("headline_only")).toBool(false);
    // ANTS-3537 — count_only: rows-ELIMINATED existence/frequency mode.
    // When true, the rg scan still runs (we must count), but matches[] is
    // omitted entirely and the envelope carries only {count, files_count,
    // truncated}. Complements headline_only (row-shape trim) and
    // max_match_bytes (row-length trim). Default false → byte-identical to
    // the pre-3537 envelope.
    const bool countOnly =
        req.value(QStringLiteral("count_only")).toBool(false);
    // ANTS-3549 — files_only: rows-ELIMINATED "which files matched" mode.
    // Returns the distinct matched-file set (with per-file hit counts) and
    // drops the match rows entirely — the answer to "where is X referenced?"
    // when the caller will open the files next. Much smaller than
    // headline_only when a symbol recurs many times in one file. Default
    // false. count_only (leaner still) takes precedence if both are set.
    const bool filesOnly =
        req.value(QStringLiteral("files_only")).toBool(false);
    // ANTS-3547 — offset cursor: skip the first `offset` matches so a
    // truncated search can be CONTINUED (page N+1) instead of re-run wider
    // from scratch. Default 0 (byte-identical to the pre-3547 envelope);
    // negative values clamp to 0. Mirrors roadmap_query's offset/next_offset
    // contract. Ignored by count_only / files_only (they emit no rows).
    const int offset =
        qMax(0, req.value(QStringLiteral("offset")).toInt(0));

    // ANTS-1565-INV-1/2: per-call wall-clock budget. Default 5 s
    // (kWorkspaceSearchHardKillMs); accept `timeout_sec` integer in
    // [1, 30]; out-of-range / non-numeric falls back to default. The
    // effective value is echoed on both success and hard-kill paths
    // (INV-4) so callers can see what they got.
    int budgetMs = kWorkspaceSearchHardKillMs;
    const QJsonValue tsVal = req.value(QStringLiteral("timeout_sec"));
    if (tsVal.isDouble()) {
        const int requestedSec = tsVal.toInt();
        const int requestedMs  = requestedSec * 1000;
        if (requestedMs >= kWorkspaceSearchMinBudgetMs &&
            requestedMs <= kWorkspaceSearchMaxBudgetMs) {
            budgetMs = requestedMs;
        }
    }
    const int budgetSec = budgetMs / 1000;

    // ANTS-1452: gitignore-bypass + hidden-file opt-ins. Both default to
    // pre-1452 behaviour (`respect_gitignore=true`, `include_hidden=false`).
    // Default-preserving toBool overload — non-bool JSON values fall back
    // to the default rather than coercing to false (matches the existing
    // `regex` parse idiom). Effective values are echoed back on the
    // ok:true envelope so a caller hitting 0 matches can diagnose
    // filter-induced silence vs. genuine miss.
    const bool respect_gitignore =
        req.value(QStringLiteral("respect_gitignore")).toBool(true);
    const bool include_hidden =
        req.value(QStringLiteral("include_hidden")).toBool(false);

    // ANTS-1248-INV-3: shell-less argv. Every flag is a separate
    // QString in the argv list — QProcess does not invoke a shell.
    // Two-argument start() overload (QString program, QStringList args).
    QStringList argv;
    argv << QStringLiteral("--json")
         << QStringLiteral("--no-heading")
         << QStringLiteral("--line-number")
         << QStringLiteral("--max-columns") << QString::number(kWorkspaceSearchMaxColumns)
         << QStringLiteral("--threads")
         << QString::number(kWorkspaceSearchThreads);
    if (caseMode == QLatin1String("smart"))           argv << QStringLiteral("--smart-case");
    else if (caseMode == QLatin1String("insensitive")) argv << QStringLiteral("--ignore-case");
    else if (caseMode == QLatin1String("sensitive"))   argv << QStringLiteral("--case-sensitive");
    if (!isRegex) argv << QStringLiteral("--fixed-strings");
    if (context > 0) argv << QStringLiteral("--context") << QString::number(context);
    if (!glob.isEmpty()) argv << QStringLiteral("--glob") << glob;
    // ANTS-3704 — rendered AFTER the positive glob: rg resolves competing globs
    // by last-one-wins, so an exclusion must follow the inclusion it narrows.
    for (const QString &ex : std::as_const(excludeGlobs))
        argv << QStringLiteral("--glob") << (QLatin1Char('!') + ex);
    // ANTS-1452-INV-1: when respect_gitignore is false, disable both the
    // VCS-specific ignore source (.gitignore, .git/info/exclude) and the
    // umbrella ignore that covers .ignore + per-user global. Belt-and-
    // braces — rg accepts both flags without conflict.
    if (!respect_gitignore) {
        argv << QStringLiteral("--no-ignore-vcs")
             << QStringLiteral("--no-ignore");
    }
    // ANTS-1452-INV-2: opt into dotfile paths. rg still excludes .git/
    // itself regardless of --hidden.
    if (include_hidden) {
        argv << QStringLiteral("--hidden");
    }
    argv << QStringLiteral("--") << pattern << laneAbs;

    QProcess rg;
    rg.setWorkingDirectory(rootCanonical);
    rg.setProcessChannelMode(QProcess::SeparateChannels);
    // ANTS-1248-INV-3: QProcess::start(QString, QStringList) — argv
    // form. No shell, no single-string overload.
    rg.start(QStringLiteral("rg"), argv);
    if (!rg.waitForStarted(500)) {
        return QJsonDocument(wsErr("rg_failed",
            QStringLiteral("workspace-search: rg failed to start (is ripgrep installed?)")));
    }

    // ANTS-1248-INV-5: hard kill via budgetMs (default 5 s,
    // ANTS-1565 expanded to a per-call override; was a hard-coded 2 s
    // until ANTS-1565). waitForFinished returns false on timeout.
    // On timeout we terminate(), then grant 200 ms grace, then kill().
    const bool finished = rg.waitForFinished(budgetMs);
    bool hardKilled = false;
    if (!finished) {
        hardKilled = true;
        rg.terminate();
        if (!rg.waitForFinished(kWorkspaceSearchKillGraceMs)) {
            rg.kill();
            rg.waitForFinished(kWorkspaceSearchKillGraceMs);
        }
    }

    // ANTS-1248-INV-8: stderr cap. Read up to 4 KiB; emit only on
    // the error branch to avoid path-enumeration leaks on success.
    QByteArray stderrTail = rg.readAllStandardError();
    if (stderrTail.size() > kWorkspaceSearchStderrCapBytes) {
        stderrTail.truncate(kWorkspaceSearchStderrCapBytes);
    }

    const QByteArray stdoutBytes = rg.readAllStandardOutput();

    // rg --json emits one event per line. We want type=="match" events.
    // Each match event has data.path.text, data.line_number, and
    // data.lines.text. NDJSON-style parse: split on '\n', QJsonDocument
    // per line.
    //
    // ANTS-1304: when context > 0, rg also emits type=="context" events
    // around each match. Attribute them to the surrounding match by
    // line distance — pending-before buffer for events that precede
    // their owning match, direct-append for events that trail one.
    QJsonArray matches;
    int seenMatchEvents = 0;
    // ANTS-3537 — distinct files that contained a match. rg --json emits
    // exactly one `begin` event per matched file, so counting begins gives
    // the true file count without a per-path set (and independent of the
    // matches[] max_results cap, which count_only bypasses).
    int filesWithMatches = 0;
    // ANTS-3549 — files_only capture: matched files in rg output order plus a
    // parallel per-file match count. rg groups a file's events between its
    // begin/end, so the path is taken from the begin event and each match is
    // attributed to the file whose begin we last saw (filesOnlyCurIdx).
    QStringList filesOnlyOrder;
    QList<int>  filesOnlyHits;
    int         filesOnlyCurIdx = -1;
    bool truncated = false;
    int lastMatchIdx = -1;  // ANTS-1304: index into matches[] for the
                            // most recent match in the current file
                            // (reset on each begin / end event).
    struct PendingCtx { int line; QString text; };
    QList<PendingCtx> pendingBefore;
    const QList<QByteArray> lines = stdoutBytes.split('\n');
    // ANTS-3405 — bound the parse loop by the wall budget too, not just the
    // rg process. rg --json over a repo with large data blobs (a ~1.2 MB
    // YAML, per-locale files, *.mo binaries) can emit an enormous match
    // stream; this loop iterates EVERY line
    // regardless of max_results (to count seenMatchEvents), so a huge
    // stream blew the client's ~60 s transport timer with a raw -32000 and
    // no envelope (RetroDB feedback). Cap the total at rg-budget + an equal
    // parse budget and return the same soft rg_failed envelope instead.
    // Checked every 2048 events so the timer cost is negligible.
    const qint64 totalBudgetMs = static_cast<qint64>(budgetMs) * 2;
    bool parseBudgetExceeded = false;
    int scanCounter = 0;
    for (const QByteArray &line : lines) {
        if (line.isEmpty()) continue;
        if ((++scanCounter & 0x7FF) == 0 && wall.hasExpired(totalBudgetMs)) {
            parseBudgetExceeded = true;
            break;
        }
        QJsonParseError perr{};
        const QJsonDocument doc = QJsonDocument::fromJson(line, &perr);
        if (perr.error != QJsonParseError::NoError || !doc.isObject()) continue;
        const QJsonObject ev = doc.object();
        const QString evType = ev.value("type").toString();

        // ANTS-1304: file boundaries reset context tracking — a context
        // event in file B never belongs to a match in file A.
        if (evType == QLatin1String("begin") ||
            evType == QLatin1String("end")) {
            if (evType == QLatin1String("begin")) {
                ++filesWithMatches;   // ANTS-3537: one begin per matched file
                if (filesOnly) {
                    // ANTS-3549 — capture the path from the begin event (rg
                    // emits exactly one per matched file) and open a fresh
                    // per-file count slot for the matches that follow.
                    const QJsonObject bdata = ev.value("data").toObject();
                    QString bp = bdata.value("path").toObject()
                                     .value("text").toString();
                    if (bp.startsWith(rootCanonical + QLatin1Char('/')))
                        bp = bp.mid(rootCanonical.size() + 1);
                    filesOnlyCurIdx = filesOnlyOrder.size();
                    filesOnlyOrder.append(bp);
                    filesOnlyHits.append(0);
                }
            }
            lastMatchIdx = -1;
            pendingBefore.clear();
            continue;
        }

        // ANTS-1304: type=="context" — buffer if no prior match in this
        // file or out-of-window; append to lastMatch.context_after if
        // within +N of its line.
        if (evType == QLatin1String("context") && context > 0) {
            const QJsonObject data = ev.value("data").toObject();
            const int ctxLine = data.value("line_number").toInt();
            QString ctxText = data.value("lines").toObject().value("text").toString();
            if (ctxText.endsWith(QLatin1Char('\n'))) ctxText.chop(1);
            if (lastMatchIdx >= 0) {
                const int anchorLine =
                    matches.at(lastMatchIdx).toObject().value("line").toInt();
                if (ctxLine > anchorLine && ctxLine - anchorLine <= context) {
                    QJsonObject prim = matches.at(lastMatchIdx).toObject();
                    QJsonArray after = prim.value("context_after").toArray();
                    QJsonObject c;
                    c["line"] = ctxLine;
                    c["text"] = ctxText;
                    after.append(c);
                    prim["context_after"] = after;
                    matches.replace(lastMatchIdx, prim);
                    continue;
                }
            }
            pendingBefore.append({ctxLine, ctxText});
            continue;
        }

        if (evType != QLatin1String("match")) continue;
        ++seenMatchEvents;
        // ANTS-3549 — files_only: attribute the match to its file and skip
        // building the row. The increment sits BEFORE the max_results cap so
        // a file's count reflects ALL its matches, not just the first N.
        if (filesOnly) {
            if (filesOnlyCurIdx >= 0) ++filesOnlyHits[filesOnlyCurIdx];
            continue;
        }
        // ANTS-3547 — offset cursor: skip the first `offset` matches. Placed
        // after ++seenMatchEvents (the total count stays uncapped and
        // offset-independent) and before the max_results cap (offset pages
        // within the same cap). Context lines buffered for a skipped match are
        // bounded out by the per-match line-distance filter when the first
        // kept match drains pendingBefore.
        if (seenMatchEvents <= offset) continue;
        if (matches.size() >= maxResults) { truncated = true; continue; }

        const QJsonObject data = ev.value("data").toObject();
        QString path = data.value("path").toObject().value("text").toString();
        // Trim absolute prefix back to project-relative if possible —
        // callers want stable, short paths.
        if (path.startsWith(rootCanonical + QLatin1Char('/'))) {
            path = path.mid(rootCanonical.size() + 1);
        }
        const int lineNo = data.value("line_number").toInt();
        QString text = data.value("lines").toObject().value("text").toString();
        // Strip a single trailing newline that rg includes in `lines.text`.
        if (text.endsWith(QLatin1Char('\n'))) text.chop(1);

        QJsonObject m;
        m["file"] = path;
        m["line"] = lineNo;
        m["text"] = text;
        // ANTS-1304: drain pending-before context — keep entries in
        // [lineNo-N, lineNo-1]; drop older ones (they belonged to
        // nothing reachable in this file).
        if (context > 0) {
            QJsonArray before;
            for (const auto &p : pendingBefore) {
                if (p.line >= lineNo - context && p.line < lineNo) {
                    QJsonObject c;
                    c["line"] = p.line;
                    c["text"] = p.text;
                    before.append(c);
                }
            }
            pendingBefore.clear();
            m["context_before"] = before;
            m["context_after"]  = QJsonArray();
        }
        matches.append(m);
        lastMatchIdx = matches.size() - 1;
    }

    if (rg.exitStatus() != QProcess::NormalExit && !hardKilled) {
        QJsonObject o = wsErr("rg_failed",
            QStringLiteral("workspace-search: rg crashed (exit status not normal)"));
        if (!stderrTail.isEmpty()) o["stderr"] = QString::fromUtf8(stderrTail);
        return QJsonDocument(o);
    }
    // rg exit codes: 0 = matches found, 1 = no matches (still ok),
    // 2 = error. Anything ≥ 2 (or hard-kill) is treated as failure
    // only when no matches were parsed.
    if (rg.exitCode() >= 2 && matches.isEmpty() && !hardKilled) {
        QJsonObject o = wsErr("rg_failed",
            QStringLiteral("workspace-search: rg returned non-zero exit code %1")
                .arg(rg.exitCode()));
        if (!stderrTail.isEmpty()) o["stderr"] = QString::fromUtf8(stderrTail);
        return QJsonDocument(o);
    }
    if (hardKilled && matches.isEmpty()) {
        // No partial results — surface the hard kill rather than
        // pretending the search finished cleanly. ANTS-1565-INV-3/4 —
        // include the effective budget and a fallback hint so callers
        // know what to try next without a doc round-trip.
        QJsonObject o = wsErr("rg_failed",
            QStringLiteral("workspace-search: rg exceeded %1 s wall budget, hard-killed")
                .arg(budgetSec));
        o["timeout_sec"] = budgetSec;
        o["hint"] = QStringLiteral(
            "try a narrower lane= or glob= filter, raise timeout_sec "
            "(max 30), or fall back to `Bash rg` for one-off queries");
        return QJsonDocument(o);
    }
    // ANTS-3405 — parse budget blown with no partial results: return the
    // same soft rg_failed envelope (never a raw transport timeout). With
    // partial results, fall through and flag truncated below.
    if (parseBudgetExceeded && matches.isEmpty()) {
        QJsonObject o = wsErr("rg_failed",
            QStringLiteral("workspace-search: match stream too large to parse "
                           "within %1 s wall budget").arg(budgetSec));
        o["timeout_sec"] = budgetSec;
        o["hint"] = QStringLiteral(
            "the query matched too much to parse in time — narrow with a "
            "lane= / glob= filter or a more specific pattern, exclude large "
            "data blobs, raise timeout_sec (max 30), or fall back to `Bash rg`");
        return QJsonDocument(o);
    }
    // ANTS-1248-INV-4: post-cap detection — truncated iff we either
    // saw more match events than max_results, or the hard kill / parse
    // budget cut us off mid-stream (ANTS-3405).
    // ANTS-3547 — `offset` shifts the returned window to [offset, offset+N):
    // there is "more" iff the total exceeds what this page covers. Capture the
    // raw (pre-dedup) page size now for the next_offset cursor below — dedup
    // reassigns matches[] to the collapsed array, losing the count.
    const int pageMatchCount = matches.size();
    if (seenMatchEvents > offset + matches.size() ||
        hardKilled || parseBudgetExceeded)
        truncated = true;

    // ANTS-3537 — count_only: return the totals with no row bodies. Placed
    // after the genuine-error guards above (rg crash / exit≥2 / hard-kill or
    // parse-budget with zero results already returned rg_failed) but before
    // the dedup / clip / enclosing_symbol / byte-cap stages, all of which
    // operate on matches[] we do not emit. `count` is seenMatchEvents — the
    // TRUE total (it is incremented before the max_results cap), so unlike
    // the normal envelope a >max_results result is NOT flagged truncated;
    // truncated here means the COUNT itself is partial (the scan was
    // hard-killed or the parse budget was exceeded mid-stream). The filter
    // echoes mirror the ok:true path so a 0-count stays diagnosable.
    if (countOnly) {
        QJsonObject out;
        out["ok"]          = true;
        out["pattern"]     = pattern;
        out["count"]       = seenMatchEvents;
        out["files_count"] = filesWithMatches;
        out["truncated"]   = (hardKilled || parseBudgetExceeded);
        out["count_only"]  = true;
        out["respect_gitignore"] = respect_gitignore;
        out["include_hidden"]    = include_hidden;
        out["timeout_sec"]       = budgetSec;
        out["elapsed_ms"]        = static_cast<int>(wall.elapsed());
        return QJsonDocument(out);
    }

    // ANTS-3549 — files_only: return the distinct matched-file set (with
    // per-file hit counts) and no row bodies. Placed alongside count_only —
    // after the genuine-error guards, before the dedup / clip / enclosing /
    // byte-cap stages that all operate on matches[] we never emit. The file
    // list is NOT capped by max_results (like count_only's count it is
    // complete), and the per-file counts are uncapped too. `count` is the
    // true total match count across all files; `truncated` means the SCAN was
    // cut off (hard-kill / parse budget), never the row cap. count_only
    // (checked first) wins if both flags are set.
    if (filesOnly) {
        QJsonArray files;
        for (int i = 0; i < filesOnlyOrder.size(); ++i) {
            QJsonObject fe;
            fe["file"]  = filesOnlyOrder.at(i);
            fe["count"] = filesOnlyHits.at(i);
            files.append(fe);
        }
        QJsonObject out;
        out["ok"]          = true;
        out["pattern"]     = pattern;
        out["files"]       = files;
        out["files_count"] = filesWithMatches;
        out["count"]       = seenMatchEvents;
        out["truncated"]   = (hardKilled || parseBudgetExceeded);
        out["files_only"]  = true;
        out["respect_gitignore"] = respect_gitignore;
        out["include_hidden"]    = include_hidden;
        out["timeout_sec"]       = budgetSec;
        out["elapsed_ms"]        = static_cast<int>(wall.elapsed());
        return QJsonDocument(out);
    }

    // ANTS-1501 — near-duplicate excerpt dedup. Broad queries that hit
    // a common code shape ("emit signalName", `connect(`, "qDebug() <<")
    // repeat the same surrounding text across N files. Group by
    // whitespace-normalised excerpt; emit the first verbatim with
    // `also_at: [{file, line}, …]` carrying the rest. Default on; pass
    // dedup:false to preserve per-match verbatim output.
    const bool dedupOn =
        req.value(QStringLiteral("dedup")).toBool(true);
    int dedupCollapsed = 0;
    if (dedupOn && matches.size() > 1) {
        QHash<QString, int> firstByKey;  // normalised text → matches index
        QJsonArray collapsed;
        for (const QJsonValue &v : std::as_const(matches)) {
            const QJsonObject m = v.toObject();
            QString key = m.value("text").toString().simplified();
            const auto it = firstByKey.find(key);
            if (it == firstByKey.end()) {
                firstByKey.insert(key, collapsed.size());
                collapsed.append(m);
            } else {
                QJsonObject prim = collapsed.at(*it).toObject();
                QJsonArray alsoAt = prim.value("also_at").toArray();
                QJsonObject loc;
                loc["file"] = m.value("file").toString();
                loc["line"] = m.value("line").toInt();
                alsoAt.append(loc);
                prim["also_at"] = alsoAt;
                collapsed.replace(*it, prim);
                ++dedupCollapsed;
            }
        }
        matches = collapsed;
    }

    // ANTS-1876 — pipeline step 3: max_match_bytes clip (post-dedup
    // so the dedup key sees the unclipped text, INV-4). The clip
    // walks both the primary `text` and every `text` inside the
    // context_before / context_after arrays.
    if (maxMatchBytes > 0) {
        rcClipMatchTextFields(matches, maxMatchBytes);
    }
    // ANTS-1876 — pipeline step 4: headline_only rename + context
    // drop. Per match: text → headline; context_before /
    // context_after removed. also_at is untouched (it carries no
    // text field; INV-5b).
    if (headlineOnly) {
        rcApplyHeadlineOnly(matches);
    }

    // ANTS-2220 — enclosing_symbol: annotate each match with the function /
    // method it lives inside, folding the common "which function is this in?"
    // follow-up into the search (the way find_definition include_body folded
    // in the post-find read). Opt-in (default off): one FileOutline::compute
    // per UNIQUE matched file (cached), then each match resolves to the
    // nearest-preceding symbol by start line — the same flat outline map
    // read_region symbol-mode uses. Heuristic: a match in the gap between two
    // top-level symbols attributes to the preceding one (the flat outline
    // carries start lines only — precise brace-bounded attribution is out of
    // scope). Cost scales with the number of distinct matched files, so it is
    // off by default and only paid when asked for.
    const bool enclosingSymbol =
        req.value(QStringLiteral("enclosing_symbol")).toBool(false);
    if (enclosingSymbol && !matches.isEmpty()) {
        // One outline scan per UNIQUE matched file, cached by relative path.
        QHash<QString, QJsonArray> outlineCache;
        auto symbolsFor = [&](const QString &relFile) -> const QJsonArray & {
            const auto it = outlineCache.find(relFile);
            if (it != outlineCache.end()) return it.value();
            QJsonArray syms;
            const QString absPath = QDir::isAbsolutePath(relFile)
                ? relFile
                : rootCanonical + QLatin1Char('/') + relFile;
            const QJsonObject outline = FileOutline::compute(
                absPath, FileOutline::Mode::Auto,
                /*includeDocComment=*/false, /*maxSymbols=*/2000);
            if (outline.value("ok").toBool())
                syms = outline.value("symbols").toArray();
            return *outlineCache.insert(relFile, syms);
        };
        for (qsizetype i = 0; i < matches.size(); ++i) {
            QJsonObject m = matches.at(i).toObject();
            const QString enclosing = enclosingSymbolForLine(
                symbolsFor(m.value("file").toString()),
                m.value("line").toInt());
            if (!enclosing.isEmpty()) {
                m["enclosing"] = enclosing;
                matches.replace(i, m);
            }
        }
    }

    QJsonObject out;
    out["ok"]         = true;
    out["pattern"]    = pattern;
    out["matches"]    = matches;
    out["truncated"]  = truncated;
    // ANTS-3547 — offset cursor echoes. Echo `offset` only when non-default
    // (keeps the common offset=0 envelope byte-identical), and emit
    // `next_offset` — the page-N+1 cursor — when more matches remain beyond
    // this page (mirrors roadmap_query). next_offset is in raw match-event
    // space so it composes with dedup: each page dedups within itself, and the
    // next page resumes at the raw cursor. Not emitted on a hard-kill / parse
    // -budget / byte-cap-only truncation (there is no coherent match cursor).
    if (offset > 0) out["offset"] = offset;
    if (seenMatchEvents > offset + pageMatchCount)
        out["next_offset"] = offset + pageMatchCount;
    if (dedupOn) {
        out["dedup"]            = true;
        out["dedup_collapsed"]  = dedupCollapsed;
    }
    // ANTS-2045 — a multi-word query is matched as ONE literal/regex
    // pattern, not as AND-combined terms, so a natural-language query like
    // "About modal RetroDB version" silently returns zero matches even when
    // each word exists. When a whitespace-bearing query hits zero matches,
    // surface an advisory hint (pure response-shaping; search semantics
    // unchanged). Reproduced across four CC sessions.
    if (matches.isEmpty() && pattern.trimmed().contains(QChar(' '))) {
        out["hint"] = isRegex
            ? QStringLiteral("query matched as one regex pattern (its spaces "
                "are literal); for a phrase search pass a single token, or "
                "AND terms with .*")
            : QStringLiteral("query matched as one literal phrase, not as "
                "separate words; pass a single token, or set regex:true and "
                "join terms with .* to AND them");
    }
    // ANTS-3466 — companion to ANTS-2045 for the no-whitespace case: a
    // metacharacter-bearing single token (e.g. `A|B|C`) with regex:false is
    // matched literally, finds nothing, and previously returned no hint — an
    // LLM caller misreads that as "symbol absent". Only fires when the phrase
    // hint above did not (no whitespace), so `hint` is never double-set.
    else if (matches.isEmpty() && !isRegex
             && rcLooksLikeRegexButLiteral(pattern)) {
        out["hint"] = QStringLiteral(
            "pattern contains regex metacharacters (e.g. |, .*, [ ]) but "
            "regex:false, so it was matched literally and found nothing — "
            "did you mean regex:true?");
    }
    // ANTS-2181 — complementary advisory: a regex alternation carrying very
    // short bare terms substring-matches inside longer words (the "tan in
    // constant" self-inflicted-noise class). Pure response-shaping, distinct
    // key from the zero-match `hint` above; pairs with the leaner_call_hint
    // appended downstream.
    if (isRegex) {
        const QStringList shortTerms = rcShortBareAltTerms(pattern);
        if (!shortTerms.isEmpty()) {
            out["regex_advisory"] = QStringLiteral(
                "alternation contains short bare term(s) [%1] that match "
                "inside longer words (e.g. \"tan\" in \"constant\"); anchor "
                "with \\b (e.g. \\b%2\\b) or raise specificity to cut noise")
                    .arg(shortTerms.join(QStringLiteral(", ")),
                         shortTerms.first());
        }
    }
    // ANTS-1452-INV-4: echo effective filter values so callers can tell
    // a filter-induced 0-match result from a genuinely clean tree.
    out["respect_gitignore"] = respect_gitignore;
    out["include_hidden"]    = include_hidden;
    // ANTS-1565-INV-4: echo effective wall-clock budget so a caller can
    // see whether they got their requested timeout_sec or the default.
    out["timeout_sec"] = budgetSec;
    out["elapsed_ms"] = static_cast<int>(wall.elapsed());
    // ANTS-1876 / ANTS-3548 — activation-gated echo (INV-6). The clip
    // echoes iff its EFFECTIVE value > 0. Since ANTS-3548 made the
    // default 512 (> 0), a default call now echoes `max_match_bytes:512`
    // — the deliberate transparency signal that the default clip is
    // active. Only an explicit opt-out (`max_match_bytes <= 0` → 0)
    // suppresses the echo. Only the ok:true path reaches here; error
    // envelopes return early via wsErr() before this point (INV-6b).
    if (maxMatchBytes > 0) {
        out["max_match_bytes"] = maxMatchBytes;
    }
    if (headlineOnly) {
        out["headline_only"] = true;
    }
    // ANTS-1293: byte-cap the response. max_results bounds the count; this
    // bounds total size so wide context windows / long lines can't blow
    // the transport budget. Trims matches[] from the tail and sets
    // truncated=true (the existing flag already means "not everything").
    // ANTS-3543 — auto-downshift: if the cap drops rows and the caller isn't
    // already lean (headline_only), re-project the FULL matches to their lean
    // {file,line,headline} shape and re-cap so a scanning caller keeps every
    // file:line instead of losing the tail. `scanTruncated` snapshots the
    // pre-cap truncated (scan-cutoff meaning) so the helper can recompute an
    // honest truncated. count_only / files_only returned earlier — they carry
    // no matches[] and never reach here.
    {
        const bool scanTruncated = out.value("truncated").toBool();  // pre-cap
        RemoteControl::downshiftMatches(out, headlineOnly, scanTruncated,
                                        req.value("max_bytes").toInt(0),
                                        rcApplyHeadlineOnly);
    }
    // ANTS-1248-INV-6: stateless — no cache, no member-state mutation.
    // ANTS-1248-INV-10: reachability gated by the existing UDS +
    // MCP-socket trust model (SO_PEERCRED UID + 0700 + S_ISSOCK).
    // Nothing extra to do here.
    // ANTS-1248-INV-7: tools/list schema declared in claudeintegration.cpp
    // (this body's contract; the schema lives at the wire boundary).
    return QJsonDocument(out);
}

// ANTS-2223 — outline ONE already-root-resolved file. The per-file body
// shared by the single-path and multi-path (`paths:[...]`) forms of
// cmdFileOutline (Rule of Three: two call-sites + a clean contract). Returns
// the per-file outline object on success (ok:true + path/symbols/…), or an
// ok:false {code,error} object on a per-file failure (empty/escaping/missing
// path) — so the multi-path loop can record one bad entry without aborting
// the batch.
static QJsonObject outlineOneFile(const QString &rawPath,
                                  const QString &rootCanonical,
                                  FileOutline::Mode mode, bool includeDoc,
                                  int maxSymbols, int maxBytes) {
    if (rawPath.isEmpty()) {
        QJsonObject o;
        o["ok"]    = false;
        o["error"] = QStringLiteral("file_outline: empty path entry");
        o["code"]  = QStringLiteral("bad_path");
        return o;
    }
    // ANTS-1249-INV-1 / ANTS-1295: anchor through the central
    // PathValidation chokepoint; not_found is distinct from the
    // anchor-fail bad_path envelope.
    const auto check = PathValidation::validatePath(
        rawPath, rootCanonical,
        QStringLiteral("file_outline"), QStringLiteral("path"));
    if (check.bad) return check.err;
    if (check.resolved.isEmpty()) {
        QJsonObject o;
        o["ok"]    = false;
        o["error"] = QStringLiteral("file_outline: \"%1\" does not exist")
                         .arg(rawPath);
        o["code"]  = QStringLiteral("not_found");
        return o;
    }
    QJsonObject result = FileOutline::compute(check.resolved, mode,
                                              includeDoc, maxSymbols);
    // Reframe the path back to project-relative so callers get stable
    // paths regardless of where the binary was launched.
    if (result.value("ok").toBool()) {
        const QString abs = result.value("path").toString();
        if (abs.startsWith(rootCanonical + QLatin1Char('/')))
            result["path"] = abs.mid(rootCanonical.size() + 1);
        // ANTS-1293: byte-cap so a file full of long signatures can't blow
        // the transport budget. Trims symbols[] from the tail.
        const auto cap = RemoteControl::capJsonArrayToBytes(
            result, QStringLiteral("symbols"),
            QStringLiteral("symbols_dropped"), maxBytes);
        if (cap.capClamped) result["bytes_cap_clamped"] = true;
    }
    return result;
}

// ANTS-2223 — per-file etag for the multi-path form. Same hex16-of-sha256
// shape as ClaudeIntegration::etagFor so callers see ONE etag format across
// the whole MCP surface (kept in sync deliberately — a 2-line hash, not worth
// a cross-layer dependency from remotecontrol → claudeintegration).
static QString outlineFileEtag(const QJsonObject &fileObj) {
    const QByteArray buf =
        QJsonDocument(fileObj).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(QCryptographicHash::hash(
        buf, QCryptographicHash::Sha256).toHex().left(16));
}

// ANTS-1249: file_outline — structured file outline (header_doc +
// symbols[]). Replaces a full Read of a 5 000-line file with a ~1 K
// token orientation envelope. Path-escape guarded by canonical-path
// startswith (mirrors ANTS-1248's lane check). The regex-scanner
// work itself lives in fileoutline.cpp — this body validates input,
// resolves the path, and delegates.
QJsonDocument RemoteControl::cmdFileOutline(const QJsonObject &req) {
    // ANTS-1391: prefer caller_cwd's project root over the focused tab.
    // ANTS-1390: `~global` / `~claude-config` sentinel routes to
    // ~/.claude/ so global-config edits (skills, agents, the global
    // CLAUDE.md) can be outlined without a project root. Resolved once
    // and shared across every path in the multi-path form.
    const QString callerRaw =
        req.value(QStringLiteral("caller_cwd")).toString();
    const QString sentinelRoot =
        ants::expandGlobalConfigSentinel(callerRaw);
    const QString rootCanonical =
        !sentinelRoot.isEmpty() ? sentinelRoot
                                : resolveRootCanonical(m_main, req);
    if (rootCanonical.isEmpty()) {
        QJsonObject o;
        o["ok"]    = false;
        o["error"] = QStringLiteral("file_outline: no focused project");
        o["code"]  = QStringLiteral("bad_path");
        return QJsonDocument(o);
    }

    // ANTS-1249: mode + flags, parsed once (uniform across a multi-path
    // batch).
    const FileOutline::Mode mode = FileOutline::parseMode(
        req.value("mode").toString());
    const bool includeDoc = req.value("include_doc_comment").toBool(true);
    const int  maxSymbols = req.value("max_symbols").toInt(200);
    const int  maxBytes   = req.value("max_bytes").toInt(0);

    // ANTS-2223 — multi-path form: outline several related files (a header +
    // its impl + a consumer) in ONE call instead of N. Triggered by a `paths`
    // array (wins over `path` when both are sent). Each entry resolves
    // independently and carries its own per-file `etag`; an optional `etags`
    // map ({relPath: priorEtag}) 304s any unchanged file to a compact
    // {path, unchanged:true, etag} stub, so a re-outline after editing one
    // file in the set re-sends only the changed bodies.
    const QJsonValue pathsVal = req.value(QStringLiteral("paths"));
    if (pathsVal.isArray()) {
        const QJsonArray  paths      = pathsVal.toArray();
        const QJsonObject priorEtags =
            req.value(QStringLiteral("etags")).toObject();
        QJsonArray files;
        for (const QJsonValue &pv : paths) {
            QJsonObject fileObj = outlineOneFile(
                pv.toString(), rootCanonical, mode, includeDoc,
                maxSymbols, maxBytes);
            const QString etag = outlineFileEtag(fileObj);
            const QString rel  = fileObj.value(QStringLiteral("path")).toString();
            const QString prior = priorEtags.value(rel).toString();
            if (fileObj.value("ok").toBool() && !prior.isEmpty() &&
                prior == etag) {
                QJsonObject stub;
                stub["path"]      = rel;
                stub["ok"]        = true;
                stub["unchanged"] = true;
                stub["etag"]      = etag;
                files.append(stub);
            } else {
                fileObj["etag"] = etag;
                files.append(fileObj);
            }
        }
        QJsonObject out;
        out["ok"]    = true;
        out["files"] = files;
        out["count"] = files.size();
        return QJsonDocument(out);
    }

    // Single-path form (back-compat, unchanged response shape).
    // ANTS-1249-INV-2: empty path → bad_path.
    // ANTS-2149 — accept `file_path` as an alias for `path` (mirrors the
    // sibling codebase_index verb). `path` stays the source of truth.
    QString rawPath = req.value("path").toString();
    if (rawPath.isEmpty())
        rawPath = req.value(QStringLiteral("file_path")).toString();
    if (rawPath.isEmpty()) {
        QJsonObject o;
        o["ok"]    = false;
        o["error"] = QStringLiteral("file_outline: missing or empty \"path\" (alias: \"file_path\", \"paths\")");
        o["code"]  = QStringLiteral("bad_path");
        return QJsonDocument(o);
    }
    // ANTS-1249-INV-10: reachability gate is upstream (UDS SO_PEERCRED).
    return QJsonDocument(outlineOneFile(rawPath, rootCanonical, mode,
                                        includeDoc, maxSymbols, maxBytes));
}

// ANTS-1855 — read_log: filter a log file, return only matching lines.
// No `path` → the Ants debug log (DebugLog::logFilePath(), a known
// internal path read directly). A `path` → resolved under caller_cwd
// via the central PathValidation chokepoint (ANTS-1295). The filtering
// + streaming byte-cap + since_cursor logic lives in ReadLog::filter.
QJsonDocument RemoteControl::cmdReadLog(const QJsonObject &req) {
    const QString rawPath = req.value(QStringLiteral("path")).toString();
    QString resolved;
    if (rawPath.isEmpty()) {
        resolved = DebugLog::logFilePath();
    } else {
        const QString callerRaw =
            req.value(QStringLiteral("caller_cwd")).toString();
        const QString sentinelRoot =
            ants::expandGlobalConfigSentinel(callerRaw);
        const QString rootCanonical =
            !sentinelRoot.isEmpty() ? sentinelRoot
                                    : resolveRootCanonical(m_main, req);
        if (rootCanonical.isEmpty()) {
            QJsonObject o;
            o["ok"]    = false;
            o["error"] = QStringLiteral("read_log: no focused project");
            o["code"]  = QStringLiteral("bad_path");
            return QJsonDocument(o);
        }
        const auto check = PathValidation::validatePath(
            rawPath, rootCanonical,
            QStringLiteral("read_log"), QStringLiteral("path"));
        if (check.bad) return QJsonDocument(check.err);
        if (check.resolved.isEmpty()) {
            QJsonObject o;
            o["ok"]    = false;
            o["error"] = QStringLiteral("read_log: \"%1\" does not exist")
                             .arg(rawPath);
            o["code"]  = QStringLiteral("not_found");
            return QJsonDocument(o);
        }
        resolved = check.resolved;
    }

    ReadLog::Options opts;
    opts.include  = req.value(QStringLiteral("include")).toString();
    opts.exclude  = req.value(QStringLiteral("exclude")).toString();
    opts.contains = req.value(QStringLiteral("contains")).toString();
    opts.since    = req.value(QStringLiteral("since")).toString();
    opts.tail     = req.value(QStringLiteral("tail")).toInt(0);
    opts.maxBytes = req.value(QStringLiteral("max_bytes")).toInt(0);
    const QJsonValue cursorVal = req.value(QStringLiteral("since_cursor"));
    if (cursorVal.isString()) {
        opts.hasSinceCursor = true;
        opts.sinceCursor = cursorVal.toString();
    } else if (cursorVal.isDouble()) {
        opts.hasSinceCursor = true;
        opts.sinceCursor = QString::number(cursorVal.toInteger());
    }

    // The echoed `path` stays the ABSOLUTE resolved path (the debug-log
    // default lives outside any project root, so no reframe — ANTS-1855
    // § 2.3).
    return QJsonDocument(ReadLog::filter(resolved, opts));
}

// ANTS-2021 — read_region: return a line range or a named symbol's body
// from a caller_cwd-relative project file. caller_cwd Required; the path is
// always supplied (no debug-log default). Selector validation + slicing
// live in the pure ReadRegion::extract helper.
QJsonDocument RemoteControl::cmdReadRegion(const QJsonObject &req) {
    const QString rawPath = req.value(QStringLiteral("path")).toString();
    if (rawPath.isEmpty()) {
        QJsonObject o;
        o["ok"]    = false;
        o["error"] = QStringLiteral("read_region: \"path\" is required");
        o["code"]  = QStringLiteral("bad_args");
        return QJsonDocument(o);
    }
    const QString callerRaw = req.value(QStringLiteral("caller_cwd")).toString();
    const QString sentinelRoot = ants::expandGlobalConfigSentinel(callerRaw);
    const QString rootCanonical =
        !sentinelRoot.isEmpty() ? sentinelRoot
                                : resolveRootCanonical(m_main, req);
    if (rootCanonical.isEmpty()) {
        QJsonObject o;
        o["ok"]    = false;
        o["error"] = QStringLiteral("read_region: no focused project");
        o["code"]  = QStringLiteral("bad_path");
        return QJsonDocument(o);
    }
    const auto check = PathValidation::validatePath(
        rawPath, rootCanonical,
        QStringLiteral("read_region"), QStringLiteral("path"));
    if (check.bad) return QJsonDocument(check.err);
    if (check.resolved.isEmpty()) {
        QJsonObject o;
        o["ok"]    = false;
        o["error"] = QStringLiteral("read_region: \"%1\" does not exist").arg(rawPath);
        o["code"]  = QStringLiteral("not_found");
        return QJsonDocument(o);
    }

    ReadRegion::Options opts;
    opts.symbol       = req.value(QStringLiteral("symbol")).toString();
    opts.section      = req.value(QStringLiteral("section")).toString();  // ANTS-2221
    opts.maxBytes     = req.value(QStringLiteral("max_bytes")).toInt(0);
    opts.callSequence =                                        // ANTS-2157
        req.value(QStringLiteral("call_sequence")).toBool(false);
    const QJsonValue startV = req.value(QStringLiteral("start_line"));
    const QJsonValue endV   = req.value(QStringLiteral("end_line"));
    if (startV.isDouble()) {
        opts.hasLine   = true;
        opts.startLine = startV.toInt();
        opts.endLine   = endV.isDouble() ? endV.toInt() : opts.startLine;
    }
    return QJsonDocument(ReadRegion::extract(check.resolved, opts));
}

// ANTS-2219 — read_regions: batched multi-selector read, the read-side mirror
// of apply_edits' batched writes. Collapses "outline → read the 6 interesting
// symbols" from N calls to one. Thin wrapper: resolve the project root (same
// sentinel + caller_cwd path as cmdReadRegion), then delegate the per-item
// loop, validation, per-item etag/304 and shared-budget logic to the pure
// ReadRegion::extractBatch (core, unit-testable without a MainWindow).
QJsonDocument RemoteControl::cmdReadRegions(const QJsonObject &req) {
    const QString callerRaw =
        req.value(QStringLiteral("caller_cwd")).toString();
    const QString sentinelRoot = ants::expandGlobalConfigSentinel(callerRaw);
    const QString rootCanonical =
        !sentinelRoot.isEmpty() ? sentinelRoot
                                : resolveRootCanonical(m_main, req);
    if (rootCanonical.isEmpty()) {
        QJsonObject o;
        o["ok"]    = false;
        o["error"] = QStringLiteral("read_regions: no focused project");
        o["code"]  = QStringLiteral("bad_path");
        return QJsonDocument(o);
    }
    // ANTS-3500 — accept `requests`/`paths`/`regions` as aliases for the
    // `items` batch key (natural first-guesses for the array of slice
    // selectors). Canonical `items` wins; otherwise the first array-valued
    // alias in preference order. If none is an array, pass `items` through so
    // extractBatch emits its precise "items array is required" bad_args error.
    QJsonValue itemsVal = req.value(QStringLiteral("items"));
    if (!itemsVal.isArray()) {
        for (const char *alias : {"requests", "paths", "regions"}) {
            const QJsonValue v = req.value(QLatin1String(alias));
            if (v.isArray()) { itemsVal = v; break; }
        }
    }
    // ANTS-3589 — a top-level `path` is the per-item default: an item that
    // omits its own `path` reads from this instead, so the single-file case
    // (N slices of one module) passes the filename once. Per-item `path` wins.
    return QJsonDocument(ReadRegion::extractBatch(
        rootCanonical, itemsVal,
        req.value(QStringLiteral("max_bytes")).toInt(0),
        req.value(QStringLiteral("path")).toString()));
}

// ANTS-2094 — read_spill: re-read a body spilled by the offload path, by its
// content-addressed handle, byte-paged. The spill store is global (under
// ~/.cache, not project-scoped), so this verb takes no project root and
// resolves ONLY under the spill dir keyed by the validated handle — it adds
// no filesystem reach beyond that dir.
QJsonDocument RemoteControl::cmdReadSpill(const QJsonObject &req) {
    const QString handle = req.value(QStringLiteral("handle")).toString();
    static const QRegularExpression handleRe(QStringLiteral("^[0-9a-f]{64}$"));
    if (!handleRe.match(handle).hasMatch()) {
        QJsonObject o;
        o["ok"]    = false;
        o["error"] = QStringLiteral(
            "read_spill: \"handle\" must be a 64-char lowercase sha256");
        o["code"]  = QStringLiteral("bad_args");
        return QJsonDocument(o);
    }
    // ANTS-3545 — row mode: a numeric row_offset/row_count routes to
    // readSpillRows (row-paged), which owns the negative-arg / not_found /
    // too_large / not_array refusals. isDouble() (not mere presence) mirrors
    // byte mode's arg reads, so a non-numeric row arg falls to byte mode. When
    // a request mixes row keys with byte offset/max_bytes, row mode wins and
    // the byte args are ignored (§ 2.4.1).
    const QJsonValue roV = req.value(QStringLiteral("row_offset"));
    const QJsonValue rcV = req.value(QStringLiteral("row_count"));
    if (roV.isDouble() || rcV.isDouble()) {
        const qint64 rowOffset = roV.isDouble() ? static_cast<qint64>(roV.toDouble()) : 0;
        const qint64 rowCount  = rcV.isDouble() ? static_cast<qint64>(rcV.toDouble()) : 0;
        const mcp::SpillRows r = mcp::readSpillRows(handle, rowOffset, rowCount);
        if (!r.ok) {
            QJsonObject o;
            o["ok"]   = false;
            o["code"] = r.code;
            if (r.code == QStringLiteral("not_found")) {
                o["error"] = QStringLiteral(
                    "read_spill: handle not found (never spilled, or evicted — "
                    "re-issue the original call)");
            } else if (r.code == QStringLiteral("too_large")) {
                o["error"] = QStringLiteral(
                    "read_spill: body too large (> 1 MiB) to parse for "
                    "row-paging");
                o["hint"]  = QStringLiteral(
                    "byte-page it via offset/max_bytes — byte mode does not "
                    "parse the body");
            } else if (r.code == QStringLiteral("not_array")) {
                o["error"] = QStringLiteral(
                    "read_spill: body has no row-shaped array to page");
                o["hint"]  = QStringLiteral(
                    "byte-page it via offset/max_bytes instead");
            } else {  // bad_args
                o["error"] = QStringLiteral(
                    "read_spill: \"row_offset\"/\"row_count\" must be >= 0");
            }
            return QJsonDocument(o);
        }
        QJsonObject o;
        o["ok"]         = true;
        o["mode"]       = QStringLiteral("rows");
        o["key"]        = r.key;
        o["rows"]       = r.rows;
        o["row_offset"] = r.rowOffset;
        o["total_rows"] = r.totalRows;
        o["truncated"]  = r.truncated;
        return QJsonDocument(o);
    }
    // Byte mode (§ 2.4) — unchanged; the negative-byte-arg gate below is now
    // reached only in byte mode (row mode returned above).
    const QJsonValue offV = req.value(QStringLiteral("offset"));
    const QJsonValue mbV  = req.value(QStringLiteral("max_bytes"));
    const qint64 offset   = offV.isDouble() ? static_cast<qint64>(offV.toDouble()) : 0;
    const qint64 maxBytes = mbV.isDouble()  ? static_cast<qint64>(mbV.toDouble())  : 0;
    if (offset < 0 || (mbV.isDouble() && maxBytes < 0)) {
        QJsonObject o;
        o["ok"]    = false;
        o["error"] = QStringLiteral("read_spill: \"offset\"/\"max_bytes\" must be >= 0");
        o["code"]  = QStringLiteral("bad_args");
        return QJsonDocument(o);
    }

    const mcp::SpillSlice s = mcp::readSpill(handle, offset, maxBytes);
    if (!s.ok) {
        QJsonObject o;
        o["ok"]    = false;
        o["error"] = (s.code == QStringLiteral("not_found"))
            ? QStringLiteral("read_spill: handle not found (never spilled, or "
                             "evicted — re-issue the original call)")
            : QStringLiteral("read_spill: %1").arg(s.code);
        o["code"]  = s.code;
        return QJsonDocument(o);
    }
    QJsonObject o;
    o["ok"]          = true;
    o["content"]     = s.content;
    o["offset"]      = s.offset;
    o["bytes"]       = s.bytes;
    o["total_bytes"] = s.totalBytes;
    o["truncated"]   = s.truncated;
    return QJsonDocument(o);
}

// ANTS-2022 — apply_edits: apply N {path, old, new} edits across M project
// files in one call, atomic per file. caller_cwd Required. Path-escape is a
// fail-closed whole-call refusal (bad_path); a missing file / absent or
// ambiguous `old` / oversized file / failed commit is a per-edit soft skip.
QJsonDocument RemoteControl::cmdApplyEdits(const QJsonObject &req) {
    const QJsonValue editsV = req.value(QStringLiteral("edits"));
    if (!editsV.isArray() || editsV.toArray().isEmpty()) {
        QJsonObject o;
        o["ok"]    = false;
        o["error"] = QStringLiteral("apply_edits: \"edits\" must be a non-empty array");
        o["code"]  = QStringLiteral("bad_args");
        return QJsonDocument(o);
    }
    const QJsonArray edits = editsV.toArray();

    // ANTS-2227 — dry_run preview: resolve + apply every edit IN MEMORY but
    // skip the atomic write, returning the would-be applied/skipped tallies so
    // a caller can pre-flight a multi-file edit (catch a no_match before any
    // partial write) without touching disk.
    const bool dryRun = req.value(QStringLiteral("dry_run")).toBool();

    const QString callerRaw = req.value(QStringLiteral("caller_cwd")).toString();
    const QString sentinelRoot = ants::expandGlobalConfigSentinel(callerRaw);
    const QString rootCanonical =
        !sentinelRoot.isEmpty() ? sentinelRoot
                                : resolveRootCanonical(m_main, req);
    if (rootCanonical.isEmpty()) {
        QJsonObject o;
        o["ok"]    = false;
        o["error"] = QStringLiteral("apply_edits: no focused project");
        o["code"]  = QStringLiteral("bad_path");
        return QJsonDocument(o);
    }

    // Pass 1 — validate args + paths (fail-closed on escape) before writing.
    // ANTS-3711 — an edit names its target EITHER by unique `old` text or by an
    // inclusive 1-based `start_line`/`end_line` range. Exactly one, never both.
    struct E {
        int index; QString rawPath; QString resolved;
        QString oldStr; QString newStr; bool replaceAll = false;
        bool isRange = false;
        int startLine = 0, endLine = 0;
        QString expectFirst, expectLast;
    };
    QVector<E> es;
    es.reserve(edits.size());
    auto argErr = [](const QString &msg) {
        QJsonObject o;
        o["ok"]    = false;
        o["error"] = msg;
        o["code"]  = QStringLiteral("bad_args");
        return QJsonDocument(o);
    };
    for (int i = 0; i < edits.size(); ++i) {
        const QJsonObject e = edits.at(i).toObject();
        const QString rawPath = e.value(QStringLiteral("path")).toString();
        const bool hasOld   = e.contains(QStringLiteral("old"));
        const bool hasNew   = e.contains(QStringLiteral("new"));
        const bool hasStart = e.contains(QStringLiteral("start_line"));
        const bool hasEnd   = e.contains(QStringLiteral("end_line"));
        const bool hasRange = hasStart || hasEnd;
        const QString oldStr = e.value(QStringLiteral("old")).toString();
        if (rawPath.isEmpty() || !hasNew) {
            return argErr(QStringLiteral("apply_edits: edit %1 needs a non-empty "
                                         "\"path\" and a \"new\"").arg(i));
        }
        if (hasOld && hasRange) {
            return argErr(QStringLiteral(
                "apply_edits: edit %1 has both \"old\" and a line range — an "
                "edit names its target one way or the other").arg(i));
        }
        if (!hasOld && !hasRange) {
            return argErr(QStringLiteral(
                "apply_edits: edit %1 needs either a non-empty \"old\" or a "
                "\"start_line\"/\"end_line\" range").arg(i));
        }
        E rec;
        rec.index      = i;
        rec.rawPath    = rawPath;
        rec.newStr     = e.value(QStringLiteral("new")).toString();
        rec.replaceAll = e.value(QStringLiteral("replace_all")).toBool(false);
        if (hasOld) {
            if (oldStr.isEmpty())
                return argErr(QStringLiteral("apply_edits: edit %1 needs a "
                                             "non-empty \"old\"").arg(i));
            rec.oldStr = oldStr;
        } else {
            // Both bounds, and both expectations. The expectations are NOT
            // optional: `old` carries its own uniqueness guard, a line number
            // carries none, so an unguarded range replace would corrupt on a
            // stale number exactly the way the Bash line splice this exists to
            // replace does. `read_region` returns the line text alongside the
            // numbers, so the caller is already holding them.
            if (!hasStart || !hasEnd) {
                return argErr(QStringLiteral(
                    "apply_edits: edit %1 needs both \"start_line\" and "
                    "\"end_line\"").arg(i));
            }
            if (!e.contains(QStringLiteral("expect_first_line")) ||
                !e.contains(QStringLiteral("expect_last_line"))) {
                return argErr(QStringLiteral(
                    "apply_edits: edit %1 is a line range and must carry "
                    "\"expect_first_line\" and \"expect_last_line\" (the "
                    "verbatim text of those lines) — a line number has no "
                    "uniqueness guard of its own, and a stale one would "
                    "replace the wrong lines silently").arg(i));
            }
            rec.isRange     = true;
            rec.startLine   = e.value(QStringLiteral("start_line")).toInt();
            rec.endLine     = e.value(QStringLiteral("end_line")).toInt();
            rec.expectFirst = e.value(QStringLiteral("expect_first_line")).toString();
            rec.expectLast  = e.value(QStringLiteral("expect_last_line")).toString();
        }
        const auto check = PathValidation::validatePath(
            rawPath, rootCanonical,
            QStringLiteral("apply_edits"), QStringLiteral("path"));
        if (check.bad) return QJsonDocument(check.err);  // fail-closed bad_path
        rec.resolved = check.resolved;
        es.push_back(rec);
    }

    // Pass 2 — group existing files (first-seen order), skip missing, then
    // apply each file's edits in array order and write atomically.
    QJsonArray applied, skipped;
    int editsApplied = 0, editsSkipped = 0, filesWritten = 0;
    auto addSkip = [&](int index, const QString &path, const QString &reason) {
        QJsonObject s; s["index"] = index; s["path"] = path; s["reason"] = reason;
        skipped.append(s); ++editsSkipped;
    };

    QVector<QString> fileOrder;
    QHash<QString, QVector<int>> byFile;  // resolved → indices into es
    QHash<QString, QString> rawOf;        // resolved → first-seen rawPath
    for (int k = 0; k < es.size(); ++k) {
        if (es[k].resolved.isEmpty()) {   // in-root but file absent
            addSkip(es[k].index, es[k].rawPath, QStringLiteral("not_found"));
            continue;
        }
        if (!byFile.contains(es[k].resolved)) {
            fileOrder.push_back(es[k].resolved);
            rawOf[es[k].resolved] = es[k].rawPath;
        }
        byFile[es[k].resolved].push_back(k);
    }

    for (const QString &resolved : fileOrder) {
        const QString rawPath = rawOf.value(resolved);
        const QVector<int> &group = byFile[resolved];

        if (QFileInfo(resolved).size() > kReadToolMaxBytesCeiling) {
            for (int k : group) addSkip(es[k].index, rawPath, QStringLiteral("too_large"));
            continue;
        }
        QFile in(resolved);
        if (!in.open(QIODevice::ReadOnly)) {
            for (int k : group) addSkip(es[k].index, rawPath, QStringLiteral("not_found"));
            continue;
        }
        QString working = QString::fromUtf8(in.readAll());
        in.close();

        int fileReplacements = 0;
        QVector<int> appliedIdx;
        for (int k : group) {
            // ANTS-3711 — a range edit resolves against `working`, i.e. the
            // file as earlier edits in this same group have left it, not as it
            // was on disk. That is the only coherent choice when edits compose,
            // and it is exactly why expect_first_line/expect_last_line are
            // mandatory: an earlier edit that added or removed lines shifts
            // every later range, and the guard turns that into a loud
            // range_mismatch skip instead of a silent wrong-lines write.
            const auto oc = es[k].isRange
                ? ApplyEdits::applyRangeToContent(
                      working, es[k].startLine, es[k].endLine,
                      es[k].expectFirst, es[k].expectLast, es[k].newStr)
                : ApplyEdits::applyToContent(
                      working, es[k].oldStr, es[k].newStr, es[k].replaceAll);
            if (oc.applied) {
                working = oc.newContents;
                fileReplacements += oc.replacements;
                appliedIdx.push_back(k);
            } else {
                addSkip(es[k].index, rawPath, oc.skipReason);
            }
        }
        if (appliedIdx.isEmpty()) continue;  // nothing applied → don't touch

        // Atomic write — the applyRepair idiom (QSaveFile write-full-or-fail
        // + commit + fsyncParentDir). A whole-content substring replace, so
        // a trailing newline is preserved without split/rejoin.
        bool wrote = false;
        if (dryRun) {
            // ANTS-2227 — would-write: disk untouched. Every edit in this
            // group was already resolved against `working` above, so the
            // applied/skipped/replacements tallies below are the real ones.
            wrote = true;
        } else {
            QSaveFile sf(resolved);
            if (sf.open(QIODevice::WriteOnly)) {
                const QByteArray bytes = working.toUtf8();
                if (sf.write(bytes) == bytes.size() && sf.commit()) {
                    fsyncParentDir(resolved);
                    wrote = true;
                }
            }
        }
        if (wrote) {
            QJsonObject a; a["path"] = rawPath; a["replacements"] = fileReplacements;
            applied.append(a);
            ++filesWritten;
            editsApplied += appliedIdx.size();
        } else {
            for (int k : appliedIdx)
                addSkip(es[k].index, rawPath, QStringLiteral("commit_failed"));
        }
    }

    QJsonObject env;
    env["ok"]            = true;
    if (dryRun) env["dry_run"] = true;   // ANTS-2227 — would-be result
    env["applied"]       = applied;
    env["skipped"]       = skipped;
    env["files_written"] = filesWritten;
    env["edits_total"]   = edits.size();
    env["edits_applied"] = editsApplied;
    env["edits_skipped"] = editsSkipped;
    return QJsonDocument(env);
}

// ----- ANTS-1961 / ANTS-1962 — feedback-file MCP tools --------------
//
// Shared helpers: refusal envelope + target resolution (suffix guard +
// PathValidation with allowOutsideRoot=true). Both verbs are
// m_main-independent — an absolute `path` is used as-is; a relative
// `path` resolves under the caller_cwd canonical (mirrors the
// changelog_log resolution posture, NOT the focused-tab fallback).

namespace rcdetail {

// Forward decl — isValidSpecId is defined in the spec-tools anonymous
// namespace block further down (it predates these verbs). cmdSpecLog
// reuses it for <PREFIX>-NNNN / phase_* id routing parity with cmdSpecQuery.
bool isValidSpecId(const QString &id);
// ANTS-3356 — forward decl (defined alongside isValidSpecId). cmdSpecLog
// and cmdSpecQuery share it to resolve `<id>.md` then a `<id>-*.md` glob.
QString resolveSpecRelForId(const QString &rootCanonical,
                            const QString &dirRel, const QString &id);

QJsonObject fbErr(const QString &code, const QString &message) {
    QJsonObject o;
    o["ok"]    = false;
    o["error"] = message;
    o["code"]  = code;
    return o;
}

// Canonical suffix-guard literal (mcp-feedback-files.md § "File location
// & name"). Checked on the resolved/canonical basename.
constexpr char kFeedbackSuffix[] = "_Ants_MCP_Feedback.md";

// ANTS-3376 — the caller's project leaf (the caller_cwd basename). Used to
// (a) derive the conventional default path when `path` is omitted and
// (b) rank the caller's own sibling first in a not_found candidate list.
// Empty when caller_cwd is absent / unresolvable.
QString feedbackCallerLeaf(const QJsonObject &req) {
    const QString callerRaw =
        req.value(QStringLiteral("caller_cwd")).toString();
    return QFileInfo(QFileInfo(callerRaw).canonicalFilePath()).fileName();
}

// ANTS-3366: a project whose leaf name collides with the convention suffix
// (e.g. "DOOM_Ants" → "DOOM_Ants_Ants_MCP_Feedback.md") derives a path that
// doesn't exist. Rather than leave the not_found envelope a dead end —
// forcing the caller to shell out to `ls | grep feedback` — list the sibling
// *_Ants_MCP_Feedback.md files in the same dir so the correct basename is one
// retry away. Cheap dir read; returns absolute paths in name order.
QJsonArray feedbackSiblingCandidates(const QString &candidatePath) {
    QJsonArray out;
    const QDir dir = QFileInfo(candidatePath).absoluteDir();
    const QStringList names = dir.entryList(
        {QLatin1String("*") + QLatin1String(kFeedbackSuffix)},
        QDir::Files, QDir::Name);
    for (const QString &n : names)
        out.append(dir.absoluteFilePath(n));
    return out;
}

// ANTS-3439 — normalize a feedback-file stem (or a checkout-dir leaf) to a
// case- and separator-insensitive token, so a checkout named `Fin_Break`
// matches the `finbreak_Ants_MCP_Feedback.md` its package ships under. Lower-
// cases and drops every non-alphanumeric char (`_`, `-`, spaces, `.`).
QString normalizeFeedbackStem(const QString &s) {
    QString out;
    out.reserve(s.size());
    for (const QChar &c : s)
        if (c.isLetterOrNumber()) out.append(c.toLower());
    return out;
}

// Strip the trailing kFeedbackSuffix from a feedback filename → its stem.
// (`sizeof - 1` = suffix length without the NUL.)
QString feedbackStemOf(const QString &fileName) {
    QString stem = fileName;
    if (stem.endsWith(QLatin1String(kFeedbackSuffix)))
        stem.chop(static_cast<int>(sizeof(kFeedbackSuffix)) - 1);
    return stem;
}

// ANTS-3426 — the conventional feedback basename(s) for a project leaf, most
// preferred first. Normally just "<leaf>_Ants_MCP_Feedback.md". When the leaf
// already ends in "_Ants" (a fork checkout like "DOOM_Ants") that naive form
// DOUBLES the token → "DOOM_Ants_Ants_MCP_Feedback.md", which never exists;
// the real file drops the redundant "_Ants" → "DOOM_Ants_MCP_Feedback.md"
// (the fork's project name is the leaf minus "_Ants"). Return the de-doubled
// name first so it wins on both the read (adopt the existing file) and the
// fresh-create (correct name, no forked history) paths. Deterministic — an
// exact second candidate, not a fuzzy match, so ANTS-3366's "no silent
// fuzzy-accept" non-goal is preserved. (ANTS-3439's normalized scan can't
// catch this: the leaf normalizes to "doomants" but the file stem to "doom".)
QStringList feedbackConventionalNames(const QString &leaf) {
    QStringList names;
    if (leaf.endsWith(QLatin1String("_Ants"), Qt::CaseInsensitive)) {
        QString stem = leaf;
        stem.chop(5);  // strip the trailing "_Ants"
        names << stem + QLatin1String(kFeedbackSuffix);
    }
    names << leaf + QLatin1String(kFeedbackSuffix);
    return names;
}

// ANTS-3366: augment a feedback not_found envelope with the sibling
// candidate list (+ a one-line hint) when any exist.
// ANTS-3376: when `callerLeaf` is known, float the caller's OWN file
// (`<leaf>_Ants_MCP_Feedback.md`) to the front of `candidates` so the
// obvious retry is first; if no candidate matches the leaf, the siblings
// all belong to OTHER projects — say so (`all_other_projects`) rather than
// implying one fits.
QJsonObject fbNotFound(const QString &message, const QString &resolved,
                       const QString &callerLeaf) {
    QJsonObject e = fbErr(QStringLiteral("not_found"), message);
    QJsonArray cands = feedbackSiblingCandidates(resolved);
    if (cands.isEmpty())
        return e;

    bool ownMatch = false;
    bool normMatch = false;   // ANTS-3439 — leaf<->package normalized match
    QString ownName;          // ANTS-3426 — the matched own basename, for the hint
    if (!callerLeaf.isEmpty()) {
        // ANTS-3376 — the caller's own file is "<leaf>_Ants_MCP_Feedback.md".
        // ANTS-3426 — an "_Ants"-suffixed leaf's real file drops the doubled
        // token, so accept the de-doubled convention as the caller's own too
        // (feedbackConventionalNames returns both forms, preferred first).
        const QStringList wantedNames = feedbackConventionalNames(callerLeaf);
        for (int i = 0; i < cands.size(); ++i) {
            const QString base = QFileInfo(cands.at(i).toString()).fileName();
            if (wantedNames.contains(base)) {
                if (i != 0)
                    cands.prepend(cands.takeAt(i));
                ownMatch = true;
                ownName  = base;
                break;
            }
        }
        if (!ownMatch) {
            // ANTS-3439 — the checkout-dir leaf need not equal the file's
            // package-name stem (`Fin_Break` vs `finbreak`). Fall back to a
            // normalized comparison so a normalized-equal sibling is treated
            // as this project's own file, not an "other project".
            const QString normLeaf = normalizeFeedbackStem(callerLeaf);
            for (int i = 0; i < cands.size(); ++i) {
                const QString base =
                    QFileInfo(cands.at(i).toString()).fileName();
                if (normalizeFeedbackStem(feedbackStemOf(base)) == normLeaf) {
                    if (i != 0)
                        cands.prepend(cands.takeAt(i));
                    normMatch = true;
                    break;
                }
            }
        }
    }

    e["candidates"] = cands;
    if (ownMatch) {
        e["hint"] = QStringLiteral(
            "no file at that path; this project's own %1 is listed "
            "first under candidates").arg(ownName);
    } else if (normMatch) {
        e["hint"] = QStringLiteral(
            "no file at that path; this project's own feedback file is "
            "listed first under candidates — its basename normalizes to the "
            "caller leaf \"%1\" (the checkout-dir name and the file's "
            "package-name stem differ; ANTS-3439)").arg(callerLeaf);
    } else if (!callerLeaf.isEmpty()) {
        e["all_other_projects"] = true;
        e["hint"] = QStringLiteral(
            "no file at that path; the %1 sibling *_Ants_MCP_Feedback.md "
            "file(s) all belong to OTHER projects (none matches this "
            "project's leaf \"%2\")").arg(cands.size()).arg(callerLeaf);
    } else {
        e["hint"] = QStringLiteral(
            "no file at that path; %1 sibling *_Ants_MCP_Feedback.md "
            "file(s) exist in the same directory — see candidates")
            .arg(cands.size());
    }
    return e;
}

// Resolve + validate the feedback-file `path` arg. On reject, fills
// `err` and returns false. On success, `resolvedOut` is the absolute
// path to read/write (which may not yet exist when `mustExist` is
// false), and `existsOut` says whether it currently exists. When
// `derivedOut` is non-null it reports whether the path was auto-derived
// from caller_cwd (ANTS-3376) rather than supplied by the caller.
bool resolveFeedbackPath(const QJsonObject &req, const QString &toolName,
                         QString &resolvedOut, bool &existsOut,
                         QJsonObject &err, bool *derivedOut) {
    if (derivedOut) *derivedOut = false;
    const QString rawPath = req.value(QStringLiteral("path")).toString();
    if (rawPath.isEmpty()) {
        // ANTS-3376 — no explicit path: derive the conventional
        // `<leaf>_Ants_MCP_Feedback.md` at the shared root (the parent of
        // caller_cwd, where the corpus lives — mcp-feedback-files.md
        // § "File location & name"), mirroring how roadmap_log /
        // changelog_log anchor on caller_cwd. With no resolvable
        // caller_cwd there is nothing to derive from — keep the original
        // bad_args refusal.
        const QString callerRaw =
            req.value(QStringLiteral("caller_cwd")).toString();
        const QString rootCanonical =
            QFileInfo(callerRaw).canonicalFilePath();
        const QString leaf = QFileInfo(rootCanonical).fileName();
        if (rootCanonical.isEmpty() || leaf.isEmpty()) {
            err = fbErr(QStringLiteral("bad_args"),
                        toolName + QStringLiteral(": \"path\" is required "
                        "(no resolvable caller_cwd to derive a default)"));
            return false;
        }
        const QString sharedRoot = QFileInfo(rootCanonical).absolutePath();
        // ANTS-3376 default is "<leaf>_Ants_MCP_Feedback.md". ANTS-3426 — a
        // leaf already ending in "_Ants" doubles the token, so try the
        // de-doubled convention first (see feedbackConventionalNames). Adopt
        // the first candidate that exists; when none exist, default to the
        // most-preferred name so a fresh feedback_log create uses the correct
        // (de-doubled) basename rather than forking history into a doubled one.
        QString derived;
        existsOut = false;
        for (const QString &name : feedbackConventionalNames(leaf)) {
            const QString cand = QDir::cleanPath(
                sharedRoot + QLatin1Char('/') + name);
            if (derived.isEmpty()) derived = cand;   // preferred create-default
            if (QFileInfo::exists(cand)) { derived = cand; existsOut = true; break; }
        }
        resolvedOut = derived;
        if (!existsOut) {
            // ANTS-3439 — the checkout-dir leaf may not equal the feedback
            // file's package-name stem (`Fin_Break` vs `finbreak`). If a
            // sibling's stem normalizes (lowercase, strip non-alphanumerics)
            // to the same token as the leaf, adopt that sibling as the derived
            // default rather than pointing at a name that will never exist.
            const QString normLeaf = normalizeFeedbackStem(leaf);
            const QDir sdir(sharedRoot);
            const QStringList sibs = sdir.entryList(
                {QLatin1String("*") + QLatin1String(kFeedbackSuffix)},
                QDir::Files, QDir::Name);
            for (const QString &n : sibs) {
                if (normalizeFeedbackStem(feedbackStemOf(n)) == normLeaf) {
                    resolvedOut = sdir.absoluteFilePath(n);
                    existsOut   = true;
                    break;
                }
            }
        }
        if (derivedOut) *derivedOut = true;
        return true;
    }
    // Root for the relative case: the caller_cwd canonical dir. Absolute
    // paths bypass it (allowOutsideRoot=true lets the shared-root files,
    // which live outside any project, be accepted). An empty rootCanonical
    // is fine for an absolute path; PathValidation only anchors when
    // allowOutsideRoot=false, which we never pass here.
    const QString callerRaw =
        req.value(QStringLiteral("caller_cwd")).toString();
    const QString rootCanonical =
        QFileInfo(callerRaw).canonicalFilePath();

    const auto check = PathValidation::validatePath(
        rawPath, rootCanonical, toolName, QStringLiteral("path"),
        /*allowOutsideRoot=*/true);
    if (check.bad) { err = check.err; return false; }

    // Determine the path to act on. validatePath leaves `resolved` empty
    // for a non-existent path; reconstruct the would-be absolute path so
    // the writer (feedback_log) can create it. For an absolute rawPath we
    // use it directly; for a relative one we join the caller_cwd root.
    QString candidate = check.resolved;
    existsOut = !candidate.isEmpty();
    if (candidate.isEmpty()) {
        const QFileInfo rawFi(rawPath);
        if (rawFi.isAbsolute()) {
            candidate = QDir::cleanPath(rawPath);
        } else if (!rootCanonical.isEmpty()) {
            candidate = QDir::cleanPath(
                rootCanonical + QLatin1Char('/') + rawPath);
        } else {
            // Relative path with no resolvable caller_cwd root.
            err = fbErr(QStringLiteral("bad_path"),
                        toolName + QStringLiteral(": relative \"path\" "
                        "needs a resolvable caller_cwd"));
            return false;
        }
    }

    // Suffix guard on the resolved/canonical basename (a symlink or `..`
    // that lands on a non-feedback file is rejected — validatePath has
    // already canonicalised existing paths into `check.resolved`).
    const QString base = QFileInfo(candidate).fileName();
    if (!base.endsWith(QLatin1String(kFeedbackSuffix))) {
        err = fbErr(QStringLiteral("not_feedback_file"),
                    toolName + QStringLiteral(": \"path\" basename must "
                    "end in ") + QLatin1String(kFeedbackSuffix));
        return false;
    }

    resolvedOut = candidate;
    return true;
}

}  // namespace rcdetail

// ANTS-1637 — codebase_index: serve a pre-computed project structural map.
// caller_cwd Required. A `file_path` selector routes through PathValidation
// (bad_path on escape, incl. an in-root symlink resolving outside root). The
// disk cache + lazy refresh live in CodebaseIndex::serve.
QJsonDocument RemoteControl::cmdCodebaseIndex(const QJsonObject &req) {
    const QString callerRaw = req.value(QStringLiteral("caller_cwd")).toString();
    const QString sentinelRoot = ants::expandGlobalConfigSentinel(callerRaw);
    const QString rootCanonical =
        !sentinelRoot.isEmpty() ? sentinelRoot
                                : resolveRootCanonical(m_main, req);
    if (rootCanonical.isEmpty()) {
        QJsonObject o;
        o["ok"]    = false;
        o["error"] = QStringLiteral("codebase_index: no focused project");
        o["code"]  = QStringLiteral("bad_path");
        return QJsonDocument(o);
    }

    CodebaseIndex::QueryParams params;
    params.symbol = req.value(QStringLiteral("symbol")).toString();
    params.lane   = req.value(QStringLiteral("lane")).toString();
    // ANTS-3468 — summary-only opt-in: a per-lane source-file digest so the
    // caller (notably the session_orient bundle) gets a navigable map, not
    // just counts. Ignored when a selector is set (query() emits it only in
    // the n==0 summary branch).
    params.laneFiles = req.value(QStringLiteral("lane_files")).toBool();

    // ANTS-2149 — accept `path` as an alias for `file_path`, mirroring the
    // sibling file_outline verb (which keys on `path`). `file_path` stays
    // the source of truth; `path` only fills in when `file_path` is absent.
    QString rawFilePath = req.value(QStringLiteral("file_path")).toString();
    if (rawFilePath.isEmpty())
        rawFilePath = req.value(QStringLiteral("path")).toString();
    if (!rawFilePath.isEmpty()) {
        const auto check = PathValidation::validatePath(
            rawFilePath, rootCanonical,
            QStringLiteral("codebase_index"), QStringLiteral("file_path"));
        if (check.bad) return QJsonDocument(check.err);
        // Project-relative form for the index lookup (empty .resolved = a
        // not-yet-existing in-root path, still a valid soft-miss query).
        QString rel = check.resolved.isEmpty() ? rawFilePath : check.resolved;
        if (rel.startsWith(rootCanonical))
            rel = rel.mid(rootCanonical.size()).startsWith(QLatin1Char('/'))
                      ? rel.mid(rootCanonical.size() + 1)
                      : rel.mid(rootCanonical.size());
        params.filePath = rel;
    }

    return QJsonDocument(
        CodebaseIndex::serve(rootCanonical, QDateTime::currentMSecsSinceEpoch(),
                             params));
}

