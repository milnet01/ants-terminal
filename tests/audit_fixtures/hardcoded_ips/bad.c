// Fixture for the `hardcoded_ips` audit rule.
//
// Rule regex: \b([0-9]{1,3}\.){3}[0-9]{1,3}\b — four dotted 1-3 digit groups.
// The full check in auditdialog.cpp drops version/license strings; the test
// harness only exercises the regex, so keep bad.* to obvious LAN/WAN IPs.

#include <stdio.h>

static const char *PEERS[] = {
    "10.0.0.1",            // @expect hardcoded_ips
    "192.168.1.42",        // @expect hardcoded_ips
    "8.8.8.8",             // @expect hardcoded_ips
};

int main(void) {
    printf("peer: %s\n", PEERS[0]);
    return 0;
}
