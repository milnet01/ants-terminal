# ANTS-1723: workflow_state MCP Tool Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `workflow_state` MCP tool that lets superpowers skills store and retrieve their current step/phase as compact structured data, so a skill can resume from a fresh `/clear` session with one tool call instead of reconstructing state from full conversation history.

**Architecture:** New `cmdWorkflowState` in `RemoteControl` that delegates to `SessionMemoryEngine::execute` using a `wf.<skill>` key namespace (dot separator — slash is not valid in `^[A-Za-z0-9._-]{1,64}$`). Write ops (`set`, `clear`) enforce the ANTS-1435 RcGate confused-deputy guard, mirroring `cmdSessionMemory` exactly. Read ops (`get`) anchor to `caller_cwd` directly. The value payload is `{step, phase, notes, updated_at_ms}`. A lazy TTL purge (72 h) runs on every `set` call. Four-point registration: `remotecontrol.h` → `remotecontrol.cpp` → `claudeintegration.cpp` (schema + CallerCwdContract + kindForName + tokenCostFor) → `mainwindow.cpp`. Tests use GoogleTest in the existing `test_claude` bundle.

**Tech Stack:** Qt 6 / C++20. `SessionMemoryEngine`, `RcGate` (already in `remotecontrol.cpp` includes). No new deps.

---

### Task 1: Write the feature spec + failing test scaffold

**Files:**
- Create: `tests/features/mcp_workflow_state/spec.md`
- Create: `tests/features/mcp_workflow_state/test_mcp_workflow_state.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Create the spec**

```markdown
# mcp_workflow_state — Feature Spec (ANTS-1723)

## Purpose
`workflow_state` gives superpowers skills a per-project, per-skill
scratch-pad that survives `/clear`. Skills call `op:"get"` at the start
of a turn to resume from a compact "current step" snapshot.

## Storage
Same backing file as `session_memory`:
`~/.cache/ants-terminal/mcp-state/<sha256(cwd)>.json`.
Key namespace: `wf.<skill>` (e.g. `wf.tdd`, `wf.systematic-debugging`).
Note: dot separator — slash is not valid in the key charset.
Skill name: `^[A-Za-z0-9_-]{1,32}$`.
Value shape: `{step:int, phase:str, notes:str[], updated_at_ms:float64}`.

## ANTS-1435 write-gate
`set` and `clear` ops use `RcGate::checkCallerCwd` (same as
`session_memory`). Read ops (`get`) anchor to `caller_cwd` directly.

## Invariants

- **INV-1** `op:"get"` returns `{ok:true, found:true, state:{…}}` when
  an entry exists and `{ok:true, found:false}` when absent or expired.
- **INV-2** `op:"set"` stores `{step, phase, notes, updated_at_ms}`
  (server clock overwrites `updated_at_ms`) and returns `{ok:true}`.
- **INV-3** `op:"clear"` deletes the `wf.<skill>` entry and returns
  `{ok:true, deleted:true}` if it existed, `{ok:true, deleted:false}`
  if absent.
- **INV-4** A `wf.<skill>` entry whose `updated_at_ms` is older than
  72 h is treated as absent on `get` (returns `found:false, expired:true`).
- **INV-5** On every `set`, all `wf.` keys with `updated_at_ms` older
  than 72 h are deleted from the store (lazy TTL purge).
- **INV-6** `caller_cwd` absent → refuse `{ok:false, code:"cwd_missing"}`.
  Unresolvable → `{ok:false, code:"cwd_bad"}`. (mcp-error-codes.md §input-validation.)
- **INV-7** Invalid skill name (fails `^[A-Za-z0-9_-]{1,32}$`) → refuse
  `{ok:false, code:"bad_args", error:"workflow_state: invalid skill name"}`.
- **INV-8** `step` and `phase` are required on `set`. Missing either →
  `{ok:false, code:"bad_args"}`.
- **INV-9** Stored value payload ≤ 4 KiB (serialised). Exceeding →
  `{ok:false, code:"payload_too_large"}`.
- **INV-10** Keys stored as `wf.<skill>` (dot separator). They appear
  in `session_memory op:list` results distinguished by the `wf.` prefix.
