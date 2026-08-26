// Feature-conformance test for spec.md — ANTS-2132.
//
// Source-grep lock over the async-dispatch structure. The runtime
// observations (a GUI tick during a verb, arrival order, the queue cap)
// belong to tests/features/mcp_async_dispatch/, which drives a real
// ClaudeIntegration; this file holds the four invariants only a scrape can
// hold, because each is a claim about where code IS rather than what it does.

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"

#include <cctype>
#include <regex>
#include <set>
#include <string>
#include <vector>
#include <gtest/gtest.h>

#ifndef SRC_MAINWINDOW_CPP_PATH
#  error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif
#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#  error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif
#ifndef ANTS_RC_SOURCES
#  error "ANTS_RC_SOURCES compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {

// Body of the function whose signature starts at `defMarker`: from that
// marker to the first line that is exactly "}" at column 0. Empty when the
// marker is absent, so a caller's expect() reports the miss rather than
// silently asserting over nothing.
std::string bodyAfter(const std::string &hay, const std::string &defMarker) {
    const size_t at = hay.find(defMarker);
    if (at == std::string::npos) return {};
    const size_t end = hay.find("\n}\n", at);
    return hay.substr(at, end == std::string::npos ? std::string::npos
                                                   : end + 3 - at);
}

// End offset of the call whose opening paren follows `open`, counting nested
// parens and skipping string / char literals so a "(" inside a literal cannot
// unbalance the scan.
size_t callEnd(const std::string &s, size_t open) {
    int depth = 0;
    for (size_t i = open; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '"' || c == '\'') {
            const char q = c;
            for (++i; i < s.size(); ++i) {
                if (s[i] == '\\') { ++i; continue; }
                if (s[i] == q) break;
            }
            continue;
        }
        if (c == '(') ++depth;
        else if (c == ')' && --depth == 0) return i;
    }
    return s.size();
}

struct Def {
    size_t offset;
    std::string cmd;  // "" when the definition is not a RemoteControl:: member
};

// Every definition line at column 0, in file order. Body lines are indented,
// so the nearest preceding entry is the body a given offset sits in.
std::vector<Def> definitions(const std::string &s) {
    std::vector<Def> out;
    size_t line = 0;
    while (line < s.size()) {
        const size_t eol = s.find('\n', line);
        const size_t len = (eol == std::string::npos ? s.size() : eol) - line;
        if (len > 0 && !std::isspace(static_cast<unsigned char>(s[line])) &&
            s.compare(line, 2, "//") != 0 && s[line] != '#' &&
            s.find('(', line) < line + len) {
            std::string cmd;
            const size_t rc = s.find("RemoteControl::", line);
            if (rc != std::string::npos && rc < line + len) {
                size_t p = rc + std::string("RemoteControl::").size();
                while (p < s.size() &&
                       (std::isalnum(static_cast<unsigned char>(s[p])) ||
                        s[p] == '_'))
                    cmd += s[p++];
            }
            out.push_back({line, cmd});
        }
        if (eol == std::string::npos) break;
        line = eol + 1;
    }
    return out;
}

std::vector<std::string> rcSourcePaths() {
    const std::string list = ANTS_RC_SOURCES;
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= list.size()) {
        const size_t sep = list.find(';', start);
        const size_t end = (sep == std::string::npos) ? list.size() : sep;
        if (end > start) out.push_back(list.substr(start, end - start));
        if (sep == std::string::npos) break;
        start = sep + 1;
    }
    return out;
}

