# ANTS-1226: Model Recommender Chip (Shape A) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a passive "model recommender" chip to the Ants Terminal status bar that scores the current session's complexity and suggests a Claude model tier (Haiku / Sonnet / Opus). Clicking the chip injects `/model <name>` into the focused terminal.

**Architecture:** Two components: (1) `ModelRecommender` — a free-function scorer in new `src/modelrecommender.h/.cpp` that reads the last 20 assistant turns from the transcript JSONL (tail-read, ≤ 512 KB) and returns a `Tier` recommendation. The active model is read from `message.model` in the most-recent assistant turn (it is always present in real Claude Code transcripts, confirmed from live data). (2) A `m_modelBtn` QPushButton in `ClaudeStatusBarController` wired via `m_statusTimer` in `mainwindow.cpp` (same pattern as `refreshTasksButton` at line 767).

**Tech Stack:** Qt 6 / C++20. No new deps — `QFile`/`QJsonDocument` for transcript reading. Tests use GoogleTest in `ants_add_core_bundle(test_core ...)` (Qt::Core only, no Widgets). `sendToPty` for model-switch injection (public on `TerminalWidget`).

---

### Task 1: Write the feature spec + failing test

**Files:**
- Create: `tests/features/model_recommender/spec.md`
- Create: `tests/features/model_recommender/test_model_recommender.cpp`
- Modify: `CMakeLists.txt` (add source to `ants_add_core_bundle(test_core ...)`)

- [ ] **Step 1: Create the spec**

```markdown
# model_recommender — Feature Spec (ANTS-1226 Shape A)

## Purpose
Passive status-bar chip that scores session complexity from the Claude
Code transcript and recommends a model tier, saving spend on simple
turns under a Max(5) subscription.

## Active model detection
The running model is read from `message.model` of the most-recent
`assistant` turn in the transcript JSONL (verified: always present as
e.g. "claude-opus-4-7"). Returns empty string when the transcript has
no assistant turns → treated as Sonnet tier.

## Scoring algorithm (last 20 assistant turns, tail-read ≤ 512 KB)

Feature                                        | Weight
---------------------------------------------- | ------
file_write_count ≥ 4 (Edit/Write tool calls)   | +2 (Opus signal)
tool_diversity ≥ 6 unique tool names           | +1 (Opus signal)
plan_keyword in assistant text                 | +2 (Opus: "spec","design","architecture","review","plan","refactor")
avg_message_len ≥ 500 chars                    | +1 (Opus: long context)
file_write_count == 0 AND tool_diversity ≤ 2   | -2 (Haiku: mechanical)

Score ≥ 3  → OPUS_TIER
Score ≤ -1 → HAIKU_TIER
Otherwise  → SONNET_TIER

## Invariants

- **INV-1** `score(transcriptPath)` returns `SONNET_TIER` when the
  transcript file does not exist or has 0 assistant turns.
- **INV-2** Returns `OPUS_TIER` when a plan_keyword is present AND
  file_write_count ≥ 4 across the last 20 turns.
- **INV-3** Returns `HAIKU_TIER` when file_write_count == 0 AND
  tool_diversity ≤ 2 across the last 20 turns.
- **INV-4** The chip is hidden when the recommended tier matches the
  tier inferred from `message.model` in the most-recent assistant turn.
- **INV-5** The chip shows "→ Haiku" / "→ Sonnet" / "→ Opus" text when
  the recommendation differs from the current model tier.
- **INV-6** Clicking the chip calls `sendToPty("/model haiku\n")` (or
  sonnet/opus) on the focused TerminalWidget.
- **INV-7** `score()` reads at most 512 KB of trailing transcript data;
  does not load a 100 MB transcript into a QStringList.
- **INV-8** Only the last 20 assistant turns are scored (older turns
  do not influence the recommendation).
- **INV-9** `score()` is a stateless free function — no per-call
  mutable state; calling it twice with the same path and unchanged
  file returns the same result.
```

- [ ] **Step 2: Create the test file (GoogleTest format)**

