// Threaded resize synchronous — see spec.md.
//
// Static inspection of src/terminalwidget.cpp + src/vtstream.h to pin
// the BlockingQueuedConnection contract on resize. A silent regression
// to QueuedConnection would not show up at runtime until under heavy
// load, by which point the flicker would ship.

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#ifndef SRC_TERMINALWIDGET_PATH
#error "SRC_TERMINALWIDGET_PATH compile definition required"
#endif
#ifndef SRC_VTSTREAM_HEADER_PATH
#error "SRC_VTSTREAM_HEADER_PATH compile definition required"
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
    int failures = 0;

    const std::string widget = slurp(SRC_TERMINALWIDGET_PATH);
    const std::string header = slurp(SRC_VTSTREAM_HEADER_PATH);

    // (1) Every invokeMethod(..., "resize", ...) on m_vtStream must be
    //     BlockingQueuedConnection. We match the full call up to the
    //     ConnectionType argument.
    std::regex invokeResize(
        R"(QMetaObject::invokeMethod\s*\(\s*m_vtStream\s*,\s*"resize"\s*,\s*([A-Za-z_:]+))",
        std::regex::ECMAScript);

    auto it = std::sregex_iterator(widget.begin(), widget.end(), invokeResize);
    auto end = std::sregex_iterator();

    int invokes = 0;
    for (; it != end; ++it) {
        ++invokes;
        const std::string conn = (*it)[1].str();
        if (conn != "Qt::BlockingQueuedConnection") {
            std::fprintf(stderr,
                         "FAIL: invokeMethod(m_vtStream, \"resize\", ...) "
                         "uses %s, expected Qt::BlockingQueuedConnection\n",
                         conn.c_str());
            ++failures;
        }
    }
    if (invokes == 0) {
        std::fprintf(stderr,
                     "FAIL: no invokeMethod(m_vtStream, \"resize\", ...) "
                     "calls found — has the worker-path resize been removed?\n");
        ++failures;
    } else {
        std::printf("resize invocations found: %d (all BlockingQueuedConnection)\n",
                    invokes);
    }

    // (2) VtStream::resize must be declared as a slot so invokeMethod
    //     by string name resolves at runtime. Look for public slots:
    //     block containing a `resize(int` signature.
    //
    //     Relaxed regex: find "public slots:" and scan forward up to
    //     the next access specifier (or class close) for a resize(
    //     declaration. Tighter check would parse C++ which is
    //     overkill here.
    std::regex publicSlots(R"(public\s+slots\s*:)");
    auto slotsMatch = std::sregex_iterator(header.begin(), header.end(), publicSlots);
    if (slotsMatch == std::sregex_iterator()) {
        std::fprintf(stderr, "FAIL: no `public slots:` block in vtstream.h\n");
        ++failures;
    } else {
        // Take the tail after public slots: and look for `void resize(int`
        size_t pos = slotsMatch->position() + slotsMatch->length();
        std::string tail = header.substr(pos);
        // Stop at next access specifier or class-close to avoid matching
        // private helpers with the same name.
        size_t stop = tail.find_first_of("}");
        size_t stopPrivate = tail.find("private");
        size_t stopSignals = tail.find("signals");
        size_t clip = std::min({stop, stopPrivate, stopSignals});
        if (clip != std::string::npos) tail = tail.substr(0, clip);

        std::regex resizeDecl(R"(\bvoid\s+resize\s*\(\s*int\b)");
        if (!std::regex_search(tail, resizeDecl)) {
            std::fprintf(stderr,
                         "FAIL: VtStream::resize(int, ...) is not declared "
                         "in the public slots: block — invokeMethod(\"resize\") "
                         "will fail at runtime.\n");
            ++failures;
        }
        std::regex writeDecl(R"(\bvoid\s+write\s*\(\s*const\s+QByteArray\b)");
        if (!std::regex_search(tail, writeDecl)) {
            std::fprintf(stderr,
                         "FAIL: VtStream::write(const QByteArray&) is not "
                         "declared in the public slots: block — "
                         "invokeMethod(\"write\") will fail at runtime.\n");
            ++failures;
        }
    }

    if (failures) {
        std::fprintf(stderr, "\n%d assertion(s) failed\n", failures);
        return 1;
    }
    std::printf("threaded_resize_synchronous: OK\n");
    return 0;
}
