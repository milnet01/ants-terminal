#pragma once

// ANTS-6437 — follows, key by key, whether the line the user is typing starts
// with "/mo": `/model`, `/model <tier>`, or `/mo` completed from Claude Code's
// slash menu. TerminalWidget feeds it every key the user sends, and stamps
// the time when Enter submits such a line; the user-typed "Switch model?"
// auto-confirm (ModelAutoSwitch::shouldAutoConfirmUnarmedSwitch) fires only
// shortly after that stamp.
//
// It holds no typed text, so a password typed at a prompt leaves nothing
// behind: only the line's length and where it stopped matching. Pasted text
// and input-method text do not pass through it, so a pasted `/model` is not
// seen and its dialog waits for the user. Header-only and Qt-core-only so
// TerminalWidget can hold one without pulling in modelautoswitch.h.

#include <QString>

namespace ModelAutoSwitch {

struct ModelCommandTracker {
    int len = 0;           // characters on the line, as far as we can tell
    int mismatchAt = -1;   // first position not matching "/mo"; -1 if none
};

enum class TypedKey : unsigned char { Text, Backspace, Tab, Enter, Other };

// Feeds one key; `text` is the key's printable text for TypedKey::Text. Tab
// keeps the line (it completes a slash command); Other (a cursor move or a
// control key) makes the line unknown, so it cannot count. Returns true when
// Enter submits a line that starts with "/mo"; Enter starts a fresh line.
inline bool feedModelCommandKey(ModelCommandTracker &t, TypedKey key,
                                const QString &text) {
    static const QString kPrefix = QStringLiteral("/mo");
    switch (key) {
    case TypedKey::Text:
        for (const QChar c : text) {
            if (t.mismatchAt < 0 && t.len < kPrefix.size() && c != kPrefix[t.len])
                t.mismatchAt = t.len;
            ++t.len;
        }
        return false;
    case TypedKey::Backspace:
        if (t.len > 0) --t.len;
        // Erasing back over the first wrong character makes the line match again.
        if (t.mismatchAt >= t.len) t.mismatchAt = -1;
        return false;
    case TypedKey::Tab:
        return false;
    case TypedKey::Enter: {
        const bool submitted = t.mismatchAt < 0 && t.len >= kPrefix.size();
        t = ModelCommandTracker{};
        return submitted;
    }
    case TypedKey::Other:
        t.mismatchAt = 0;
        return false;
    }
    return false;
}

}  // namespace ModelAutoSwitch
