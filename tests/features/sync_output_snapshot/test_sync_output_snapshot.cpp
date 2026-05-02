// Feature-conformance test for ANTS-1148 (0.7.75) — DEC mode 2026
// (sync output) unified snapshot path. Source-grep only.
//
// INVs map 1:1 to docs/specs/ANTS-1148.md §Acceptance:
//   INV-1   batchEntersSyncOutput helper in anon namespace
//   INV-2   pre-scan ordered before the processAction loop
//   INV-3   capture disjunction (sync || batchEnters) + empty guard
//   INV-4   end-of-loop clear (no wasSync gate)
//   INV-5   safety-timer slot clears the snapshot
//   INV-6   m_frozenCursorRow / m_frozenCursorCol declared
//   INV-7   captureScreenSnapshot stores cursor row/col
//   INV-8   clearScreenSnapshot resets cursor row/col to 0
//   INV-9   effectiveCursorRow/Col accessors centralise the policy;
//           paintEvent + blinkCursor use them; other readers stay
//           live.

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#ifndef SRC_TERMINALWIDGET_CPP_PATH
#error "SRC_TERMINALWIDGET_CPP_PATH compile definition required"
#endif
#ifndef SRC_TERMINALWIDGET_H_PATH
#error "SRC_TERMINALWIDGET_H_PATH compile definition required"
#endif

