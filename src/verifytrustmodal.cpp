// VerifyTrustModalClient implementation. See verifytrustmodal.h +
// docs/specs/ANTS-1337.md § 4.3.

#include "verifytrustmodal.h"

#include "guithread.h"

#include <QCheckBox>
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

// The gates VerifyEngine::parseVerifyJson runs, in its order: each key whose
// object carries a non-empty command.
QStringList gateNames(const QJsonObject &root) {
    QStringList names;
    for (const QString &gate : {QStringLiteral("build"),
                                QStringLiteral("tests"),
                                QStringLiteral("lint")}) {
        if (!root.value(gate).toObject()
                 .value(QStringLiteral("command")).toString().isEmpty())
            names << gate;
    }
    return names;
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
    const QStringList gates = gateNames(root);
    if (gates.isEmpty()) return QStringLiteral("(no command field found)");
    QString preview = root.value(gates.first()).toObject()
                          .value(QStringLiteral("command")).toString();
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
    // ANTS-5025 — verify_changes runs off the GUI thread (ANTS-2132), and a
    // widget may only be built and run there (ANTS-1337 INV-8). A refused
    // marshal means Ants is shutting down: nobody can answer, so Headless.
    const auto decision = ants::onGuiThread([&]() {
        return showPrompt(projectPath, shaHex, configBytes);
    });
    if (!decision) return {Outcome::Headless, shaHex};
    return *decision;
}

Decision ModalClient::showPrompt(const QString &projectPath,
                                 const QString &shaHex,
                                 const QByteArray &configBytes) {
    QMessageBox box(m_parent);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(QObject::tr("Trust .ants/verify.json?"));

    const QString shortHex = shortSha(shaHex);
    const QString preview  = commandPreview(configBytes);
    // ANTS-1337 § 4.3 — "Gates: N — name1, name2".
    const QStringList gates =
        gateNames(QJsonDocument::fromJson(configBytes).object());
    const QString gatesLine = gates.isEmpty()
        ? QStringLiteral("0")
        : QStringLiteral("%1 — %2").arg(gates.size())
              .arg(gates.join(QStringLiteral(", ")));

    box.setText(QObject::tr(
        "A Claude Code session in <code>%1</code> wants to run "
        "<b>verify_changes</b>, which executes commands defined in "
        "this repo's <code>.ants/verify.json</code>. The first time "
        "you trust a verify.json, Ants asks for confirmation."
    ).arg(projectPath.toHtmlEscaped()));

    box.setInformativeText(QObject::tr(
        "<b>Repo:</b> <code>%1</code><br>"
        "<b>SHA-256:</b> <code>%2</code><br>"
        "<b>Gates:</b> %3<br>"
        "<b>First command preview:</b><pre>%4</pre>"
    ).arg(projectPath.toHtmlEscaped(),
          shortHex,
          gatesLine,
          preview.toHtmlEscaped()));

    // ANTS-1808 — label this accurately: %2 is the entire .ants/verify.json
    // file (all gates), not just the first gate's command. The previous
    // "Full first-gate command" label was a trust-decision hazard — a user
    // reading it would not realise a multi-gate config's 2nd/3rd commands are
    // also shown (and will run). The informative text above shows the first
    // command preview; this detailed view shows the whole config.
    box.setDetailedText(QObject::tr(
        "Full SHA-256:\n%1\n\n"
        "Full config file (.ants/verify.json):\n%2"
    ).arg(shaHex, QString::fromUtf8(configBytes)));

    auto *bTrustSha  = box.addButton(QObject::tr("Trust this SHA"),
                                     QMessageBox::AcceptRole);
    auto *bTrustRepo = box.addButton(QObject::tr("Trust this repo"),
                                     QMessageBox::AcceptRole);
    auto *bDeny      = box.addButton(QObject::tr("Deny once"),
                                     QMessageBox::RejectRole);
    box.addButton(QObject::tr("Cancel"), QMessageBox::RejectRole);
    box.setDefaultButton(bDeny);
    // ANTS-1337 § 4.3 — the sub-option of "Trust this repo", default ON: a
    // changed verify.json asks again. The box owns the checkbox.
    auto *reprompt = new QCheckBox(QObject::tr(
        "Trust this repo: auto-re-prompt if the file changes"));
    reprompt->setChecked(true);
    box.setCheckBox(reprompt);

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
        if (!addTrustedRepo(projectPath, shaHex, reprompt->isChecked())) {
            return {Outcome::Headless, shaHex};
        }
        return {Outcome::Trusted, shaHex};
    }
    // Deny / Cancel / window-close — all fall back.
    return {Outcome::UntrustedFellBack, shaHex};
}

}  // namespace VerifyTrust