```cpp
// tests/features/model_recommender/test_model_recommender.cpp
// ANTS-1226 — ModelRecommender feature conformance test.
// Exercises the scorer's pure logic using synthetic transcript data.
#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryFile>
#include <QTextStream>
#include "modelrecommender.h"

namespace {

// Write a synthetic JSONL transcript and return the file path.
QString writeSyntheticTranscript(const QVector<QJsonArray> &turns,
                                  QTemporaryFile &f)
{
    f.open();
    QTextStream out(&f);
    for (const QJsonArray &content : turns) {
        QJsonObject turn;
        turn["type"] = "assistant";
        turn["timestamp"] = "2026-05-21T00:00:00.000Z";
        QJsonObject msg;
        msg["content"] = content;
        msg["model"] = "claude-sonnet-4-6";
        turn["message"] = msg;
        out << QJsonDocument(turn).toJson(QJsonDocument::Compact) << "\n";
    }
    f.close();
    return f.fileName();
}

QJsonObject toolUse(const QString &name) {
    QJsonObject t;
    t["type"] = "tool_use";
    t["name"] = name;
    t["id"]   = "tu_test";
    t["input"] = QJsonObject();
    return t;
}

QJsonObject textBlock(const QString &text) {
    QJsonObject t;
    t["type"] = "text";
    t["text"] = text;
    return t;
}

} // namespace

// INV-1: empty transcript → Sonnet
TEST(ModelRecommender, Inv1EmptyTranscriptReturnsSonnet) {
    QTemporaryFile f;
    f.open(); f.close();
    const auto result = ModelRecommender::score(f.fileName());
    EXPECT_EQ(result.tier, ModelRecommender::Tier::Sonnet);
}

// INV-1: missing file → Sonnet (no crash)
TEST(ModelRecommender, Inv1MissingFileReturnsSonnet) {
    const auto result =
        ModelRecommender::score("/tmp/no_such_file_ants_1226.jsonl");
    EXPECT_EQ(result.tier, ModelRecommender::Tier::Sonnet);
}

// INV-2: plan keyword + 4 writes → Opus
TEST(ModelRecommender, Inv2PlanKeywordPlusFourWritesReturnsOpus) {
    QTemporaryFile f;
    QJsonArray content;
    content.append(textBlock("Let me design the architecture for this spec."));
    content.append(toolUse("Edit"));
    content.append(toolUse("Edit"));
    content.append(toolUse("Edit"));
    content.append(toolUse("Edit"));
    const auto result = ModelRecommender::score(
        writeSyntheticTranscript({{content}}, f));
    EXPECT_EQ(result.tier, ModelRecommender::Tier::Opus)
        << "reason: " << result.reason.toStdString();
}

// INV-3: no writes, ≤2 tools → Haiku
TEST(ModelRecommender, Inv3NoWritesFewToolsReturnsHaiku) {
    QTemporaryFile f;
    QVector<QJsonArray> turns;
    for (int i = 0; i < 5; ++i) {
        QJsonArray content;
        content.append(toolUse("Bash"));
        content.append(toolUse("Bash"));
        turns.append(content);
    }
    const auto result = ModelRecommender::score(
        writeSyntheticTranscript(turns, f));
    EXPECT_EQ(result.tier, ModelRecommender::Tier::Haiku);
}

// INV-8: only last 20 turns scored — oldest turns don't affect result
TEST(ModelRecommender, Inv8OnlyLast20TurnsScored) {
    QTemporaryFile f;
    QVector<QJsonArray> turns;
    // Turns 0–4: have plan keywords (oldest; should be ignored)
    for (int i = 0; i < 5; ++i) {
        QJsonArray content;
        content.append(textBlock("Let me design the architecture."));
        content.append(toolUse("Edit"));
        content.append(toolUse("Edit"));
        content.append(toolUse("Edit"));
        content.append(toolUse("Edit"));
        turns.append(content);
    }
    // Turns 5–24: purely mechanical (most-recent 20; should dominate)
    for (int i = 0; i < 20; ++i) {
        QJsonArray content;
        content.append(toolUse("Bash"));
        turns.append(content);
    }
    // With 20 mechanical turns dominating: score ≤ -1 → Haiku
    const auto result = ModelRecommender::score(
        writeSyntheticTranscript(turns, f));
    EXPECT_EQ(result.tier, ModelRecommender::Tier::Haiku)
        << "Only the last 20 turns should be scored; "
           "oldest plan-keyword turns must not influence result. "
           "reason: " << result.reason.toStdString();
}

// INV-4/INV-5: tierFromModelId maps correctly
TEST(ModelRecommender, TierFromModelId) {
    EXPECT_EQ(ModelRecommender::tierFromModelId("claude-opus-4-7"),
              ModelRecommender::Tier::Opus);
    EXPECT_EQ(ModelRecommender::tierFromModelId("claude-sonnet-4-6"),
              ModelRecommender::Tier::Sonnet);
    EXPECT_EQ(ModelRecommender::tierFromModelId("claude-haiku-4-5"),
              ModelRecommender::Tier::Haiku);
    EXPECT_EQ(ModelRecommender::tierFromModelId(""),
              ModelRecommender::Tier::Sonnet);
}
```

