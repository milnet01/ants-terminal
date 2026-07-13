// Feature-conformance test for spec.md (ANTS-1510).
//
// Verifies the freeform-mode opt-in for cold_eyes_fold_in:
//   - engine declares + implements templateColdEyesFoldInBlockFreeform
//   - cmdColdEyesFoldIn recognises + dispatches on id_allocation
//   - descriptor surfaces the new param with the right shape
//
// Pure source-grep — no engine link needed; runs under test_claude
// (which already exposes SRC_CLAUDE_INTEGRATION_CPP_PATH +
// SRC_REMOTECONTROL_CPP_PATH).

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <string>

namespace {


bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

TEST(ColdEyesFoldInFreeform, EngineDeclaresAndImplementsFreeformTemplate) {
    const std::string hdr = ants_test::slurpFile(SRC_COLDEYESENGINE_H_PATH);
    const std::string src = ants_test::slurpFile(SRC_COLDEYESENGINE_CPP_PATH);
    ASSERT_FALSE(hdr.empty());
    ASSERT_FALSE(src.empty());

    // INV-1 — header declaration.
    EXPECT_TRUE(contains(hdr, "templateColdEyesFoldInBlockFreeform"))
        << "missing freeform-template declaration in coldeyesengine.h";

    // INV-2 — implementation present + emits the freeform shape
    // (no `[ANTS-` prefix on bullets; `- **Cold-eyes finding:**`).
    EXPECT_TRUE(contains(src, "templateColdEyesFoldInBlockFreeform"))
        << "missing freeform-template implementation in coldeyesengine.cpp";

    // Locate the freeform body and verify it doesn't emit `[ANTS-` IDs.
    const auto bodyStart = src.find("templateColdEyesFoldInBlockFreeform");
    ASSERT_NE(bodyStart, std::string::npos);
    // Body window: up to the next function definition.
    const auto bodyEnd = src.find("\n}\n", bodyStart);
    ASSERT_NE(bodyEnd, std::string::npos);
    const std::string body = src.substr(bodyStart, bodyEnd - bodyStart);
    EXPECT_FALSE(contains(body, "[ANTS-"))
        << "freeform template must not emit `[ANTS-` ID prefix";
    EXPECT_TRUE(contains(body, "- **Cold-eyes finding:**"))
        << "freeform bullet shape missing";
}

TEST(ColdEyesFoldInFreeform, HandlerRecognisesIdAllocation) {
    const std::string rc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(rc.empty());

    // Locate the cmdColdEyesFoldIn function body.
    const auto fnStart = rc.find("RemoteControl::cmdColdEyesFoldIn");
    ASSERT_NE(fnStart, std::string::npos);
    // ANTS-1644 — bound by the next handler rather than a fixed
    // 4000-char window. The narrative-mode short-circuit grew the
    // body and pushed the id_allocation parsing past the old window.
    const auto fnEnd =
        rc.find("RemoteControl::cmdColdEyesSingleDoc", fnStart);
    ASSERT_NE(fnEnd, std::string::npos);
    const std::string region = rc.substr(fnStart, fnEnd - fnStart);

    // INV-3 — id_allocation parsed and validated.
    EXPECT_TRUE(contains(region, "id_allocation"))
        << "handler must read the id_allocation parameter";
    // The handler uses string compares against "auto" / "skip".
    EXPECT_TRUE(contains(region, "\"auto\""))
        << "handler must accept \"auto\" mode";
    EXPECT_TRUE(contains(region, "\"skip\""))
        << "handler must accept \"skip\" mode";

    // INV-4 — when skip, the freeform template is used.
    EXPECT_TRUE(contains(region, "templateColdEyesFoldInBlockFreeform"))
        << "handler must dispatch to the freeform template on skip";

    // INV-5 — id_allocation echoed in the response envelope.
    EXPECT_TRUE(contains(region, "env[\"id_allocation\"]"))
        << "response envelope must echo the id_allocation mode";
}

TEST(ColdEyesFoldInFreeform, DescriptorSurfacesParam) {
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());

    // Locate the cold_eyes_fold_in descriptor block.
    const auto nameLine =
        ci.find("t[\"name\"] = \"cold_eyes_fold_in\"");
    ASSERT_NE(nameLine, std::string::npos);
    // Block window: up to the next tools.append (end of this descriptor).
    const auto blockEnd = ci.find("tools.append(t);", nameLine);
    ASSERT_NE(blockEnd, std::string::npos);
    const std::string block = ci.substr(nameLine, blockEnd - nameLine);

    // INV-6 — id_allocation property + enum + default mentioned.
    EXPECT_TRUE(contains(block, "id_allocation"))
        << "descriptor must declare id_allocation property";
    EXPECT_TRUE(contains(block, "iaEnum"))
        << "id_allocation enum array variable expected";
    // The enum values themselves.
    EXPECT_TRUE(contains(block, "iaEnum.append(\"auto\")"));
    EXPECT_TRUE(contains(block, "iaEnum.append(\"skip\")"));
    // Default + free text mentioning the .roadmap-counter requirement
    // and the freeform escape hatch.
    EXPECT_TRUE(contains(block, "iaProp[\"default\"] = \"auto\""));
    EXPECT_TRUE(contains(block, ".roadmap-counter"))
        << "descriptor should name the counter file requirement";
}
