#pragma once

#include "terminalgrid.h"
#include "vtparser.h"
#include "ptyhandler.h"

#include <QWidget>
#include <QFont>
#include <QTimer>
#include <QImage>

class TerminalWidget : public QWidget {
    Q_OBJECT

public:
    explicit TerminalWidget(QWidget *parent = nullptr);
    ~TerminalWidget() override;

    bool startShell();
    void applyThemeColors(const QColor &fg, const QColor &bg,
                          const QColor &cursorColor);
    int scrollOffset() const { return m_scrollOffset; }

signals:
    void titleChanged(const QString &title);
    void shellExited(int code);
    void imagePasted(const QImage &image);

protected:
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

private slots:
    void onPtyData(const QByteArray &data);
    void onPtyFinished(int exitCode);
    void blinkCursor();

private:
    void recalcGridSize();
    QPoint cellToPixel(int row, int col) const;

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
};
