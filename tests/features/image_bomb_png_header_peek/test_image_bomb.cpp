// Feature-conformance test for spec.md — enforces that every
// QImage::loadFromData() call on untrusted inline image bytes in
// terminalgrid.cpp is gated by a QImageReader-based dimension peek.
//
// Static inspection rather than runtime fuzz: a live image-bomb test
// would either succeed (no regression signal) or hang the test
// process (no signal either), both unsuitable for CI. The regression
// class we guard against is "future edit drops the peek" — perfectly
// suited to grep.
//
// Exit 0 = all invariants hold. Non-zero = regression.

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#ifndef SRC_TERMINALGRID_PATH
#error "SRC_TERMINALGRID_PATH must be baked at compile time"
#endif
#ifndef SRC_TERMINALGRID_HEADER
#error "SRC_TERMINALGRID_HEADER must be baked at compile time"
#endif

namespace {

std::vector<std::string> readLines(const char *path) {
    std::ifstream f(path);
    std::vector<std::string> out;
    std::string line;
    while (std::getline(f, line)) out.push_back(line);
    return out;
}

bool linePrecedingContainsQImageReader(const std::vector<std::string> &lines,
                                       size_t idx, size_t window) {
    size_t start = (idx > window) ? idx - window : 0;
    for (size_t i = start; i < idx; ++i) {
        if (lines[i].find("QImageReader") != std::string::npos)
            return true;
    }
    return false;
}

int fail(const char *label, const std::string &detail) {
    std::fprintf(stderr, "FAIL [%s]: %s\n", label, detail.c_str());
    return 1;
}

}  // namespace

int main() {
    const auto lines = readLines(SRC_TERMINALGRID_PATH);
    if (lines.empty())
        return fail("setup",
                    std::string("could not read ") + SRC_TERMINALGRID_PATH);

    int loadCalls = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string &l = lines[i];
        if (l.find(".loadFromData(") == std::string::npos) continue;
        // Skip comment-only lines (e.g. doc comments that mention the API).
        size_t firstNonSpace = l.find_first_not_of(" \t");
        if (firstNonSpace != std::string::npos
            && l.compare(firstNonSpace, 2, "//") == 0) continue;

        ++loadCalls;
        bool hasMarker = l.find("// image-peek-ok") != std::string::npos;
        bool hasPeek = linePrecedingContainsQImageReader(lines, i, 10);
        if (!hasMarker && !hasPeek) {
            std::fprintf(stderr,
                "FAIL [I1]: loadFromData call at %s:%zu has no "
                "QImageReader peek in the preceding 10 lines and no "
                "// image-peek-ok marker.\n  line: %s\n",
                SRC_TERMINALGRID_PATH, i + 1, l.c_str());
            return 1;
        }
    }

    if (loadCalls == 0)
        return fail("I1 coverage",
                    "expected at least one loadFromData call in terminalgrid.cpp "
                    "(0 found — test invariant is meaningless without a guarded "
                    "call site; did the image-decode paths move?)");

    // I2 — MAX_IMAGE_DIM must be defined (header) and used (implementation).
    // Definition lives in terminalgrid.h as a static constexpr class member;
    // usages live in terminalgrid.cpp around every decode allocation path.
    const auto headerLines = readLines(SRC_TERMINALGRID_HEADER);
    bool dimDefined = false;
    for (const auto &l : headerLines) {
        if (l.find("MAX_IMAGE_DIM") == std::string::npos) continue;
        if (l.find("constexpr") != std::string::npos
            || l.find("const ") != std::string::npos
            || l.find("#define") != std::string::npos) {
            dimDefined = true;
            break;
        }
    }
    bool dimUsed = false;
    for (const auto &l : lines) {
        if (l.find("MAX_IMAGE_DIM") != std::string::npos) { dimUsed = true; break; }
    }
    if (!dimDefined)
        return fail("I2", "MAX_IMAGE_DIM constant not defined in terminalgrid.h");
    if (!dimUsed)
        return fail("I2", "MAX_IMAGE_DIM defined but never referenced in terminalgrid.cpp");

    std::printf("image_bomb_png_header_peek: %d loadFromData call%s guarded; "
                "MAX_IMAGE_DIM defined and used\n",
                loadCalls, loadCalls == 1 ? "" : "s");
    return 0;
}
