#pragma once

// ANTS-5078 — writes a scrollback or command-block export to a file in
// bounded slices, so a large export never builds the whole text in memory
// or holds the GUI thread for its whole length. Contract:
// docs/specs/ANTS-5078-export-streaming.md.

#include "terminalgrid.h"

#include <QColor>
#include <QSaveFile>
#include <QString>

#include <unordered_map>
#include <vector>

class ScrollbackExporter {
public:
    enum class Format { Text, Html, Cast };
    enum class Step { More, Done, Failed };

    struct Request {
        Format format = Format::Text;
        // Every member has an initializer, so a designated initializer that
        // names only some of them does not trip -Wmissing-field-initializers.
        QString path{};
        QColor defaultFg{}, defaultBg{};   // Html
        int fontPointSize = 0;             // Html
        quint64 blockId = 0;               // Cast: PromptRegion::id
        QString command{};                 // Cast: the block's command text
    };

    ScrollbackExporter(const TerminalGrid &grid, Request request);

    bool open();                  // opens the QSaveFile, writes the header
    Step step(int maxLines);      // formats and writes up to maxLines lines
    QString error() const { return m_error; }
    qint64 bytesWritten() const { return m_bytesWritten; }

private:
    using Combining = std::unordered_map<int, std::vector<uint32_t>>;

    enum class State { Closed, Open, Done, Failed };

    qint64 evictedLines() const;
    bool write(const QByteArray &bytes);
    bool writeLine(const std::vector<Cell> &cells, const Combining &combining);
    Step fail(const QString &why);

    const TerminalGrid &m_grid;
    Request m_request;
    QSaveFile m_file;
    QString m_error;
    qint64 m_bytesWritten = 0;
    State m_state = State::Closed;

    // Fixed at open(). Line numbers are global lines as they were then.
    int m_cols = 0;
    int m_scrollbackAtOpen = 0;
    qint64 m_evictedAtOpen = 0;
    int m_next = 0;               // next line to write
    int m_end = 0;                // one past the last line
    bool m_castOutputEvent = false;
    std::vector<std::vector<Cell>> m_screenCells;   // screen rows in range
    std::vector<Combining> m_screenCombining;

    int m_pendingNewlines = 0;    // Text: separators owed before the next non-empty line
};
