// Claude Code UserPromptSubmit git-context hook — installer source-grep
// regression test. See spec.md.
//
// Asserts that SettingsDialog::installClaudeGitContextHook exists, writes
// the canonical script, targets UserPromptSubmit, preserves existing
// entries via the dedup loop, and refuses to overwrite a corrupt
// ~/.claude/settings.json.

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#ifndef SRC_SETTINGSDIALOG_CPP
#error "SRC_SETTINGSDIALOG_CPP compile definition required"
#endif
#ifndef SRC_SETTINGSDIALOG_H
#error "SRC_SETTINGSDIALOG_H compile definition required"
#endif
#ifndef SRC_CONFIGPATHS_H
#error "SRC_CONFIGPATHS_H compile definition required"
#endif

static std::string slurp(const char *path)
{
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(2);
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int main()
{
    const std::string sdH   = slurp(SRC_SETTINGSDIALOG_H);
    const std::string sdCpp = slurp(SRC_SETTINGSDIALOG_CPP);
    const std::string cpH   = slurp(SRC_CONFIGPATHS_H);

    int failures = 0;
    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };

    // INV-1a: ConfigPaths helper exists for the script path.
    if (cpH.find("antsClaudeGitContextScript") == std::string::npos) {
        fail("INV-1a: ConfigPaths::antsClaudeGitContextScript() must exist "
             "(src/configpaths.h) so the script path has a single source of truth");
    }
    if (cpH.find("claude-git-context.sh") == std::string::npos) {
        fail("INV-1b: ConfigPaths::antsClaudeGitContextScript() must point at "
             "the canonical 'claude-git-context.sh' filename");
    }

    // INV-5a: SettingsDialog declares the installer + refresh methods.
    if (sdH.find("installClaudeGitContextHook") == std::string::npos) {
        fail("INV-5a: SettingsDialog::installClaudeGitContextHook() must be "
             "declared in settingsdialog.h");
    }
    if (sdH.find("refreshClaudeGitContextStatus") == std::string::npos) {
        fail("INV-5a (cont): SettingsDialog::refreshClaudeGitContextStatus() "
             "must be declared alongside the installer");
    }

    // INV-5 (main): locate the installer body. Anchor on the method
    // definition ("void SettingsDialog::installClaudeGitContextHook"),
    // not any earlier mention in the file — the UI-wiring line
    // "&SettingsDialog::installClaudeGitContextHook" also contains the
    // identifier but lives hundreds of lines before the body, so a
    // substring match there would slice the wrong region.
    const std::string anchor = "void SettingsDialog::installClaudeGitContextHook";
    const std::size_t installerPos = sdCpp.find(anchor);
    if (installerPos == std::string::npos) {
        fail("INV-5 (def): SettingsDialog::installClaudeGitContextHook() "
             "definition missing from settingsdialog.cpp");
        std::fprintf(stderr, "\n%d invariant(s) failed — see spec.md\n", failures);
        return 1;
    }
    // Span from the installer definition to the next top-level
    // `void SettingsDialog::` (or end of file). The script body alone
    // is ~2 KiB of string literal; a naive 6 KiB slice truncated the
    // latter half of the function on an earlier iteration.
    std::size_t nextDef = sdCpp.find("\nvoid SettingsDialog::",
                                      installerPos + anchor.size());
    if (nextDef == std::string::npos) nextDef = sdCpp.size();
    const std::string installer = sdCpp.substr(installerPos, nextDef - installerPos);

    // INV-1 (runtime use): the installer writes the path from the
    // ConfigPaths helper, not a hard-coded string.
    if (installer.find("antsClaudeGitContextScript") == std::string::npos &&
        installer.find("gitContextScriptPath") == std::string::npos) {
        fail("INV-1 (use): installer must derive the script path from "
             "ConfigPaths::antsClaudeGitContextScript() (directly or via a "
             "file-local helper) — no hard-coded path literals");
    }

    // INV-3: the script body contains all six contract-line markers so a
    // future edit can't silently change the emitted format.
    for (const char *needle : {"<git-context>", "</git-context>",
                               "Branch: ", "Upstream: ",
                               "Staged: ", "Unstaged: ", "Untracked: "}) {
        if (installer.find(needle) == std::string::npos) {
            std::fprintf(stderr, "  missing needle: %s\n", needle);
            fail("INV-3: the installed script body must contain every "
                 "contract-line marker — see spec.md Invariant 3");
        }
    }

    // INV-3 (CLAUDE_PROJECT_DIR): the script respects Claude Code's
    // canonical cwd env var.
    if (installer.find("CLAUDE_PROJECT_DIR") == std::string::npos) {
        fail("INV-8: the script must honor $CLAUDE_PROJECT_DIR — without it "
             "the hook runs git in whatever cwd Claude Code happened to "
             "spawn it, which may not match the session's project root");
    }

    // INV-5b: installer targets UserPromptSubmit in the merged JSON.
    if (installer.find("UserPromptSubmit") == std::string::npos) {
        fail("INV-5b: installer must target hooks.UserPromptSubmit in "
             "~/.claude/settings.json — that's the event Claude Code fires "
             "before each user message");
    }

    // INV-5c: parse-error-refuse pattern is reused verbatim — the corrupt
    // settings file is rotated aside, not clobbered.
    if (installer.find("rotateCorruptFileAside") == std::string::npos) {
        fail("INV-5c: installer must call rotateCorruptFileAside() on a "
             "parse failure — matches the 0.7.12 /indie-review cross-cutting "
             "fix shape (the settings file may contain unrelated Claude-Code "
             "config the user hand-edited)");
    }
    // And the refusal path must return before writing.
    std::regex refuseAndReturn(R"(rotateCorruptFileAside[\s\S]{0,1500}?return\s*;)");
    if (!std::regex_search(installer, refuseAndReturn)) {
        fail("INV-5c (cont): after rotateCorruptFileAside, installer must "
             "return without writing — otherwise a corrupt settings.json "
             "gets silently overwritten");
    }

    // INV-5d + INV-6 + INV-10: dedup loop prevents appending the same
    // entry twice and preserves user-added sibling entries.
    if (installer.find("ourHookPresent") == std::string::npos) {
        fail("INV-6/10: installer must dedup before appending via an "
             "`ourHookPresent` (or equivalent) scan of existing entries — "
             "prevents (a) duplicates on re-install (INV-6) and (b) clobbering "
             "pre-existing user hooks on the same event (INV-10)");
    }

    // INV-7: installer writes the global settings file, not a project-
    // scope one. `.claude/settings.json` appearing as a substring alone
    // would be fine because the GLOBAL path also ends in that suffix, but
    // we guard against anyone reaching for a project path by requiring
    // the claudeSettingsPath() helper OR the ConfigPaths equivalent.
    if (installer.find("claudeSettingsPath") == std::string::npos &&
        installer.find("claudeSettingsJson") == std::string::npos) {
        fail("INV-7: installer must use claudeSettingsPath() / "
             "ConfigPaths::claudeSettingsJson() — the global ~/.claude/"
             "settings.json path. Project-scope installs would narrow the "
             "feature and write into arbitrary project dirs (forbidden)");
    }

    // INV-9: dedicated UI button wired to installClaudeGitContextHook
    // via a connect() that references the button by name. Upper bound
    // covers the button's tooltip + sibling-label init lines that live
    // between the assignment and the connect.
    const std::regex btnWire(
        R"(connect\(\s*m_installClaudeGitContextBtn\b[\s\S]{0,200}?installClaudeGitContextHook)");
    if (!std::regex_search(sdCpp, btnWire)) {
        fail("INV-9: Settings dialog must wire m_installClaudeGitContextBtn "
             "to installClaudeGitContextHook via QPushButton::clicked — the "
             "user-facing opt-in surface");
    }

    if (failures > 0) {
        std::fprintf(stderr, "\n%d invariant(s) failed — see spec.md\n", failures);
        return 1;
    }
    std::printf("OK: claude_git_context_hook installer invariants present\n");
    return 0;
}