```

- [ ] **Step 2: Create the test file (GoogleTest format)**

```cpp
// tests/features/mcp_workflow_state/test_mcp_workflow_state.cpp
// ANTS-1723 — workflow_state MCP tool feature conformance test.
#include <gtest/gtest.h>
#include <QFile>

// INV-6: CallerCwdContract::Required registered
TEST(McpWorkflowState, Inv6CallerCwdRequired) {
    QFile f(QString::fromUtf8(SRC_CLAUDE_INTEGRATION_CPP_PATH));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray txt = f.readAll();
    EXPECT_TRUE(txt.contains("workflow_state") && txt.contains("Required"))
        << "CallerCwdContract::Required must be registered for workflow_state";
}

// INV-1: "found" field emitted in cmdWorkflowState
TEST(McpWorkflowState, Inv1FoundFieldPresent) {
    QFile f(QString::fromUtf8(SRC_REMOTECONTROL_CPP_PATH));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray txt = f.readAll();
    EXPECT_TRUE(txt.contains("workflow_state") && txt.contains("\"found\""))
        << "found field missing in cmdWorkflowState";
}

// INV-4: 72h TTL logic present (kTtlMs constant)
TEST(McpWorkflowState, Inv4TtlLogicPresent) {
    QFile f(QString::fromUtf8(SRC_REMOTECONTROL_CPP_PATH));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray txt = f.readAll();
    // 72 * 3600 * 1000 = 259200000, or constexpr kTtlMs
    EXPECT_TRUE(txt.contains("kTtlMs") || txt.contains("259200000"))
        << "72h TTL constant missing in cmdWorkflowState";
    EXPECT_TRUE(txt.contains("updated_at_ms"))
        << "updated_at_ms field missing";
}

// INV-7: skill name regex present
TEST(McpWorkflowState, Inv7SkillNameRegex) {
    QFile f(QString::fromUtf8(SRC_REMOTECONTROL_CPP_PATH));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray txt = f.readAll();
    EXPECT_TRUE(txt.contains("A-Za-z0-9_-") && txt.contains("32"))
        << "skill name regex ^[A-Za-z0-9_-]{1,32}$ missing";
}

// INV-9: 4 KiB payload cap
TEST(McpWorkflowState, Inv9PayloadCap) {
    QFile f(QString::fromUtf8(SRC_REMOTECONTROL_CPP_PATH));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray txt = f.readAll();
    EXPECT_TRUE(txt.contains("payload_too_large"))
        << "payload_too_large refusal code missing";
}

// INV-10: wf. prefix used (dot, not slash)
TEST(McpWorkflowState, Inv10DotPrefixNotSlash) {
    QFile f(QString::fromUtf8(SRC_REMOTECONTROL_CPP_PATH));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray txt = f.readAll();
    EXPECT_FALSE(txt.contains("\"wf/\""))
        << "slash separator must not be used (not valid in key charset)";
    EXPECT_TRUE(txt.contains("\"wf.\"") || txt.contains("wf."))
        << "dot separator wf.<skill> must be used";
}
```

- [ ] **Step 3: Add to test_claude bundle in CMakeLists.txt**

Add inside `ants_add_gui_bundle(test_claude ...)` SOURCES, after the `mcp_session_brief` entry (or after `mcp_session_memory`):

```cmake
                tests/features/mcp_workflow_state/test_mcp_workflow_state.cpp
```

- [ ] **Step 4: Confirm test compiles and fails**

```bash
cmake --build build --target test_claude 2>&1 | tail -5
ctest --test-dir build -R McpWorkflowState --output-on-failure 2>&1 | tail -10
```

Expected: compiles, tests FAIL.

---

### Task 2: Declare cmdWorkflowState in RemoteControl

**Files:**
- Modify: `src/remotecontrol.h`

- [ ] **Step 1: Add declaration**

Find `QJsonDocument cmdSessionMemory(const QJsonObject &req);`. Add after:
```cpp
    QJsonDocument cmdWorkflowState(const QJsonObject &req);
