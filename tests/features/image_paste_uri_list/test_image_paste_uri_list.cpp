// ANTS-3828 — pasting a copied image FILE must insert its bare local
// path, not the `file:///…` URI the clipboard actually carries.
//
// Spec: tests/features/image_paste_uri_list/spec.md
//
// The behavioural half calls TerminalWidget::imagePathsFromUrls(), a
// static helper, so no TerminalWidget is constructed — that class is a
// QOpenGLWidget with a live PTY and half of MainWindow's indirect
// dependencies, and keyPressEvent is protected. The wiring half scrapes
// src/terminalwidget.cpp, which is what makes the branch's presence and
// its position relative to the raster branch testable at all.
//
// Exit 0 on pass, non-zero on fail.

#include "terminalwidget.h"

#include <QFile>
#include <QImageReader>
#include <QList>
#include <QString>
#include <QTextStream>
#include <QUrl>

#include <string>

#include <gtest/gtest.h>

// Supplied by the CMake build (see the test_chrome block in
// CMakeLists.txt). The empty fallback keeps a non-CMake LSP parse quiet;
// the scrape check below fails fast and loudly when it is empty, which
// is the right outcome for a run that bypassed CMake.
#ifndef SRC_TERMINALWIDGET_PATH
#define SRC_TERMINALWIDGET_PATH ""
#endif

namespace {

#define CHECK(cond, msg) do {                                                \
    if (!(cond)) {                                                           \
        ADD_FAILURE_AT(__FILE__, __LINE__) << msg;                          \
    }                                                                        \
} while (0)

// std::string so a mismatch prints both sides; gtest has no printer for
// QString and would otherwise dump raw bytes.
std::string pasteTextFor(const QStringList &localPaths) {
    QList<QUrl> urls;
    urls.reserve(localPaths.size());
    for (const QString &p : localPaths) urls << QUrl::fromLocalFile(p);
    return TerminalWidget::imagePathsFromUrls(urls).toStdString();
}

QString readTerminalWidgetSource() {
    const QString path = QStringLiteral(SRC_TERMINALWIDGET_PATH);
    if (path.isEmpty()) {
        ADD_FAILURE() << "SRC_TERMINALWIDGET_PATH is empty — this test must "
                         "run from the CMake build";
        return {};
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        ADD_FAILURE() << "cannot open " << qUtf8Printable(path);
        return {};
    }
    QTextStream in(&f);
    return in.readAll();
}

// INV-1 — a local image URL becomes its bare filesystem path. This is
// the whole bug: before the fix this pasted "file:///home/u/…".
TEST(ImagePasteUriList, BareLocalImagePath) {
    EXPECT_EQ(pasteTextFor({QStringLiteral("/home/u/Pictures/shot.png")}),
              "/home/u/Pictures/shot.png");
}

// INV-2 (positive) — an ordinary path is NOT quoted, so the common case
// still pastes exactly what Claude Code expects to read.
TEST(ImagePasteUriList, LeavesOrdinaryPathBare) {
    EXPECT_EQ(pasteTextFor({QStringLiteral("/home/u/a-b_c.1/shot.jpeg")}),
              "/home/u/a-b_c.1/shot.jpeg");
}

// INV-2 (negative) — a filename carrying shell metacharacters is quoted.
// pasteRiskReasons() flags newlines, `sudo`, pipe-to-shell and control
// characters, but NOT `;`, so nothing downstream would warn.
TEST(ImagePasteUriList, QuotesUnsafePath) {
    EXPECT_EQ(pasteTextFor({QStringLiteral("/home/u/a;rm -rf ~.png")}),
              "'/home/u/a;rm -rf ~.png'");
}

// INV-2 — an embedded single quote is closed, escaped and reopened
// (POSIX `'\''`), not left to terminate the quoting early.
TEST(ImagePasteUriList, EscapesEmbeddedSingleQuote) {
    EXPECT_EQ(pasteTextFor({QStringLiteral("/home/u/it's a shot.png")}),
              "'/home/u/it'\\''s a shot.png'");
}

