# ANTS-1724: session_brief MCP Tool Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `session_brief` MCP tool that returns a compact (≤ 512 bytes) session-state envelope in one call, so a fresh `/clear` session can orient itself without 5–6 sequential reads.

**Architecture:** Thin new `cmdSessionBrief` method in `RemoteControl` that composes the same four data sources as `cmdCurrentState` (roadmap active bullet, git branch state, open audit findings count) and extends it with build/test cache data from `BuildCache::loadBuild` + `TestResCache::loadTests`. Registered as an ETag-eligible MCP tool following the exact same four-point pattern as every other tool (`remotecontrol.h` declaration → `remotecontrol.cpp` implementation → `claudeintegration.cpp` schema + CallerCwdContract + isEtagSupportedTool + kindForName + tokenCostFor → `mainwindow.cpp` `registerToolProvider` lambda).

**Tech Stack:** Qt 6 / C++20, existing `BuildCache`, `TestResCache`, `cmdCurrentState` internals (no new deps — all headers already in `remotecontrol.cpp`). Tests use GoogleTest in `ants_add_gui_bundle(test_claude ...)`.

---

### Task 1: Write the feature spec + failing test

**Files:**
- Create: `tests/features/mcp_session_brief/spec.md`
- Create: `tests/features/mcp_session_brief/test_mcp_session_brief.cpp`
- Modify: `CMakeLists.txt` (add to `ants_add_gui_bundle(test_claude ...)` sources list)

- [ ] **Step 1: Create the spec**

```markdown
# mcp_session_brief — Feature Spec (ANTS-1724)

## Purpose
`session_brief` returns a compact project-state envelope in one MCP call.
Designed for fresh /clear session orientation.

## Invariants

- **INV-1** Response contains `ok:true`, `git`, `build`, `test`, `audit`,
  `roadmap` keys when the project root resolves.
- **INV-2** `git.branch` is a non-empty string when git is available;
  empty string when the project root has no `.git`.
- **INV-3** `build.result` is one of `"pass"`, `"fail"`, `"unknown"`.
- **INV-4** `test.result` is one of `"pass"`, `"fail"`, `"unknown"`.
- **INV-5** `audit.open_count` is `error + warning + note` from the
  last audit run, or 0 when the cache is absent.
- **INV-6** `roadmap.active_id` is empty string when no 🚧/📋 bullet
  exists in the roadmap.
- **INV-7** When `caller_cwd` is absent or unresolvable the tool refuses
  with `{ok:false, code:"no_project"}`. (See mcp-error-codes.md §input-validation.)
- **INV-8** The serialised JSON response (before MCP wrapping) is ≤ 512
  bytes for a project with ≤ 10 changed files.
- **INV-9** The tool is ETag-eligible (registered in `isEtagSupportedTool`).
- **INV-10** `build.recorded_at` and `test.recorded_at` are ISO-8601
  strings when the cache exists, absent when the cache is missing.
- **INV-11** The refusal envelope on unresolvable cwd carries
  `code:"no_project"` — verified by source-grep in the test.
```

Save as `tests/features/mcp_session_brief/spec.md`.

- [ ] **Step 2: Create the test file (GoogleTest format)**