- [ ] **Step 3: Add to test_core bundle in CMakeLists.txt**

Find `ants_add_core_bundle(test_core` (line ~1354). Inside its SOURCES list add:
```cmake
        tests/features/model_recommender/test_model_recommender.cpp
        src/modelrecommender.cpp
```

If the bundle doesn't already have `target_include_directories` covering `src/`, add:
```cmake
target_include_directories(test_core PRIVATE src)
```
after `ants_add_core_bundle(test_core ...)`.

- [ ] **Step 4: Confirm test compiles and fails**

```bash
cmake --build build --target test_core 2>&1 | tail -5
```

Expected: build fails with "modelrecommender.h not found" — correct, not yet written.

---

### Task 2: Implement ModelRecommender (scorer)

**Files:**
- Create: `src/modelrecommender.h`
- Create: `src/modelrecommender.cpp`

- [ ] **Step 1: Write modelrecommender.h**

```cpp
// src/modelrecommender.h
// ANTS-1226 — Passive model-tier recommender.
// Stateless free function: score(transcriptPath) → Tier + reason.
#pragma once
#include <QString>

namespace ModelRecommender {

enum class Tier { Haiku, Sonnet, Opus };

struct Result {
    Tier    tier   = Tier::Sonnet;
    QString reason;      // short rationale for the tooltip
    QString currentModel; // message.model from most-recent assistant turn
};

// score() reads at most 512 KB from the tail of the JSONL transcript
// at transcriptPath, scores the last 20 assistant turns.
// Returns Sonnet if the file is absent/empty.
// Stateless: no per-call mutable state.
Result score(const QString &transcriptPath);

// tierName() converts a Tier to the /model command argument string.
// Haiku → "haiku", Sonnet → "sonnet", Opus → "opus".
QString tierName(Tier tier);

// tierFromModelId() maps a Claude model ID string to a Tier.
// "claude-haiku*" → Haiku, "claude-opus*" → Opus, anything else → Sonnet.
Tier tierFromModelId(const QString &modelId);

}  // namespace ModelRecommender
```

- [ ] **Step 2: Write modelrecommender.cpp**