// INV-3 — a non-image local file falls through to the text paste.
TEST(ImagePasteUriList, IgnoresNonImageFile) {
    EXPECT_EQ(pasteTextFor({QStringLiteral("/home/u/notes.txt")}), "");
}

// INV-3 — a remote URL is still pasted as text, not turned into a path.
TEST(ImagePasteUriList, IgnoresRemoteUrl) {
    const QList<QUrl> urls{QUrl(QStringLiteral("https://example.com/a.png"))};
    EXPECT_EQ(TerminalWidget::imagePathsFromUrls(urls).toStdString(), "");
}

// INV-3 — an empty payload yields nothing to paste.
TEST(ImagePasteUriList, IgnoresEmptyList) {
    EXPECT_EQ(TerminalWidget::imagePathsFromUrls({}).toStdString(), "");
}

// INV-4 — several images become one space-separated line; a non-image in
// the same selection is skipped rather than poisoning the whole paste.
//
// The second image is `.bmp` and NOT `.webp`, and the reason is the whole
// point of the helper under test: it filters on
// QImageReader::supportedImageFormats() — deliberately, "rather than a
// hardcoded suffix list that would drift from the installed image plugins"
// — so which suffixes count is a property of the MACHINE, not of the code.
// WebP ships in Qt's separate imageformats plugin package, present on this
// developer's distro and absent on the GitHub runner, so a `.webp`
// expectation asserted the runner's package list and failed there while
// passing locally. png and bmp are built into QtGui itself. The guard below
// keeps that reasoning enforced rather than merely written down.
TEST(ImagePasteUriList, JoinsMultipleImages) {
    const auto supported = QImageReader::supportedImageFormats();
    ASSERT_TRUE(supported.contains("png") && supported.contains("bmp"))
        << "this Qt build decodes neither png nor bmp — the fixture below "
           "asserts joining, and cannot if its suffixes are not images here";

    EXPECT_EQ(pasteTextFor({QStringLiteral("/home/u/a.png"),
                            QStringLiteral("/home/u/notes.txt"),
                            QStringLiteral("/home/u/b.bmp")}),
              "/home/u/a.png /home/u/b.bmp");
}

// INV-5 — the branch is actually wired into Ctrl+Shift+V, and sits after
// the raster branch. Either half missing silently restores the bug: the
// helper would be correct and unreachable, or a raster paste would be
// intercepted by the URL branch.
TEST(ImagePasteUriList, HandlerWiredAfterRasterBranch) {
    const QString src = readTerminalWidgetSource();
    if (src.isEmpty()) return;

    const int rasterPos = src.indexOf(QStringLiteral("mime->hasImage()"));
    const int urlPos = src.indexOf(QStringLiteral("mime->hasUrls()"));
    const int textPos = src.indexOf(QStringLiteral("mime->hasText()"));

    CHECK(rasterPos > 0, "Ctrl+Shift+V raster branch (mime->hasImage()) not "
                         "found — handler restructured?");
    CHECK(urlPos > 0,
          "ANTS-3828: Ctrl+Shift+V must branch on mime->hasUrls() so a copied "
          "image FILE pastes its path instead of a file:// URI");
    CHECK(src.contains(QStringLiteral("imagePathsFromUrls(")),
          "ANTS-3828: the hasUrls() branch must call imagePathsFromUrls()");

    if (rasterPos > 0 && urlPos > 0 && textPos > 0) {
        CHECK(rasterPos < urlPos,
              "the hasImage() branch must come BEFORE the hasUrls() branch — "
              "a screenshot paste must not be intercepted by the URL path");
        CHECK(urlPos < textPos,
              "the hasUrls() branch must come BEFORE the plain-text fallback, "
              "which is what pasted the raw file:// URI");
    }
}

}  // namespace
