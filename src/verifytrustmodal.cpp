// VerifyTrustModalClient implementation. See verifytrustmodal.h +
// docs/specs/ANTS-1337.md § 4.3.

#include "verifytrustmodal.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMessageBox>
#include <QPushButton>
#include <QString>
#include <QStringList>

namespace VerifyTrust {

namespace {

// First-gate command preview, capped at this many chars + ellipsis.
constexpr int kCommandPreviewChars = 200;

// Format a SHA-256 hex for human reading: first 12 hex chars +
// "…" + last 4. Full SHA is in the tooltip.
QString shortSha(const QString &fullHex) {
    if (fullHex.size() < 20) return fullHex;
    return fullHex.left(12) + QStringLiteral("…") + fullHex.right(4);
}

QString commandPreview(const QByteArray &configBytes) {
    // ANTS-1763 — parse with QJsonDocument rather than a naive quote
    // scan. The previous backslash-aware scan misread a value ending in
    // an even number of backslashes (e.g. "foo\\"): it treated the real
    // closing quote as escaped and read on into the rest of the JSON.
    // This preview is the ONLY human-readable summary in the trust
    // prompt, so a misrepresentation is a trust-decision hazard. Schema
    // (VerifyEngine::parseVerifyJson): { build|tests|lint: { command } }.
    // Return the FIRST gate's command in the engine's gate order so the
    // "First command preview" matches what would actually run first.
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(configBytes, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        return QStringLiteral("(preview unavailable — malformed JSON)");
    }
    const QJsonObject root = doc.object();
    QString preview;
    for (const QString &gate : {QStringLiteral("build"),
                                QStringLiteral("tests"),
                                QStringLiteral("lint")}) {
        const QString cmd = root.value(gate).toObject()
                                .value(QStringLiteral("command")).toString();
        if (!cmd.isEmpty()) { preview = cmd; break; }
    }
    if (preview.isEmpty()) return QStringLiteral("(no command field found)");
    if (preview.size() > kCommandPreviewChars) {
        preview = preview.left(kCommandPreviewChars)
                  + QStringLiteral("…");
    }
    return preview;
}

}  // namespace

ModalClient::ModalClient(QWidget *parent, const QString &trustFilePath)
    : FilePersistedTrustClient(trustFilePath.isEmpty()
                                   ? QString()  // default-path ctor
                                   : trustFilePath),
      m_parent(parent) {}

Decision ModalClient::prompt(const QString &projectPath,
                             const QString &shaHex,
                             const QByteArray &configBytes) {
    QMessageBox box(m_parent);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(QObject::tr("Trust .ants/verify.json?"));

    const QString shortHex = shortSha(shaHex);
    const QString preview  = commandPreview(configBytes);

    box.setText(QObject::tr(
        "A Claude Code session in <code>%1</code> wants to run "
        "<b>verify_changes</b>, which executes commands defined in "
        "this repo's <code>.ants/verify.json</code>. The first time "
        "you trust a verify.json, Ants asks for confirmation."
    ).arg(projectPath.toHtmlEscaped()));

    box.setInformativeText(QObject::tr(
        "<b>Repo:</b> <code>%1</code><br>"
        "<b>SHA-256:</b> <code>%2</code><br>"
        "<b>First command preview:</b><pre>%3</pre>"
    ).arg(projectPath.toHtmlEscaped(),
          shortHex,
          preview.toHtmlEscaped()));

    box.setDetailedText(QObject::tr(
        "Full SHA-256:\n%1\n\n"
        "Full first-gate command:\n%2"
    ).arg(shaHex, QString::fromUtf8(configBytes)));

    auto *bTrustSha  = box.addButton(QObject::tr("Trust this SHA"),
                                     QMessageBox::AcceptRole);
    auto *bTrustRepo = box.addButton(QObject::tr("Trust this repo"),
                                     QMessageBox::AcceptRole);
    auto *bDeny      = box.addButton(QObject::tr("Deny once"),
                                     QMessageBox::RejectRole);
    box.addButton(QObject::tr("Cancel"), QMessageBox::RejectRole);
    box.setDefaultButton(bDeny);

    box.exec();
    auto *clicked = box.clickedButton();
    if (clicked == bTrustSha) {
        // Persist the SHA. addTrustedSha returns false on disk-write
        // failure — surface as Headless so the engine falls back to
        // auto-detect rather than running an untrusted config (the
        // user's intent was to trust, but persistence failed; safer
        // to defer than to honour without saving).
        if (!addTrustedSha(shaHex,
                QObject::tr("Trusted via modal on %1")
                    .arg(projectPath))) {
            return {Outcome::Headless, shaHex};
        }
        return {Outcome::Trusted, shaHex};
    }
    if (clicked == bTrustRepo) {
        if (!addTrustedRepo(projectPath, shaHex,
                            /*untilShaChanges*/ true)) {
            return {Outcome::Headless, shaHex};
        }
        return {Outcome::Trusted, shaHex};
    }
    // Deny / Cancel / window-close — all fall back.
    return {Outcome::UntrustedFellBack, shaHex};
}

}  // namespace VerifyTrust
