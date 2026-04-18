// Threaded ptyWrite gating — see spec.md.
//
// Source inspection: no `if (m_pty)` or `&& m_pty)` gate on any ptyWrite
// call site, because `m_pty` is null on the worker path and the gate
// would silently swallow every write. This is the 0.6.34 keystroke-
// not-echoing regression — pinned here so it can't come back.

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#ifndef SRC_TERMINALWIDGET_PATH
#error "SRC_TERMINALWIDGET_PATH compile definition required"
#endif

static std::string slurp(const char *path) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(2);
    }
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

int main() {
    const std::string src = slurp(SRC_TERMINALWIDGET_PATH);
    int failures = 0;

    // Split into lines so we can report line numbers on failures.
    std::vector<std::string> lines;
    {
        std::stringstream ss(src);
        std::string line;
        while (std::getline(ss, line)) lines.push_back(line);
    }

    // (1) No line may contain both `m_pty` and `ptyWrite(`. A call site
    //     gating ptyWrite on m_pty means the worker path (where m_pty
    //     is null) silently drops the write — the exact 0.6.34 bug.
    //     The one-line `if (m_pty) ptyWrite(...)` form and the
    //     `&& m_pty)` form both trip this.
    std::regex ptyWriteCall(R"(\bptyWrite\s*\()");
    std::regex badGate(R"(\bm_pty\b)");
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string &line = lines[i];
        if (!std::regex_search(line, ptyWriteCall)) continue;
        if (!std::regex_search(line, badGate)) continue;
        // Allowlist: the ptyWrite helper itself carries both tokens for
        // the legacy-path fallthrough.
        if (line.find("if (auto *p = m_pty) p->write(data)") != std::string::npos) continue;
        std::fprintf(stderr,
                     "FAIL: line %zu: ptyWrite() gated by m_pty — worker "
                     "path will drop this write\n    %s\n",
                     i + 1, line.c_str());
        ++failures;
    }

    // (2) No multi-line gate of the form `if (... m_pty) {` followed by
    //     a ptyWrite(...) call on a later line, unless the `m_pty`
    //     mention is part of hasPty() (we don't grep for that — false
    //     negative is fine, we mainly want to catch the sloppy-edit
    //     class). Scan only: `m_pty)` without a `->` after it.
    //     Allowlisted contexts: startShell, recalcGridSize helper,
    //     destructor, ptyWrite, ptyChildPid definitions.
    //
    //     Implementation: find every `m_pty)` occurrence (pointer used
    //     as a boolean); assert each appears in an allowlisted
    //     function scope.
    std::regex mPtyBareBool(R"(\bm_pty\))");
    auto allowed = [&](size_t lineIdx) -> bool {
        // Walk backwards until we hit a "void TerminalWidget::FN(" /
        // "bool TerminalWidget::FN(" / "pid_t TerminalWidget::FN(".
        std::regex funcDef(R"(TerminalWidget::(ptyWrite|ptyChildPid|startShell|recalcGridSize|~TerminalWidget)\b)");
        for (size_t j = lineIdx + 1; j-- > 0;) {
            if (std::regex_search(lines[j], funcDef)) return true;
            // Stop if we hit another top-level function definition.
            static const std::regex anyFuncDef(R"(^\w.*TerminalWidget::\w+\s*\()");
            if (std::regex_search(lines[j], anyFuncDef)) return false;
        }
        return false;
    };
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string &line = lines[i];
        if (!std::regex_search(line, mPtyBareBool)) continue;
        // Ignore member-access forms: `m_pty->`, `m_pty =`, `m_pty,`.
        // The regex above already requires `m_pty)` with a close-paren,
        // which matches bool-cast uses. `m_pty ? ... : ...` is caught
        // separately below.
        if (allowed(i)) continue;
        std::fprintf(stderr,
                     "FAIL: line %zu: bare `m_pty)` boolean check outside the "
                     "allowlisted legacy-path functions — worker path will "
                     "misbehave\n    %s\n",
                     i + 1, line.c_str());
        ++failures;
    }

    // (3) No `&& m_pty` or `!m_pty` gate outside allowlisted functions.
    std::regex amperstand(R"(&&\s*m_pty\b)");
    std::regex bang(R"(!\s*m_pty\b)");
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string &line = lines[i];
        bool hit = std::regex_search(line, amperstand) || std::regex_search(line, bang);
        if (!hit) continue;
        if (allowed(i)) continue;
        std::fprintf(stderr,
                     "FAIL: line %zu: `&& m_pty` or `!m_pty` gate outside "
                     "allowlisted functions — worker path will silently drop "
                     "writes\n    %s\n",
                     i + 1, line.c_str());
        ++failures;
    }

    if (failures) {
        std::fprintf(stderr, "\n%d assertion(s) failed\n", failures);
        return 1;
    }
    std::printf("threaded_ptywrite_gating: OK\n");
    return 0;
}