```

- [ ] **Step 2: Build**

```bash
cmake --build build --target ants-terminal 2>&1 | tail -5
```

---

### Task 3: Implement cmdWorkflowState in remotecontrol.cpp

**Files:**
- Modify: `src/remotecontrol.cpp` (after `cmdSessionMemory` closing `}`)

- [ ] **Step 1: Find insertion point**

```bash
grep -n "^QJsonDocument RemoteControl::cmdSessionMemory\|^// -----" src/remotecontrol.cpp | grep -A1 "cmdSessionMemory" | head -5
```

Insert immediately after the closing `}` of `cmdSessionMemory`.

- [ ] **Step 2: Verify `RcGate::checkCallerCwd` signature**

```bash
grep -n "checkCallerCwd\|struct.*Gate\|GateResult" src/remotecontrol.h src/rcgate.h 2>/dev/null | head -10
```

This confirms the call shape used in the write path below.

- [ ] **Step 3: Write cmdWorkflowState**

```cpp
// ANTS-1723 — workflow_state: per-project, per-skill step/phase store.
// Uses session_memory's backing store with "wf.<skill>" key namespace.
// Write ops use the ANTS-1435 RcGate (same as session_memory).
// Read ops anchor to caller_cwd directly.
// 72 h lazy-TTL purge on every set. 4 KiB payload cap.
QJsonDocument RemoteControl::cmdWorkflowState(const QJsonObject &req)
{
    if (!m_main) {
        return QJsonDocument(csErr(QStringLiteral("no_window"),
            QStringLiteral("workflow_state: no MainWindow")));
    }

    // --- parse op ---
    const QString opRaw = req.value(QStringLiteral("op")).toString();
    enum class Op { Get, Set, Clear };
    Op op;
    if      (opRaw == QStringLiteral("get"))   op = Op::Get;
    else if (opRaw == QStringLiteral("set"))   op = Op::Set;
    else if (opRaw == QStringLiteral("clear")) op = Op::Clear;
    else {
        return QJsonDocument(csErr(QStringLiteral("bad_args"),
            QStringLiteral("workflow_state: op must be get/set/clear")));
    }

    // --- validate skill name: ^[A-Za-z0-9_-]{1,32}$ ---
    const QString skill = req.value(QStringLiteral("skill")).toString();
    static const QRegularExpression kSkillRe(
        QStringLiteral("^[A-Za-z0-9_-]{1,32}$"));
    if (!kSkillRe.match(skill).hasMatch()) {
        return QJsonDocument(csErr(QStringLiteral("bad_args"),
            QStringLiteral("workflow_state: invalid skill name — "
                           "must match ^[A-Za-z0-9_-]{1,32}$")));
    }

    // --- key: "wf.<skill>" — dot separator, valid in key charset ---
    const QString key = QStringLiteral("wf.") + skill;

    // --- TTL constant: 72 h in milliseconds ---
    constexpr qint64 kTtlMs = 72LL * 3600LL * 1000LL;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    // ----------------------------------------------------------------
    // GET — read op: anchor to caller_cwd directly (ANTS-1435).
    // ----------------------------------------------------------------
    if (op == Op::Get) {
        const QString rawCaller =
            req.value(QStringLiteral("caller_cwd")).toString();
        if (rawCaller.isEmpty()) {
            return QJsonDocument(csErr(QStringLiteral("cwd_missing"),
                QStringLiteral("workflow_state: caller_cwd required")));
        }
        const QFileInfo fi(rawCaller);
        const QString canon = fi.canonicalFilePath();
        if (canon.isEmpty() || !QFileInfo(canon).isDir()) {
            return QJsonDocument(csErr(QStringLiteral("cwd_bad"),
                QStringLiteral("workflow_state: caller_cwd unresolvable")));
        }
        SessionMemoryEngine::OpResult r =
            SessionMemoryEngine::execute(canon,
                SessionMemoryEngine::Op::Get, key, QJsonValue());
        if (!r.ok) return QJsonDocument(csErr(r.code, r.error));
        if (!r.found) {
            QJsonObject out;
            out[QStringLiteral("ok")]    = true;
            out[QStringLiteral("found")] = false;
            return QJsonDocument(out);
        }
        // Check 72 h TTL.
        const qint64 updatedAt =
            r.value.toObject()
                .value(QStringLiteral("updated_at_ms")).toDouble(0);
        if (updatedAt > 0 && (nowMs - updatedAt) > kTtlMs) {
            QJsonObject out;
            out[QStringLiteral("ok")]      = true;
            out[QStringLiteral("found")]   = false;
            out[QStringLiteral("expired")] = true;
            return QJsonDocument(out);
        }
        QJsonObject out;
        out[QStringLiteral("ok")]    = true;
        out[QStringLiteral("found")] = true;
        out[QStringLiteral("state")] = r.value;
        return QJsonDocument(out);
    }

    // ----------------------------------------------------------------
    // CLEAR / SET — write ops: enforce RcGate (ANTS-1435).
    // ----------------------------------------------------------------
    const auto gate = RcGate::checkCallerCwd(
        resolveRootCanonical(m_main), req,
        QStringLiteral("workflow_state"));
    if (!gate.ok) return QJsonDocument(RcGate::gateErrorEnvelope(gate));
    const QString cwd = gate.focused;

    // --- CLEAR ---
    if (op == Op::Clear) {
        // Issue Delete directly; r.found tells us if the key existed.
        SessionMemoryEngine::OpResult r =
            SessionMemoryEngine::execute(cwd,
                SessionMemoryEngine::Op::Delete, key, QJsonValue());
        if (!r.ok) return QJsonDocument(csErr(r.code, r.error));
        QJsonObject out;
        out[QStringLiteral("ok")]      = true;
        out[QStringLiteral("deleted")] = r.found;
        return QJsonDocument(out);
    }

    // --- SET ---
    // Validate required fields: step (int) and phase (non-empty string).
    const QJsonValue stepVal  = req.value(QStringLiteral("step"));
    const QString    phase    = req.value(QStringLiteral("phase")).toString();
    if (!stepVal.isDouble() || phase.isEmpty()) {
        return QJsonDocument(csErr(QStringLiteral("bad_args"),
            QStringLiteral("workflow_state: set requires step (int) "
                           "and phase (non-empty string)")));
    }
    const QJsonArray notes = req.value(QStringLiteral("notes")).toArray();

    // Build the stored value; always overwrite updated_at_ms with server clock.
    QJsonObject state;
    state[QStringLiteral("step")]          = static_cast<int>(stepVal.toDouble());
    state[QStringLiteral("phase")]         = phase;
    state[QStringLiteral("notes")]         = notes;
    state[QStringLiteral("updated_at_ms")] = static_cast<double>(nowMs);

    // Payload cap: 4 KiB serialised.
    const QByteArray payload =
        QJsonDocument(state).toJson(QJsonDocument::Compact);
    if (payload.size() > 4096) {
        return QJsonDocument(csErr(QStringLiteral("payload_too_large"),
            QStringLiteral("workflow_state: state payload exceeds 4 KiB")));
    }

    // Lazy TTL purge: delete all wf. keys older than 72 h before writing.
    {
        SessionMemoryEngine::OpResult listR =
            SessionMemoryEngine::execute(cwd,
                SessionMemoryEngine::Op::List,
                QString(), QJsonValue());
        if (listR.ok) {
            for (const QJsonValue &kv : listR.keys) {
                // OpResult::keys is [{key:str, bytes:N}, ...] for List.
                const QString k =
                    kv.toObject().value(QStringLiteral("key")).toString();
                if (!k.startsWith(QStringLiteral("wf."))) continue;
                SessionMemoryEngine::OpResult getR =
                    SessionMemoryEngine::execute(cwd,
                        SessionMemoryEngine::Op::Get, k, QJsonValue());
                if (!getR.ok || !getR.found) continue;
                const qint64 ts =
                    getR.value.toObject()
                        .value(QStringLiteral("updated_at_ms"))
                        .toDouble(0);
                if (ts > 0 && (nowMs - ts) > kTtlMs) {
                    SessionMemoryEngine::execute(cwd,
                        SessionMemoryEngine::Op::Delete, k, QJsonValue());
                }
            }
        }
    }

    // Write the new state.
    SessionMemoryEngine::OpResult wr =
        SessionMemoryEngine::execute(cwd,
            SessionMemoryEngine::Op::Set, key, QJsonValue(state));
    if (!wr.ok) return QJsonDocument(csErr(wr.code, wr.error));

    QJsonObject out;
    out[QStringLiteral("ok")] = true;
    return QJsonDocument(out);
}
```

- [ ] **Step 4: Verify `RcGate::checkCallerCwd` result fields**

```bash
grep -n "errorCode\|focused\b\|struct.*GateResult\|struct.*CheckResult" src/remotecontrol.h src/rcgate.h 2>/dev/null | head -10
```

If the result fields are named differently (e.g. `error` instead of `errorCode`, `cwd` instead of `focused`), update the two lines:
```cpp
if (!gate.ok) return QJsonDocument(csErr(gate.errorCode, gate.error));
const QString cwd = gate.focused;
```

- [ ] **Step 5: Build**

```bash
cmake --build build --target ants-terminal 2>&1 | tail -10
```

Expected: clean. If `QRegularExpression` is missing, add `#include <QRegularExpression>` near the top of `remotecontrol.cpp`.