namespace {

std::string slurp(const char *path) {
    std::ifstream in(path);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

std::size_t countOccurrences(const std::string &hay, const std::string &needle) {
    if (needle.empty()) return 0;
    std::size_t count = 0;
    for (std::size_t pos = hay.find(needle);
         pos != std::string::npos;
         pos = hay.find(needle, pos + 1)) {
        ++count;
    }
    return count;
}

int fail(const char *label, const std::string &why) {
    std::fprintf(stderr, "[%s] FAIL: %s\n", label, why.c_str());
    return 1;
}

// Extract the body of a function from the source by searching for the
// signature, then returning everything from the opening `{` through
// the matching closing `}`. Heuristic — relies on balanced braces in
// the body. Good enough for the project's top-level free / member
// functions.
std::string functionBody(const std::string &src, const std::string &sig) {
    const auto sigPos = src.find(sig);
    if (sigPos == std::string::npos) return {};
    auto bracePos = src.find('{', sigPos + sig.size());
    if (bracePos == std::string::npos) return {};
    int depth = 1;
    auto i = bracePos + 1;
    while (i < src.size() && depth > 0) {
        if (src[i] == '{') ++depth;
        else if (src[i] == '}') --depth;
        ++i;
    }
    if (depth != 0) return {};
    return src.substr(bracePos, i - bracePos);
}

}  // namespace

int main() {
    const std::string tw  = slurp(SRC_TERMINALWIDGET_CPP_PATH);
    const std::string twH = slurp(SRC_TERMINALWIDGET_H_PATH);

    if (tw.empty())  return fail("setup", "terminalwidget.cpp not readable");
    if (twH.empty()) return fail("setup", "terminalwidget.h not readable");

    // INV-1 — batchEntersSyncOutput helper present. Parameter
    // is named `b` (not `batch`) per the helper's own comment —
    // avoids tripping vtbatch_zero_copy's "stale batch.actions"
    // sentinel against the new helper's reference dot-access.
    if (!contains(tw, "bool batchEntersSyncOutput(const VtBatch &b)"))
        return fail("INV-1",
            "anonymous-namespace helper "
            "`bool batchEntersSyncOutput(const VtBatch &b)` missing");
    if (!contains(tw, "intermediate == \"?\""))
        return fail("INV-1",
            "batchEntersSyncOutput must check `intermediate == \"?\"`");
    if (!contains(tw, "finalChar == 'h'"))
        return fail("INV-1",
            "batchEntersSyncOutput must check `finalChar == 'h'` (BSU "
            "is `set` not `reset`; ESU at finalChar=='l' is the inverse)");
    if (!contains(tw, "p == 2026"))
        return fail("INV-1",
            "batchEntersSyncOutput must compare `p == 2026` against "
            "params (literal in-loop comparison; bare `2026` token would "
            "false-match doc comments and the spec ID)");

    // INV-2 — pre-scan is ordered BEFORE the action loop in onVtBatch.
    {
        const std::string body = functionBody(tw,
            "void TerminalWidget::onVtBatch(VtBatchPtr batch)");
        if (body.empty())
            return fail("INV-2", "could not locate onVtBatch body");
        const auto preScanPos = body.find("batchEntersSyncOutput(*batch)");
        const auto loopPos = body.find("for (const auto &action : batch->actions)");
        if (preScanPos == std::string::npos)
            return fail("INV-2",
                "onVtBatch body does not call `batchEntersSyncOutput(*batch)`");
        if (loopPos == std::string::npos)
            return fail("INV-2",
                "onVtBatch body does not contain the processAction loop "
                "header `for (const auto &action : batch->actions)`");
        if (preScanPos >= loopPos)
            return fail("INV-2",
                "pre-scan call must appear ABOVE the processAction loop "
                "in onVtBatch (so the snapshot is captured before any "
                "BSU action mutates the live grid)");
    }

    // INV-3 — capture disjunction.
    {
        const std::string body = functionBody(tw,
            "void TerminalWidget::onVtBatch(VtBatchPtr batch)");
        // Tolerant regex: `m_syncOutputActive || batchEntersSyncOutput(...)`
        // followed within ~150 chars by `captureScreenSnapshot()`.
        std::regex captureGuard(
            R"((m_syncOutputActive\s*\|\|\s*batchEntersSyncOutput|batchEntersSyncOutput.*?\|\|\s*m_syncOutputActive)[\s\S]{0,200}?captureScreenSnapshot\s*\()");
        if (!std::regex_search(body, captureGuard))
            return fail("INV-3",
                "onVtBatch must capture the snapshot under the "
                "disjunction `(m_syncOutputActive || batchEntersSyncOutput(*batch))` — "
                "without it, sync straddling a resize / RIS leaves the "
                "snapshot stranded and the rest of the sync block paints "
                "the live mid-state");
        if (!contains(body, "m_frozenScreenRows.empty()"))
            return fail("INV-3",
                "capture guard must include `m_frozenScreenRows.empty()` "
                "to avoid clobbering the existing scroll-up snapshot");
    }

    // INV-4 — end-of-loop clear without the prior wasSync gate.
    // Substring check — regex with bounded lookahead missed when
    // explanatory comments grew past the window. The substring
    // ordering is enforced by INV-2's pre-scan-before-loop check
    // and by the spec's procedural narrative; this just locks in
    // that the cleanup predicate dropped the wasSync gate (cold-
    // eyes C2 — same-batch BSU+ESU would otherwise strand the
    // pre-scan-captured snapshot).
    {
        const std::string body = functionBody(tw,
            "void TerminalWidget::onVtBatch(VtBatchPtr batch)");
        if (!contains(body, "!m_syncOutputActive"))
            return fail("INV-4",
                "post-loop block must check `!m_syncOutputActive`");
        if (!contains(body, "m_scrollOffset == 0 && !m_frozenScreenRows.empty()"))
            return fail("INV-4",
                "cleanup guard must be exactly "
                "`m_scrollOffset == 0 && !m_frozenScreenRows.empty()` — "
                "covers same-batch BSU+ESU (cold-eyes C2) where "
                "wasSync would falsely guard against the clear");
        if (!contains(body, "clearScreenSnapshot()"))
            return fail("INV-4",
                "post-loop block must call clearScreenSnapshot()");
    }

    // INV-5 — safety-timer slot.
    {
        // The lambda body lives between `m_syncTimer` connect and the
        // closing of the connect call. Heuristic: search for
        // `m_syncOutputActive = false` followed within a small window
        // by `clearScreenSnapshot()`.
        std::regex timerSlot(
            R"(m_syncOutputActive\s*=\s*false[\s\S]{0,500}?clearScreenSnapshot\s*\()");
        if (!std::regex_search(tw, timerSlot))
            return fail("INV-5",
                "safety-timer slot must call clearScreenSnapshot() "
                "when force-ending sync; otherwise the snapshot lingers "
                "after a truncated escape sequence");
    }

    // INV-6 — frozen cursor members in the header.
    if (!contains(twH, "int m_frozenCursorRow"))
        return fail("INV-6",
            "terminalwidget.h missing `int m_frozenCursorRow` member");
    if (!contains(twH, "int m_frozenCursorCol"))
        return fail("INV-6",
            "terminalwidget.h missing `int m_frozenCursorCol` member");

    // INV-7 — captureScreenSnapshot stores the cursor.
    {
        const std::string body = functionBody(tw,
            "void TerminalWidget::captureScreenSnapshot()");
        if (body.empty())
            return fail("INV-7", "captureScreenSnapshot body missing");
        if (!contains(body, "m_frozenCursorRow = m_grid->cursorRow()"))
            return fail("INV-7",
                "captureScreenSnapshot must store "
                "`m_frozenCursorRow = m_grid->cursorRow()`");
        if (!contains(body, "m_frozenCursorCol = m_grid->cursorCol()"))
            return fail("INV-7",
                "captureScreenSnapshot must store "
                "`m_frozenCursorCol = m_grid->cursorCol()`");
    }

    // INV-8 — clearScreenSnapshot resets the cursor.
    {
        const std::string body = functionBody(tw,
            "void TerminalWidget::clearScreenSnapshot()");
        if (body.empty())
            return fail("INV-8", "clearScreenSnapshot body missing");
        if (!contains(body, "m_frozenCursorRow = 0"))
            return fail("INV-8",
                "clearScreenSnapshot must reset "
                "`m_frozenCursorRow = 0`");
        if (!contains(body, "m_frozenCursorCol = 0"))
            return fail("INV-8",
                "clearScreenSnapshot must reset "
                "`m_frozenCursorCol = 0`");
    }

    // INV-9 — effectiveCursor accessors + paintEvent / blinkCursor
    // adoption + non-render functions stay live.
    {
        const std::string rowAcc =
            "int effectiveCursorRow() const { return m_frozenScreenRows.empty() ? m_grid->cursorRow() : m_frozenCursorRow; }";
        const std::string colAcc =
            "int effectiveCursorCol() const { return m_frozenScreenRows.empty() ? m_grid->cursorCol() : m_frozenCursorCol; }";
        if (!contains(twH, rowAcc))
            return fail("INV-9",
                "terminalwidget.h missing exact accessor body for "
                "effectiveCursorRow()");
        if (!contains(twH, colAcc))
            return fail("INV-9",
                "terminalwidget.h missing exact accessor body for "
                "effectiveCursorCol()");
    }
    {
        // paintEvent: ≥ 3 calls to each accessor; zero direct
        // m_grid->cursorRow()/Col() reads in the body.
        const std::string body = functionBody(tw,
            "void TerminalWidget::paintEvent(QPaintEvent *)");
        if (body.empty())
            return fail("INV-9", "paintEvent body missing");
        const auto rowCalls = countOccurrences(body, "effectiveCursorRow()");
        const auto colCalls = countOccurrences(body, "effectiveCursorCol()");
        if (rowCalls < 3 || colCalls < 3) {
            std::fprintf(stderr,
                "[INV-9] FAIL: paintEvent expected ≥ 3 effectiveCursorRow/Col() "
                "calls each, got row=%zu col=%zu\n", rowCalls, colCalls);
            return 1;
        }
        if (contains(body, "m_grid->cursorRow()") ||
            contains(body, "m_grid->cursorCol()"))
            return fail("INV-9",
                "paintEvent body still contains `m_grid->cursorRow()` "
                "or `m_grid->cursorCol()` — every render-path read must "
                "go through the accessor; otherwise the cursor leaks "
                "the live position during sync");
    }
    {
        // blinkCursor: uses both accessors; no direct cursorRow()/Col().
        const std::string body = functionBody(tw,
            "void TerminalWidget::blinkCursor()");
        if (body.empty())
            return fail("INV-9", "blinkCursor body missing");
        if (!contains(body, "effectiveCursorRow()"))
            return fail("INV-9",
                "blinkCursor must use `effectiveCursorRow()` for the "
                "partial-update rect; otherwise the rect won't cover "
                "the cell paintEvent will draw during sync");
        if (!contains(body, "effectiveCursorCol()"))
            return fail("INV-9",
                "blinkCursor must use `effectiveCursorCol()` for the "
                "partial-update rect");
        if (contains(body, "m_grid->cursorRow()") ||
            contains(body, "m_grid->cursorCol()"))
            return fail("INV-9",
                "blinkCursor body still contains direct cursor reads");
    }

    std::fprintf(stderr,
        "OK — DEC mode 2026 sync output snapshot INVs hold.\n");
    return 0;
}
