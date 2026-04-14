// Fixture for the `format_string` audit rule.
//
// Rule regex: \b[fs]?n?printf\s*\([^"]*\b\w+\s*\)
// Matches printf-family calls whose content contains no `"` — i.e. the
// format argument is a variable, a known footgun.

#include <stdio.h>

void emit(const char *userFmt, FILE *logFile, char *buf, char *message) {
    printf(userFmt);                       // @expect format_string
    fprintf(logFile, userFmt);             // @expect format_string
    sprintf(buf, message);                 // @expect format_string
}
