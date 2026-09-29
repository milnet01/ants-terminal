// ANTS-5558 — see welcomedialog.h. Interface stubs: the behaviour lands with
// the implementation, after the conformance tests are proven red.

#include "welcomedialog.h"

#include "config.h"

WelcomeDialog::Options WelcomeDialog::detect() { return {}; }

WelcomeDialog::WelcomeDialog(const QString &, const Options &, QWidget *parent)
    : QDialog(parent) {}

namespace welcome {

void maybeAutoShow(Config &, const std::function<void()> &) {}

}  // namespace welcome
