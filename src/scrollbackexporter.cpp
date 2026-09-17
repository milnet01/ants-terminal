#include "scrollbackexporter.h"

#include <QDateTime>
#include <QStringList>

#include <utility>

namespace {

const Cell kBlankCell{' ', {}};
const std::vector<Cell> kNoCells;
const std::unordered_map<int, std::vector<uint32_t>> kNoCombining;

void appendCodepoint(QString &text, uint32_t cp) {
    if (cp < 0x10000) {
        text += QChar(static_cast<char16_t>(cp));
    } else {
        cp -= 0x10000;
        text += QChar(static_cast<char16_t>(0xD800 + (cp >> 10)));
        text += QChar(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
    }
}

// One line's text, as TerminalWidget::lineText builds it: a cell past the
// stored cells reads as a space, and combining characters follow their cell.
QString lineText(const std::vector<Cell> &cells,
                 const std::unordered_map<int, std::vector<uint32_t>> &combining,
                 int cols) {
    QString text;
    text.reserve(cols);
    const int stored = std::min(cols, static_cast<int>(cells.size()));
    for (int c = 0; c < cols; ++c) {
        uint32_t cp = c < stored ? cells[c].codepoint : ' ';
        if (cp == 0) cp = ' ';
        appendCodepoint(text, cp);
        if (!combining.empty()) {
            auto it = combining.find(c);
            if (it != combining.end())
                for (uint32_t combCp : it->second)
                    appendCodepoint(text, combCp);
        }
    }
    return text;
}

QString escapeJson(const QString &s) {
    QString out;
    out.reserve(s.size() + 8);
    for (QChar c : s) {
        ushort u = c.unicode();
        switch (u) {
        case '\\': out += "\\\\"; break;
        case '"':  out += "\\\""; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (u < 0x20) out += QString("\\u%1").arg(u, 4, 16, QChar('0'));
            else out += c;
        }
    }
    return out;
}

}  // namespace

ScrollbackExporter::ScrollbackExporter(const TerminalGrid &grid, Request request)
    : m_grid(grid), m_request(std::move(request)), m_file(m_request.path) {}

qint64 ScrollbackExporter::evictedLines() const {
    return static_cast<qint64>(m_grid.scrollbackPushed()) - m_grid.scrollbackSize();
}

bool ScrollbackExporter::write(const QByteArray &bytes) {
    if (m_file.write(bytes) != bytes.size()) return false;
    m_bytesWritten += bytes.size();
    return true;
}

ScrollbackExporter::Step ScrollbackExporter::fail(const QString &why) {
    // Never committed: the QSaveFile removes its temporary file when it is
    // destroyed, and the target is untouched.
    m_state = State::Failed;
    m_error = why;
    return Step::Failed;
}

bool ScrollbackExporter::open() {
    if (m_state != State::Closed) return false;
    const int scrollback = m_grid.scrollbackSize();
    const int rows = m_grid.rows();
    const int total = scrollback + rows;
    m_cols = m_grid.cols();
    m_scrollbackAtOpen = scrollback;
    m_evictedAtOpen = evictedLines();

    qint64 castTimestamp = 0;
    double castDurationSec = 0.1;
    if (m_request.format == Format::Cast) {
        const auto &regions = m_grid.promptRegions();
        const int idx = m_grid.promptRegionIndexById(m_request.blockId);
        if (idx < 0) {
            fail(QStringLiteral("the command block is no longer in scrollback"));
            return false;
        }
        const PromptRegion &pr = regions[idx];
        // The range outputTextAt reads.
        if (pr.hasOutput) {
            m_next = (pr.outputStartLine > 0) ? pr.outputStartLine : pr.endLine + 1;
            m_end = (idx + 1 < static_cast<int>(regions.size()))
                        ? regions[idx + 1].startLine
                        : total;
        }
        if (m_next > m_end) m_end = m_next;
        m_castOutputEvent = m_next < m_end;
        castTimestamp = pr.commandStartMs > 0 ? pr.commandStartMs / 1000
                                              : QDateTime::currentSecsSinceEpoch();
        if (pr.commandEndMs > pr.commandStartMs && pr.commandStartMs > 0)
            castDurationSec = (pr.commandEndMs - pr.commandStartMs) / 1000.0;
    } else {
        m_next = 0;
        m_end = total;
    }

    // Screen rows can change under the export without leaving the grid, so
    // the ones in range are copied now. Scrollback lines are read when their
    // slice runs.
    m_screenCells.assign(rows, {});
    m_screenCombining.assign(rows, {});
    for (int r = 0; r < rows; ++r) {
        const int line = scrollback + r;
        if (line < m_next || line >= m_end) continue;
        auto &cells = m_screenCells[r];
        cells.reserve(m_cols);
        for (int c = 0; c < m_cols; ++c)
            cells.push_back(m_grid.cellAt(r, c));
        m_screenCombining[r] = m_grid.screenCombining(r);
    }

    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        fail(m_file.errorString());
        return false;
    }
    m_state = State::Open;

