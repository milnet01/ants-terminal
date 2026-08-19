// Feature-conformance test for
// tests/features/bump_recipe_no_generated_files/spec.md.
//
// ANTS-4529: ROADMAP.md is generated from the roadmap store, so a bump edit to
// its markdown is discarded by the next roadmap_log write. Source-scrape of
// .claude/bump.json + packaging/check-version-drift.sh + the rendered banner.
//
// Exit 0 = all invariants hold.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <regex>
#include <string>

#ifndef BUMP_JSON_PATH
#error "BUMP_JSON_PATH compile definition required"
#endif
#ifndef CHECK_VERSION_DRIFT_SH_PATH
#error "CHECK_VERSION_DRIFT_SH_PATH compile definition required"
#endif
#ifndef LIVE_ROADMAP_PATH
#error "LIVE_ROADMAP_PATH compile definition required"
#endif

namespace {

// A file the roadmap render owns: the live roadmap and every rotated archive.
// Written as a predicate rather than a list because § 3.9 rotation adds an
// archive per closed minor, and a per-name list would go stale silently.
bool isRenderGenerated(const std::string &path) {
    return path == "ROADMAP.md" || path.rfind("docs/roadmap/", 0) == 0;
}

}  // namespace

// INV-1 — the bump recipe names no generated file.
TEST(BumpRecipeNoGeneratedFiles, RecipeNamesNoGeneratedFile) {
    const std::string raw = ants_test::slurpFile(BUMP_JSON_PATH);
    ASSERT_FALSE(raw.empty()) << ".claude/bump.json not readable";

    QJsonParseError err{};
    const QJsonDocument doc =
        QJsonDocument::fromJson(QByteArray::fromStdString(raw), &err);
    ASSERT_EQ(err.error, QJsonParseError::NoError)
        << "bump.json must parse: " << err.errorString().toStdString();

    const QJsonArray files = doc.object().value(QStringLiteral("files")).toArray();
    ASSERT_FALSE(files.isEmpty()) << "bump.json carries no files[] to check";

    for (const QJsonValue &v : files) {
        const std::string path =
            v.toObject().value(QStringLiteral("path")).toString().toStdString();
        EXPECT_FALSE(isRenderGenerated(path))
            << "INV-1: bump.json lists '" << path << "', which the roadmap "
               "render regenerates — a pattern/replace edit there is discarded "
               "by the next roadmap_log write of any op (ANTS-4529). The "
               "version belongs in CMakeLists.txt, which the render never "
               "touches.";
    }
}

// INV-2 — the drift gate checks no generated file.
TEST(BumpRecipeNoGeneratedFiles, DriftGateChecksNoGeneratedFile) {
    const std::string sh = ants_test::slurpFile(CHECK_VERSION_DRIFT_SH_PATH);
    ASSERT_FALSE(sh.empty()) << "check-version-drift.sh not readable";

    // Only invocations count, not the comment explaining the removal: a live
    // call is `check` at the head of a line. Split by line rather than using
    // std::regex::multiline, whose libstdc++ support is younger than the
    // Qt 6.2-era toolchains the qt62-baseline job builds on.
    const std::regex call(R"(^check[ \t]+(\S+))");
    bool sawAnyCall = false;
    for (size_t pos = 0; pos <= sh.size();) {
        const size_t nl = sh.find('\n', pos);
        const std::string line =
            sh.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? sh.size() + 1 : nl + 1;

        std::smatch m;
        if (!std::regex_search(line, m, call))
            continue;
        sawAnyCall = true;
        const std::string target = m[1].str();
        EXPECT_FALSE(isRenderGenerated(target))
            << "INV-2: check-version-drift.sh checks '" << target << "', a "
               "render-generated file. Its version is not hand-maintained, so "
               "the check can only re-open drift after a re-render reverts a "
               "bump edit (ANTS-4529).";
    }
    EXPECT_TRUE(sawAnyCall)
        << "INV-2: no check() calls found at all — the scrape has drifted from "
           "the script's shape and is asserting nothing";
}

// INV-3 — the rendered banner states no version.
TEST(BumpRecipeNoGeneratedFiles, RenderedBannerStatesNoVersion) {
    QFile f(QStringLiteral(LIVE_ROADMAP_PATH));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text))
        << "ROADMAP.md not readable";
    // The banner is the head of the file; reading the whole 3 MB roadmap to
    // check line 4 would be the expensive way to ask a cheap question.
    const std::string head = f.read(4096).toStdString();
    ASSERT_FALSE(head.empty());

    const std::regex banner(R"(Current version:[^\n]*[0-9]+\.[0-9]+\.[0-9]+)");
    EXPECT_FALSE(std::regex_search(head, banner))
        << "INV-3: the ROADMAP.md banner states a version again. That number "
           "duplicates CMakeLists.txt's project(VERSION) in a file nobody can "
           "hand-edit durably — the store's root-section intro is what the "
           "render replays, and the next roadmap_log write reverts anything "
           "written to the markdown (ANTS-4529).";
}