std::string baseName(const std::string &path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

TEST(McpVerbOffthreadGuard, Main) {
    expect_reset();

    const std::string mw = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    const std::string ci =
        ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    expect(!mw.empty(), "load/mainwindow.cpp");
    expect(!ci.empty(), "load/claudeintegration.cpp");

    const std::string dispatcher =
        bodyAfter(ci, "void ClaudeIntegration::onMcpConnection() {");
    const std::string finish =
        bodyAfter(ci, "void ClaudeIntegration::finishToolDispatch(");
    const std::string post =
        bodyAfter(ci, "bool ClaudeIntegration::postToolDispatch(");
    const std::string teardown =
        bodyAfter(ci, "void ClaudeIntegration::shutdownDispatchWorker() {");
    // The factory is a lambda nested inside setupClaudeMcpProviders(), so its
    // body ends at the lambda's own "    };" — not at a column-0 brace.
    const std::string factory = [&mw]() -> std::string {
        const size_t at = mw.find("    auto rcDelegate =");
        if (at == std::string::npos) return {};
        const size_t end = mw.find("\n    };\n", at);
        return mw.substr(at, end == std::string::npos ? std::string::npos
                                                      : end - at);
    }();

    // INV-3 — the dispatch path spins no nested QEventLoop on the GUI thread
    // (ANTS-2131 preserved). audit_run and indie_review_dispatch stay inline
    // and are locked by tests/features/socket_readyread_uaf_guard/, not here.
    expect(!dispatcher.empty(), "INV-3/onMcpConnection-found");
    expect(dispatcher.find("QEventLoop") == std::string::npos,
           "INV-3/dispatcher-has-no-nested-loop");
    expect(!factory.empty(), "INV-3/rcDelegate-factory-found");
    expect(factory.find("QEventLoop") == std::string::npos &&
               factory.find("wait()") == std::string::npos,
           "INV-3/rcDelegate-neither-pumps-nor-joins");
    // The deleted ANTS-2131 factory must not come back: under this design it
    // would be byte-for-byte rcDelegate (spec § 2.4).
    expect(mw.find("auto rcDelegateWorker =") == std::string::npos,
           "INV-3/worker-factory-stays-deleted");

    // INV-7 — the GUI thread never blocks on the dispatch worker while
    // serving a request. The teardown join is the sole exception, and it is
    // reached only from the destructor.
    expect(!teardown.empty(), "INV-7/shutdownDispatchWorker-found");
    expect(teardown.find("m_dispatchWorker->wait()") != std::string::npos,
           "INV-7/teardown-joins");
    {
        size_t joins = 0;
        for (size_t at = ci.find("m_dispatchWorker->wait()");
             at != std::string::npos;
             at = ci.find("m_dispatchWorker->wait()", at + 1))
            ++joins;
        expect(joins == 1, "INV-7/exactly-one-join-in-the-file");
    }
    expect(!post.empty() && post.find("->wait()") == std::string::npos,
           "INV-7/postToolDispatch-does-not-join");
    expect(!finish.empty() && finish.find("->wait()") == std::string::npos,
           "INV-7/finishToolDispatch-does-not-join");
    expect(bodyAfter(ci, "ClaudeIntegration::~ClaudeIntegration() {")
                   .find("shutdownDispatchWorker()") != std::string::npos,
           "INV-7/destructor-runs-the-teardown");

    // INV-9 — one response pipeline, shared by both paths. A second
    // definition, or a wrap done inside the dispatcher, would let the
    // synchronous and deferred replies drift apart.
    {
        size_t defs = 0;
        const std::string marker = "void ClaudeIntegration::finishToolDispatch(";
        for (size_t at = ci.find(marker); at != std::string::npos;
             at = ci.find(marker, at + 1))
            ++defs;
        expect(defs == 1, "INV-9/one-finishToolDispatch-definition");
    }
    expect(finish.find("wrapMcpData(") != std::string::npos,
           "INV-9/the-wrap-lives-in-the-pipeline");
    expect(dispatcher.find("wrapMcpData(") == std::string::npos,
           "INV-9/dispatcher-keeps-no-second-wrap");
    expect(dispatcher.find("finishToolDispatch(ctx,") != std::string::npos,
           "INV-9/dispatcher-hands-off-to-the-pipeline");

    // INV-6 — outside a body that always runs on the GUI thread, no verb
    // reaches MainWindow except through ants::onGuiThread. The exempt set is
    // DERIVED from the registration table, never hard-coded: a verb is exempt
    // only while it is not registered off-thread (TabSpecific, registered
    // through an inline lambda, or a --remote CLI verb that is not registered
    // at all). Move one to rcDelegate and its unwrapped reads fail here.
    std::set<std::string> offThreadCmds;
    {
        static const std::regex rx(
            R"RX(registerToolProvider\("[^"]+",\s*ClaudeIntegration::CallerCwdContract::(\w+),\s*rcDelegate\(&RemoteControl::(\w+)\))RX");
        for (auto it = std::sregex_iterator(mw.begin(), mw.end(), rx);
             it != std::sregex_iterator(); ++it) {
            if ((*it)[1].str() != "TabSpecific")
                offThreadCmds.insert((*it)[2].str());
        }
        expect(!offThreadCmds.empty(), "INV-6/registration-table-parsed");
    }

    // Matches the arrow, not a list of accessor names, so a NEW MainWindow
    // accessor cannot be added without this test noticing.
    static const std::regex reach(R"RX(\b(m_main|main)->[A-Za-z_]+)RX");
    for (const std::string &path : rcSourcePaths()) {
        const std::string src = ants_test::slurpFile(path);
        expect(!src.empty(), "INV-6/load", baseName(path).c_str());
        if (src.empty()) continue;

        std::vector<std::pair<size_t, size_t>> marshalled;
        for (size_t at = src.find("onGuiThread("); at != std::string::npos;
             at = src.find("onGuiThread(", at + 1)) {
            const size_t open = src.find('(', at);
            marshalled.emplace_back(open, callEnd(src, open));
        }
        const std::vector<Def> defs = definitions(src);

        for (auto it = std::sregex_iterator(src.begin(), src.end(), reach);
             it != std::sregex_iterator(); ++it) {
            const size_t at = static_cast<size_t>(it->position(0));
            bool wrapped = false;
            for (const auto &span : marshalled)
                if (at > span.first && at < span.second) { wrapped = true; break; }
            if (wrapped) continue;

            std::string owner;
            for (const auto &d : defs) {
                if (d.offset > at) break;
                owner = d.cmd;
            }
            const bool exempt = !owner.empty() && !offThreadCmds.count(owner);
            const std::string where =
                baseName(path) + ":" + (owner.empty() ? "<free function>" : owner);
            expect(exempt, "INV-6/MainWindow-read-must-be-marshalled",
                   where.c_str());
        }
    }


    // INV-11 — ANTS-4682. INV-6's scrape covers remotecontrol*.cpp and matches
    // `m_main->`. An INLINE handler lives in mainwindow.cpp and reaches
    // MainWindow through its OWN members, which that scrape cannot see — so
    // the moment an inline verb is registered off-thread (ANTS-4682 moved
    // six), an unmarshalled member read there is INV-6's defect with no guard
    // on it. Derived, not listed: the subject is every RcHandler{ registration
    // in this file, so moving one more inline verb off-thread enrols it here.
    // Passing a bare `this` to an already-marshalled free function
    // (ants::resolveCallerCwdRoot) is not a member read and is not matched.
    {
        static const std::regex member(R"RX(\bm_[A-Za-z_]+)RX");
        static const std::regex nameRx(R"RX(registerToolProvider\("([^"]+)")RX");
        // Segment the file by the registration statements themselves rather
        // than paren-matching a call: callEnd() skips ' as a char literal, so
        // an apostrophe in a comment inside a handler body ("the caller's
        // cwd") opens a literal that never closes and the span runs off into
        // unrelated code. This bound also terminates the LAST registration,
        // which has no following registration to stop at, and it excludes the
        // rcDelegate factory's own RcHandler{ — that is a factory definition,
        // not a registration, and carries no verb name.
        static const std::string kStmt = "\n    m_claudeIntegration->";
        std::vector<size_t> bounds;
        for (size_t b = mw.find(kStmt); b != std::string::npos;
             b = mw.find(kStmt, b + 1))
            bounds.push_back(b);
        bounds.push_back(mw.size());

        size_t seen = 0;
        for (size_t i = 0; i + 1 < bounds.size(); ++i) {
            const std::string seg = mw.substr(bounds[i], bounds[i + 1] - bounds[i]);
            const size_t rc = seg.find("RcHandler{");
            if (rc == std::string::npos) continue;
            std::smatch nm;
            if (!std::regex_search(seg, nm, nameRx)) continue;  // not a registration
            const std::string verb = nm[1].str();

            std::vector<std::pair<size_t, size_t>> marshalled;
            for (size_t g = seg.find("onGuiThread("); g != std::string::npos;
                 g = seg.find("onGuiThread(", g + 1)) {
                const size_t o = seg.find('(', g);
                marshalled.emplace_back(o, callEnd(seg, o));
            }

            // Scan only the HANDLER, so the registration's own
            // `m_claudeIntegration->` receiver is not itself a finding.
            for (auto it = std::sregex_iterator(seg.begin() + static_cast<long>(rc),
                                                seg.end(), member);
                 it != std::sregex_iterator(); ++it) {
                const size_t off = rc + static_cast<size_t>(it->position(0));
                bool wrapped = false;
                for (const auto &sp : marshalled)
                    if (off > sp.first && off < sp.second) { wrapped = true; break; }
                expect(wrapped,
                       "INV-11/off-thread-inline-member-read-must-be-marshalled",
                       (verb + ":" + it->str(0)).c_str());
            }
            ++seen;
        }
        expect(seen > 0, "INV-11/off-thread-inline-registrations-found");
    }

    EXPECT_EQ(0, expect_failures());
}
