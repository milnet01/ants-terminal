// Negative fixtures — these catch handlers all contain at least one
// statement (logging, rethrow, assignment, return) so the empty-body
// pattern MUST NOT fire. The audit rule targets only same-line empty
// bodies; this file exercises the cases that should pass clean.

#include <cstdio>
#include <exception>
#include <string>

int parse_with_log(const char *s) {
    try {
        return std::stoi(s);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "parse failed: %s\n", e.what());
        return -1;
    }
}

int parse_with_rethrow(const char *s) {
    try {
        return std::stoi(s);
    } catch (...) {
        throw;
    }
}

int parse_with_return(const char *s) {
    try {
        return std::stoi(s);
    } catch (...) { return -1; }
}

// Paragraph prose mentioning that empty handlers (a pair of braces right
// after catch parentheses) are the rule's target, written so no literal
// code pattern appears on any single line of this comment.