```cpp
// src/modelrecommender.cpp
// ANTS-1226 — ModelRecommender implementation.
#include "modelrecommender.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

namespace ModelRecommender {

namespace {

const QStringList kPlanKeywords{
    QStringLiteral("spec"),
    QStringLiteral("design"),
    QStringLiteral("architecture"),
    QStringLiteral("review"),
    QStringLiteral("plan"),
    QStringLiteral("refactor"),
};

bool hasPlanKeyword(const QString &text) {
    const QString lower = text.toLower();
    for (const QString &kw : kPlanKeywords) {
        if (lower.contains(kw)) return true;
    }
    return false;
}

}  // namespace

Result score(const QString &transcriptPath)
{
    Result def;
    def.reason = QStringLiteral("default");

    QFile f(transcriptPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return def;

    // INV-7: tail-read at most 512 KB to avoid loading a 100 MB
    // transcript into memory. Store the tail-seek flag BEFORE close()
    // since QFile::size() returns 0 after close on most platforms.
    constexpr qint64 kMaxTailBytes = 512LL * 1024LL;
    const bool didTailSeek = (f.size() > kMaxTailBytes);
    if (didTailSeek) {
        f.seek(f.size() - kMaxTailBytes);
    }

    QStringList allLines;
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (!line.isEmpty()) allLines.append(line);
    }
    f.close();

    // Discard the first line when tail-seeking — it may be truncated.
    if (didTailSeek && !allLines.isEmpty())
        allLines.removeFirst();

    // INV-8: collect the last 20 assistant turns in document order.
    // Walk in reverse and append; reverse once at the end — O(n) total.
    QVector<QJsonObject> turns;
    turns.reserve(20);
    for (int i = allLines.size() - 1; i >= 0 && turns.size() < 20; --i) {
        const QJsonObject obj =
            QJsonDocument::fromJson(allLines[i].toUtf8()).object();
        if (obj.value(QStringLiteral("type")).toString() ==
                QStringLiteral("assistant")) {
            turns.append(obj);
        }
    }
    // Reverse to restore document order (oldest → newest).
    std::reverse(turns.begin(), turns.end());

    if (turns.isEmpty()) return def;

    // Read model from most-recent assistant turn's message.model field.
    // This is always present in real Claude Code transcripts.
    def.currentModel =
        turns.last()
            .value(QStringLiteral("message")).toObject()
            .value(QStringLiteral("model")).toString();

    // --- extract features ---
    int fileWriteCount = 0;
    bool planKeyword   = false;
    QSet<QString> toolNames;
    qint64 totalMsgLen = 0;
    int msgCount       = 0;

    for (const QJsonObject &turn : turns) {
        const QJsonArray content =
            turn.value(QStringLiteral("message"))
                .toObject()
                .value(QStringLiteral("content"))
                .toArray();
        for (const QJsonValue &cv : content) {
            const QJsonObject c = cv.toObject();
            const QString type = c.value(QStringLiteral("type")).toString();
            if (type == QStringLiteral("tool_use")) {
                const QString name =
                    c.value(QStringLiteral("name")).toString();
                toolNames.insert(name);
                if (name == QStringLiteral("Edit") ||
                        name == QStringLiteral("Write")) {
                    ++fileWriteCount;
                }
            } else if (type == QStringLiteral("text")) {
                const QString text =
                    c.value(QStringLiteral("text")).toString();
                totalMsgLen += text.length();
                ++msgCount;
                if (hasPlanKeyword(text)) planKeyword = true;
            }
        }
    }

    const int toolDiversity = toolNames.size();
    const double avgLen =
        (msgCount > 0) ? static_cast<double>(totalMsgLen) / msgCount : 0.0;

    // --- score ---
    int sc = 0;
    QString reasons;
    if (fileWriteCount >= 4)     { sc += 2; reasons += "many_writes "; }
    if (toolDiversity >= 6)      { sc += 1; reasons += "tool_diversity "; }
    if (planKeyword)             { sc += 2; reasons += "plan_keyword "; }
    if (avgLen >= 500.0)         { sc += 1; reasons += "long_prompts "; }
    if (fileWriteCount == 0 &&
            toolDiversity <= 2)  { sc -= 2; reasons += "mechanical "; }

    Result r;
    r.currentModel = def.currentModel;
    if (sc >= 3) {
        r.tier   = Tier::Opus;
        r.reason = reasons.trimmed();
    } else if (sc <= -1) {
        r.tier   = Tier::Haiku;
        r.reason = reasons.trimmed();
    } else {
        r.tier   = Tier::Sonnet;
        r.reason = reasons.trimmed();
    }
    return r;
}

QString tierName(Tier tier)
{
    switch (tier) {
    case Tier::Haiku: return QStringLiteral("haiku");
    case Tier::Opus:  return QStringLiteral("opus");
    default:          return QStringLiteral("sonnet");
    }
}

Tier tierFromModelId(const QString &modelId)
{
    if (modelId.contains(QStringLiteral("haiku"), Qt::CaseInsensitive))
        return Tier::Haiku;
    if (modelId.contains(QStringLiteral("opus"),  Qt::CaseInsensitive))
        return Tier::Opus;
    return Tier::Sonnet;
}

}  // namespace ModelRecommender
```

- [ ] **Step 3: Build test and run**

```bash
cmake --build build --target test_core 2>&1 | tail -10
ctest --test-dir build -R ModelRecommender --output-on-failure 2>&1 | tail -15
```

Expected: all tests PASS.

- [ ] **Step 4: Commit the scorer alone**