---

### Task 4: Register in claudeintegration.cpp

**Files:**
- Modify: `src/claudeintegration.cpp`

- [ ] **Step 1: CallerCwdContract entry**

Find `if (toolName == QStringLiteral("session_memory")) return C::Required;`. Add after:
```cpp
    if (toolName == QStringLiteral("workflow_state"))     return C::Required;
```

- [ ] **Step 2: kindForName entry**

Find `if (name == QLatin1String("session_memory")) return QStringLiteral("mcp-state");`. Add after:
```cpp
                    if (name == QLatin1String("workflow_state"))
                        return QStringLiteral("mcp-state");
```

- [ ] **Step 3: tokenCostFor entry**

Find `{QStringLiteral("session_memory"), {200, 1000}}`. Add alongside:
```cpp
                        {QStringLiteral("workflow_state"),    {200,  1000}},
```

- [ ] **Step 4: Add tools/list schema**

Find where the `session_memory` schema block ends (`tools.append(t)`). Add after:

```cpp
                {
                    QJsonObject t;
                    t["name"] = "workflow_state";
                    t["selection_hint"] = QStringLiteral(
                        "Use to persist skill step/phase across /clear. "
                        "Call op:\"get\" at session start to resume.");
                    t["description"] = QStringLiteral(
                        "Per-project, per-skill step/phase store for "
                        "superpowers skills. Survives /clear — call "
                        "op:\"get\" at session start to resume from the "
                        "last saved step. ops: get (returns {found,state?}), "
                        "set (stores {step,phase,notes}), clear (deletes). "
                        "Entries expire after 72 h of inactivity. "
                        "skill: ^[A-Za-z0-9_-]{1,32}$ "
                        "(e.g. \"tdd\", \"systematic-debugging\"). "
                        "Keys stored as wf.<skill> in session_memory "
                        "backing store. Write ops (set/clear) require "
                        "focused-tab cwd match (ANTS-1435). "
                        "ANTS-1723.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    schema["additionalProperties"] = false;
                    QJsonObject opProp;
                    opProp["type"] = "string";
                    QJsonArray opEnum;
                    opEnum.append(QStringLiteral("get"));
                    opEnum.append(QStringLiteral("set"));
                    opEnum.append(QStringLiteral("clear"));
                    opProp["enum"] = opEnum;
                    opProp["description"] = QStringLiteral("get/set/clear.");
                    QJsonObject cwdProp;
                    cwdProp["type"] = "string";
                    cwdProp["description"] = QStringLiteral("Your $PWD (required).");
                    QJsonObject skillProp;
                    skillProp["type"] = "string";
                    skillProp["description"] = QStringLiteral(
                        "Skill identifier, ^[A-Za-z0-9_-]{1,32}$. "
                        "E.g. \"tdd\", \"systematic-debugging\".");
                    QJsonObject stepProp;
                    stepProp["type"] = "integer";
                    stepProp["description"] =
                        QStringLiteral("Current step number (required for set).");
                    QJsonObject phaseProp;
                    phaseProp["type"] = "string";
                    phaseProp["description"] =
                        QStringLiteral("Current phase label (required for set).");
                    QJsonObject notesProp;
                    notesProp["type"] = "array";
                    QJsonObject notesItems;
                    notesItems["type"] = "string";
                    notesProp["items"] = notesItems;
                    notesProp["description"] =
                        QStringLiteral("Optional carry-forward notes.");
                    QJsonObject props;
                    props["op"]         = opProp;
                    props["caller_cwd"] = cwdProp;
                    props["skill"]      = skillProp;
                    props["step"]       = stepProp;
                    props["phase"]      = phaseProp;
                    props["notes"]      = notesProp;
                    schema["properties"] = props;
                    QJsonArray reqArr;
                    reqArr.append(QStringLiteral("op"));
                    reqArr.append(QStringLiteral("caller_cwd"));
                    reqArr.append(QStringLiteral("skill"));
                    schema["required"] = reqArr;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
```

