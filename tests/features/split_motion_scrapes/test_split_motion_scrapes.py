#!/usr/bin/env python3
"""ANTS-6423 / ANTS-6424 / ANTS-6657 — see spec.md. Exits 77 when git is missing."""
import os
import shutil
import subprocess
import sys
import tempfile

TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                    "..", "..", "..", "tools", "split-motion-check.py")

TERNARY_TEST = r'''
TEST(X, Window) {
    const std::string ci = ants_test::slurpClaudeIntegration();
    const auto pos = ci.find("ANCHOR");
    const auto endPos = ci.find("END", pos);
    const std::string w = endPos == std::string::npos ? ci.substr(pos)
                                                      : ci.substr(pos, endPos - pos);
}
'''

OPEN_TEST = r'''
TEST(X, Open) {
    const std::string ci = ants_test::slurpClaudeIntegration();
    const std::string w = ci.substr(ci.find("ANCHOR"));
}
'''

# The callable's window never closes: its bracket has no match before EOF.
UNCLOSED_TEST = r'''
TEST(X, Callable) {
    const std::string ci = ants_test::slurpClaudeIntegration();
    cut(ci);
}

static std::string cut(const std::string &t) {
    return t.substr(0, t.find("END");
}
'''


def run_case(test_src: str, pre: str, post: str) -> subprocess.CompletedProcess:
    repo = tempfile.mkdtemp(prefix="split-motion-")
    try:
        def git(*args):
            subprocess.run(["git", *args], cwd=repo, check=True, capture_output=True,
                           env={**os.environ, "GIT_CONFIG_GLOBAL": "/dev/null",
                                "GIT_CONFIG_NOSYSTEM": "1"})
        git("init", "-q")
        os.makedirs(os.path.join(repo, "src"))
        os.makedirs(os.path.join(repo, "tests"))
        with open(os.path.join(repo, "tests", "a_test.cpp"), "w") as f:
            f.write(test_src)
        for text in (pre, post):
            with open(os.path.join(repo, "src", "claudeintegration.cpp"), "w") as f:
                f.write(text)
            git("add", "-A")
            git("-c", "user.name=T", "-c", "user.email=t@example.invalid",
                "commit", "-q", "-m", "c")
        proc = subprocess.run([sys.executable, TOOL, "--scrapes", "--pre", "HEAD~1",
                               "--post", "HEAD", "--stem", "claudeintegration"],
                              cwd=repo, capture_output=True, text=True)
        if proc.returncode not in (0, 1):
            print(proc.stdout, proc.stderr)
        return proc
    finally:
        shutil.rmtree(repo, ignore_errors=True)


def main() -> int:
    if shutil.which("git") is None:
        return 77
    failures = 0

    def expect(name, got, want):
        nonlocal failures
        if got != want:
            failures += 1
            print("FAIL %s: exit %d, expected %d" % (name, got, want))

    # INV-1: END is found at both revisions, so ci.substr(pos) is never taken;
    # only the text after END differs.
    expect("INV-1 untaken arm", run_case(TERNARY_TEST, "a ANCHOR b END tail one\n",
                                         "a ANCHOR b END tail two\n").returncode, 0)
    # INV-2: text appended after everything the open window's parent held.
    expect("INV-2 appended", run_case(OPEN_TEST, "a ANCHOR b\n", "a ANCHOR b\nmore\n").returncode, 0)
    # INV-3: the text the bounded arm reads changes.
    expect("INV-3 real change", run_case(TERNARY_TEST, "a ANCHOR b END\n",
                                         "a ANCHOR c END\n").returncode, 1)
    expect("INV-3 open window changed",
           run_case(OPEN_TEST, "a ANCHOR b\n", "a ANCHOR c\n").returncode, 1)
    # INV-4: an unclosed window in a callable is listed by its head, not as "".
    out = run_case(UNCLOSED_TEST, "a ANCHOR b END\n", "a ANCHOR c END\n").stdout
    if "`t.substr(`" not in out:
        failures += 1
        print("FAIL INV-4 unclosed window: no `t.substr(` site in:\n" + out)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
