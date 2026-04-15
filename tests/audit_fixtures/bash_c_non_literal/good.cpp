// Pretend C++ source. Each `-c` is paired with a string literal — the
// command is hardcoded, so there's no injection risk.

void safe1() {
    QProcess p;
    p.start("/bin/bash", {"-c", "echo hello"});
}

void safe2() {
    QProcess::startDetached("/bin/sh", {"-c", "ls -la /tmp"});
}

// argv-style start (not via shell) is also safe — no shell parsing.
void safe3(const QString &userInput) {
    QProcess p;
    p.start("/usr/bin/grep", {"-c", userInput});  // -c here is grep's count flag
}
// The above is a noted limitation: grep -c with userInput would also be
// flagged if it appeared with bash/sh in the same call. Acceptable
// noise — the rule is `-c` adjacent to an identifier in *any* QProcess
// call. Manual review distinguishes.