    bool ok = true;
    switch (m_request.format) {
    case Format::Text:
        break;
    case Format::Html: {
        QString html;
        html += "<!DOCTYPE html>\n<html><head><meta charset='utf-8'>\n";
        html += "<title>Ants Terminal Export</title>\n";
        html += "<style>\n";
        html += "body { background: " + m_request.defaultBg.name() + "; ";
        html += "color: " + m_request.defaultFg.name() + "; ";
        html += "font-family: 'JetBrains Mono', 'Fira Code', monospace; ";
        html += "font-size: " + QString::number(m_request.fontPointSize) + "pt; ";
        html += "white-space: pre; }\n";
        html += "span.bold { font-weight: bold; }\n";
        html += "span.italic { font-style: italic; }\n";
        html += "span.underline { text-decoration: underline; }\n";
        html += "span.strikethrough { text-decoration: line-through; }\n";
        html += "</style>\n</head><body>\n";
        ok = write(html.toUtf8());
        break;
    }
    case Format::Cast: {
        // Asciicast v2: one header object + one JSON array per output event.
        // Spec: https://docs.asciinema.org/manual/asciicast/v2/
        //
        // A captured block has no per-byte timing, only commandStartMs and
        // commandEndMs, so two events are synthesized: the command echo at
        // t=0, and the entire output at t=(commandEndMs - commandStartMs)/1000.
        const QString header = QString(
            R"({"version": 2, "width": %1, "height": %2, "timestamp": %3, "env": {"TERM": "xterm-256color"}})"
        ).arg(m_cols).arg(rows).arg(castTimestamp);
        ok = write(header.toUtf8() + "\n");
        if (ok && !m_request.command.isEmpty()) {
            const QString evt = QString(R"([0.0, "o", "%1\r\n"])")
                                    .arg(escapeJson(m_request.command));
            ok = write(evt.toUtf8() + "\n");
        }
        // The output event is written a line at a time; step() closes it.
        if (ok && m_castOutputEvent)
            ok = write(QString(R"([%1, "o", ")")
                           .arg(castDurationSec, 0, 'f', 3).toUtf8());
        break;
    }
    }
    if (!ok) {
        fail(m_file.errorString());
        return false;
    }
    return true;
}