- [ ] **Step 5: Build**

```bash
cmake --build build --target ants-terminal 2>&1 | tail -10
```

---

### Task 5: Register in mainwindow.cpp

**Files:**
- Modify: `src/mainwindow.cpp`

- [ ] **Step 1: Add registerToolProvider**

Find `registerToolProvider("session_memory", ...` (or `session_brief` if that was already added). Add after:

```cpp
m_claudeIntegration->registerToolProvider("workflow_state",
    ClaudeIntegration::CallerCwdContract::Required,
    [this](const QJsonObject &args) -> QString {
        if (!m_remoteControl) return QString::fromUtf8(kRcUnavailable);
        return QString::fromUtf8(
            m_remoteControl->cmdWorkflowState(args)
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
ctest --test-dir build -R McpWorkflowState --output-on-failure 2>&1 | tail -20
```

Expected: all PASS.

- [ ] **Step 2: Run full suite**

```bash
ctest --test-dir build -L features --output-on-failure 2>&1 | tail -20
```

- [ ] **Step 3: Commit**

```bash
git add src/remotecontrol.h src/remotecontrol.cpp \
        src/claudeintegration.cpp src/mainwindow.cpp \
        tests/features/mcp_workflow_state/ CMakeLists.txt
git commit -m "$(cat <<'EOF'
ANTS-1723: add workflow_state MCP tool — superpowers skill step/phase store.

Lets skills persist current step+phase between /clear sessions.
Uses session_memory backing store with wf.<skill> key namespace (dot separator).
72 h lazy-TTL purge on every set. 4 KiB payload cap.
Write ops guarded by ANTS-1435 RcGate (same as session_memory).

Co-Authored-By: Claude Sonnet 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Self-Review

**All cold-eyes loop-1 findings applied:**
- C-1: `wf.` dot separator throughout (spec, implementation, purge loop, tests) ✓
- C-2: RcGate on `set` and `clear` ops, read-path uses direct `caller_cwd` ✓
- H-1: Purge loop uses `.toObject().value("key").toString()` ✓
- H-2: `additionalProperties:false` + `selection_hint` in schema ✓
- H-3: `kindForName` entry → `"mcp-state"` ✓
- M-1: `tokenCostFor` entry `{200, 1000}` ✓
- M-2: `clear` uses single Delete (r.found for `deleted` field) ✓
- M-3: Error codes `cwd_missing` / `cwd_bad` (not `no_project`) ✓
- L-1: `session_brief` anchor removed; `session_memory` used ✓

**RcGate error pattern (loop 2 fix):** The write-gate error return uses `RcGate::gateErrorEnvelope(gate)` — consistent with all other `remotecontrol.cpp` call sites (lines 6733, 7140, 7207, 7477, 7770, 8277). Do NOT use `csErr(gate.errorCode, gate.error)` which produces a non-conforming envelope.

**RcGate field `gate.focused`:** Verify against the actual `GateResult` struct — the plan uses `gate.focused` for the focused-tab cwd. If the struct uses a different name, update Task 3 Step 3 accordingly.