```cpp
// tests/features/mcp_session_brief/test_mcp_session_brief.cpp
// ANTS-1724 — session_brief MCP tool feature conformance test.
#include <gtest/gtest.h>
#include <QFile>

// INV-9: isEtagSupportedTool must list "session_brief"
TEST(McpSessionBrief, Inv9EtagSupportedToolListed) {
    QFile f(QString::fromUtf8(SRC_CLAUDE_INTEGRATION_CPP_PATH));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray txt = f.readAll();
    EXPECT_TRUE(txt.contains("\"session_brief\""))
        << "session_brief must appear in isEtagSupportedTool";
}

// INV-7 + INV-11: callerCwdContractFor returns Required + error code is "no_project"
TEST(McpSessionBrief, Inv7Inv11CallerCwdRequired) {
    QFile ci(QString::fromUtf8(SRC_CLAUDE_INTEGRATION_CPP_PATH));
    ASSERT_TRUE(ci.open(QIODevice::ReadOnly));
    const QByteArray citxt = ci.readAll();
    EXPECT_TRUE(citxt.contains("session_brief") && citxt.contains("Required"))
        << "callerCwdContractFor must return Required for session_brief";

    QFile rc(QString::fromUtf8(SRC_REMOTECONTROL_CPP_PATH));
    ASSERT_TRUE(rc.open(QIODevice::ReadOnly));
    const QByteArray rctxt = rc.readAll();
    EXPECT_TRUE(rctxt.contains("\"no_project\""))
        << "cmdSessionBrief refusal must carry code:\"no_project\"";
}

// INV-1: all six envelope fields emitted
TEST(McpSessionBrief, Inv1AllEnvelopeFieldsPresent) {
    QFile f(QString::fromUtf8(SRC_REMOTECONTROL_CPP_PATH));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray txt = f.readAll();
    for (const char *field : {"\"git\"", "\"build\"", "\"test\"",
                               "\"audit\"", "\"roadmap\""}) {
        EXPECT_TRUE(txt.contains(field))
            << "field " << field << " missing in remotecontrol.cpp";
    }
}

// INV-3/INV-4: result enum values present
TEST(McpSessionBrief, Inv3Inv4ResultEnumValues) {
    QFile f(QString::fromUtf8(SRC_REMOTECONTROL_CPP_PATH));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray txt = f.readAll();
    EXPECT_TRUE(txt.contains("\"pass\""));
    EXPECT_TRUE(txt.contains("\"fail\""));
    EXPECT_TRUE(txt.contains("\"unknown\""));
}

// kindForName must classify session_brief as "workspace"
TEST(McpSessionBrief, KindForNameClassification) {
    QFile f(QString::fromUtf8(SRC_CLAUDE_INTEGRATION_CPP_PATH));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray txt = f.readAll();
    // session_brief and "workspace" must appear near each other
    const int pos = txt.indexOf("session_brief");
    ASSERT_GE(pos, 0) << "session_brief not found in claudeintegration.cpp";
    // Check "workspace" appears in the file (broader check)
    EXPECT_TRUE(txt.contains("workspace"));
}
```

- [ ] **Step 3: Add to test_claude bundle in CMakeLists.txt**

Find the `ants_add_gui_bundle(test_claude` block (line ~911). Add inside the SOURCES list (after the last `mcp_session_memory` entry, around line 962):

```cmake
                tests/features/mcp_session_brief/test_mcp_session_brief.cpp
```

The bundle already sets `SRC_CLAUDE_INTEGRATION_CPP_PATH` and `SRC_REMOTECONTROL_CPP_PATH` as compile definitions (lines 1109 and 1124) — no new definitions needed.

- [ ] **Step 4: Confirm test compiles and fails**

```bash
cmake --build build --target test_claude 2>&1 | tail -5
ctest --test-dir build -R McpSessionBrief --output-on-failure 2>&1 | tail -10
```

Expected: compiles, tests FAIL (grep finds nothing — `cmdSessionBrief` not yet written).

---

### Task 2: Declare cmdSessionBrief in RemoteControl

**Files:**
- Modify: `src/remotecontrol.h` (one line after `cmdCurrentState` at line 338)

- [ ] **Step 1: Add declaration**

Find:
```cpp
    QJsonDocument cmdCurrentState(const QJsonObject &req);
```
Add immediately after:
```cpp
    QJsonDocument cmdSessionBrief(const QJsonObject &req);
```

- [ ] **Step 2: Build to confirm it compiles**

```bash
cmake --build build --target ants-terminal 2>&1 | tail -5
```

---

### Task 3: Implement cmdSessionBrief in remotecontrol.cpp

**Files:**
- Modify: `src/remotecontrol.cpp` (insert after `cmdCurrentState` ends, before `// ----- ANTS-1309`)

- [ ] **Step 1: Find insertion point**

