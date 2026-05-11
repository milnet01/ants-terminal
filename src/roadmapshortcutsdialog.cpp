#include "roadmapshortcutsdialog.h"

#include "dialogchrome.h"
#include "roadmapdialog.h"   // declares roadmapShortcutRows()
#include "themes.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLatin1String>
#include <QPalette>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

RoadmapShortcutsDialog::RoadmapShortcutsDialog(const QString &themeName,
                                               QWidget *parent)
    : QDialog(parent) {
    setObjectName(QStringLiteral("roadmapShortcutsDialog"));
    setWindowTitle(tr("Roadmap Keyboard Shortcuts"));
    setAccessibleName(tr("Roadmap keyboard shortcuts overlay"));
    setAttribute(Qt::WA_DeleteOnClose, false);   // INV-6: reuse instance

    auto chrome = DialogChrome::install(this, themeName);
    QWidget *content = chrome.contentArea;

    auto *root = new QVBoxLayout(content);

    const auto rows = roadmapShortcutRows();

    auto *table = new QTableWidget(rows.size(), 2, this);
    table->setObjectName(QStringLiteral("roadmapShortcutsTable"));
    table->setHorizontalHeaderLabels({tr("Shortcut"), tr("Action")});
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setShowGrid(false);
    table->setAlternatingRowColors(true);
    table->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::Stretch);

    for (int i = 0; i < rows.size(); ++i) {
        auto *keysItem   = new QTableWidgetItem(rows[i].first);
        auto *actionItem = new QTableWidgetItem(rows[i].second);
        table->setItem(i, 0, keysItem);
        table->setItem(i, 1, actionItem);
    }
    table->resizeRowsToContents();

    // Theme palette — match the parent dialog's bgPrimary/textPrimary
    // pair so the overlay reads as part of the Roadmap window, not a
    // detached Qt-default popup.
    const Theme &th = Themes::byName(themeName);
    QPalette dp = palette();
    dp.setColor(QPalette::Window,         th.bgPrimary);
    dp.setColor(QPalette::WindowText,     th.textPrimary);
    dp.setColor(QPalette::Base,           th.bgPrimary);
    dp.setColor(QPalette::AlternateBase,  th.bgSecondary);
    dp.setColor(QPalette::Text,           th.textPrimary);
    dp.setColor(QPalette::Button,         th.bgSecondary);
    dp.setColor(QPalette::ButtonText,     th.textPrimary);
    dp.setColor(QPalette::Highlight,      th.accent);
    dp.setColor(QPalette::HighlightedText, th.bgPrimary);
    setPalette(dp);
    setAutoFillBackground(true);

    QPalette tp = table->palette();
    tp.setColor(QPalette::Base,           th.bgPrimary);
    tp.setColor(QPalette::AlternateBase,  th.bgSecondary);
    tp.setColor(QPalette::Text,           th.textPrimary);
    tp.setColor(QPalette::Highlight,      th.accent);
    tp.setColor(QPalette::HighlightedText, th.bgPrimary);
    table->setPalette(tp);

    root->addWidget(table, 1);

    auto *btnRow = new QHBoxLayout();
    btnRow->addStretch(1);
    auto *closeBtn = new QPushButton(tr("Close"), this);
    closeBtn->setObjectName(QStringLiteral("roadmapShortcutsClose"));
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);
    btnRow->addWidget(closeBtn);
    root->addLayout(btnRow);

    resize(480, 360);
}

void RoadmapShortcutsDialog::keyPressEvent(QKeyEvent *event) {
    // INV-3: same `?` press that opened the dialog closes it (toggle).
    // Layout-robust route — text() carries the resolved character so
    // AltGr-on-Polish / dead-key paths work even where Qt does not
    // emit Key_Question. Esc remains routed via QDialog::reject as
    // usual; this override only adds the `?` toggle behaviour.
    if (event->text() == QLatin1String("?")
        && !(event->modifiers() & Qt::ControlModifier)) {
        accept();
        event->accept();
        return;
    }
    QDialog::keyPressEvent(event);
}