```bash
git add src/modelrecommender.h src/modelrecommender.cpp \
        tests/features/model_recommender/ CMakeLists.txt
git commit -m "$(cat <<'EOF'
ANTS-1226 scorer: add ModelRecommender stateless-free-function tier scorer.

Reads last 20 assistant turns (tail-read ≤ 512 KB) from transcript JSONL.
Detects current model from message.model field. Scores file writes,
tool diversity, plan keywords, message length.

Co-Authored-By: Claude Sonnet 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: Add modelrecommender.cpp to ants-terminal target

**Files:**
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Find where claudestatuswidgets.cpp is listed**

```bash
grep -n "claudestatuswidgets\|ants_chrome_lib\|ants_gui_lib" CMakeLists.txt | head -5
```

- [ ] **Step 2: Add modelrecommender.cpp to the same target**

Add `src/modelrecommender.cpp` to the same `target_sources(...)` block or lib that contains `src/claudestatuswidgets.cpp`.

- [ ] **Step 3: Build**

```bash
cmake --build build --target ants-terminal 2>&1 | tail -10
```

---

### Task 4: Add the model chip to ClaudeStatusBarController

**Files:**
- Modify: `src/claudestatuswidgets.h`
- Modify: `src/claudestatuswidgets.cpp`

- [ ] **Step 1: Add member and method declarations to header**

In `src/claudestatuswidgets.h`, find `QPushButton *m_tasksBtn = nullptr;`. Add after:
```cpp
    QPushButton         *m_modelBtn = nullptr;
```

Find `void refreshTasksButton();`. Add after:
```cpp
    void refreshModelChip();
```

Do NOT add `#include "modelrecommender.h"` to the header — include it in the `.cpp` only.

- [ ] **Step 2: Add the chip in the constructor**

In `src/claudestatuswidgets.cpp`, add `#include "modelrecommender.h"` near the other `#include` lines at the top.

Find the block ending with `m_statusBar->addPermanentWidget(m_tasksBtn);` (line ~116). Add immediately after:

```cpp
    m_modelBtn = new QPushButton(QString(), m_statusBar);
    m_modelBtn->setObjectName(QStringLiteral("claudeModelBtn"));
    m_modelBtn->setAccessibleName(tr("Model recommendation"));
    m_modelBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    m_modelBtn->hide();
    m_statusBar->addPermanentWidget(m_modelBtn);
    connect(m_modelBtn, &QPushButton::clicked, this, [this]() {
        if (!m_modelBtn) return;
        // Extract tier name from label "→ Xxx" → last word lowercased.
        const QString label = m_modelBtn->text();
        const QString tier = label.mid(label.lastIndexOf(QChar(' ')) + 1)
                                   .toLower();
        if (tier.isEmpty()) return;
        auto *focused = m_focusedTerminalProvider
            ? m_focusedTerminalProvider() : nullptr;
        if (!focused) return;
        focused->sendToPty(
            (QStringLiteral("/model ") + tier + QStringLiteral("\n"))
                .toUtf8());
    });
```

- [ ] **Step 3: Implement refreshModelChip**

Add to `claudestatuswidgets.cpp` (after `refreshTasksButton`):

```cpp
void ClaudeStatusBarController::refreshModelChip()
{
    if (!m_modelBtn) return;

    // Resolve transcript path — same pattern as refreshTasksButton.
    QString cwd;
    auto *focused = m_focusedTerminalProvider
        ? m_focusedTerminalProvider() : nullptr;
    if (focused) cwd = focused->shellCwd();
    QString transcriptPath;
    if (m_integration) transcriptPath = m_integration->activeSessionPath(cwd);

    if (transcriptPath.isEmpty()) {
        m_modelBtn->hide();
        return;
    }

    const ModelRecommender::Result rec =
        ModelRecommender::score(transcriptPath);

    // INV-4: hide chip when recommendation matches current model tier.
    const ModelRecommender::Tier currentTier =
        ModelRecommender::tierFromModelId(rec.currentModel);
    if (rec.tier == currentTier) {
        m_modelBtn->hide();
        return;
    }

    const QString tierLabel = [&]() -> QString {
        switch (rec.tier) {
        case ModelRecommender::Tier::Haiku: return tr("→ Haiku");
        case ModelRecommender::Tier::Opus:  return tr("→ Opus");
        default:                            return tr("→ Sonnet");
        }
    }();

    m_modelBtn->setText(tierLabel);
    m_modelBtn->setToolTip(
        tr("Suggested model: %1\nReason: %2\n"
           "Click to send /model %3 to the focused terminal.")
            .arg(ModelRecommender::tierName(rec.tier))
            .arg(rec.reason.isEmpty()
                 ? tr("default heuristic") : rec.reason)
            .arg(ModelRecommender::tierName(rec.tier)));
    m_modelBtn->show();
}
```

- [ ] **Step 4: Wire refreshModelChip to the 2-second timer in mainwindow.cpp**

