// Safe patterns — pulled from env or keystore, no literal secret.
#include <cstdlib>
#include <string>

std::string get_key() {
    const char *k = std::getenv("API_KEY");
    return k ? k : "";
}

void describe_field(const char * /*name*/) {
    // The word 'password' appears in a comment, not as an assignment.
    // Placeholder: if you need a password, set PASSWORD env var.
}