bool ScrollbackExporter::writeLine(const std::vector<Cell> &cells,
                                   const Combining &combining) {
    switch (m_request.format) {
    case Format::Text: {
        // exportAsText joins lines with '\n' after dropping trailing empty
        // lines, so an empty line's separator is written only once a later
        // non-empty line proves it is not trailing.
        QString line = lineText(cells, combining, m_cols);
        while (line.endsWith(' ')) line.chop(1);
        if (m_next > 0) ++m_pendingNewlines;
        if (line.isEmpty()) return true;
        QByteArray bytes(m_pendingNewlines, '\n');
        m_pendingNewlines = 0;
        return write(bytes + line.toUtf8());
    }
    case Format::Html: {
        QString html;
        QColor lastFg, lastBg;
        bool inSpan = false;
        const int stored = static_cast<int>(cells.size());
        for (int c = 0; c < m_cols; ++c) {
            const Cell &cell = c < stored ? cells[c] : kBlankCell;
            if (cell.isWideCont) continue;

            QColor fg = cell.attrs.fg;
            QColor bg = cell.attrs.bg;
            if (cell.attrs.inverse) std::swap(fg, bg);

            // Open new span if colors changed
            if (fg != lastFg || bg != lastBg || c == 0) {
                if (inSpan) html += "</span>";
                QString style = "color:" + fg.name() + ";";
                if (bg != m_request.defaultBg)
                    style += "background:" + bg.name() + ";";
                QStringList classes;
                if (cell.attrs.bold) classes << "bold";
                if (cell.attrs.italic) classes << "italic";
                if (cell.attrs.underline) classes << "underline";
                if (cell.attrs.strikethrough) classes << "strikethrough";
                html += "<span";
                if (!classes.isEmpty())
                    html += " class='" + classes.join(' ') + "'";
                html += " style='" + style + "'>";
                inSpan = true;
                lastFg = fg;
                lastBg = bg;
            }

            uint32_t cp = cell.codepoint;
            if (cp == 0) cp = ' ';
            if (cp == '<') html += "&lt;";
            else if (cp == '>') html += "&gt;";
            else if (cp == '&') html += "&amp;";
            else html += QString::fromUcs4(reinterpret_cast<const char32_t *>(&cp), 1);
        }
        if (inSpan) html += "</span>";
        html += "\n";
        return write(html.toUtf8());
    }
    case Format::Cast: {
        QString line = lineText(cells, combining, m_cols);
        while (!line.isEmpty() && line.back().isSpace() && line.back() != '\n')
            line.chop(1);
        line += '\n';
        return write(escapeJson(line).toUtf8());
    }
    }
    return false;
}

ScrollbackExporter::Step ScrollbackExporter::step(int maxLines) {
    switch (m_state) {
    case State::Closed: return fail(QStringLiteral("the export was not opened"));
    case State::Done: return Step::Done;
    case State::Failed: return Step::Failed;
    case State::Open: break;
    }
    // A width change reflows scrollback, so no recorded line number holds.
    if (m_grid.cols() != m_cols)
        return fail(QStringLiteral("the terminal was resized"));

    for (int n = 0; n < maxLines && m_next < m_end; ++n, ++m_next) {
        const std::vector<Cell> *cells = &kNoCells;
        const Combining *combining = &kNoCombining;
        if (m_next < m_scrollbackAtOpen) {
            const qint64 index = m_next - (evictedLines() - m_evictedAtOpen);
            if (index < 0 || index >= m_grid.scrollbackSize())
                return fail(QStringLiteral("scrollback was dropped before it was written"));
            cells = &m_grid.scrollbackLine(static_cast<int>(index));
            combining = &m_grid.scrollbackCombining(static_cast<int>(index));
        } else if (m_next - m_scrollbackAtOpen < static_cast<int>(m_screenCells.size())) {
            cells = &m_screenCells[m_next - m_scrollbackAtOpen];
            combining = &m_screenCombining[m_next - m_scrollbackAtOpen];
        }
        if (!writeLine(*cells, *combining))
            return fail(m_file.errorString());
    }
    if (m_next < m_end) return Step::More;

    bool ok = true;
    if (m_request.format == Format::Html)
        ok = write("</body></html>\n");
    else if (m_request.format == Format::Cast && m_castOutputEvent)
        ok = write("\"]\n");
    if (!ok || !m_file.commit())
        return fail(m_file.errorString());
    m_state = State::Done;
    return Step::Done;
}
