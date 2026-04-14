// Fixture for the `todo_scan` audit rule.
//
// Rule regex (from auditdialog.cpp addGrepCheck): (TODO|FIXME|HACK|XXX)(\(|:|\s)
// Each `@expect todo_scan` marker below tags a line that MUST match.

#include <stdio.h>

int main(void) {
    // TODO: rewrite this           // @expect todo_scan
    // FIXME leaks on early return  // @expect todo_scan
    /* HACK(tmp) silences warning */ // @expect todo_scan
    // XXX: revisit after refactor  // @expect todo_scan
    return 0;
}
