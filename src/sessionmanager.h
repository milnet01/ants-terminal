#pragma once

#include <QString>
#include <QByteArray>

class QDataStream;
class TerminalGrid;

// ANTS-5225 — the three fields a saved tab needs before its history loads.
struct SessionPeek { QString title; QString cwd; QString pinnedTitle; };

// Serializes and deserializes terminal session state (scrollback + screen)
class SessionManager {
public:
    // ANTS-5031 — the largest file restore() accepts and the largest
    // uncompressed stream it decodes. serialize() writes no more than these.
    // MAX_UNCOMPRESSED is the cap restore() checks, both on qCompress's
    // 4-byte length claim (before qUncompress allocates) and on the result.
    static constexpr qint64   MAX_RESTORE_FILE_BYTES = 100LL * 1024 * 1024;
    static constexpr uint32_t MAX_UNCOMPRESSED = 500u * 1024u * 1024u;
    static constexpr qint64   MAX_RESTORE_RAW_BYTES  = MAX_UNCOMPRESSED;

    // Serialize grid state to compressed binary data. `pinnedTitle` is
    // the manual right-click "Rename Tab…" label (empty if the user
    // hasn't pinned one) — written to the session file so the rename
    // survives app restart, not just the current session's m_tabTitlePins
    // lifetime. Stored as a V3 trailing field; V2 readers ignore it
    // (falls off the end of the stream).
    // ANTS-5031 — the output stays within `maxRawBytes` uncompressed and
    // `maxFileBytes` on disk; the oldest scrollback lines are left out
    // first.
    static QByteArray serialize(const TerminalGrid *grid,
                                const QString &cwd = {},
                                const QString &pinnedTitle = {},
                                qint64 maxRawBytes = MAX_RESTORE_RAW_BYTES,
                                qint64 maxFileBytes = MAX_RESTORE_FILE_BYTES);

    // Restore grid state from serialized data. Out-params:
    //   `cwd`: saved working directory (V2+; empty for V1 files).
    //   `pinnedTitle`: saved manual tab rename (V3+; empty for V2/V1
    //   files, or when the user never pinned a title on that tab).
    static bool restore(TerminalGrid *grid, const QByteArray &data,
                        QString *cwd = nullptr,
                        QString *pinnedTitle = nullptr);
    // ANTS-5225 — the title, cwd and pin restore() would return, read
    // without building lines or touching a grid. Accepts exactly the input
    // restore() accepts.
    static bool peek(const QByteArray &data, SessionPeek *out);

    // Session file management
    static QString sessionDir();
    static QString sessionPath(const QString &tabId);
    static void saveSession(const QString &tabId, const TerminalGrid *grid,
                            const QString &cwd = {},
                            const QString &pinnedTitle = {});
    // ANTS-5131 — saveSession with the compress, hash and write on a worker
    // thread; only the grid walk stays on the caller's thread. One save runs
    // at a time, in call order. A blob that compresses past `maxFileBytes`
    // is not written; takeOvershoot() then reports the tab so the caller can
    // redo it with saveSession, which re-reads the grid.
    static void saveSessionAsync(const QString &tabId, const TerminalGrid *grid,
                                 const QString &cwd = {},
                                 const QString &pinnedTitle = {},
                                 qint64 maxFileBytes = MAX_RESTORE_FILE_BYTES);
    // True once for a tab whose async save overshot its cap.
    static bool takeOvershoot(const QString &tabId);
    // Blocks until the async save in flight, if any, has finished.
    static void waitForPendingSaves();
    static bool loadSession(const QString &tabId, TerminalGrid *grid,
                            QString *cwd = nullptr,
                            QString *pinnedTitle = nullptr);
    // ANTS-5225 — peek() on tabId's file. A refused file stays where it is;
    // the loadSession that later wakes the tab moves it aside.
    static bool peekSession(const QString &tabId, SessionPeek *out);
    static void removeSession(const QString &tabId);
    static QStringList savedSessions();

    // Tab order persistence
    static void saveTabOrder(const QStringList &tabIds, int activeIndex = 0);
    static QStringList loadTabOrder(int *activeIndex = nullptr);
    // ANTS-5654 — whether tab_order.txt exists, whatever it names.
    static bool hasTabOrder();

    // Removes session files older than maxAgeDays, except one whose id
    // tab_order.txt names (ANTS-5225: a sleeping tab's file keeps its age).
    static void cleanupOldSessions(int maxAgeDays = 30);

    // ANTS-5131 — writes `data` to `tmpPath`, fsyncs, renames it over
    // `path` and fsyncs the directory. Safe off the GUI thread: it creates
    // the temp file 0600 itself instead of changing the process umask.
    // Public so tests/perf/bench_session_save can time it alone (ANTS-5661).
    static bool writeBlob(const QString &path, const QString &tmpPath,
                          const QByteArray &data);

private:
    // ANTS-5031 — the uncompressed stream serialize() compresses, with
    // scrollback lines from `firstLine` on.
    static QByteArray serializeStream(const TerminalGrid *grid,
                                      const QString &cwd,
                                      const QString &pinnedTitle,
                                      int firstLine);
    // ANTS-5131 — the first scrollback line kept so the stream fits
    // `maxRawBytes`.
    static int firstLineWithin(const TerminalGrid *grid, const QString &cwd,
                               const QString &pinnedTitle, qint64 maxRawBytes);
    // ANTS-5131 — wraps a compressed payload in the V4 envelope.
    static QByteArray seal(const QByteArray &compressed);

    // ANTS-5225 — restore()'s and peek()'s shared decoder, up to the line
    // records. unpack: the size cap, envelope, SHA-256, length guards and
    // qUncompress. readHeader: magic, version and dimensions. readTrailer:
    // title, cwd (V2+) and pin (V3+), then the stream's final status.
    struct StreamHeader { uint32_t version; int32_t rows, cols, curRow, curCol; };
    static bool unpack(const QByteArray &input, QByteArray *raw);
    static bool readHeader(QDataStream &in, StreamHeader *h);
    static bool readTrailer(QDataStream &in, uint32_t version, SessionPeek *out);

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
};
