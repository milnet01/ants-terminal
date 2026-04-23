#include "claudetranscript.h"
#include "claudeintegration.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QJsonObject>
#include <QJsonDocument>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QScrollBar>

ClaudeTranscriptDialog::ClaudeTranscriptDialog(ClaudeIntegration *integration,
                                                 QWidget *parent)
    : QDialog(parent), m_integration(integration) {
    setWindowTitle("Claude Code - Session Transcript");
    setMinimumSize(700, 500);
    resize(800, 600);

    auto *layout = new QVBoxLayout(this);

    // Session selector row
    auto *topRow = new QHBoxLayout();
    topRow->addWidget(new QLabel("Session:", this));

    m_sessionCombo = new QComboBox(this);
    m_sessionCombo->setMinimumWidth(400);
    connect(m_sessionCombo, &QComboBox::activated,
            this, &ClaudeTranscriptDialog::onSessionSelected);
    topRow->addWidget(m_sessionCombo, 1);

    auto *refreshBtn = new QPushButton("Refresh", this);
    connect(refreshBtn, &QPushButton::clicked, this, &ClaudeTranscriptDialog::onRefresh);
    topRow->addWidget(refreshBtn);

    layout->addLayout(topRow);

    // Transcript view
    m_transcriptView = new QTextEdit(this);
    m_transcriptView->setReadOnly(true);
    m_transcriptView->setPlaceholderText("Select a session to view its transcript...");
    layout->addWidget(m_transcriptView, 1);

    // Load sessions
    onRefresh();
}

void ClaudeTranscriptDialog::onRefresh() {
    m_sessionCombo->clear();
    QStringList sessions = m_integration->recentSessions();
    for (const QString &path : sessions) {
        QFileInfo fi(path);
        QString label = fi.dir().dirName() + "/" + fi.fileName()
                        + " (" + fi.lastModified().toString("yyyy-MM-dd hh:mm") + ")";
        m_sessionCombo->addItem(label, path);
    }

    // Auto-select most recent
    if (m_sessionCombo->count() > 0) {
        m_sessionCombo->setCurrentIndex(0);
        onSessionSelected(0);
    }
}

void ClaudeTranscriptDialog::onSessionSelected(int index) {
    QString path = m_sessionCombo->itemData(index).toString();
    if (!path.isEmpty())
        loadTranscript(path);
}

void ClaudeTranscriptDialog::loadTranscript(const QString &path) {
    QJsonArray entries = m_integration->loadTranscript(path);
    m_transcriptView->clear();

    QString html;
    for (const QJsonValue &val : entries) {
        html += formatEntry(val.toObject());
    }

    m_transcriptView->setHtml(html);
    // Scroll to bottom
    auto *sb = m_transcriptView->verticalScrollBar();
    if (sb) sb->setValue(sb->maximum());
}

QString ClaudeTranscriptDialog::formatEntry(const QJsonObject &entry) const {
    QString type = entry.value("type").toString();
    QString html;

    if (type == "user") {
        QJsonObject msg = entry.value("message").toObject();
        QString content;
        QJsonArray contentArr = msg.value("content").toArray();
        if (!contentArr.isEmpty()) {
            for (const QJsonValue &c : contentArr) {
                QJsonObject block = c.toObject();
                if (block.value("type").toString() == "text")
                    content += block.value("text").toString();
            }
        }
        // Fall back to string content
        if (content.isEmpty())
            content = msg.value("content").toString();

        html += QString("<p><b style='color:#89B4FA;'>User:</b> %1</p>")
                .arg(content.toHtmlEscaped().replace("\n", "<br>"));

    } else if (type == "assistant") {
        QJsonObject msg = entry.value("message").toObject();
        QJsonArray content = msg.value("content").toArray();

        for (const QJsonValue &c : content) {
            QJsonObject block = c.toObject();
            QString blockType = block.value("type").toString();

            if (blockType == "text") {
                QString text = block.value("text").toString();
                html += QString("<p><b style='color:#A6E3A1;'>Claude:</b> %1</p>")
                        .arg(text.toHtmlEscaped().replace("\n", "<br>"));
            } else if (blockType == "thinking") {
                // Extended-thinking blocks — real assistant events routinely
                // lead with one. Rendered italic / dimmed so readers can tell
                // them apart from the final visible response.
                QString text = block.value("thinking").toString();
                if (text.isEmpty())
                    text = block.value("text").toString();
                if (text.isEmpty()) continue;
                html += QString("<p style='color:#7F849C;font-style:italic;'>"
                               "<b>Thinking:</b> %1</p>")
                        .arg(text.toHtmlEscaped().replace("\n", "<br>"));
            } else if (blockType == "tool_use") {
                QString tool = block.value("name").toString();
                QJsonObject input = block.value("input").toObject();
                QString inputStr = QJsonDocument(input).toJson(QJsonDocument::Compact);
                if (inputStr.size() > 200) inputStr = inputStr.left(200) + "...";
                html += QString("<p style='margin-left:20px;'>"
                               "<b style='color:#F9E2AF;'>Tool: %1</b><br>"
                               "<code style='color:#CDD6F4;font-size:11px;'>%2</code></p>")
                        .arg(tool.toHtmlEscaped(), inputStr.toHtmlEscaped());
            } else if (blockType == "tool_result") {
                // Compact display of tool results
                html += "<p style='margin-left:20px;color:#585B70;'>[tool result]</p>";
            }
        }

        // Show token usage if available
        QJsonObject usage = msg.value("usage").toObject();
        int inputTokens = usage.value("input_tokens").toInt();
        int outputTokens = usage.value("output_tokens").toInt();
        if (inputTokens > 0) {
            html += QString("<p style='color:#585B70;font-size:10px;margin-left:20px;'>"
                           "tokens: %1 in / %2 out</p>")
                    .arg(inputTokens).arg(outputTokens);
        }
    }

    return html;
}
