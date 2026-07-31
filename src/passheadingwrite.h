// ANTS-2126: pure helpers for the `#### Pass N.M` heading-format
// roadmap_log writer. The reader (RoadmapDialog::parsePassHeadingBullets,
// ANTS-1530) classifies a doc as `pass-headings` and synthesises a
// `PASS-N-M[-SUB]` id per heading; these helpers are the inverse — they
// render and edit pass blocks so the round-trip (write → reader) is
// faithful. Qt6::Core-only; lives in ants_core_lib so the remotecontrol
// handlers and the feature test share one implementation (mirrors
// changeloglog.{h,cpp}). See docs/specs/ANTS-2126.md.

#pragma once

#include <QString>

namespace PassHeadingWrite {

// The canonical write keyword for a roadmap_log status value. Total over
// the four-value enum (planned / in-progress / shipped / considered);
// the single keyword it emits re-parses through the reader
// (RoadmapParse::parsePassHeadingBullets) to the intended emoji:
//   planned     → "todo"        (reader else-branch → 📋)
//   in-progress → "in-progress" (reader → 🚧)
//   shipped     → "done"        (reader → ✅)
//   considered  → "deferred"    (reader → 💭)
// Returns an empty string for any other input (unknown status).
QString passStatusKeyword(const QString &roadmapStatus);

// The canonical emoji glyph for a write keyword (the inverse direction,
// for emoji-only / emoji+keyword Status lines): todo→📋, in-progress→🚧,
// done→✅, deferred→💭. Empty for an unknown keyword.
QString passStatusEmoji(const QString &keyword);

// True iff `pass` matches the designator shape
// ^\d+\.\d+(?:\.[A-Za-z][A-Za-z0-9]*)?$ (e.g. "43.5", "43.5.B").
bool isValidPassDesignator(const QString &pass);

// Synthesise the reader's id for a designator, exactly as the reader
// does (major/minor via toInt → leading zeros stripped):
//   "43.5"   → "PASS-43-5"
//   "43.5.B" → "PASS-43-5-B"
// Empty if the designator is invalid.
QString passIdFromDesignator(const QString &pass);

// Render a pass block (no surrounding blank lines, no trailing newline):
//   #### Pass <pass> <headline>
//   - **Status**: <keyword>
//   <body, verbatim>
// `body` is written verbatim under the Status line when non-empty.
QString formatPassBlock(const QString &pass, const QString &headline,
                        const QString &keyword, const QString &body);

// Result of a flip / annotate edit on a pass-headings roadmap body.
struct WriteResult {
    bool    ok = false;
    QString markdown;          // the new file body (valid iff ok)
    QString code;              // refusal code iff !ok (bullet_not_found)
    QString matchedId;         // synthesised id of the located pass (iff ok)
    QString matchedHeadline;   // located pass heading tail (iff ok)
    int     headingLine = -1;  // 0-based line of the located heading (iff ok)
    int     changedLine = -1;  // 0-based line rewritten/inserted (iff ok)
};

// Locate the pass whose synthesised id == `locatorId` (when non-empty),
// else whose heading tail matches `locatorHeadline`, and rewrite its
// FIRST `- **Status**:` line to `keyword`, preserving the line's style
// (keyword-only / emoji-only / emoji+keyword). If the located pass has
// no Status line within the lookahead window, insert one directly under
// the heading. First match wins; every other byte is unchanged.
// code: bullet_not_found.
WriteResult flipPassStatus(const QString &markdown,
                           const QString &locatorId,
                           const QString &locatorHeadline,
                           const QString &keyword);

// Locate the pass (same rule as flipPassStatus) and append `note` as
// body line(s) at the end of its block (before the next heading, or at
// EOF for the last block). Status untouched, no anchor injected.
// code: bullet_not_found.
WriteResult annotatePass(const QString &markdown,
                         const QString &locatorId,
                         const QString &locatorHeadline,
                         const QString &note);

}  // namespace PassHeadingWrite