The 2-second timer is `m_statusTimer` in `MainWindow` (declared at `src/mainwindow.h:295`). The existing wiring is at `src/mainwindow.cpp:767–769`:
```cpp
    connect(m_statusTimer, &QTimer::timeout,
            m_claudeStatusBarController,
            &ClaudeStatusBarController::refreshTasksButton);
```
Add the following line immediately after line 769 (before `m_statusTimer->start()`):
```cpp
    connect(m_statusTimer, &QTimer::timeout,
            m_claudeStatusBarController,
            &ClaudeStatusBarController::refreshModelChip);
```

- [ ] **Step 5: Build**

```bash
cmake --build build --target ants-terminal 2>&1 | tail -10
```

Expected: clean.

---

### Task 5: Run all tests, commit

- [ ] **Step 1: Run feature tests**

```bash
ctest --test-dir build -L features --output-on-failure 2>&1 | tail -20
```

Expected: all pass including `ModelRecommender.*`.

- [ ] **Step 2: Manual smoke-test**

Launch `build/ants-terminal`. Open a Claude Code session with several file edits. After ~2 seconds, a "→ Opus" or "→ Haiku" chip should appear in the bottom bar (if the recommendation differs from the active model). Click it — `/model <name>` should be typed into the terminal. Switch to a tab with no active session — chip should disappear.

- [ ] **Step 3: Commit**

```bash
git add src/modelrecommender.h src/modelrecommender.cpp \
        src/claudestatuswidgets.h src/claudestatuswidgets.cpp \
        src/mainwindow.cpp CMakeLists.txt
git commit -m "$(cat <<'EOF'
ANTS-1226: add model recommender chip — passive session-complexity scorer.

Scores last 20 assistant turns (tail-read ≤ 512 KB) for file writes,
tool diversity, plan keywords, and message length. Reads active model
from message.model field in transcript. Recommends Haiku/Sonnet/Opus;
shows status-bar chip when recommendation differs from active model.
Click injects /model <name> into the focused terminal.

Co-Authored-By: Claude Sonnet 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Self-Review

**All cold-eyes loop-1 findings applied:**
- C-1: `readCurrentModel()` removed; model read from `message.model` in transcript ✓
- C-2: Test uses GoogleTest `TEST()` in `test_core` bundle ✓
- H-1: Timer wiring description is exact: `src/mainwindow.cpp` line 769, `m_statusTimer` signal ✓
- H-2: File-size cap 512 KB tail-seek; `didTailSeek` bool stored BEFORE `f.close()` ✓
- H-3: "stateless free function" (not "pure function") in spec ✓
- Loop-2-H-1: `f.size()` after `f.close()` bug fixed (bool stored before close) ✓
- Loop-2-H-2: `turns.prepend()` O(n²) fixed — `append()` + `std::reverse()` at end ✓
- M-2: INV-8 test is behavioral (25 turns, oldest 5 have plan keywords, last 20 are mechanical → Haiku) ✓
- M-3: `#include "modelrecommender.h"` in `.cpp` only ✓
- L-1: `static` removed from `kPlanKeywords` in anonymous namespace ✓
- L-3: `ConfigPaths::claudeHome()` — not needed since `readCurrentModel()` is removed ✓
- L-5: Co-Authored-By on intermediate commit (Task 2 Step 4) ✓

**Spec coverage:**
- INV-1 (empty/absent → Sonnet) → Task 2 Step 2 (early return)
- INV-2 (plan keyword + 4 writes → Opus) → Task 2 Step 2 + test
- INV-3 (no writes + ≤2 tools → Haiku) → Task 2 Step 2 + test
- INV-4 (chip hidden when rec == current) → Task 4 Step 3 (`rec.tier == currentTier`)
- INV-5 (chip text "→ Xxx") → Task 4 Step 3 (tierLabel switch)
- INV-6 (click → sendToPty) → Task 4 Step 2 (click lambda)
- INV-7 (≤ 512 KB tail-read) → Task 2 Step 2 (kMaxTailBytes guard)
- INV-8 (last 20 turns only) → Task 2 Step 2 + behavioral test
- INV-9 (stateless) → Task 2 Step 2 (no persistent state)

**Open question for implementer:** `ModelRecommender::Result::currentModel` is populated inside `score()` from the last turn's `message.model`. In `refreshModelChip`, `rec.currentModel` is used to derive `currentTier`. If the transcript has no model field (impossible in real CC transcripts but possible in tests), `currentModel` is `""` → `tierFromModelId("")` → `Sonnet`. This is safe fallback behaviour.
