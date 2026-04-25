#pragma once

#include <QString>
#include <QByteArray>

class TerminalGrid;

// Serializes and deserializes terminal session state (scrollback + screen)
class SessionManager {
public:
    // Serialize grid state to compressed binary data. `pinnedTitle` is
    // the manual right-click "Rename Tab…" label (empty if the user
    // hasn't pinned one) — written to the session file so the rename
    // survives app restart, not just the current session's m_tabTitlePins
    // lifetime. Stored as a V3 trailing field; V2 readers ignore it
    // (falls off the end of the stream).
    static QByteArray serialize(const TerminalGrid *grid,
                                const QString &cwd = {},
                                const QString &pinnedTitle = {});

    // Restore grid state from serialized data. Out-params:
    //   `cwd`: saved working directory (V2+; empty for V1 files).
    //   `pinnedTitle`: saved manual tab rename (V3+; empty for V2/V1
    //   files, or when the user never pinned a title on that tab).
    static bool restore(TerminalGrid *grid, const QByteArray &data,
                        QString *cwd = nullptr,
                        QString *pinnedTitle = nullptr);

    // Session file management
    static QString sessionDir();
    static QString sessionPath(const QString &tabId);
    static void saveSession(const QString &tabId, const TerminalGrid *grid,
                            const QString &cwd = {},
                            const QString &pinnedTitle = {});
    static bool loadSession(const QString &tabId, TerminalGrid *grid,
                            QString *cwd = nullptr,
                            QString *pinnedTitle = nullptr);
    static void removeSession(const QString &tabId);
    static QStringList savedSessions();

    // Tab order persistence
    static void saveTabOrder(const QStringList &tabIds, int activeIndex = 0);
    static QStringList loadTabOrder(int *activeIndex = nullptr);

    // Cleanup old sessions (older than maxAgeDays)
    static void cleanupOldSessions(int maxAgeDays = 30);

private:
    static constexpr uint32_t MAGIC = 0x414E5453; // "ANTS"
    // V2 added `cwd` after the window title.
    // V3 adds `pinnedTitle` (manual right-click Rename-Tab label) after cwd.
    static constexpr uint32_t VERSION = 3;

    // Outer envelope wrapping the qCompress payload. The legacy on-disk
    // format was raw qCompress() output; V4+ files prefix that payload
    // with [SHEC magic][envelope version][SHA-256(payload)][payload len].
    // The inner stream `VERSION` is unchanged (still 3). The envelope
    // magic disambiguates from legacy files: qCompress's first 4 bytes
    // are a big-endian uncompressed-length, which for any real session
    // never reaches 0x53484543. Read path peeks ENVELOPE_MAGIC and
    // falls through to the legacy decoder on mismatch.
    static constexpr uint32_t ENVELOPE_MAGIC = 0x53484543; // "SHEC"
    static constexpr uint32_t ENVELOPE_VERSION = 1;
    static constexpr int      ENVELOPE_HEADER_SIZE = 4 + 4 + 32 + 4;
    // Hard cap on the uncompressed-length claim inside qCompress's
    // 4-byte big-endian prefix. Mirrors the post-decompression cap so
    // we reject the allocation BEFORE qUncompress sees the file.
    static constexpr uint32_t MAX_UNCOMPRESSED = 500u * 1024u * 1024u;
};