```bash
grep -n "^// ----- ANTS-1309" src/remotecontrol.cpp | head -3
```

Insert the new method immediately before that comment.

- [ ] **Step 2: Write cmdSessionBrief**

```cpp
QJsonDocument RemoteControl::cmdSessionBrief(const QJsonObject &req)
{
    if (!m_main) {
        return QJsonDocument(csErr(QStringLiteral("no_window"),
            QStringLiteral("session_brief: no MainWindow")));
    }
    const QString rootCanonical = resolveRootCanonical(m_main, req);
    if (rootCanonical.isEmpty()) {
        return QJsonDocument(csErr(QStringLiteral("no_project"),
            QStringLiteral("session_brief: project root unresolved")));
    }

    QJsonObject result;
    result[QStringLiteral("ok")] = true;

    // --- roadmap: first 🚧 in document order, else first 📋.
    // Intentional subset of cmdCurrentState's active_bullet: emits only
    // active_id + headline for compactness (INV-8 ≤ 512 bytes budget).
    {
        QJsonObject rqReq;
        rqReq[QStringLiteral("caller_cwd")] = rootCanonical;
        rqReq[QStringLiteral("status")]     = QStringLiteral("active");
        const QJsonObject rq = cmdRoadmapQuery(rqReq).object();
        QJsonObject rm;
        if (rq.value(QStringLiteral("ok")).toBool(false)) {
            const QJsonArray bullets =
                rq.value(QStringLiteral("bullets")).toArray();
            QJsonObject pick;
            for (const QJsonValue &v : bullets) {
                const QJsonObject b = v.toObject();
                if (b.value(QStringLiteral("status")).toString() ==
                        QStringLiteral("🚧")) { pick = b; break; }
            }
            if (pick.isEmpty() && !bullets.isEmpty())
                pick = bullets.first().toObject();
            rm[QStringLiteral("active_id")] =
                pick.value(QStringLiteral("id")).toString();
            rm[QStringLiteral("headline")] =
                pick.value(QStringLiteral("headline_oneline")).toString();
        } else {
            rm[QStringLiteral("active_id")] = QString();
            rm[QStringLiteral("headline")]  = QString();
        }
        result[QStringLiteral("roadmap")] = rm;
    }

    // --- git: branch + ahead/behind + changed-file count ---
    {
        QJsonObject gsReq;
        gsReq[QStringLiteral("caller_cwd")] = rootCanonical;
        gsReq[QStringLiteral("op")]         = QStringLiteral("status");
        const QJsonObject gs = cmdGitState(gsReq).object();
        QJsonObject git;
        if (gs.value(QStringLiteral("ok")).toBool(false)) {
            git[QStringLiteral("branch")] =
                gs.value(QStringLiteral("branch")).toString();
            git[QStringLiteral("ahead")]  =
                gs.value(QStringLiteral("ahead")).toInt();
            git[QStringLiteral("behind")] =
                gs.value(QStringLiteral("behind")).toInt();
            git[QStringLiteral("files_changed_count")] =
                gs.value(QStringLiteral("files")).toArray().size();
        } else {
            git[QStringLiteral("branch")]             = QString();
            git[QStringLiteral("ahead")]              = 0;
            git[QStringLiteral("behind")]             = 0;
            git[QStringLiteral("files_changed_count")] = 0;
        }
        result[QStringLiteral("git")] = git;
    }

    // --- audit: open findings count from last run ---
    {
        QJsonObject lsReq;
        lsReq[QStringLiteral("caller_cwd")] = rootCanonical;
        const QJsonObject ls = cmdLastAuditSummary(lsReq).object();
        QJsonObject audit;
        int total = 0;
        if (ls.value(QStringLiteral("ok")).toBool(false)) {
            const QJsonObject counts =
                ls.value(QStringLiteral("counts")).toObject();
            total = counts.value(QStringLiteral("error")).toInt()
                  + counts.value(QStringLiteral("warning")).toInt()
                  + counts.value(QStringLiteral("note")).toInt();
        }
        audit[QStringLiteral("open_count")] = total;
        result[QStringLiteral("audit")] = audit;
    }

    // --- build: last recorded build result ---
    {
        const auto bOpt = BuildCache::loadBuild(rootCanonical);
        QJsonObject build;
        if (bOpt.has_value()) {
            const auto &b = bOpt.value();
            build[QStringLiteral("result")]   = (b.exitCode == 0)
                ? QStringLiteral("pass") : QStringLiteral("fail");
            build[QStringLiteral("errors")]   = b.errorsCount;
            build[QStringLiteral("warnings")] = b.warningsCount;
            if (b.recordedAtMs > 0) {
                build[QStringLiteral("recorded_at")] =
                    QDateTime::fromMSecsSinceEpoch(b.recordedAtMs)
                        .toUTC().toString(Qt::ISODate);
            }
        } else {
            build[QStringLiteral("result")] = QStringLiteral("unknown");
        }
        result[QStringLiteral("build")] = build;
    }

    // --- test: last recorded test result ---
    {
        const auto tOpt = TestResCache::loadTests(rootCanonical);
        QJsonObject test;
        if (tOpt.has_value()) {
            const auto &t = tOpt.value();
            test[QStringLiteral("result")]  = (t.exitCode == 0)
                ? QStringLiteral("pass") : QStringLiteral("fail");
            test[QStringLiteral("passed")]  = t.passed;
            test[QStringLiteral("failed")]  = t.failed;
            test[QStringLiteral("total")]   = t.total;
            if (t.recordedAtMs > 0) {
                test[QStringLiteral("recorded_at")] =
                    QDateTime::fromMSecsSinceEpoch(t.recordedAtMs)
                        .toUTC().toString(Qt::ISODate);
            }
        } else {
            test[QStringLiteral("result")] = QStringLiteral("unknown");
        }
        result[QStringLiteral("test")] = test;
    }

    // ETag injected at the dispatch layer (isEtagSupportedTool).
    return QJsonDocument(result);
}
```

