#pragma once

#include "terminalgrid.h"
#include "vtparser.h"
#include "ptyhandler.h"

#include <QWidget>
#include <QFont>
#include <QTimer>
#include <QImage>
#include <QLineEdit>
#include <QRegularExpression>

class QLabel;
class QHBoxLayout;

class TerminalWidget : public QWidget {
    Q_OBJECT

public:
    explicit TerminalWidget(QWidget *parent = nullptr);
    ~TerminalWidget() override;

    bool startShell();
    void applyThemeColors(const QColor &fg, const QColor &bg,
                          const QColor &cursorColor);
    int scrollOffset() const { return m_scrollOffset; }
    QString shellTitle() const { return m_lastTitle; }

signals:
    void titleChanged(const QString &title);
    void shellExited(int code);
    void imagePasted(const QImage &image);

protected:
    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

private slots:
    void onPtyData(const QByteArray &data);
    void onPtyFinished(int exitCode);
    void blinkCursor();

private:
    void recalcGridSize();
    QPoint cellToPixel(int row, int col) const;
    QPoint pixelToCell(const QPoint &pos) const;
    QString selectedText() const;
    void copySelection();
    void clearSelection();
    bool hasSelection() const;
    bool isCellSelected(int globalLine, int col) const;

    // URL detection
    struct UrlSpan { int startCol; int endCol; QString url; };
    std::vector<UrlSpan> detectUrls(int globalLine) const;
    QString urlAtCell(int globalLine, int col) const;
    QString lineText(int globalLine) const;

    // Search
    struct SearchMatch { int globalLine; int col; int length; };
    void showSearchBar();
    void hideSearchBar();
    void performSearch();
    void scrollToMatch();
    void searchNext();
    void searchPrev();
    bool isCellSearchMatch(int globalLine, int col) const;
    bool isCellCurrentMatch(int globalLine, int col) const;

    // Bracket matching
    struct BracketPair { int line1; int col1; int line2; int col2; };
    BracketPair findMatchingBracket() const;
    bool isCellBracketHighlight(int globalLine, int col) const;
    uint32_t getCellCodepoint(int globalLine, int col) const;

    Pty *m_pty = nullptr;
    TerminalGrid *m_grid = nullptr;
    VtParser *m_parser = nullptr;

    QFont m_font;
    int m_cellWidth = 0;
    int m_cellHeight = 0;
    int m_fontAscent = 0;
    int m_padding = 4;

    // Cursor blink
    QTimer m_cursorTimer;
    bool m_cursorBlinkOn = true;
    bool m_hasFocus = true;

    // Scrollback viewing
    int m_scrollOffset = 0;

    // Theme cursor color
    QColor m_cursorColor{0x89, 0xB4, 0xFA};

    // Track last title to avoid redundant signals
    QString m_lastTitle;

    // Selection (in global line coordinates)
    bool m_selecting = false;
    bool m_hasSelection = false;
    QPoint m_selStart{-1, -1};  // (globalLine, col)
    QPoint m_selEnd{-1, -1};

    // Selection highlight color
    QColor m_selectionBg{0x89, 0xB4, 0xFA};
    QColor m_selectionFg{0x1E, 0x1E, 0x2E};

    // URL detection
    QColor m_urlColor{0x89, 0xB4, 0xFA};

    // Search bar
    QWidget *m_searchBar = nullptr;
    QLineEdit *m_searchInput = nullptr;
    QLabel *m_searchLabel = nullptr;
    bool m_searchVisible = false;
    QString m_searchText;
    std::vector<SearchMatch> m_searchMatches;
    int m_currentMatchIdx = -1;
    QColor m_searchHighlightBg{0xF9, 0xE2, 0xAF};
    QColor m_searchHighlightFg{0x1E, 0x1E, 0x2E};
    QColor m_searchCurrentBg{0xFA, 0xB3, 0x87};
    QColor m_searchCurrentFg{0x1E, 0x1E, 0x2E};

    // Bracket matching
    BracketPair m_bracketMatch{-1, -1, -1, -1};
    QColor m_bracketHighlightBg{0x58, 0x5B, 0x70};
};
