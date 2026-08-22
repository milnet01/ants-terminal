// ANTS-3833 TU 6/16 — Workspace and code index verbs.
#include "remotecontrol.h"
#include "mutationprobe.h"   // ANTS-4398
#include "remotecontrol_internal.h"
#include "wrapmatch.h"   // ANTS-4547 — the wrapped-quotation matching rule
#include "buildtargets.h"        // ANTS-3745 — build_target_for engine
#include "fileoutline.h"
#include "readlog.h"
#include "readregion.h"
#include "mcpspill.h"        // ANTS-2094 — read_spill
#include "applyedits.h"
#include "codebaseindex.h"
#include "cochangefamily.h"   // ANTS-3368 — co_change_family pure seam
#include "claudeintegration.h"
#include "mainwindow.h"
#include "pathvalidation.h"
#include "projectsettings.h"   // ANTS-3716 — cited_by's default scope
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
#include <QMap>
#include <QSet>
#include <algorithm>

using namespace rcdetail;  // ANTS-3833

namespace {

// ANTS-3716 § 2.4 — the ONE rg call site in this TU (INV-9). Runs a ripgrep
// invocation to completion — start, wall budget, terminate/kill grace, stderr
// cap — and returns the raw result.
//
// It classifies NOTHING, and that is not style. `workspace_search`'s
// classification reads its PARSED matches, which only exist after the caller
// has parsed the stdout returned here, so it cannot move into a process runner
// even in principle. And the two verbs classify differently on purpose:
// `workspace_search` treats three of its four failure branches as failures only
// when no matches were parsed, while `cited_by` refuses on any failed run,
// because a partial cell set is indistinguishable from a complete one.
struct RgRun {
    QByteArray stdoutBytes;
    QByteArray stderrTail;        // capped at kWorkspaceSearchStderrCapBytes
    int  exitCode    = 0;
    bool startFailed = false;
    bool crashed     = false;     // abnormal exit that was not our own kill
    bool hardKilled  = false;
};

RgRun rcRunRg(const QStringList &argv, const QString &workingDir, int budgetMs) {
    RgRun out;
    QProcess rg;
    rg.setWorkingDirectory(workingDir);
    rg.setProcessChannelMode(QProcess::SeparateChannels);
    // ANTS-1248-INV-3: QProcess::start(QString, QStringList) — argv
    // form. No shell, no single-string overload.
    rg.start(QStringLiteral("rg"), argv);
    if (!rg.waitForStarted(500)) {
        out.startFailed = true;
        return out;
    }

    // ANTS-1248-INV-5: hard kill via budgetMs. waitForFinished returns false
    // on timeout. On timeout we terminate(), then grant 200 ms grace, then
    // kill().
    if (!rg.waitForFinished(budgetMs)) {
        out.hardKilled = true;
        rg.terminate();
        if (!rg.waitForFinished(kWorkspaceSearchKillGraceMs)) {
            rg.kill();
            rg.waitForFinished(kWorkspaceSearchKillGraceMs);
        }
    }

    // ANTS-1248-INV-8: stderr cap. Read up to 4 KiB; the CALLER decides
    // whether to emit it — emitting on success leaks path enumeration.
    out.stderrTail = rg.readAllStandardError();
    if (out.stderrTail.size() > kWorkspaceSearchStderrCapBytes)
        out.stderrTail.truncate(kWorkspaceSearchStderrCapBytes);
    out.stdoutBytes = rg.readAllStandardOutput();
    out.exitCode    = rg.exitCode();
    out.crashed     = (rg.exitStatus() != QProcess::NormalExit) && !out.hardKilled;
    return out;
}

}  // namespace

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

    // ANTS-4547 — wrapped-quotation matching. Prose here is hard-wrapped at
    // ~70 columns, so a quotation of ordinary sentence length spans a break
    // and a line-oriented search misses text that is present and exact. For a
    // review gate that DISMISSES a finding whose quote cannot be located, that
    // miss is indistinguishable from a defect and the real defect ships —
    // measured by the reporter at 18% of quotations. The rule itself lives in
    // WrapMatch so this verb and roadmap_log op:"amend_body" cannot answer one
    // quotation differently.
    //
    // Off by default, and LITERAL only: re-flowing a caller's regex would
    // change what their own pattern means, so that combination refuses rather
    // than surprising them.
    const bool matchWrapped =
        req.value(QStringLiteral("match_wrapped")).toBool(false);
    if (matchWrapped && isRegex) {
        QJsonObject e = wsErr("bad_args",
            QStringLiteral("workspace-search: \"match_wrapped\" applies to a "
                           "literal pattern — it re-flows whitespace, which "
                           "would change what a regex means"));
        e["hint"] = QStringLiteral(
            "drop `regex:true`, or write the wrap into the pattern yourself "
            "(e.g. `\\s+` between words with `--multiline` semantics).");
        return QJsonDocument(e);
    }
    // The pattern handed to rg: the literal itself, or — under
    // match_wrapped — the same literal with every whitespace run widened to
    // "whitespace, plus any markdown blockquote markers that follow it".
    QString rgPattern = pattern;
    if (matchWrapped) {
        rgPattern = WrapMatch::toRegex(pattern);
        if (rgPattern.isEmpty()) {
            return QJsonDocument(wsErr("bad_pattern",
                QStringLiteral("workspace-search: \"pattern\" holds no text "
                               "to match once its whitespace and blockquote "
                               "markers are folded")));
        }
    }

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
    // ANTS-4388 — matches_only: the distinct MATCHED SUBSTRINGS, not rows.
    // Every other trim here is row-shaped — count_only drops rows, files_only
    // gives one row per file, headline_only thins rows, max_match_bytes
    // shortens rows — so "what is the SET of X in this tree" had no answer.
    // `dedup` looks like it and is not: its key is the whitespace-normalised
    // LINE, so two lines carrying the same token stay two rows and one line
    // carrying two different tokens stays one.
    //
    // Not a duplicate of count_only either, which counts occurrences of ONE
    // pattern; this asks which distinct strings that pattern matched.
    const bool matchesOnly =
        req.value(QStringLiteral("matches_only")).toBool(false);
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
    // QString in the argv list; rcRunRg() above starts rg with the
    // two-argument start() overload and no shell.
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
    // ANTS-4547 — the wrapped form IS a regex (WrapMatch::toRegex), and it
    // must be allowed to cross a line boundary, which is rg's --multiline.
    if (matchWrapped) argv << QStringLiteral("--multiline");
    else if (!isRegex) argv << QStringLiteral("--fixed-strings");
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
    argv << QStringLiteral("--") << rgPattern << laneAbs;

    // ANTS-3716 § 2.4 — the process run (start, budget, hard kill, stderr cap)
    // moved to rcRunRg() so `cited_by` shares one rg call site (INV-9). The
    // classification below stays here: it reads the parsed `matches`, which do
    // not exist until this handler has parsed the stdout the helper returned.
    const RgRun run = rcRunRg(argv, rootCanonical, budgetMs);
    if (run.startFailed) {
        return QJsonDocument(wsErr("rg_failed",
            QStringLiteral("workspace-search: rg failed to start (is ripgrep installed?)")));
    }
    const bool hardKilled          = run.hardKilled;
    const QByteArray &stderrTail   = run.stderrTail;
    const QByteArray &stdoutBytes  = run.stdoutBytes;

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
    // ANTS-4388 — matches_only capture. First-appearance order, so the reply
    // is deterministic and reads like the scan.
    QStringList        distinctOrder;
    QHash<QString,int> distinctIndex;
    QList<int>         distinctHits;
    QList<QSet<QString>> distinctFiles;
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
        // ANTS-4388 — harvest the distinct matched substrings. rg --json
        // already carries each match's own text in data.submatches[].match,
        // so no second scan and no `-o` invocation. Placed BEFORE the
        // max_results cap on purpose: that cap counts ROWS, and the whole
        // defect is that a 3-string answer was truncated at 15 rows with no
        // way to tell whether a fourth existed past the cut. `count` and
        // `files_count` therefore come from the full uncapped scan, as
        // count_only's `count` already does.
        if (matchesOnly) {
            const QJsonObject mdata = ev.value("data").toObject();
            QString mpath =
                mdata.value("path").toObject().value("text").toString();
            if (mpath.startsWith(rootCanonical + QLatin1Char('/')))
                mpath = mpath.mid(rootCanonical.size() + 1);
            const QJsonArray subs = mdata.value("submatches").toArray();
            for (const QJsonValue &sv : subs) {
                const QString mtext =
                    sv.toObject().value("match").toObject()
                      .value("text").toString();
                if (mtext.isEmpty()) continue;
                int idx = distinctIndex.value(mtext, -1);
                if (idx < 0) {
                    idx = distinctOrder.size();
                    distinctIndex.insert(mtext, idx);
                    distinctOrder.append(mtext);
                    distinctHits.append(0);
                    distinctFiles.append(QSet<QString>());
                }
                ++distinctHits[idx];
                distinctFiles[idx].insert(mpath);
            }
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
        // ANTS-4547 — a --multiline hit's `text` is the WHOLE matched span,
        // line breaks and continuation indent included. A row is one line by
        // contract (every consumer, dedup and the clip included, assumes it),
        // so fold it the same way the match itself was folded. `line` still
        // reports where the span STARTS, which is the question the caller
        // asked.
        if (matchWrapped) text = text.simplified();

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

    if (run.crashed) {
        QJsonObject o = wsErr("rg_failed",
            QStringLiteral("workspace-search: rg crashed (exit status not normal)"));
        if (!stderrTail.isEmpty()) o["stderr"] = QString::fromUtf8(stderrTail);
        return QJsonDocument(o);
    }
    // rg exit codes: 0 = matches found, 1 = no matches (still ok),
    // 2 = error. Anything ≥ 2 (or hard-kill) is treated as failure
    // only when no matches were parsed.
    if (run.exitCode >= 2 && matches.isEmpty() && !hardKilled) {
        QJsonObject o = wsErr("rg_failed",
            QStringLiteral("workspace-search: rg returned non-zero exit code %1")
                .arg(run.exitCode));
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
    // ANTS-4388 — matches_only: the DISTINCT matched substrings. Placed
    // beside count_only for the same reason (no rows to dedup, clip or cap),
    // and before it in precedence only where both are absent — count_only
    // stays the leanest and wins if both are set, as files_only already does.
    if (matchesOnly && !countOnly) {
        QJsonArray distinct;
        // The cap applies to DISTINCT VALUES, not occurrences. That is the
        // property the whole mode exists for: a search for `{{[A-Z_]+}}`
        // returned 15 rows and truncated:true where the true answer was three
        // strings, and a caller stopping at the default had no way to know
        // whether a fourth existed past the cut — which is how a placeholder
        // missed at scaffold time ships into a new project's README.
        // `emitCount`, not `emit` — the latter is a Qt keyword.
        const int emitCount =
            qMin(static_cast<int>(distinctOrder.size()), maxResults);
        for (int i = 0; i < emitCount; ++i) {
            QJsonObject m;
            m["text"]        = distinctOrder.at(i);
            m["count"]       = distinctHits.at(i);
            m["files_count"] = static_cast<int>(distinctFiles.at(i).size());
            distinct.append(m);
        }
        QJsonObject out;
        out["ok"]             = true;
        out["pattern"]        = pattern;
        out["matches"]        = distinct;
        out["distinct_count"] = static_cast<int>(distinctOrder.size());
        out["count"]          = seenMatchEvents;
        out["files_count"]    = filesWithMatches;
        out["matches_only"]   = true;
        // Truncated means the ANSWER is partial — the distinct set exceeded
        // max_results, or the scan itself was cut off. Not merely that many
        // occurrences were seen, which is the normal case here.
        out["truncated"] = (distinctOrder.size() > emitCount) ||
                           hardKilled || parseBudgetExceeded;
        out["respect_gitignore"] = respect_gitignore;
        out["include_hidden"]    = include_hidden;
        out["timeout_sec"]       = budgetSec;
        out["elapsed_ms"]        = static_cast<int>(wall.elapsed());
        return QJsonDocument(out);
    }

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
    // ANTS-4420 — ordered FIRST among the zero-match hints, and deliberately.
    // An entity-bearing pattern is the most specific diagnosis available: when
    // one also carries whitespace, the entity is the actual defect and the
    // phrase advisory below would send the caller to fix the wrong thing. The
    // chain still sets `hint` at most once.
    //
    // The machinery this uses already exists and already works — the same
    // session that reported this filed a POSITIVE report that `regex_advisory`
    // caught a short-bare-alternation trap unprompted, and cited it as
    // precedent for exactly this case. Same channel, same trigger shape: a
    // pattern that is probably not what the caller meant.
    if (matches.isEmpty() && rcContainsHtmlEntity(pattern)) {
        out["hint"] = QStringLiteral(
            "pattern contains HTML entities (e.g. &lt; &gt; &amp;) — did you "
            "mean the literal characters (< > & \") ? Searching HTML/XML "
            "source is where this slip happens, and a zero-match reply is the "
            "one case with no other signal to re-examine the pattern.");
    }
    else if (matches.isEmpty() && pattern.trimmed().contains(QChar(' '))) {
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
    // ANTS-4547 — activation-gated: a caller reading zero matches needs to
    // know whether the wrapped rule was in force.
    if (matchWrapped) out["match_wrapped"] = true;
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

// ───────────────────────────── ANTS-3716 — cited_by ──────────────────────────

namespace {

// § 2.1 — argv and run count both stay bounded by this.
constexpr int kCitedByMaxAnchors      = 64;
constexpr int kCitedByDefaultMaxCells = 500;
constexpr int kCitedByMaxCellsCap     = 5000;
// § 4 — the pathological-case guard, 100× the default `max_cells`. Past it
// cell COLLECTION stops and every run is still drained to completion: the
// matched-anchor set is 64 booleans and the matching-file set is a path set,
// and both are what INV-2 and INV-8 promise are computed over every run.
constexpr int kCitedByCellCeiling     = 50000;

struct CitedByCell {
    int count     = 0;   // OCCURRENCES (summed submatches), not matching lines
    int firstLine = 0;
};

}  // namespace

// ANTS-3716 — `cited_by`: one call that says which document cites which anchor.
// Spec: docs/specs/ANTS-3716-cited-by-sweep.md
//
// The post-fix sweep a review loop runs by hand: given the anchors a loop
// changed, which documents mention any of them? It is a pure search being
// performed as reasoning, N round-trips wide, four times per loop. Here it is
// one call and one response. The verdicts stay with the caller — this verb
// produces the grid they are recorded against.
QJsonDocument RemoteControl::cmdCitedBy(const QJsonObject &req) {
    QElapsedTimer wall;
    wall.start();

    // Root resolution mirrors cmdWorkspaceSearch's, ANTS-1390's `~global`
    // sentinel included — the documents this verb sweeps routinely live under
    // ~/.claude. Deliberately duplicated rather than hoisted into a shared
    // helper: ANTS-1390's INV-4 scrapes for the sentinel call *inside*
    // cmdWorkspaceSearch's body, so extracting it would redden that test.
    const QString callerRaw = req.value(QStringLiteral("caller_cwd")).toString();
    QString rootCwd;
    const QString sentinelRoot = ants::expandGlobalConfigSentinel(callerRaw);
    if (!sentinelRoot.isEmpty()) {
        rootCwd = sentinelRoot;
    } else if (!callerRaw.isEmpty()) {
        rootCwd = callerRaw;
    } else if (m_main) {
        if (auto *t = m_main->currentTerminal()) rootCwd = t->shellCwd();
    }
    if (rootCwd.isEmpty()) rootCwd = QDir::currentPath();
    const QString rootCanonical = QFileInfo(rootCwd).canonicalFilePath();
    if (rootCanonical.isEmpty()) {
        return QJsonDocument(wsErr("bad_path",
            QStringLiteral("cited_by: project root \"%1\" does not exist").arg(rootCwd)));
    }

    // § 2.1 — anchors: 1…64 literal strings, none empty (INV-6, INV-13). The
    // empty-string rejection is not pedantry: measured, `rg -F -e ''` matches
    // at every byte position — 25 submatches from a 24-character file — so one
    // empty anchor saturates the collection ceiling with junk and starves
    // every real anchor of the cap.
    const QJsonValue anchorsVal = req.value(QStringLiteral("anchors"));
    QStringList anchors;
    if (anchorsVal.isArray()) {
        const QJsonArray arr = anchorsVal.toArray();
        anchors.reserve(arr.size());
        for (const QJsonValue &v : arr) anchors << v.toString();
    }
    if (!anchorsVal.isArray() || anchors.isEmpty() ||
        anchors.size() > kCitedByMaxAnchors) {
        return QJsonDocument(wsErr("bad_args",
            QStringLiteral("cited_by: \"anchors\" must be an array of 1-%1 "
                           "non-empty strings").arg(kCitedByMaxAnchors)));
    }
    if (anchors.contains(QString())) {
        return QJsonDocument(wsErr("bad_args",
            QStringLiteral("cited_by: \"anchors\" contains an empty string, which "
                           "matches at every byte position and would crowd out "
                           "every real anchor")));
    }

    // § 2.2 — `insensitive` by default and NO `smart` mode. rg resolves
    // --smart-case over the COMBINED pattern set, so one anchor carrying a
    // capital silently flips every other anchor in the request to
    // case-sensitive. A sweep whose results depend on the company an anchor
    // keeps is worse than no sweep, because it looks complete.
    const QString caseMode =
        req.value(QStringLiteral("case")).toString(QStringLiteral("insensitive"));
    if (caseMode != QLatin1String("insensitive") &&
        caseMode != QLatin1String("sensitive")) {
        return QJsonDocument(wsErr("bad_args",
            QStringLiteral("cited_by: \"case\" must be \"insensitive\" (default) or "
                           "\"sensitive\"; \"smart\" is deliberately absent because "
                           "rg resolves it over the whole pattern set")));
    }

    int maxCells = kCitedByDefaultMaxCells;
    const QJsonValue cellsVal = req.value(QStringLiteral("max_cells"));
    if (cellsVal.isDouble()) {
        maxCells = std::clamp(cellsVal.toInt(), 1, kCitedByMaxCellsCap);
    }

    // § 2.3 — the budget is for the WHOLE set of runs, not per run.
    int budgetMs = kWorkspaceSearchHardKillMs;
    const QJsonValue tsVal = req.value(QStringLiteral("timeout_sec"));
    if (tsVal.isDouble()) {
        const int requestedMs = tsVal.toInt() * 1000;
        if (requestedMs >= kWorkspaceSearchMinBudgetMs &&
            requestedMs <= kWorkspaceSearchMaxBudgetMs) {
            budgetMs = requestedMs;
        }
    }
    const int budgetSec = budgetMs / 1000;

    // § 2.1 / § 2.5 — scope. Defaults to the three targets the post-fix sweep
    // names; every entry, defaulted or supplied, is validated and then STAT'd,
    // because PathValidation::validatePath does not require existence and rg
    // exits 2 on a path that is not there.
    QStringList scopeRaw;
    const QJsonValue scopeVal = req.value(QStringLiteral("scope"));
    if (scopeVal.isArray()) {
        for (const QJsonValue &v : scopeVal.toArray())
            if (v.isString() && !v.toString().isEmpty()) scopeRaw << v.toString();
    } else {
        scopeRaw << ProjectSettings::load(rootCanonical).docsDir
                        .value_or(QStringLiteral("docs"))
                 << QStringLiteral("README.md")
                 << QStringLiteral("CLAUDE.md");
    }

    QStringList scopeRel, scopeArgv, scopeCanon;
    for (const QString &raw : std::as_const(scopeRaw)) {
        const auto check = PathValidation::validatePath(
            raw, rootCanonical, QStringLiteral("cited_by"),
            QStringLiteral("scope"));
        if (check.bad) return QJsonDocument(check.err);
        if (check.resolved.isEmpty()) continue;   // § 2.5 — pruned, not passed to rg
        scopeRel   << raw;
        scopeArgv  << check.argvForm;
        scopeCanon << check.resolved;
    }
    // § 2.5 — de-overlap. rg searches a file once per positional path that
    // reaches it and does not de-duplicate, so scope:["docs","docs/specs"]
    // would DOUBLE that pair's `count` — a wire value the consumer reads as
    // occurrences (INV-12).
    QStringList keptRel, keptArgv;
    for (int i = 0; i < scopeCanon.size(); ++i) {
        bool contained = false;
        for (int j = 0; j < scopeCanon.size() && !contained; ++j) {
            if (i == j) continue;
            const QString &a = scopeCanon.at(i);
            const QString &b = scopeCanon.at(j);
            if (a == b) contained = (j < i);      // exact repeat: keep the first
            else contained = a.startsWith(b + QLatin1Char('/'));
        }
        if (!contained) { keptRel << scopeRel.at(i); keptArgv << scopeArgv.at(i); }
    }

    // § 2.3 — one rg run per anchor, in sorted anchor order, so every match in
    // a run belongs to that anchor and there is no attribution step. Repeats
    // are dropped from the RUN list only: two runs of one anchor would tally
    // into the same (anchor, file) key and double its count, the same defect
    // the scope de-overlap above prevents. The request's own array still backs
    // the matched / unmatched partition, so a caller that sent a repeat sees it
    // in both places it sent it.
    QStringList runAnchors = anchors;
    std::sort(runAnchors.begin(), runAnchors.end());
    runAnchors.removeDuplicates();

    // QMap, not QHash: its key order IS § 2.6's required (anchor, file) sort,
    // which the ETag depends on — rg's own event order is not stable across
    // runs at --threads 4.
    QMap<QString, QMap<QString, CitedByCell>> byAnchor;
    QSet<QString> matchedAnchors, matchingFiles;
    int  collectedCells = 0;
    bool truncated      = false;

    // Same shape as cmdWorkspaceSearch's parse budget (ANTS-3405): rg's budget
    // plus an equal parse budget, checked every 2048 events.
    const qint64 totalBudgetMs = static_cast<qint64>(budgetMs) * 2;
    int scanCounter = 0;

    for (const QString &anchor : std::as_const(runAnchors)) {
        if (keptArgv.isEmpty()) break;   // nothing survived scope pruning

        QStringList argv;
        argv << QStringLiteral("--json")
             << QStringLiteral("--no-heading")
             << QStringLiteral("--line-number")
             << QStringLiteral("--threads") << QString::number(kWorkspaceSearchThreads)
             // INV-3 — literal, and passed UNESCAPED. QRegularExpression::escape
             // backslashes every non-[A-Za-z0-9_] character, non-ASCII included,
             // and rg's regex engine rejects a backslash before a non-ASCII
             // character (exit 2) — so an accented anchor would abort its own run.
             << QStringLiteral("--fixed-strings")
             << (caseMode == QLatin1String("sensitive")
                     ? QStringLiteral("--case-sensitive")
                     : QStringLiteral("--ignore-case"))
             // `--` first, so an anchor like `--old-flag` is a pattern and not a flag.
             << QStringLiteral("--") << anchor;
        argv += keptArgv;

        const int remainingMs = budgetMs - static_cast<int>(wall.elapsed());
        const RgRun run = rcRunRg(argv, rootCanonical, qMax(1, remainingMs));

        // INV-10 — ANY failed run refuses, and cells tallied from earlier
        // anchors are discarded. This drops the `matches.isEmpty()` guard
        // cmdWorkspaceSearch applies to three of its four branches: there, a
        // partial result with a swallowed error is a defensible trade; here it
        // is indistinguishable from a complete sweep, and every anchor whose
        // run had not happened yet would be reported uncited.
        if (run.startFailed) {
            return QJsonDocument(wsErr("rg_failed",
                QStringLiteral("cited_by: rg failed to start (is ripgrep installed?)")));
        }
        if (run.crashed) {
            QJsonObject o = wsErr("rg_failed",
                QStringLiteral("cited_by: rg crashed (exit status not normal) on "
                               "anchor \"%1\"").arg(anchor));
            if (!run.stderrTail.isEmpty()) o["stderr"] = QString::fromUtf8(run.stderrTail);
            return QJsonDocument(o);
        }
        if (run.hardKilled) {
            QJsonObject o = wsErr("rg_failed",
                QStringLiteral("cited_by: exceeded the %1 s wall budget for the whole "
                               "anchor set, hard-killed on anchor \"%2\"")
                    .arg(budgetSec).arg(anchor));
            o["timeout_sec"] = budgetSec;
            o["hint"] = QStringLiteral(
                "narrow `scope`, send fewer anchors, or raise timeout_sec (max 30)");
            return QJsonDocument(o);
        }
        // rg exit codes: 0 = matches, 1 = no matches (still fine), >= 2 = error.
        if (run.exitCode >= 2) {
            QJsonObject o = wsErr("rg_failed",
                QStringLiteral("cited_by: rg returned exit code %1 on anchor \"%2\"")
                    .arg(run.exitCode).arg(anchor));
            if (!run.stderrTail.isEmpty()) o["stderr"] = QString::fromUtf8(run.stderrTail);
            return QJsonDocument(o);
        }

        const QList<QByteArray> lines = run.stdoutBytes.split('\n');
        for (const QByteArray &line : lines) {
            if (line.isEmpty()) continue;
            if ((++scanCounter & 0x7FF) == 0 && wall.hasExpired(totalBudgetMs)) {
                QJsonObject o = wsErr("rg_failed",
                    QStringLiteral("cited_by: match stream too large to parse within "
                                   "the %1 s wall budget").arg(budgetSec));
                o["timeout_sec"] = budgetSec;
                o["hint"] = QStringLiteral(
                    "narrow `scope`, send fewer anchors, or raise timeout_sec (max 30)");
                return QJsonDocument(o);
            }
            QJsonParseError perr{};
            const QJsonDocument doc = QJsonDocument::fromJson(line, &perr);
            if (perr.error != QJsonParseError::NoError || !doc.isObject()) continue;
            const QJsonObject ev = doc.object();
            if (ev.value(QStringLiteral("type")).toString() != QLatin1String("match"))
                continue;

            const QJsonObject data = ev.value(QStringLiteral("data")).toObject();
            QString path = data.value(QStringLiteral("path")).toObject()
                               .value(QStringLiteral("text")).toString();
            if (path.startsWith(rootCanonical + QLatin1Char('/')))
                path = path.mid(rootCanonical.size() + 1);
            const int lineNo = data.value(QStringLiteral("line_number")).toInt();
            // § 2.6 — `count` is OCCURRENCES: the summed length of submatches[],
            // so an anchor named twice on one line counts 2. The two readings
            // are one `++` apart and the field name does not disambiguate them.
            const int hits =
                qMax(1, data.value(QStringLiteral("submatches")).toArray().size());

            // Before the ceiling check, on purpose (INV-2 / INV-8): these two
            // are what survive a truncated collection.
            matchedAnchors.insert(anchor);
            matchingFiles.insert(path);

            QMap<QString, CitedByCell> &files = byAnchor[anchor];
            const auto it = files.find(path);
            if (it == files.end()) {
                if (collectedCells >= kCitedByCellCeiling) { truncated = true; continue; }
                files.insert(path, CitedByCell{hits, lineNo});
                ++collectedCells;
            } else {
                it->count += hits;
                if (lineNo < it->firstLine) it->firstLine = lineNo;
            }
        }
    }

    // § 2.6 — the cap is applied AFTER the sort, never during a run: truncating
    // while parsing would retain a run-dependent subset that sorting cannot
    // restore to byte-identity. QMap's iteration order is the sort.
    QJsonArray cells;
    int totalCells = 0;
    for (auto ai = byAnchor.cbegin(); ai != byAnchor.cend(); ++ai) {
        for (auto fi = ai.value().cbegin(); fi != ai.value().cend(); ++fi) {
            ++totalCells;
            if (cells.size() >= maxCells) continue;
            QJsonObject c;
            c["anchor"]     = ai.key();
            c["file"]       = fi.key();
            c["count"]      = fi.value().count;
            c["first_line"] = fi.value().firstLine;
            cells.append(c);
        }
    }
    if (totalCells > cells.size()) truncated = true;

    // INV-2 — the two arrays partition `anchors` exactly, in REQUEST order, and
    // are computed over every run, so they hold when `truncated` is true.
    QJsonArray matchedArr, unmatchedArr;
    for (const QString &a : std::as_const(anchors)) {
        if (matchedAnchors.contains(a)) matchedArr.append(a);
        else                            unmatchedArr.append(a);
    }

    QJsonObject out;
    out["ok"]                = true;
    out["cells"]             = cells;
    out["cells_count"]       = cells.size();          // the CAPPED length
    out["anchors_matched"]   = matchedArr;
    out["anchors_unmatched"] = unmatchedArr;
    out["files_count"]       = matchingFiles.size();  // UNCAPPED (INV-8)
    out["scope_resolved"]    = QJsonArray::fromStringList(keptRel);
    out["truncated"]         = truncated;
    // No `elapsed_ms`, deliberately, where cmdWorkspaceSearch carries one: this
    // verb opts into the CENTRAL ETag, whose hash covers the whole envelope, so
    // one per-run measurement would differ on every call and the 304 could never
    // fire (mcp-tools.md step 7's ANTS-4108 case).
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
                                  int maxSymbols, int maxBytes,
                                  bool withSizes = false,
                                  int maxHeadingLevel = 0,
                                  const QString &filter = QString()) {
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
    // ANTS-3839 — when a name filter is in play the engine must see the WHOLE
    // file. `maxSymbols` is enforced during collection, so asking for the
    // caller's budget and filtering afterwards would search only the first
    // 200 symbols and silently report no match for one at line 900 — a
    // confident wrong answer, which is worse than the empty envelope this
    // feature exists to improve on. Ask for the engine's hard cap instead and
    // apply the caller's budget to the FILTERED set below.
    const int computeBudget =
        filter.isEmpty() ? maxSymbols : FileOutline::kMaxSymbolsCap;
    QJsonObject result = FileOutline::compute(check.resolved, mode,
                                              includeDoc, computeBudget,
                                              withSizes, maxHeadingLevel);
    // ANTS-3839 — `filter=<substr>`: keep only symbols whose NAME contains
    // the substring. The `leaner_call_hint` had advertised this argument since
    // ANTS-1720 while the schema exposed no such property, so the nudge was
    // unactionable and — because its gate was `!args.contains("filter")` —
    // fired on every single call, since no caller could ever satisfy it.
    //
    // Applied BEFORE the byte cap below, which is the point: the cap trims
    // from the tail, so filtering afterwards would pay the full payload and
    // then discard it.
    //
    // Case-INSENSITIVE, deliberately. A case-sensitive symbol filter silently
    // returns nothing for `outline` against `FileOutline`, and a silent empty
    // result is the failure mode ANTS-4374 exists to stop.
    if (result.value("ok").toBool() && !filter.isEmpty()) {
        const QJsonArray all = result.value(QStringLiteral("symbols")).toArray();
        QJsonArray kept;
        for (const QJsonValue &v : all) {
            if (v.toObject().value(QStringLiteral("name")).toString()
                    .contains(filter, Qt::CaseInsensitive))
                kept.append(v);
        }
        // Now apply the caller's own budget to the filtered set, and say so
        // if it bit — otherwise a capped list reads as the complete set of
        // matches, which is the "cap with no flag" failure.
        if (kept.size() > maxSymbols) {
            // ANTS-4469 — same rule as the engine's own cap: say how many.
            // Here the drop is against the FILTERED set, so it is not the same
            // number as symbols_filtered_out below and both are needed.
            const int dropped = kept.size() - maxSymbols;
            while (kept.size() > maxSymbols) kept.removeLast();
            result[QStringLiteral("truncated")]       = true;
            result[QStringLiteral("symbols_dropped")] = dropped;
        }
        result[QStringLiteral("symbols")] = kept;
        // ANTS-4374's invariant — a zero has to say what was looked at.
        // Without these three fields "no symbol matches `foo`" and "this file
        // has no symbols" are the same envelope, and a caller reads the second
        // as "nothing here" and stops looking.
        result[QStringLiteral("filter")]               = filter;
        result[QStringLiteral("symbols_considered")]   = all.size();
        result[QStringLiteral("symbols_filtered_out")] = all.size() - kept.size();
    }

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
    // ANTS-4384 — opt-in per-symbol extents (uniform across a batch).
    const bool withSizes  = req.value(QStringLiteral("sizes")).toBool(false);
    // ANTS-4396 — md heading-depth filter (0 = off). Clamped to the syntax's
    // own range; a value outside it means "no filter" rather than "no
    // headings", since the latter would return an empty outline for a typo.
    int maxHeadingLevel =
        req.value(QStringLiteral("max_heading_level")).toInt(0);
    if (maxHeadingLevel < 0 || maxHeadingLevel > 6) maxHeadingLevel = 0;
    // ANTS-3839 — symbol-name substring filter, uniform across a batch like
    // every other option here. Parsed once so the `paths` form cannot drift
    // from the single-`path` form.
    const QString symbolFilter =
        req.value(QStringLiteral("filter")).toString();

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
                maxSymbols, maxBytes, withSizes, maxHeadingLevel,
                symbolFilter);
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
        // ANTS-4349 — `ok` in the batch form means "the call was well-formed",
        // and it stays that way: a partial hit IS a success, and a batch verb
        // that refuses on any miss is unusable for the "outline whatever
        // exists" case this form was added for. What was missing is a signal a
        // caller can branch on — with two absent paths the reply was
        // {count:2, ok:true, files:[{ok:false},{ok:false}]}, so a caller
        // reading the documented success flag saw a TOTAL miss as a success and
        // went on to reason about an empty symbol set. The per-file `ok`s
        // carried the truth one level down, where nobody looks.
        int found = 0;
        for (const QJsonValue &fv : std::as_const(files))
            if (fv.toObject().value("ok").toBool()) ++found;
        QJsonObject out;
        out["ok"]            = true;
        out["files"]         = files;
        out["count"]         = files.size();
        out["files_found"]   = found;
        out["files_missing"] = files.size() - found;
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
                                        includeDoc, maxSymbols, maxBytes,
                                        withSizes, maxHeadingLevel,
                                        symbolFilter));
}

