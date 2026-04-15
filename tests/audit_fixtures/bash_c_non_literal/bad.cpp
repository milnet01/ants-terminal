// Pretend C++ source. Each `-c` paired with an identifier (rather than
// a string literal) should match — this is the dangerous pattern where
// a runtime value is interpreted as shell code.

void unsafe1(const QString &userInput) {
    QProcess p;
    p.start("/bin/bash", {"-c", userInput});  // @expect bash_c_non_literal
}

void unsafe2(const QString &cfg) {
    QProcess::startDetached("/bin/sh", {"-c", cfg});  // @expect bash_c_non_literal
}

void unsafe3() {
    const QString cmd = buildCommandFromConfig();
    QProcess().start("bash", {"-c", cmd});  // @expect bash_c_non_literal
}
