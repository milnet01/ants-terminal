// VerifyTrustModalClient — chrome-layer subclass of
// FilePersistedTrustClient that overrides `prompt()` to show a
// QMessageBox-based modal trust dialog. ANTS-1337 Phase 2.
//
// Lives in ants_chrome_lib (QtWidgets dep). RemoteControl owns it
// via the base-class interface so core-lib doesn't drag QtWidgets.

#pragma once

#include "verifytrust.h"

#include <QPointer>

class QWidget;

namespace VerifyTrust {

class ModalClient : public FilePersistedTrustClient {
public:
    // `parent` is used as the QMessageBox parent so the modal appears
    // centred on the main window. May be null (modal becomes
    // application-modal on the active screen).
    explicit ModalClient(QWidget *parent = nullptr,
                         const QString &trustFilePath = {});

protected:
    Decision prompt(const QString &projectPath,
                    const QString &shaHex,
                    const QByteArray &configBytes) override;

    // ANTS-5025 — the dialog itself: builds the QMessageBox, runs it and
    // persists a trust choice. Virtual so a test can see which thread runs
    // it without showing a dialog.
    virtual Decision showPrompt(const QString &projectPath,
                                const QString &shaHex,
                                const QByteArray &configBytes);

private:
    QPointer<QWidget> m_parent;
};

}  // namespace VerifyTrust