// ANTS-4398 — mutation_probe: apply a mutation, run a test selector, restore.
//
// The guarantees are the reason this is a verb rather than a documented bash
// recipe. A shell loop can do the mechanics; it cannot promise them.
//
//  * An INERT mutation gets its own outcome, never "survived". See
//    mutationprobe.h — this is the field the whole verb is for.
//  * The restore is guaranteed, INCLUDING on a failed or timed-out run. The
//    hand-rolled version leaks a mutated source file when the run is
//    interrupted, which is genuinely dangerous in a repo the session then
//    commits. `restored_clean` is verified against the baseline bytes, not
//    assumed.
//  * Each mutation runs against a CLEAN baseline, so two cannot compound.
//  * Per-mutation pass/fail counts, so "died for the wrong reason" (a
//    collection error rather than the target assertion) is visible.
//
// `test_command` is an ARGV ARRAY, never a shell string. That is a deliberate
// narrowing: this verb writes to a source file and spawns a process, and a
// shell string would make it an arbitrary-command surface reachable from a
// tool call. Everything a test selector needs is expressible as argv.
QJsonDocument RemoteControl::cmdMutationProbe(const QJsonObject &req) {
    auto mpErr = [](const QString &code, const QString &message) {
        QJsonObject o;
        o[QStringLiteral("ok")]    = false;
        o[QStringLiteral("code")]  = code;
        o[QStringLiteral("error")] = message;
        return QJsonDocument(o);
    };

    const QString rootCanonical = resolveRootCanonical(m_main, req);
    if (rootCanonical.isEmpty())
        return mpErr(QStringLiteral("bad_path"),
                     QStringLiteral("mutation_probe: no focused project"));

    const QString rawPath = req.value(QStringLiteral("path")).toString();
    if (rawPath.isEmpty())
        return mpErr(QStringLiteral("bad_args"),
                     QStringLiteral("mutation_probe: `path` is required"));
    const auto check = PathValidation::validatePath(
        rawPath, rootCanonical, QStringLiteral("mutation_probe"),
        QStringLiteral("path"));
    if (check.bad) return QJsonDocument(check.err);
    if (check.resolved.isEmpty())
        return mpErr(QStringLiteral("not_found"),
                     QStringLiteral("mutation_probe: \"%1\" does not exist")
                         .arg(rawPath));

    QStringList argv;
    for (const QJsonValue &v :
             req.value(QStringLiteral("test_command")).toArray())
        argv.append(v.toString());
    if (argv.isEmpty())
        return mpErr(QStringLiteral("bad_args"),
            QStringLiteral("mutation_probe: `test_command` is required and is "
                           "an ARGV ARRAY, not a shell string — e.g. "
                           "[\"pytest\", \"-k\", \"test_supervisor\"]. A shell "
                           "string is refused on purpose: this verb writes to "
                           "a source file and spawns a process."));

    const QJsonArray muts = req.value(QStringLiteral("mutations")).toArray();
    if (muts.isEmpty())
        return mpErr(QStringLiteral("bad_args"),
            QStringLiteral("mutation_probe: `mutations` must hold at least "
                           "one {label, old, new}"));
    if (muts.size() > 50)
        return mpErr(QStringLiteral("bad_args"),
            QStringLiteral("mutation_probe: %1 mutations exceeds the 50 cap — "
                           "each one is a full test run").arg(muts.size()));

    int timeoutSec = qBound(5, req.value(QStringLiteral("timeout_sec"))
                                   .toInt(300), 1800);

    // Read the baseline ONCE. Every mutation is applied to this, never to the
    // previous mutant, so two mutations cannot compound.
    QFile bf(check.resolved);
    if (!bf.open(QIODevice::ReadOnly))
        return mpErr(QStringLiteral("read_failed"),
                     QStringLiteral("mutation_probe: could not read \"%1\"")
                         .arg(rawPath));
    const QByteArray baselineBytes = bf.readAll();
    bf.close();
    const QString baseline = QString::fromUtf8(baselineBytes);

    struct RunOut { bool started; bool timedOut; int exitCode; QString output; };
    auto runTests = [&]() -> RunOut {
        QProcess p;
        p.setWorkingDirectory(rootCanonical);
        p.setProcessChannelMode(QProcess::MergedChannels);
        p.start(argv.first(), argv.mid(1));
        RunOut o{true, false, -1, QString()};
        if (!p.waitForStarted(5000)) { o.started = false; return o; }
        if (!p.waitForFinished(timeoutSec * 1000)) {
            p.kill();
            p.waitForFinished(2000);
            o.timedOut = true;
        }
        o.exitCode = p.exitCode();
        QByteArray out = p.readAllStandardOutput();
        static const int kCap = 512 * 1024;
        if (out.size() > kCap) out = out.right(kCap);
        o.output = QString::fromUtf8(out);
        return o;
    };
    // The restore, used on every exit path. Writing the ORIGINAL BYTES (not a
    // re-encode of the decoded string) is what makes `restored_clean` a real
    // guarantee rather than a hope.
    auto restore = [&]() -> bool {
        QFile w(check.resolved);
        if (!w.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        const bool wrote = (w.write(baselineBytes) == baselineBytes.size());
        w.close();
        if (!wrote) return false;
        QFile v(check.resolved);
        if (!v.open(QIODevice::ReadOnly)) return false;
        const bool same = (v.readAll() == baselineBytes);
        v.close();
        return same;
    };

    QJsonObject out;
    QJsonArray results;
    bool restoredClean = true;

    // Optional green-baseline gate. A RED baseline makes every result
    // meaningless — a mutant "dying" proves nothing when the suite was
    // already failing — so a caller can refuse the batch up front.
    if (req.value(QStringLiteral("require_green_baseline")).toBool(false)) {
        const RunOut base = runTests();
        if (!base.started)
            return mpErr(QStringLiteral("command_not_found"),
                QStringLiteral("mutation_probe: could not start \"%1\"")
                    .arg(argv.first()));
        const auto bc = MutationProbe::parseCounts(base.output);
        const auto verdict =
            MutationProbe::judgeBaseline(base.timedOut, base.exitCode, bc);
        if (verdict == MutationProbe::BaselineVerdict::NotGreen) {
            QJsonObject o2;
            o2[QStringLiteral("ok")]    = false;
            o2[QStringLiteral("code")]  = QStringLiteral("baseline_not_green");
            o2[QStringLiteral("error")] = QStringLiteral(
                "mutation_probe: the baseline run is not green, so every "
                "mutation result would be meaningless — a mutant \"dying\" "
                "proves nothing when the suite was already failing. Fix the "
                "suite first, or drop require_green_baseline to probe anyway.");
            o2[QStringLiteral("baseline_exit_code")] = base.exitCode;
            o2[QStringLiteral("baseline_timed_out")] = base.timedOut;
            o2[QStringLiteral("baseline_passed")]    = bc.passed;
            o2[QStringLiteral("baseline_failed")]    = bc.failed;
            return QJsonDocument(o2);
        }
        // ANTS-4401 — a zero exit code is not evidence of a green baseline,
        // and this gate promised evidence. Two states reach here having failed
        // nothing without having passed anything either:
        //
        //   -1 / -1  the output was unparsable. ANTS-4398 chose -1 over 0
        //            deliberately — "a run whose output could not be read has
        //            NOT said that nothing passed" — and then this guard read
        //            that same value as not-red. So the one flag whose whole
        //            job is to refuse a red suite stood down in precisely the
        //            case where the verb does not know.
        //    0 /  0  parsed, and nothing ran. A gtest binary with a filter
        //            matching no test exits 0; so does an empty suite.
        //
        // Both are the three-silences shape the reporting session named: could
        // not tell, did not fail, reads as a pass. A caller who asked to be
        // gated on green is better served by a refusal naming the reason than
        // by a batch whose premise was never checked — every mutant it then
        // reports `killed` is unfalsifiable.
        if (verdict != MutationProbe::BaselineVerdict::Green) {
            const bool unreadable =
                verdict == MutationProbe::BaselineVerdict::Unreadable;
            QJsonObject o2;
            o2[QStringLiteral("ok")]    = false;
            o2[QStringLiteral("code")]  = QStringLiteral("baseline_unreadable");
            o2[QStringLiteral("error")] =
                (unreadable
                     ? QStringLiteral(
                           "mutation_probe: the baseline exited 0 but its "
                           "output could not be parsed for pass/fail counts, "
                           "so `require_green_baseline` has nothing to check. "
                           "A run that could not be read has not said the "
                           "suite is green. Point `test_command` at a runner "
                           "whose summary this verb recognises (ctest, pytest "
                           "or gtest), or drop require_green_baseline to probe "
                           "anyway.")
                     : QStringLiteral(
                           "mutation_probe: the baseline ran no tests — 0 "
                           "passed, 0 failed, exit 0. A suite that executes "
                           "nothing cannot be green, and every mutant would "
                           "then report `survived` for the wrong reason. "
                           "Check the filter or the build, or drop "
                           "require_green_baseline to probe anyway."));
            o2[QStringLiteral("baseline_exit_code")] = base.exitCode;
            o2[QStringLiteral("baseline_timed_out")] = base.timedOut;
            o2[QStringLiteral("baseline_passed")]    = bc.passed;
            o2[QStringLiteral("baseline_failed")]    = bc.failed;
            return QJsonDocument(o2);
        }
        out[QStringLiteral("baseline_passed")] = bc.passed;
        out[QStringLiteral("baseline_failed")] = bc.failed;
    }

    for (const QJsonValue &mv : muts) {
        const QJsonObject mo = mv.toObject();
        MutationProbe::Mutation m;
        m.label   = mo.value(QStringLiteral("label")).toString();
        m.oldText = mo.value(QStringLiteral("old")).toString();
        m.newText = mo.value(QStringLiteral("new")).toString();

        QJsonObject r;
        r[QStringLiteral("label")] = m.label;

        const auto ap = MutationProbe::applyOne(baseline, m);
        if (!ap.ok) {
            // Inert: NO test run at all. Running one would produce a green
            // result against unmutated code, which is the false reading this
            // whole verb exists to prevent — so the run is skipped rather
            // than performed and disclaimed.
            r[QStringLiteral("applied")] = false;
            r[QStringLiteral("inert")]   = true;
            r[QStringLiteral("outcome")] = QStringLiteral("inert");
            r[QStringLiteral("inert_reason")] =
                ap.inert == MutationProbe::Inert::OldTextAbsent
                    ? QStringLiteral("old_text_absent")
                    : (ap.inert == MutationProbe::Inert::OldTextEmpty
                           ? QStringLiteral("old_text_empty")
                           : QStringLiteral("unchanged"));
            r[QStringLiteral("summary")] = QStringLiteral(
                "the file was NOT changed, so no test was run — a run here "
                "would pass against unmutated code and read as a weak test. "
                "This is a defect in the mutation, not evidence about the "
                "suite.");
            results.append(r);
            continue;
        }

        // ANTS-4521 — the caller may state how many sites the mutation meant,
        // and a mismatch is refused here: before the write, before the run,
        // and therefore before a verdict exists to be misread.
        //
        // `inert` guards a mutation that changed LESS than the caller believes.
        // This is the same defect in the other direction, and the dangerous
        // half is subtler than survival: with N sites mutated, the mutant can
        // be killed by a test covering a site the caller never meant to touch
        // while the site they DID mean to probe stays uncovered. The verdict
        // reads `killed`, the label — which is what gets quoted as evidence —
        // says what they intended, and the uncovered site is invisible. A
        // false GREEN in a verb whose whole purpose is refusing false greens.
        //
        // ABSENT is not 1. A mutation meant to hit every site is legitimate
        // ("the constant 3" usually means all of them), so defaulting would
        // break the common case to guard the uncommon one. Per-mutation like
        // `inert`, because the mutation list is data and one malformed entry
        // must not lose the other results.
        if (mo.contains(QStringLiteral("expect_occurrences"))) {
            const int want = mo.value(QStringLiteral("expect_occurrences")).toInt();
            if (want != ap.occurrences) {
                r[QStringLiteral("applied")]              = false;
                r[QStringLiteral("outcome")]              = QStringLiteral("occurrence_mismatch");
                r[QStringLiteral("occurrences")]          = ap.occurrences;
                r[QStringLiteral("expected_occurrences")] = want;
                r[QStringLiteral("summary")] = QStringLiteral(
                    "`old` occurs %1 time(s), not the %2 expected — no test was "
                    "run and the file was not touched. A mutation that hits "
                    "more sites than intended can be killed by a test covering "
                    "a site you never meant to probe, while the one you did "
                    "stays uncovered: the verdict reads `killed` and the label "
                    "describes a narrower mutation than the one that ran. "
                    "Narrow `old` until it is unique, or raise "
                    "`expect_occurrences` if hitting them all is the intent.")
                        .arg(ap.occurrences).arg(want);
                results.append(r);
                continue;
            }
        }

        QFile w(check.resolved);
        if (!w.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            r[QStringLiteral("applied")] = false;
            r[QStringLiteral("outcome")] = QStringLiteral("write_failed");
            results.append(r);
            continue;
        }
        const QByteArray patched = ap.patched.toUtf8();
        const bool wrote = (w.write(patched) == patched.size());
        w.close();
        if (!wrote) {
            if (!restore()) restoredClean = false;
            r[QStringLiteral("applied")] = false;
            r[QStringLiteral("outcome")] = QStringLiteral("write_failed");
            results.append(r);
            continue;
        }

        const RunOut run = runTests();
        // Restore BEFORE interpreting anything, so no early return can leak a
        // mutated file.
        if (!restore()) restoredClean = false;

        r[QStringLiteral("applied")]     = true;
        r[QStringLiteral("inert")]       = false;
        r[QStringLiteral("occurrences")] = ap.occurrences;
        if (!run.started) {
            r[QStringLiteral("outcome")] = QStringLiteral("command_not_found");
        } else if (run.timedOut) {
            // Timed out is NOT "killed" — a mutant that hangs the suite has
            // told you something, but not that the test caught it.
            r[QStringLiteral("outcome")] = QStringLiteral("timed_out");
        } else {
            const auto c = MutationProbe::parseCounts(run.output);
            r[QStringLiteral("passed")]    = c.passed;
            r[QStringLiteral("failed")]    = c.failed;
            r[QStringLiteral("exit_code")] = run.exitCode;
            r[QStringLiteral("outcome")]   = run.exitCode == 0
                ? QStringLiteral("survived")   // the suite did NOT notice
                : QStringLiteral("killed");
            if (run.exitCode == 0) {
                r[QStringLiteral("summary")] = QStringLiteral(
                    "the mutation applied (%1 occurrence(s)) and the tests "
                    "still passed — the suite does not measure this.")
                        .arg(ap.occurrences);
            }
        }
        results.append(r);
    }

    out[QStringLiteral("ok")]             = true;
    out[QStringLiteral("path")]           = rawPath;
    out[QStringLiteral("results")]        = results;
    out[QStringLiteral("restored_clean")] = restoredClean;
    if (!restoredClean) {
        out[QStringLiteral("restore_hint")] = QStringLiteral(
            "the source file could NOT be restored to its baseline — check "
            "\"%1\" with git before doing anything else. This is the failure "
            "mode the verb exists to prevent, so it is reported loudly rather "
            "than folded into a per-mutation outcome.").arg(rawPath);
    }
    return QJsonDocument(out);
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
    QString    itemsKey = QStringLiteral("items");
    if (!itemsVal.isArray()) {
        for (const char *alias : {"requests", "paths", "regions"}) {
            const QJsonValue v = req.value(QLatin1String(alias));
            if (v.isArray()) {
                itemsVal = v;
                itemsKey = QString::fromLatin1(alias);
                break;
            }
        }
    }
    // ANTS-4512 — two arrays for ONE parameter is a caller bug, and picking
    // one by preference order made it unreportable. A session sent `paths` as
    // bare filenames and `regions` as well-shaped objects; `paths` won, and
    // the reply was three copies of `item missing "path"` — true of the array
    // the verb chose, false of the array the caller meant, and naming neither.
    // So the natural next move was to re-inspect the array where nothing was
    // wrong. Refuse instead, naming every key supplied: the caller drops one
    // and is done. Same rule roadmap_query already applies to id vs section.
    QStringList suppliedKeys;
    for (const char *k : {"items", "requests", "paths", "regions"}) {
        if (req.value(QLatin1String(k)).isArray())
            suppliedKeys << QString::fromLatin1(k);
    }
    if (suppliedKeys.size() > 1) {
        QJsonObject o;
        o["ok"]    = false;
        o["error"] = QStringLiteral(
            "read_regions: %1 are aliases for the same batch key and were "
            "sent together; pass exactly one")
            .arg(suppliedKeys.join(QStringLiteral(" and ")));
        o["code"]     = QStringLiteral("bad_op_combo");
        o["supplied"] = QJsonArray::fromStringList(suppliedKeys);
        return QJsonDocument(o);
    }
    // ANTS-3589 — a top-level `path` is the per-item default: an item that
    // omits its own `path` reads from this instead, so the single-file case
    // (N slices of one module) passes the filename once. Per-item `path` wins.
    QJsonObject batch = ReadRegion::extractBatch(
        rootCanonical, itemsVal,
        req.value(QStringLiteral("max_bytes")).toInt(0),
        req.value(QStringLiteral("path")).toString());
    // ANTS-4512 — say which key the batch was read from. Without it a
    // per-item shape error cannot be attributed to an array at all, which is
    // what made the refusal above worth having.
    batch["items_key"] = itemsKey;
    return QJsonDocument(batch);
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
        // ANTS-4375 — `total_rows` is the rows IN the file, which is what
        // paging is over. When the producing verb reported a larger
        // population, the file is short and paging to the end is NOT
        // completeness — say so rather than letting the last page read as the
        // whole answer.
        if (r.population >= 0) o["population"] = r.population;
        if (r.rowsArePartial) {
            o["rows_are_partial"] = true;
            o["rows_are_partial_reason"] = QStringLiteral(
                "the spilled array holds %1 row(s) but the producing verb "
                "reported a population of %2 — it capped the array BEFORE "
                "spilling, so paging to the end of this handle does not reach "
                "the whole answer. Re-run the original call with a narrower "
                "filter or an explicit limit.")
                    .arg(r.totalRows).arg(r.population);
        }
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
        // ANTS-4089 — every session reaches this verb from the native Edit
        // tool, whose keys are old_string/new_string, so the first call is
        // written with those and refused on a message naming the key that is
        // missing but not the one that was sent. The pairing is unambiguous,
        // so accept both spellings; canonical `old`/`new` win if both arrive.
        const bool hasOld   = e.contains(QStringLiteral("old"))
                           || e.contains(QStringLiteral("old_string"));
        const bool hasNew   = e.contains(QStringLiteral("new"))
                           || e.contains(QStringLiteral("new_string"));
        const bool hasStart = e.contains(QStringLiteral("start_line"));
        const bool hasEnd   = e.contains(QStringLiteral("end_line"));
        const bool hasRange = hasStart || hasEnd;
        const QString oldStr = e.contains(QStringLiteral("old"))
            ? e.value(QStringLiteral("old")).toString()
            : e.value(QStringLiteral("old_string")).toString();
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
        rec.newStr     = e.contains(QStringLiteral("new"))
            ? e.value(QStringLiteral("new")).toString()
            : e.value(QStringLiteral("new_string")).toString();
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
    // ANTS-4418 — addSkip plus the optional near-miss annotation. A separate
    // lambda rather than a widened addSkip: the other four call sites
    // (not_found for an absent file, too_large, open failure, commit failure)
    // have no EditOutcome to carry and must stay byte-identical.
    auto addSkipNearMiss = [&](int index, const QString &path,
                               const QString &reason,
                               const ApplyEdits::EditOutcome &oc) {
        QJsonObject s; s["index"] = index; s["path"] = path; s["reason"] = reason;
        if (oc.nearMissLine > 0) {
            QJsonObject cand;
            cand["line"] = oc.nearMissLine;
            cand["text"] = oc.nearMissText;
            QJsonArray cands; cands.append(cand);
            s["candidates"]     = cands;
            s["near_miss_line"] = oc.nearMissLine;
            s["hint"] = QStringLiteral(
                "%1-only mismatch at line %2 — the file's text differs from "
                "`old` only in spacing. Retry with that line's exact text, or "
                "use the start_line/end_line form for line %2.")
                    .arg(oc.nearMissKind).arg(oc.nearMissLine);
        } else if (!oc.matchLines.isEmpty()) {
            // ANTS-4473 — the `ambiguous` half, in the SAME fields. The two
            // outcomes are mutually exclusive by construction (a near miss is
            // only sought on `not_found`, occurrences only on `ambiguous`), so
            // one `candidates` key carries both and a caller keeps one code
            // path. Filed off a WORKED WELL report: `roadmap_log op:amend_body`
            // refuses an over-broad match with a hint naming the cause and the
            // retry, and the reporting session recovered in one call — while
            // this verb's `ambiguous`, correct but silent about WHERE, cost a
            // full file read.
            QJsonArray cands;
            QStringList lineNums;
            for (int i = 0; i < oc.matchLines.size(); ++i) {
                QJsonObject cand;
                cand["line"] = oc.matchLines.at(i);
                cand["text"] = oc.matchTexts.value(i);
                cands.append(cand);
                lineNums << QString::number(oc.matchLines.at(i));
            }
            s["candidates"]  = cands;
            s["match_count"] = oc.matchCount;
            // Say so when the list is shorter than the count. A cap with no
            // flag reads as completeness, which is the defect this verb's
            // siblings were fixed for.
            const bool capped = oc.matchCount > oc.matchLines.size();
            if (capped) s["candidates_truncated"] = true;
            s["hint"] = QStringLiteral(
                "`old` occurs %1 time(s) — apply_edits requires a unique match. "
                "%2 at line(s) %3%4. Extend `old` with surrounding context to "
                "single one out, use the start_line/end_line form for the one "
                "you mean, or pass replace_all:true to change all %1.")
                    .arg(oc.matchCount)
                    .arg(capped ? QStringLiteral("First %1")
                                      .arg(oc.matchLines.size())
                                : QStringLiteral("They are"))
                    .arg(lineNums.join(QStringLiteral(", ")))
                    .arg(capped ? QStringLiteral(" (list truncated)")
                                : QString());
        }
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
                // ANTS-4418 — carry the near miss onto this skip row when the
                // helper found exactly one. `not_found` alone is equally
                // consistent with "the text is gone", "wrong file" and "you
                // are one space out", and this is the verb where the third is
                // most likely: the reported case differed only in the
                // run-length of interior spaces in a trailing-comment
                // alignment column. Costs one wasted round-trip per miss, and
                // is worst in a PARTIALLY-APPLIED batch — the file has already
                // moved under the caller, so the natural recovery (re-read and
                // retry) is also the riskiest one.
                //
                // Field names match the siblings a caller already handles:
                // read_region section-mode and roadmap_log's bullet locators
                // both return `candidates` on a miss, so one code path covers
                // all three. `near_miss_line` alone makes the line-range form
                // (ANTS-3711) usable as the immediate retry.
                addSkipNearMiss(es[k].index, rawPath, oc.skipReason, oc);
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
        // ANTS-4613 — a leaf beginning with a dot has NO derivable name. For
        // caller_cwd=~/.claude the naive derivation is
        // `<home>/.claude_Ants_MCP_Feedback.md`: a hidden-looking file in the
        // user's home directory which — ANTS-3714 settled this on the spec
        // side — cannot match the authoritative `*_Ants_MCP_Feedback.md` glob,
        // so nothing will ever read it. The verb kept deriving one anyway and
        // answered ok:true / created:true, and success plus creation reads as
        // success: the session silently started a SECOND feedback file while
        // the project's real one stayed untouched.
        //
        // Refuse rather than guess. There is no correct name to infer here,
        // and the siblings are the actionable retry — the same shape
        // op:append_tracking's not_found already uses.
        if (leaf.startsWith(QChar('.'))) {
            err = fbErr(QStringLiteral("bad_args"),
                        toolName + QStringLiteral(": \"path\" is required — "
                        "caller_cwd's leaf \"%1\" begins with a dot, and a "
                        "feedback name derived from it could never match the "
                        "*_Ants_MCP_Feedback.md convention. Name the file "
                        "explicitly.").arg(leaf));
            const QJsonArray cands = feedbackSiblingCandidates(
                QDir::cleanPath(sharedRoot + QLatin1Char('/') + leaf));
            if (!cands.isEmpty()) {
                err[QStringLiteral("candidates")] = cands;
                err[QStringLiteral("hint")] = QStringLiteral(
                    "pass one of `candidates` as `path`");
            }
            return false;
        }
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


// ANTS-3368 — co_change_family. Given one exemplar settings-field stem,
// return every edit site grouped by file: the JSON string key and the
// affixed derived names (setX, m_X, XChanged) a whole-word symbol search
// cannot reach. The matching rule, the role vocabulary and the widening all
// live in src/cochangefamily.{h,cpp} so a test can drive them without a live
// tab (INV-10); this handler is the ripgrep plumbing plus the envelope.
//
// The scan is deliberately repo-wide (INV-14): a config key's docs/ and
// CLAUDE.md mentions are co-change sites, which is exactly what a walk
// narrowed to the declared source roots would drop.
QJsonDocument RemoteControl::cmdCoChangeFamily(const QJsonObject &req) {
    QStringList rawStems;
    const QJsonArray stemsArr = req.value(QStringLiteral("stems")).toArray();
    for (const QJsonValue &v : stemsArr) {
        const QString s = v.toString().trimmed();
        if (!s.isEmpty()) rawStems.append(s);
    }
    if (rawStems.isEmpty()) {
        // `stem` is sugar for a one-element `stems`; `stems` wins when both
        // are sent, so a caller's mistake shows up in the echoed `stems`.
        const QString one =
            req.value(QStringLiteral("stem")).toString().trimmed();
        if (!one.isEmpty()) rawStems.append(one);
    }
    if (rawStems.isEmpty()) {
        return QJsonDocument(wsErr("bad_args",
            QStringLiteral("co_change_family: pass \"stem\" or \"stems\"")));
    }

    const QString callerRaw =
        req.value(QStringLiteral("caller_cwd")).toString();
    const QString rootCanonical = QFileInfo(callerRaw).canonicalFilePath();
    if (rootCanonical.isEmpty()) {
        return QJsonDocument(wsErr("no_project",
            QStringLiteral("co_change_family: caller_cwd \"%1\" does not resolve")
                .arg(callerRaw)));
    }

    const bool hasMinRun = req.contains(QStringLiteral("min_run"));
    const int  reqMinRun = req.value(QStringLiteral("min_run")).toInt();

    QVector<CoChangeFamily::Stem> stems;
    QStringList patterns;
    for (const QString &raw : rawStems) {
        if (!CoChangeFamily::isValidStem(raw)) {
            return QJsonDocument(wsErr("bad_args",
                QStringLiteral("co_change_family: stem \"%1\" must match "
                               "^[A-Za-z0-9_.-]+$").arg(raw)));
        }
        const QStringList w = CoChangeFamily::splitWords(raw);
        if (w.isEmpty()) {
            return QJsonDocument(wsErr("bad_args",
                QStringLiteral("co_change_family: stem \"%1\" yields no words")
                    .arg(raw)));
        }
        if (CoChangeFamily::allStopwords(w)) {
            // Every run this stem could form would be dropped, so an empty
            // result would read as "this field is touched nowhere".
            return QJsonDocument(wsErr("bad_args",
                QStringLiteral("co_change_family: stem \"%1\" is all "
                               "stopwords; it can form no distinctive run")
                    .arg(raw)));
        }
        CoChangeFamily::Stem s;
        s.name   = raw;
        s.words  = w;
        s.minRun = hasMinRun
                       ? CoChangeFamily::clampMinRun(reqMinRun, w.size())
                       : CoChangeFamily::defaultMinRun(w.size());
        stems.append(s);
        patterns.append(CoChangeFamily::scanPattern(w, s.minRun));
    }

    QStringList argv;
    argv << QStringLiteral("--json") << QStringLiteral("--line-number");
    for (const QString &p : patterns) argv << QStringLiteral("-e") << p;
    argv << QStringLiteral("--") << rootCanonical;

    const RgRun run = rcRunRg(argv, rootCanonical, kWorkspaceSearchHardKillMs);
    if (run.startFailed || run.crashed) {
        return QJsonDocument(wsErr("rg_failed",
            QStringLiteral("co_change_family: ripgrep did not run")));
    }
    // Exit 1 is ripgrep's "no matches" — a valid empty answer, not a failure.
    // A hard kill is a PARTIAL answer and is reported through `truncated`;
    // rg_failed is reserved for a scanner that did not run (INV-7).
    if (run.exitCode != 0 && run.exitCode != 1 && !run.hardKilled) {
        return QJsonDocument(wsErr("rg_failed",
            QStringLiteral("co_change_family: ripgrep exited %1")
                .arg(run.exitCode)));
    }

    QVector<CoChangeFamily::RawMatch> raws;
    const QList<QByteArray> events = run.stdoutBytes.split('\n');
    for (const QByteArray &evBytes : events) {
        if (evBytes.isEmpty()) continue;
        const QJsonObject ev = QJsonDocument::fromJson(evBytes).object();
        if (ev.value(QStringLiteral("type")).toString() !=
            QLatin1String("match")) {
            continue;
        }
        const QJsonObject data = ev.value(QStringLiteral("data")).toObject();
        QString path = data.value(QStringLiteral("path"))
                           .toObject().value(QStringLiteral("text")).toString();
        if (path.startsWith(rootCanonical + QLatin1Char('/'))) {
            path = path.mid(rootCanonical.size() + 1);
        }
        QString text = data.value(QStringLiteral("lines"))
                           .toObject().value(QStringLiteral("text")).toString();
        if (text.endsWith(QLatin1Char('\n'))) text.chop(1);
        const QByteArray textUtf8 = text.toUtf8();

        const QJsonArray subs =
            data.value(QStringLiteral("submatches")).toArray();
        for (const QJsonValue &sv : subs) {
            const QJsonObject sm = sv.toObject();
            const int bs = sm.value(QStringLiteral("start")).toInt();
            const int be = sm.value(QStringLiteral("end")).toInt();
            if (bs < 0 || be < bs || be > textUtf8.size()) continue;
            CoChangeFamily::RawMatch m;
            m.path = path;
            m.line = data.value(QStringLiteral("line_number")).toInt();
            m.text = text;
            // ripgrep reports BYTE offsets into lines.text; the seam indexes
            // by QChar, so convert rather than assuming the line is ASCII.
            m.matchStart = static_cast<int>(
                QString::fromUtf8(textUtf8.left(bs)).size());
            m.matchEnd = static_cast<int>(
                QString::fromUtf8(textUtf8.left(be)).size());
            raws.append(m);
        }
    }

    CoChangeFamily::Options opts;
    if (req.contains(QStringLiteral("max_sites"))) {
        opts.maxSites = CoChangeFamily::clampMaxSites(
            req.value(QStringLiteral("max_sites")).toInt());
    }
    const CoChangeFamily::Result res =
        CoChangeFamily::assemble(raws, stems, opts);

    // assemble() already ordered the sites, so grouping is a single pass:
    // every row of one file is contiguous.
    QJsonArray files;
    QJsonObject curFile;
    QJsonArray curSites;
    QString curPath;
    bool haveFile = false;
    const auto flush = [&]() {
        if (!haveFile) return;
        curFile[QStringLiteral("sites")] = curSites;
        files.append(curFile);
    };
    for (const CoChangeFamily::Site &s : res.sites) {
        if (!haveFile || s.path != curPath) {
            flush();
            haveFile = true;
            curPath  = s.path;
            curFile  = QJsonObject();
            curFile[QStringLiteral("path")] = curPath;
            curSites = QJsonArray();
        }
        QJsonObject o;
        o[QStringLiteral("line")]    = s.line;
        o[QStringLiteral("stem")]    = s.stem;
        o[QStringLiteral("name")]    = s.name;
        o[QStringLiteral("role")]    =
            QString::fromLatin1(CoChangeFamily::roleStr(s.role));
        o[QStringLiteral("run")]     = QJsonArray::fromStringList(s.run);
        o[QStringLiteral("run_len")] = s.runLen;
        o[QStringLiteral("text")]    = s.text;
        curSites.append(o);
    }
    flush();

    QJsonArray stemNames;
    QJsonObject stemWords;
    QJsonObject minRuns;
    for (const CoChangeFamily::Stem &s : stems) {
        stemNames.append(s.name);
        stemWords[s.name] = QJsonArray::fromStringList(s.words);
        minRuns[s.name]   = s.minRun;
    }

    QJsonObject out;
    out[QStringLiteral("ok")]          = true;
    out[QStringLiteral("stems")]       = stemNames;
    out[QStringLiteral("stem_words")]  = stemWords;
    out[QStringLiteral("min_run")]     = minRuns;
    out[QStringLiteral("files")]       = files;
    out[QStringLiteral("files_count")] = files.size();
    out[QStringLiteral("sites_count")] = static_cast<int>(res.sites.size());
    out[QStringLiteral("truncated")]   = res.truncated || run.hardKilled;
    // `etag` is NOT emitted here: the dispatcher injects it (mcp-tools.md
    // step 7), and a handler-written one would be overwritten or doubled.
    return QJsonDocument(out);
}

// ANTS-3745: build_target_for — which build target owns this file, and what
// would I run after editing it.
//
// The gap it closes is narrow and was hit four times in one session: after
// editing a `tests/features/<x>/test_<x>.cpp` you must know its bundle to
// build anything narrower than everything, and nothing in the toolkit said.
// The fallbacks were an `awk` walking backwards to the nearest bundle call,
// and running every `build/test_*` with `--gtest_list_tests` to find which one
// carried the suite. Both are the grep-happy shelling the MCP exists to
// replace, and the answer is static — it is in CMakeLists.txt.
//
// It returns the two things the answer is FOR — the `cmake --build --target`
// line and the `ctest -R` filter — rather than only the target name, because
// a caller that has to assemble those from the name is one CLAUDE.md lookup
// short of where it started.
//
// Read-only, opens two files at most (the CMake file, and the source when
// suites are wanted). The parse lives in buildtargets.cpp.
QJsonDocument RemoteControl::cmdBuildTargetFor(const QJsonObject &req) {
    const QString rootCanonical = resolveRootCanonical(m_main, req);
    if (rootCanonical.isEmpty()) {
        QJsonObject o;
        o[QStringLiteral("ok")]    = false;
        o[QStringLiteral("error")] = QStringLiteral(
            "build_target_for: no focused project");
        o[QStringLiteral("code")]  = QStringLiteral("bad_path");
        return QJsonDocument(o);
    }

    const QString pathRaw = req.value(QStringLiteral("path")).toString();
    if (pathRaw.isEmpty()) {
        QJsonObject o;
        o[QStringLiteral("ok")]    = false;
        o[QStringLiteral("error")] = QStringLiteral(
            "build_target_for: `path` is required");
        o[QStringLiteral("code")]  = QStringLiteral("missing_field");
        return QJsonDocument(o);
    }
    const auto pathCheck = PathValidation::validatePath(
        pathRaw, rootCanonical, QStringLiteral("build_target_for"),
        QStringLiteral("path"));
    if (pathCheck.bad) return QJsonDocument(pathCheck.err);

    const QDir root(rootCanonical);
    // The path as CMakeLists.txt would spell it: project-relative, forward
    // slashes, no leading `./`. A caller passing an absolute path or a
    // `./`-prefixed one must get the same answer as one passing the bare
    // relative form, or the verb is a lookup that only works when you already
    // know the convention.
    QString rel = pathCheck.resolved.isEmpty()
                      ? pathRaw
                      : root.relativeFilePath(pathCheck.resolved);
    while (rel.startsWith(QLatin1String("./"))) rel = rel.mid(2);

    const QString cmakeRel = req.value(QStringLiteral("cmake_path")).toString(
        QStringLiteral("CMakeLists.txt"));
    QFile cf(root.filePath(cmakeRel));
    if (!cf.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QJsonObject o;
        o[QStringLiteral("ok")]    = false;
        o[QStringLiteral("error")] = QStringLiteral(
            "build_target_for: cannot read %1").arg(cmakeRel);
        o[QStringLiteral("code")]  = QStringLiteral("not_found");
        return QJsonDocument(o);
    }
    const QList<BuildTargets::Target> all =
        BuildTargets::parse(QString::fromUtf8(cf.readAll()));
    cf.close();

    const QString buildDir =
        req.value(QStringLiteral("build_dir")).toString(QStringLiteral("build"));

    QJsonArray targets;
    const QList<BuildTargets::Target> owners = BuildTargets::ownersOf(all, rel);
    for (const BuildTargets::Target &t : owners) {
        QJsonObject o;
        o[QStringLiteral("name")]          = t.name;
        o[QStringLiteral("kind")]          = t.kind;
        o[QStringLiteral("command")]       = t.command;
        o[QStringLiteral("line")]          = t.line;
        o[QStringLiteral("source_count")]  = int(t.sources.size());
        o[QStringLiteral("build_command")] =
            QStringLiteral("cmake --build %1 --target %2").arg(buildDir, t.name);
        targets.append(o);
    }

    QJsonObject out;
    out[QStringLiteral("ok")]             = true;
    out[QStringLiteral("path")]           = rel;
    out[QStringLiteral("cmake_path")]     = cmakeRel;
    out[QStringLiteral("targets")]        = targets;
    out[QStringLiteral("targets_parsed")] = int(all.size());
    // `found:false` is a real answer, not a refusal: a header, a doc, or a
    // source reached only through a variable is genuinely owned by no target
    // this parser resolves, and saying so beats naming the wrong one.
    out[QStringLiteral("found")]          = !targets.isEmpty();
    if (targets.isEmpty()) {
        out[QStringLiteral("hint")] = QStringLiteral(
            "no target's SOURCES names this path. A header is usually not "
            "listed (build the target owning its .cpp); a new test source is "
            "not listed until it is added to a bundle's SOURCES, which is the "
            "ANTS-3745 trap — the build then succeeds silently and runs the "
            "old binary. Sources named through a CMake variable or "
            "target_sources() are not resolved.");
    }

    // The gtest suites, so `ctest -R` is usable straight from the answer
    // instead of costing a `--gtest_list_tests` launch per bundle. Skipped for
    // a non-source path and suppressible for a caller that only wants the
    // target.
    if (req.value(QStringLiteral("suites")).toBool(true)) {
        QFile sf(root.filePath(rel));
        if (sf.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QStringList suites =
                BuildTargets::gtestSuites(QString::fromUtf8(sf.readAll()));
            if (!suites.isEmpty()) {
                out[QStringLiteral("suites")] =
                    QJsonArray::fromStringList(suites);
                // ctest -R is a REGEX and is case-sensitive, so the filter is
                // built rather than left to the caller to spell.
                out[QStringLiteral("ctest_filter")] =
                    QStringLiteral("^(%1)\\.").arg(suites.join(QLatin1Char('|')));
                out[QStringLiteral("ctest_command")] = QStringLiteral(
                    "ctest --test-dir %1 -R '^(%2)\\.' --output-on-failure")
                        .arg(buildDir, suites.join(QLatin1Char('|')));
            }
        }
    }
    return QJsonDocument(out);
}
