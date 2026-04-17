// Safe patterns — pulled from env or keystore, no literal secret.
// 2026-04-16: this fixture also locks down the "RHS is a variable /
// constructor, not a literal" suppression that killed 2 FPs in the
// motivating triage (m_aiApiKey = new QLineEdit(tab), m_apiKey = apiKey).
#include <cstdlib>
#include <string>

class QLineEdit; class QObject;
QLineEdit *createEdit(QObject *);

std::string get_key() {
    const char *k = std::getenv("API_KEY");
    return k ? k : "";
}

void describe_field(const char * /*name*/) {
    // The word 'password' appears in a comment, not as an assignment.
    // Placeholder: if you need a password, set PASSWORD env var.
}

void ctor_idioms(QObject *parent) {
    // These patterns were the triage's FP source: "apiKey" in a variable
    // name on the LHS, constructor or variable on the RHS → must NOT match.
    QLineEdit *m_apiKey = createEdit(parent);    // LHS name match; RHS non-literal
    auto *m_aiApiKey = createEdit(parent);       // same shape, different decl
    (void)m_apiKey; (void)m_aiApiKey;
}
