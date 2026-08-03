// Feature-conformance test for ANTS-3800 — the file_outline GLSL lane. See
// spec.md.
//
// The defect: file_outline's mode enum was auto|cpp|py|md|json, so a shader
// file returned {language:"unknown"} with NO symbols array — while
// find_definition and find_caller had both advertised `glsl` since ANTS-3558.
// Three verbs, two answers about whether GLSL is a supported language.
//
// read_region's symbol mode resolves through this outline, so the same defect
// made it refuse symbol_not_found for a shader function that plainly exists.
// That is the half worth locking: the outline is a means, and slicing a
// 1600-line shader by function name is the end.

#include "../../_support/expect.h"
#include "fileoutline.h"

#include <gtest/gtest.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

ANTS_TEST_SCOPE();

namespace {

// A shader in the shape the reporting project actually writes: file-scope
// consts, a struct, layout() blocks, and functions whose declarations are C in
// everything but the type names.
const char *kShader = R"(// Path tracer compute stage.
#version 460
layout(local_size_x = 8, local_size_y = 8) in;

const float kEpsilon = 1e-4;

struct Ray {
    vec3 origin;
    vec3 dir;
};

layout(std430, binding = 0) buffer Scene {
    vec4 spheres[];
} scene;

float intersectSphere(Ray r, vec4 s) {
    vec3 oc = r.origin - s.xyz;
    return dot(oc, oc) - s.w * s.w;
}

vec3 traceRadiance(Ray r, int bounces) {
    vec3 acc = vec3(0.0);
    for (int i = 0; i < bounces; ++i) {
        acc += vec3(0.1);
    }
    return acc;
}

void main() {
    Ray r;
    traceRadiance(r, 4);
}
)";

QString writeShader(const QTemporaryDir &dir, const QString &name) {
    const QString path = QFileInfo(dir.path()).canonicalFilePath() + QLatin1Char('/') + name;
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(kShader);
    f.close();
    return path;
}

QStringList symbolNames(const QJsonObject &out) {
    QStringList names;
    for (const QJsonValue &v : out.value(QStringLiteral("symbols")).toArray())
        names << v.toObject().value(QStringLiteral("name")).toString();
    return names;
}

} // namespace

// INV-1 — a shader file is detected as GLSL by extension and yields symbols.
// Breaks when: pickModeByExt() has no shader branch, so the file falls through
// to Mode::Auto and compute() reports language "unknown" with no symbols.
TEST(FileOutlineGlsl, DetectsShaderExtensionsAndExtractsFunctions) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    // Every stage extension, not just .comp: the whole defect was two verbs
    // disagreeing about the SET, so a test that checks one extension would
    // pass against a fix that admitted only that one.
    const QStringList names = {QStringLiteral("a.comp"), QStringLiteral("b.frag"),
                               QStringLiteral("c.vert"), QStringLiteral("d.glsl"),
                               QStringLiteral("e.rgen"), QStringLiteral("f.mesh")};
    for (const QString &name : names) {
        const QString path = writeShader(dir, name);
        const QJsonObject out = FileOutline::compute(path, FileOutline::Mode::Auto, true, 1000);
        EXPECT_EQ(out.value(QStringLiteral("language")).toString(), QStringLiteral("glsl"))
            << "not detected as GLSL: " << name.toStdString();
        const QStringList syms = symbolNames(out);
        EXPECT_TRUE(syms.contains(QStringLiteral("traceRadiance")))
            << name.toStdString() << " symbols: " << syms.join(QStringLiteral(",")).toStdString();
    }
}

// INV-2 — the extension set MATCHES symbolquery.cpp's ANTS-3558 lane, which is
// the point of the item: three verbs agreeing about which files are GLSL.
// Breaks when: a second, shorter list is written here — the original defect
// with an extra step.
//
// `.fs` is deliberately NOT GLSL (it is also F# source), and asserting that is
// what stops a future widening from quietly claiming it.
TEST(FileOutlineGlsl, ExtensionSetMatchesTheSymbolQueryLane) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString shader = writeShader(dir, QStringLiteral("x.tese"));
    EXPECT_EQ(FileOutline::compute(shader, FileOutline::Mode::Auto, true, 1000)
                  .value(QStringLiteral("language")).toString(),
              QStringLiteral("glsl"));

    const QString fsharp = writeShader(dir, QStringLiteral("x.fs"));
    EXPECT_NE(FileOutline::compute(fsharp, FileOutline::Mode::Auto, true, 1000)
                  .value(QStringLiteral("language")).toString(),
              QStringLiteral("glsl"))
        << ".fs is F# source too — claiming it as GLSL is the collision "
           "symbolquery.cpp's lane deliberately avoids";
}

// INV-3 — an explicit mode:"glsl" is honoured, so a caller can force the lane
// for a shader with a project-specific extension.
// Breaks when: parseMode() has no "glsl" branch and silently returns Auto,
// which reads as "unknown language" rather than as a rejected argument.
TEST(FileOutlineGlsl, ExplicitModeIsHonoured) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    // A .txt file would normally be Md; forcing glsl must override that.
    const QString path = writeShader(dir, QStringLiteral("shader.txt"));

    EXPECT_EQ(FileOutline::parseMode(QStringLiteral("glsl")), FileOutline::Mode::Glsl);
    const QJsonObject out = FileOutline::compute(path, FileOutline::Mode::Glsl, true, 1000);
    EXPECT_EQ(out.value(QStringLiteral("language")).toString(), QStringLiteral("glsl"));
    EXPECT_TRUE(symbolNames(out).contains(QStringLiteral("intersectSphere")));
}

// INV-4 — the struct and the file-scope const are carried, not only functions.
// This project's shader tuning constants all live at file scope, and a lane
// that found only functions would miss what a renderer task most often edits.
// Breaks when: GLSL is routed somewhere that extracts functions alone.
TEST(FileOutlineGlsl, CarriesStructsAndFileScopeConstants) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeShader(dir, QStringLiteral("t.comp"));
    const QStringList syms =
        symbolNames(FileOutline::compute(path, FileOutline::Mode::Auto, true, 1000));

    EXPECT_TRUE(syms.contains(QStringLiteral("Ray")))
        << "struct missing — symbols: " << syms.join(QStringLiteral(",")).toStdString();
}