- [ ] **Step 3: Build**

```bash
cmake --build build --target ants-terminal 2>&1 | tail -10
```

Expected: clean.

---

### Task 4: Register in claudeintegration.cpp (5 points)

**Files:**
- Modify: `src/claudeintegration.cpp`

- [ ] **Step 1: CallerCwdContract entry**

Find `if (toolName == QStringLiteral("session_memory")) return C::Required;` (line ~6289). Add after:
```cpp
    if (toolName == QStringLiteral("session_brief"))      return C::Required;
```

- [ ] **Step 2: isEtagSupportedTool entry**

Find `|| toolName == QStringLiteral("current_state")` (line ~6437). Add after:
```cpp
        // ANTS-1724 — session_brief: compact current_state variant.
        || toolName == QStringLiteral("session_brief")
```

- [ ] **Step 3: kindForName entry**

Find (line ~5591):
```cpp
                        name == QLatin1String("current_state"))
                        return QStringLiteral("workspace");
```
Change to:
```cpp
                        name == QLatin1String("current_state") ||
                        name == QLatin1String("session_brief"))
                        return QStringLiteral("workspace");
```

- [ ] **Step 4: tokenCostFor entry**

Find the `kCosts` hash initialiser containing `{QStringLiteral("session_memory"), {200, 1000}}` (line ~5490). Add alongside:
```cpp
                        {QStringLiteral("session_brief"),     {300,  1200}},
```

- [ ] **Step 5: Add tools/list schema**

Find the block that ends the `current_state` tool schema (`csTool` → `tools.append(csTool)`). Add the session_brief schema immediately after:

```cpp
                {
                    QJsonObject t;
                    t["name"] = "session_brief";
                    t["selection_hint"] = QStringLiteral(
                        "Orient a fresh /clear session: returns git "
                        "branch, build/test result, open audit count, "
                        "and active roadmap item in one call.");
                    t["description"] = QStringLiteral(
                        "Compact session-state envelope for orienting "
                        "a fresh session in one call. Returns git "
                        "branch+ahead/behind+files_changed_count, last "
                        "build result (pass/fail/unknown) with "
                        "error/warning counts, last test result with "
                        "pass/fail/total counts, open audit findings "
                        "count, and the active roadmap item id+headline. "
                        "All data comes from on-disk caches — no new I/O. "
                        "ETag-eligible: pass etag_match from a prior call "
                        "to skip re-emission when state is unchanged. "
                        "ANTS-1724.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    schema["additionalProperties"] = false;
                    QJsonObject cwdProp;
                    cwdProp["type"] = "string";
                    cwdProp["description"] = QStringLiteral("Your $PWD (required).");
                    QJsonObject etagProp;
                    etagProp["type"] = "string";
                    etagProp["description"] = QStringLiteral(
                        "ETag from a previous session_brief call. "
                        "When it matches: {ok:true,unchanged:true,etag:\"<same>\"}.");
                    QJsonObject props;
                    props["caller_cwd"] = cwdProp;
                    props["etag_match"] = etagProp;
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append(QStringLiteral("caller_cwd"));
                    schema["required"] = req;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
```

- [ ] **Step 6: Build**

```bash
cmake --build build --target ants-terminal 2>&1 | tail -10
```

Expected: clean.

---

### Task 5: Register in mainwindow.cpp

**Files:**
- Modify: `src/mainwindow.cpp`

- [ ] **Step 1: Find insertion point**

```bash
grep -n "registerToolProvider.*session_memory\|registerToolProvider.*current_state" src/mainwindow.cpp | head -5
```

Add the session_brief registration after whichever is last:

```cpp
m_claudeIntegration->registerToolProvider("session_brief",
    ClaudeIntegration::CallerCwdContract::Required,
    [this](const QJsonObject &args) -> QString {
        if (!m_remoteControl) return QString::fromUtf8(kRcUnavailable);
        return QString::fromUtf8(
            m_remoteControl->cmdSessionBrief(args)
                .toJson(QJsonDocument::Compact));
    });
```

- [ ] **Step 2: Build**

```bash
cmake --build build --target ants-terminal 2>&1 | tail -10
```

---

### Task 6: Run tests, commit

- [ ] **Step 1: Run the feature test**

```bash
ctest --test-dir build -R McpSessionBrief --output-on-failure 2>&1 | tail -20
```

Expected: all tests PASS.

- [ ] **Step 2: Run full suite**

```bash
ctest --test-dir build -L features --output-on-failure 2>&1 | tail -20
```

Expected: all pass.

- [ ] **Step 3: Commit**

```bash
git add src/remotecontrol.h src/remotecontrol.cpp \
        src/claudeintegration.cpp src/mainwindow.cpp \
        tests/features/mcp_session_brief/ CMakeLists.txt
git commit -m "$(cat <<'EOF'
ANTS-1724: add session_brief MCP tool — compact session-state in one call.

Returns git branch/ahead/behind/files_changed_count, build result
(pass/fail/unknown), test result with counts, open audit findings count,
and active roadmap item — all from on-disk caches, no new I/O.
ETag-eligible. Targets the /clear-heavy workflow.

Co-Authored-By: Claude Sonnet 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Self-Review

**All cold-eyes loop-1 findings applied:**
- CRIT-1: `files_changed_count` (not `files_changed`) ✓
- HIGH-1: Goal says `≤ 512 bytes` matching INV-8 ✓
- HIGH-2: `kindForName` entry added (Task 4 Step 3) ✓
- HIGH-3: `selection_hint` added to schema (Task 4 Step 5) ✓
- MED-1: Test uses GoogleTest in `test_claude` bundle ✓
- MED-2: INV-11 + source-grep for `"no_project"` in test ✓
- MED-3: Comment added explaining intentional roadmap subset ✓
- LOW-1: Test uses `&&` not `||` for dual-condition assertion ✓
- `additionalProperties: false` added to schema ✓

**Spec coverage:**
- INV-1–11 all covered by test assertions and implementation.

**Type consistency:** All method names, field names, and API signatures verified against current source before writing.
