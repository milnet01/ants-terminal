// ANTS-1677 auditdialog piece 5/5 — SARIF, HTML and plain-text export
#include "auditdialog.h"
#include "auditdialog_internal.h"
#include "auditautofix.h"
#include "auditfpledger.h"
#include "debtsweepengine.h"
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>

using namespace auditdialogdetail;

// ---------------------------------------------------------------------------
// Plain-text export for the Claude Review handoff
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// SARIF v2.1.0 export (OASIS standard, consumed by GitHub code-scanning,
// VSCode SARIF viewer, SonarQube, etc.)
// ---------------------------------------------------------------------------
//
// Minimal compliant document: one run, one tool (ants-audit), a catalogue
// of rules we emitted, and one result per finding. SARIF level mapping:
//   Blocker / Critical → "error"
//   Major / Minor      → "warning"
//   Info               → "note"

QString AuditDialog::exportSarif() const {
    auto sarifLevel = [](Severity s) -> QString {
        switch (s) {
            case Severity::Blocker:
            case Severity::Critical: return "error";
            case Severity::Major:
            case Severity::Minor:    return "warning";
            case Severity::Info:     return "note";
        }
        return "none";
    };

    // Build the rule catalogue deduplicated by check id.
    QMap<QString, AuditCheck> ruleById;
    for (const auto &c : m_checks) ruleById.insert(c.id, c);

    QJsonArray rules;
    for (auto it = ruleById.constBegin(); it != ruleById.constEnd(); ++it) {
        const auto &c = it.value();
        QJsonObject r;
        r["id"] = c.id;
        r["name"] = c.name;
        QJsonObject shortDesc;  shortDesc["text"] = c.name;
        QJsonObject fullDesc;   fullDesc["text"]  = c.description;
        r["shortDescription"] = shortDesc;
        r["fullDescription"]  = fullDesc;
        QJsonObject defCfg; defCfg["level"] = sarifLevel(c.severity);
        r["defaultConfiguration"] = defCfg;
        r["properties"] = QJsonObject{
            {"category",  c.category},
            {"type",      typeLabel(c.type)},
            {"severity",  severityLabel(c.severity)},
        };
        rules.append(r);
    }

    QJsonArray results;
    for (const CheckResult &cr : m_completedResults) {
        if (cr.warning || cr.findings.isEmpty()) continue;
        for (const Finding &f : cr.findings) {
            QJsonObject res;
            res["ruleId"]   = f.checkId;
            res["level"]    = sarifLevel(f.severity);
            QJsonObject msg;
            msg["text"]     = f.message;
            res["message"]  = msg;

            if (!f.file.isEmpty()) {
                QJsonObject loc, physLoc, artLoc, region;
                artLoc["uri"] = f.file;
                physLoc["artifactLocation"] = artLoc;
                if (f.line > 0) {
                    region["startLine"] = f.line;
                    physLoc["region"]   = region;
                }
                // SARIF contextRegion — the ±3 lines around the finding.
                // Consumers (GitHub Code Scanning, SonarQube, VSCode viewer)
                // render this as the inline code preview.
                if (!f.snippet.isEmpty() && f.snippetStart > 0) {
                    QJsonObject ctxRegion;
                    ctxRegion["startLine"] = f.snippetStart;
                    ctxRegion["endLine"]   = f.snippetStart
                                           + f.snippet.count('\n');
                    ctxRegion["snippet"]   = QJsonObject{{"text", f.snippet}};
                    physLoc["contextRegion"] = ctxRegion;
                }
                loc["physicalLocation"] = physLoc;
                QJsonArray locs; locs.append(loc);
                res["locations"] = locs;
            }
            QJsonObject partialFp;
            // SARIF v2.1.0 § 3.27.13 convention: <name>/<version>.
            partialFp["primaryLocationLineHash/v1"] = f.dedupKey;
            res["partialFingerprints"] = partialFp;

            QJsonObject props{
                {"source", f.source},
                {"highConfidence", f.highConfidence},
                {"confidence", f.confidence},
            };
            // Git-blame bag — sarif-tools de-facto convention, not in the
            // formal schema but consumed by GitHub Code Scanning UI.
            if (!f.blameSha.isEmpty()) {
                QJsonObject blame{
                    {"author",      f.blameAuthor},
                    {"author-time", f.blameDate},
                    {"sha",         f.blameSha},
                };
                props["blame"] = blame;
            }
            // AI triage verdict (present only if user ran it).
            if (!f.aiVerdict.isEmpty()) {
                props["aiTriage"] = QJsonObject{
                    {"verdict",    f.aiVerdict},
                    {"confidence", f.aiConfidence},
                    {"reasoning",  f.aiReasoning},
                };
            }
            res["properties"] = props;

            // SARIF v2.1.0 §3.34 — result.suppressions[] surfaces user
            // suppressions to external consumers (GitHub Code Scanning,
            // SonarQube, VSCode SARIF Viewer). kind "external" reflects
            // that the suppression is recorded in ~/.audit_suppress
            // outside the source artifact; state "accepted" mirrors the
            // dialog's no-review-workflow semantics. Justification is
            // the user's free-text reason from the JSONL entry.
            // Indie-review-2026-05-14 lane-4 H1+H2: live `isSuppressed`
            // lookup, not the stale `f.suppressed` cached at parse-time.
            // A user who clicks "suppress" mid-session and immediately
            // exports SARIF would otherwise lose the result.suppressions[]
            // record for the just-suppressed finding (the cached flag
            // hasn't been refreshed; the parser only re-runs on next
            // audit). HTML export at :5199 already calls isSuppressed
            // live — this brings SARIF into parity.
            if (isSuppressed(f)) {
                QJsonObject sup;
                sup["kind"]  = "external";
                sup["status"] = "accepted";
                // A learned false positive has no entry in the reason map —
                // it was never suppressed by key — so fall back to the
                // ledger's own note rather than exporting a suppression with
                // no stated cause.
                QString reason =
                    m_suppressionReasons.value(f.dedupKey,
                        m_suppressionReasons.value(f.dedupKey.left(16)));
                if (reason.isEmpty()) reason = f.aiReasoning;
                if (!reason.isEmpty()) sup["justification"] = reason;
                QJsonArray suppArr; suppArr.append(sup);
                res["suppressions"] = suppArr;
            }
            results.append(res);
        }
    }

    QJsonObject driver;
    driver["name"] = "ants-audit";
    driver["version"] = QStringLiteral(ANTS_VERSION);
    driver["informationUri"] = "https://github.com/milnet01/ants-terminal";
    driver["rules"] = rules;

    QJsonObject tool;
    tool["driver"] = driver;

    QJsonObject run;
    run["tool"] = tool;
    run["results"] = results;
    QJsonObject invocation;
    invocation["workingDirectory"] = QJsonObject{{"uri", m_projectPath}};
    invocation["startTimeUtc"]     = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    QJsonArray invocations; invocations.append(invocation);
    run["invocations"] = invocations;
    // ANTS-1576 — capture VCS provenance once at SARIF emission time.
    const QJsonArray vcpBlock =
        AuditEngine::buildVcsProvenanceBlock(m_projectPath);
    if (!vcpBlock.isEmpty()) {
        run["versionControlProvenance"] = vcpBlock;
    }

    QJsonArray runs; runs.append(run);

    QJsonObject root;
    root["$schema"] = "https://raw.githubusercontent.com/oasis-tcs/sarif-spec/main/"
                      "Documents/CommitteeSpecifications/2.1.0/sarif-schema-2.1.0.json";
    root["version"] = "2.1.0";
    root["runs"] = runs;

    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

// ---------------------------------------------------------------------------
// Single-file HTML report export
// ---------------------------------------------------------------------------
//
// Embeds findings as an inline JSON payload + ~120 lines of vanilla JS/CSS
// that render severity pills, a text filter, and per-check collapsible
// cards. No external assets — the file opens standalone in any browser.
QString AuditDialog::exportHtml() const {
    // Build a compact JSON payload that the page renders client-side.
    QJsonArray findingsJson;
    for (const CheckResult &cr : m_completedResults) {
        if (cr.warning) continue;
        for (const Finding &f : cr.findings) {
            if (isSuppressed(f)) continue;
            QJsonObject o;
            o["checkId"]   = f.checkId;
            o["checkName"] = f.checkName;
            o["category"]  = f.category;
            o["type"]      = typeLabel(f.type);
            o["severity"]  = severityLabel(f.severity);
            o["source"]    = f.source;
            o["file"]      = f.file;
            o["line"]      = f.line;
            o["message"]   = f.message;
            o["dedupKey"]  = f.dedupKey;
            o["highConf"]  = f.highConfidence;
            o["confidence"] = f.confidence;
            o["snippet"]   = f.snippet;
            o["snippetStart"] = f.snippetStart;
            if (!f.blameSha.isEmpty()) {
                o["blame"] = QJsonObject{
                    {"author", f.blameAuthor},
                    {"date",   f.blameDate},
                    {"sha",    f.blameSha},
                };
            }
            if (!f.aiVerdict.isEmpty()) {
                o["ai"] = QJsonObject{
                    {"verdict",    f.aiVerdict},
                    {"confidence", f.aiConfidence},
                    {"reasoning",  f.aiReasoning},
                };
            }
            findingsJson.append(o);
        }
    }

    QJsonObject meta;
    meta["project"]    = m_projectPath;
    meta["detected"]   = m_detectedTypes.join(", ");
    meta["generated"]  = QDateTime::currentDateTime().toString(Qt::ISODate);
    meta["version"]    = QString::fromLatin1(ANTS_VERSION);
    meta["findings"]   = findingsJson;

    QString payload = QString::fromUtf8(
        QJsonDocument(meta).toJson(QJsonDocument::Compact));
    // Defuse </script> and <!-- lookalikes inside finding messages. '<' only
    // occurs inside JSON strings, where \\u003c is the same character.
    // ANTS-5084 — escaping only "</" left <!-- able to blank the page.
    payload.replace(QLatin1String("<"), QLatin1String("\\u003c"));

    // clang-format off
    // Template is a raw string — escape the literal `)"` delimiter only.
    static const char *kTemplate = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Ants audit — {{PROJECT}}</title>
<style>
  :root {
    --bg:       #1e1e1e;
    --surface:  #2a2a2a;
    --border:   #3a3a3a;
    --text:     #e0e0e0;
    --muted:    #8a8a8a;
    --accent:   #89b4fa;
    --blocker:  #8b0000;
    --critical: #e74856;
    --major:    #ffa500;
    --minor:    #ffd700;
    --info:     #4caf50;
  }
  * { box-sizing: border-box; }
  body {
    font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
    background: var(--bg); color: var(--text); margin: 0; padding: 24px;
    line-height: 1.5;
  }
  header { margin-bottom: 16px; }
  h1 { margin: 0 0 4px; font-size: 20px; }
  .meta { color: var(--muted); font-size: 12px; }
  .controls {
    background: var(--surface); border: 1px solid var(--border);
    border-radius: 6px; padding: 12px; margin-bottom: 16px;
    display: flex; gap: 12px; flex-wrap: wrap; align-items: center;
  }
  .pill {
    padding: 4px 10px; border-radius: 12px; font-size: 11px; cursor: pointer;
    border: 1px solid var(--border); user-select: none;
    background: var(--bg); color: var(--text);
  }
  .pill[data-active="false"] { opacity: 0.4; }
  .pill.BLOCKER  { color: var(--blocker);  border-color: var(--blocker); }
  .pill.CRITICAL { color: var(--critical); border-color: var(--critical); }
  .pill.MAJOR    { color: var(--major);    border-color: var(--major); }
  .pill.MINOR    { color: var(--minor);    border-color: var(--minor); }
  .pill.INFO     { color: var(--info);     border-color: var(--info); }
  #q {
    background: var(--bg); color: var(--text); border: 1px solid var(--border);
    border-radius: 4px; padding: 6px 10px; min-width: 280px; font-size: 13px;
  }
  .check {
    background: var(--surface); border: 1px solid var(--border);
    border-radius: 6px; margin-bottom: 10px; overflow: hidden;
  }
  .check summary {
    cursor: pointer; padding: 10px 14px; font-weight: 500;
    display: flex; align-items: center; gap: 10px;
  }
  .check summary::-webkit-details-marker { display: none; }
  .tag {
    font-size: 10px; padding: 2px 7px; border-radius: 3px;
    text-transform: uppercase; font-weight: 600; letter-spacing: 0.3px;
  }
  .tag.BLOCKER  { background: var(--blocker);  color: white; }
  .tag.CRITICAL { background: var(--critical); color: white; }
  .tag.MAJOR    { background: var(--major);    color: black; }
  .tag.MINOR    { background: var(--minor);    color: black; }
  .tag.INFO     { background: var(--info);     color: black; }
  .rows {
    padding: 0 14px 10px; font-family: 'SF Mono', Menlo, monospace;
    font-size: 12px;
  }
  .row {
    padding: 6px 0; border-top: 1px solid var(--border);
    white-space: pre-wrap; word-break: break-word;
  }
  .row:first-child { border-top: none; }
  .loc { color: var(--accent); }
  .key { color: var(--muted); font-size: 10px; margin-left: 8px; }
  .blame { color: var(--muted); font-size: 10px; margin-left: 6px; }
  .conf-pip { font-weight: bold; margin-right: 4px; }
  .conf-hi  { color: #4caf50; }
  .conf-md  { color: #ffa500; }
  .conf-lo  { color: #e74856; }
  .verdict { font-size: 10px; margin-left: 6px; font-weight: 600; }
  .verdict.TRUE_POSITIVE  { color: var(--critical); }
  .verdict.FALSE_POSITIVE { color: var(--info); }
  .verdict.NEEDS_REVIEW   { color: var(--major); }
  .snippet {
    margin: 6px 0 0 24px; padding: 6px 10px; background: var(--bg);
    border-left: 2px solid var(--border); border-radius: 3px;
    font-size: 11px; line-height: 1.35; overflow-x: auto;
  }
  .snippet .ln    { color: var(--muted); user-select: none; margin-right: 6px; }
  .snippet .hit   { background: rgba(231,72,86,0.15); }
  .snippet .hit .ln { color: var(--critical); }
  details.fx { margin-left: 24px; margin-top: 4px; }
  details.fx summary { cursor: pointer; color: var(--accent); font-size: 10px; }
  details.fx summary::-webkit-details-marker { display: none; }
  .reason { color: var(--muted); font-size: 10px; margin-top: 4px; }
  .empty { padding: 20px; color: var(--muted); font-style: italic; text-align: center; }
  footer { margin-top: 20px; color: var(--muted); font-size: 11px; text-align: center; }
</style>
</head>
<body>
<header>
  <h1>Ants audit report</h1>
  <div class="meta" id="meta"></div>
</header>
<div class="controls">
  <input type="search" id="q" placeholder="Filter by file, message, rule…">
  <span class="pill BLOCKER"  data-sev="BLOCKER"  data-active="true">Blocker</span>
  <span class="pill CRITICAL" data-sev="CRITICAL" data-active="true">Critical</span>
  <span class="pill MAJOR"    data-sev="MAJOR"    data-active="true">Major</span>
  <span class="pill MINOR"    data-sev="MINOR"    data-active="true">Minor</span>
  <span class="pill INFO"     data-sev="INFO"     data-active="true">Info</span>
</div>
<div id="results"></div>
<footer>Generated by ants-audit · single-file report · no external assets</footer>

<script id="data" type="application/json">{{PAYLOAD}}</script>
<script>
(function () {
  const DATA = JSON.parse(document.getElementById('data').textContent);
  const SEV_ORDER = { BLOCKER: 4, CRITICAL: 3, MAJOR: 2, MINOR: 1, INFO: 0 };
  const meta = document.getElementById('meta');
  meta.textContent = `Project: ${DATA.project} · Detected: ${DATA.detected} · ants-audit v${DATA.version || '?'} · ${DATA.generated}`;

  // Group by check.
  const byCheck = new Map();
  for (const f of DATA.findings) {
    if (!byCheck.has(f.checkId)) byCheck.set(f.checkId, { meta: f, findings: [] });
    byCheck.get(f.checkId).findings.push(f);
  }

  // Sort checks by highest-severity finding.
  const checks = Array.from(byCheck.values()).sort((a, b) =>
    SEV_ORDER[b.meta.severity] - SEV_ORDER[a.meta.severity]);

  const results = document.getElementById('results');
  const activeSevs = new Set(['BLOCKER', 'CRITICAL', 'MAJOR', 'MINOR', 'INFO']);
  let q = '';

  function escape(s) {
    return String(s).replace(/[&<>"']/g, c => ({
      '&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'
    })[c]);
  }

  function render() {
    results.innerHTML = '';
    let shown = 0;
    for (const c of checks) {
      const filtered = c.findings.filter(f => {
        if (!activeSevs.has(f.severity)) return false;
        if (!q) return true;
        const hay = (f.file + ' ' + f.message + ' ' + f.checkId).toLowerCase();
        return hay.includes(q);
      });
      if (filtered.length === 0) continue;
      shown += filtered.length;
      const rows = filtered.map(f => {
        const loc = f.file ? `<span class="loc">${escape(f.file)}${f.line > 0 ? ':' + f.line : ''}</span>  ` : '';
        const star = f.highConf ? ' <span title="Flagged by 2+ tools" style="color:#FFD700;">★</span>' : '';
        const confCls = (f.confidence >= 70) ? 'conf-hi'
                      : (f.confidence >= 40) ? 'conf-md' : 'conf-lo';
        const pip = (typeof f.confidence === 'number')
          ? `<span class="conf-pip ${confCls}" title="Confidence ${f.confidence}/100">●</span>`
          : '';
        const blame = f.blame
          ? `<span class="blame" title="git blame">[${escape(f.blame.author)} · ${escape(f.blame.date)} · ${escape(f.blame.sha)}]</span>`
          : '';
        const verdict = f.ai
          ? (() => { const v = escape(f.ai.verdict); return `<span class="verdict ${v}" title="AI: ${escape(String(f.ai.confidence))}/100">${v.replace('_',' ')}</span>`; })()
          : '';
        let snippet = '';
        if (f.snippet) {
          const lines = f.snippet.split('\n').map((ln, idx) => {
            const num = (f.snippetStart || 0) + idx;
            const hit = (num === f.line);
            return `<div class="${hit ? 'hit' : ''}"><span class="ln">${String(num).padStart(5)}</span>${escape(ln)}</div>`;
          }).join('');
          snippet = `<details class="fx"><summary>[details]</summary>
            <div class="snippet">${lines}</div>
            ${f.ai ? `<div class="reason">${escape(f.ai.reasoning || '')}</div>` : ''}
          </details>`;
        }
        return `<div class="row">${pip}${loc}${escape(f.message)}${star}${blame}${verdict}<span class="key">${escape(f.dedupKey)}</span>${snippet}</div>`;
      }).join('');
      const det = document.createElement('details');
      det.className = 'check';
      det.open = SEV_ORDER[c.meta.severity] >= 2;  // open major+
      det.innerHTML = `
        <summary>
          <span class="tag ${c.meta.severity}">${c.meta.severity}</span>
          <strong>${escape(c.meta.checkName)}</strong>
          <span class="key">· ${escape(c.meta.category)} · ${escape(c.meta.source)} · ${filtered.length} finding${filtered.length === 1 ? '' : 's'}</span>
        </summary>
        <div class="rows">${rows}</div>`;
      results.appendChild(det);
    }
    if (shown === 0) {
      results.innerHTML = '<div class="empty">No findings match current filters.</div>';
    }
  }

  document.querySelectorAll('.pill').forEach(p => {
    p.addEventListener('click', () => {
      const sev = p.dataset.sev;
      if (activeSevs.has(sev)) { activeSevs.delete(sev); p.dataset.active = 'false'; }
      else                     { activeSevs.add(sev);    p.dataset.active = 'true'; }
      render();
    });
  });
  document.getElementById('q').addEventListener('input', e => {
    q = e.target.value.trim().toLowerCase(); render();
  });
  render();
})();
</script>
</body>
</html>
)HTML";
    // clang-format on

    QString html = QString::fromUtf8(kTemplate);
    html.replace("{{PROJECT}}", QFileInfo(m_projectPath).fileName().toHtmlEscaped());
    html.replace("{{PAYLOAD}}", payload);
    return html;
}

QString AuditDialog::plainTextResults() const {
    if (m_completedResults.isEmpty()) return {};

    QString header = "Project Audit Results\n"
                     "Generator: ants-audit v" + QString::fromLatin1(ANTS_VERSION) + "\n"
                     "Project: " + m_projectPath + "\n"
                     "Detected: " + m_detectedTypes.join(", ") + "\n";
    if (m_hasBaseline) header += "Baseline: loaded\n";
    if (!m_suppressedKeys.isEmpty())
        header += QString("Suppressions: %1 loaded from .audit_suppress\n")
                    .arg(m_suppressedKeys.size());
    if (m_recentOnly)
        header += QString("Scope: files touched in last %1 commits (%2 file(s))\n")
                    .arg(m_recentCommits).arg(m_recentFiles.size());
    if (!m_recentScopeError.isEmpty())
        header += "Scope: changed-lines filter off (" + m_recentScopeError + ")\n";
    header +=
        "Legend: `conf N` = 0-100 confidence score (higher = more tools\n"
        "        corroborate). `★` = flagged by ≥2 distinct tools on the\n"
        "        same file:line. `N corroborated` in a category header means\n"
        "        that many of the findings in that check carry the ★.\n";
    header += "---\n\n";

    // Attach the project's own standards docs when present. Gives the
    // Claude-review handoff enough context to weigh findings against
    // documented project rules rather than generic best practice.
    auto appendDocIfPresent = [this, &header](const QString &name, const QString &label) {
        const QString doc = readProjectDoc(name);
        if (doc.isEmpty()) return;
        header += QString("=== %1 (%2) ===\n").arg(label, name);
        header += doc;
        header += "\n\n";
    };
    appendDocIfPresent("CLAUDE.md",                       "Project conventions");
    appendDocIfPresent("docs/standards/coding.md",        "Coding standards");
    appendDocIfPresent("docs/standards/testing.md",       "Testing standards");
    appendDocIfPresent("docs/standards/commits.md",       "Commit standards");
    appendDocIfPresent("docs/standards/documentation.md", "Documentation standards");
    appendDocIfPresent("CONTRIBUTING.md",                 "Contributing guidelines");

    // 5-phase workflow scaffold for the downstream Claude session. Gives the
    // consumer a verification + approval gate rather than letting it plunge
    // straight into fixes — a common failure mode where regex false positives
    // get "fixed" into real bugs because nothing proved they were real first.
    header +=
        "=== How to process this report ===\n"
        "\n"
        "Follow these five phases in order. Don't jump to fixes.\n"
        "\n"
        "PHASE 1 — BASELINE\n"
        "  Run the existing test suite. Record pass/fail counts, build warnings,\n"
        "  lint output. If anything is broken before you start, surface that\n"
        "  first — don't proceed until the user acknowledges.\n"
        "\n"
        "PHASE 2 — VERIFY\n"
        "  For every BLOCKER, CRITICAL, and MAJOR finding, mark it VERIFIED,\n"
        "  UNCONFIRMED, or FALSE-POSITIVE with a one-line justification citing\n"
        "  file:line evidence. Criteria:\n"
        "    - Bug:       reproduce with a code trace or a failing test.\n"
        "    - Security:  confirm the pattern is exploitable in context\n"
        "                 (attacker-reachable data flow), not just a regex\n"
        "                 match on a keyword.\n"
        "    - Dead code: confirm no callers — including dynamic dispatch,\n"
        "                 registries, reflection, exported API, tests, and\n"
        "                 build scripts.\n"
        "  Confidence scores (0-100) are inline next to each finding: higher\n"
        "  means more tools corroborate it. Prefer triaging high-confidence\n"
        "  findings first.\n"
        "\n"
        "PHASE 3 — CITATIONS\n"
        "  For any dependency / CVE finding you intend to fix, cite an\n"
        "  authoritative URL (NVD, GitHub advisory, upstream changelog) and\n"
        "  cross-check the CVE's affected version range against the pinned\n"
        "  version in the lockfile. A CVE against a version lower than what's\n"
        "  pinned is FALSE-POSITIVE, not a fix target.\n"
        "\n"
        "PHASE 4 — APPROVAL GATE\n"
        "  Produce a findings list with file:line, severity, verification\n"
        "  status, proposed fix, and blast radius (files touched, public-API\n"
        "  impact). Wait for user approval before touching code for any\n"
        "  non-trivial finding. You may proceed directly on:\n"
        "    - unused imports / obviously dead local helpers with no callers\n"
        "    - typo'd conditionals that have a reproducing test\n"
        "    - formatting-only fixes for lint rules already enforced in CI\n"
        "\n"
        "PHASE 5 — IMPLEMENT + TEST\n"
        "  Fix root causes, not symptoms. No --no-verify, no swallowed\n"
        "  exceptions, no commented-out broken code, no capped loops that\n"
        "  hide a real divergence. If a workaround is genuinely unavoidable,\n"
        "  leave a comment documenting the constraint next to it.\n"
        "  Every behavioural fix gets a regression test — a fix without a\n"
        "  test will come back. Keep edits scoped to each finding; no\n"
        "  drive-by refactoring of surrounding code.\n"
        "  After fixes, re-run the full suite and include a pre/post diff of\n"
        "  tests passed/failed/skipped, build warnings, and finding counts\n"
        "  by severity.\n"
        "\n"
        "DELIVERABLE\n"
        "  (1) Findings list with VERIFIED / UNCONFIRMED / FALSE-POSITIVE tags\n"
        "  (2) Changes made — files touched, why, test evidence\n"
        "  (3) Deferred items — why deferred, what would unblock them\n"
        "  (4) Baseline comparison — tests / warnings / findings before vs after\n"
        "\n"
        "Be terse and concrete. Skip categories with no findings rather than\n"
        "writing \"none found\" filler for each.\n\n";

    header += "=== Findings ===\n\n";

    // Re-sort a copy for the plain-text report.
    std::vector<CheckResult> sorted(m_completedResults.begin(), m_completedResults.end());
    std::sort(sorted.begin(), sorted.end(), [](const CheckResult &a, const CheckResult &b) {
        if (a.severity != b.severity) return a.severity > b.severity;
        return a.checkName < b.checkName;
    });

    QString body;
    for (const auto &r : sorted) {
        if (r.warning) {
            body += QString("--- [%1] %2 (%3 / %4) [%5] ---\n(warning) %6\n\n")
                    .arg(severityLabel(r.severity), r.checkName,
                         typeLabel(r.type), r.category, r.source, r.output);
            continue;
        }
        if (r.findings.isEmpty() && r.omittedCount == 0) continue;

        // Count cross-tool-corroborated findings for the category header.
        // A finding is corroborated when ≥2 distinct tools flagged the same
        // file:line — the same signal that drives the ★ badge and boosts
        // computeConfidence(). Surfaced here so the downstream consumer can
        // prioritise triage without having to read every finding tag first.
        int corroborated = 0;
        for (const Finding &f : r.findings)
            if (f.highConfidence) ++corroborated;
        const QString corroTag = corroborated > 0
            ? QString(" (%1 corroborated)").arg(corroborated)
            : QString();

        body += QString("--- [%1] %2 (%3 / %4) [%5] — %6 finding(s)%7 ---\n")
                .arg(severityLabel(r.severity), r.checkName,
                     typeLabel(r.type), r.category, r.source)
                .arg(r.findings.size() + r.omittedCount)
                .arg(corroTag);
        for (const Finding &f : r.findings) {
            QString line;
            if (!f.file.isEmpty() && f.line >= 0)
                line = QString("%1:%2  %3").arg(f.file, QString::number(f.line)).arg(f.message);
            else if (!f.file.isEmpty())
                line = QString("%1  %2").arg(f.file, f.message);
            else
                line = f.message;
            // Inline tags kept compact so Claude's context budget doesn't get
            // eaten by boilerplate — priority order: confidence, high-conf ★,
            // blame, triage verdict.
            QStringList tags{QString("conf %1").arg(f.confidence)};
            if (f.highConfidence) tags << "★";
            if (!f.blameSha.isEmpty())
                tags << QString("%1 @ %2").arg(f.blameAuthor, f.blameDate);
            if (!f.aiVerdict.isEmpty())
                tags << QString("AI: %1 (%2)").arg(f.aiVerdict).arg(f.aiConfidence);
            body += line + "  " + QString("[%1] [%2]").arg(tags.join(" · "), f.dedupKey) + "\n";
            // Include a 3-line snippet when we have one; expensive on token
            // count but essential for Claude to propose targeted fixes.
            if (!f.snippet.isEmpty()) {
                const QStringList lines = f.snippet.split('\n');
                for (int i = 0; i < lines.size(); ++i) {
                    const int ln = f.snippetStart + i;
                    const QChar marker = (ln == f.line) ? QChar('>') : QChar(' ');
                    body += QString("    %1 %2| %3\n")
                                .arg(marker).arg(ln, 5).arg(lines[i]);
                }
            }
        }
        if (r.omittedCount > 0)
            body += QString("… and %1 more (capped at %2 per check)\n")
                    .arg(r.omittedCount).arg(kMaxFindingsPerCheck);
        body += "\n";
    }
    if (body.isEmpty()) body = "No issues found.\n";

    return header + body;
}
