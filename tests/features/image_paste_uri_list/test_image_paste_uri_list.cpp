// ANTS-3828 / ANTS-5580 — pasting a copied image FILE must insert its bare
// local path, not the `file:///…` URI the clipboard actually carries, on
// every paste route (Ctrl+Shift+V, right-click Paste, middle-click).
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
#include <QMimeData>
#include <QRegularExpression>
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

// Body of a top-level definition: from `signature` to the next line that is
// a lone `}` in column 0. Empty (and a recorded failure) when not found, so a
// restructured or missing function fails loudly rather than matching the first
// mention elsewhere in the file.
QString bodyOf(const QString &src, const QString &signature) {
    const int start = src.indexOf(QLatin1Char('\n') + signature);
    if (start < 0) {
        ADD_FAILURE() << "definition not found in terminalwidget.cpp: "
                      << qUtf8Printable(signature);
        return {};
    }
    const int end = src.indexOf(QStringLiteral("\n}\n"), start + 1);
    if (end < 0) {
        ADD_FAILURE() << "end of definition not found: "
                      << qUtf8Printable(signature);
        return {};
    }
    return src.mid(start, end - start);
}

// What the paste of a clipboard payload carrying `urls` and `text` writes.
std::string pasteTextForPayload(const QList<QUrl> &urls, const QString &text) {
    QMimeData mime;
    if (!urls.isEmpty()) mime.setUrls(urls);
    if (!text.isEmpty()) mime.setText(text);
    return TerminalWidget::pasteTextForMime(&mime).toStdString();
}

const QString kPasteFn =
    QStringLiteral("void TerminalWidget::pasteFromClipboard(");

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

// INV-8 (ANTS-5580) — the user's case: a copied image FILE puts text/uri-list
// AND a plain-text `file://` address on the clipboard. The paste must be the
// bare local path, with no `file://` anywhere in it.
TEST(ImagePasteUriList, CopiedImageFilePastesBarePathNotFileUri) {
    const QString path = QStringLiteral(
        "/home/ants/Pictures/Screenshots/Screenshot_20260930_094154.png");
    const QUrl url = QUrl::fromLocalFile(path);
    const std::string got = pasteTextForPayload({url}, url.toString());
    EXPECT_EQ(got, path.toStdString());
    EXPECT_EQ(got.find("file://"), std::string::npos)
        << "the pasted text still carries a file:// address: " << got;
}

// INV-8 — two copied image files paste as imagePathsFromUrls joins them.
TEST(ImagePasteUriList, CopiedImageFilesPasteJoinedPaths) {
    const auto supported = QImageReader::supportedImageFormats();
    ASSERT_TRUE(supported.contains("png") && supported.contains("bmp"))
        << "this Qt build decodes neither png nor bmp (see JoinsMultipleImages)";
    const QList<QUrl> urls{QUrl::fromLocalFile(QStringLiteral("/home/u/a.png")),
                           QUrl::fromLocalFile(QStringLiteral("/home/u/b.bmp"))};
    EXPECT_EQ(pasteTextForPayload(urls, QStringLiteral("ignored")),
              "/home/u/a.png /home/u/b.bmp");
}

// INV-9 — a payload with only plain text pastes that text unchanged.
TEST(ImagePasteUriList, PlainTextPastesUnchanged) {
    EXPECT_EQ(pasteTextForPayload({}, QStringLiteral("echo hello world")),
              "echo hello world");
}

// INV-9 — a remote URL is not converted; its plain text is pasted.
TEST(ImagePasteUriList, RemoteUrlPayloadPastesItsText) {
    const QUrl url(QStringLiteral("https://example.com/a.png"));
    EXPECT_EQ(pasteTextForPayload({url}, url.toString()),
              "https://example.com/a.png");
}

// INV-9 — a local NON-image file is not converted; its plain text is pasted
// (ANTS-3828 INV-3's scope: only image files become bare paths).
TEST(ImagePasteUriList, LocalNonImageFilePayloadPastesItsText) {
    const QUrl url = QUrl::fromLocalFile(QStringLiteral("/home/u/notes.txt"));
    EXPECT_EQ(pasteTextForPayload({url}, url.toString()),
              url.toString().toStdString());
}

// INV-9 — a null payload, or one with neither urls nor text, pastes nothing.
TEST(ImagePasteUriList, NullOrEmptyPayloadPastesNothing) {
    EXPECT_EQ(TerminalWidget::pasteTextForMime(nullptr).toStdString(), "");
    EXPECT_EQ(pasteTextForPayload({}, QString()), "");
}

// INV-10 (ANTS-5580) — every paste route calls the one shared function and
// none reads the clipboard's text (or its mime data) itself, so image-file
// handling cannot differ by route. Each check is anchored inside its own
// handler's body.
TEST(ImagePasteUriList, AllThreeRoutesCallPasteFromClipboardAndNothingElse) {
    const QString src = readTerminalWidgetSource();
    if (src.isEmpty()) return;

    struct Route { const char *name; QString sig; QString call; };
    const Route routes[] = {
        {"Ctrl+Shift+V (keyPressEvent)",
         QStringLiteral("void TerminalWidget::keyPressEvent("),
         QStringLiteral("pasteFromClipboard(")},
        {"right-click Paste (contextMenuEvent)",
         QStringLiteral("void TerminalWidget::contextMenuEvent("),
         QStringLiteral("pasteFromClipboard(")},
        {"middle-click (mousePressEvent)",
         QStringLiteral("void TerminalWidget::mousePressEvent("),
         QStringLiteral("pasteFromClipboard(QClipboard::Selection)")},
    };
    const QRegularExpression direct(
        QStringLiteral("[cC]lipboard(\\(\\))?\\s*->\\s*(text|mimeData)\\("));
    for (const Route &r : routes) {
        const QString body = bodyOf(src, r.sig);
        if (body.isEmpty()) continue;
        CHECK(body.contains(r.call),
              "ANTS-5580: " << r.name << " must call " << qUtf8Printable(r.call));
        CHECK(!body.contains(direct),
              "ANTS-5580: " << r.name << " still reads the clipboard's text or "
              "mime data itself instead of going through pasteFromClipboard");
    }
}

// INV-5 — inside pasteFromClipboard, the raster (hasImage()) branch comes
// BEFORE the pasteTextForMime() step, so a screenshot paste is never taken by
// the URL/text path; and the text step hands its result to pasteToTerminal.
// (Which of URL and text wins is INV-8 / INV-9, behavioural.)
TEST(ImagePasteUriList, HandlerWiredAfterRasterBranch) {
    const QString src = readTerminalWidgetSource();
    if (src.isEmpty()) return;
    const QString body = bodyOf(src, kPasteFn);
    if (body.isEmpty()) return;

    const int rasterPos = body.indexOf(QStringLiteral("mime->hasImage()"));
    const int textPos = body.indexOf(QStringLiteral("pasteTextForMime("));
    CHECK(rasterPos > 0, "pasteFromClipboard: raster branch (mime->hasImage()) "
                         "not found — restructured?");
    CHECK(textPos > 0,
          "ANTS-5580: pasteFromClipboard must decide the non-raster paste "
          "through pasteTextForMime()");
    if (rasterPos > 0 && textPos > 0) {
        CHECK(rasterPos < textPos,
              "the hasImage() branch must come BEFORE pasteTextForMime() — a "
              "screenshot paste must not be intercepted by the URL/text step");
        CHECK(body.indexOf(QStringLiteral("pasteToTerminal("), textPos) > textPos,
              "pasteFromClipboard must hand the pasteTextForMime() result to "
              "pasteToTerminal()");
    }
}

// ANTS-3831 (INV-6) — QClipboard::mimeData() is documented nullable, and the
// branches below it dereference the result. The guard therefore sits inside
// pasteFromClipboard, before the first mime-> use.
//
// A scrape rather than a behavioural case: the null can only be produced by
// the platform plugin, and paste routes are protected members of a
// QOpenGLWidget with a live PTY.
TEST(ImagePasteUriList, NullMimeDataGuardedBeforeAnyDereference) {
    const QString src = readTerminalWidgetSource();
    if (src.isEmpty()) return;
    const QString body = bodyOf(src, kPasteFn);
    if (body.isEmpty()) return;

    const int assignPos = body.indexOf(QStringLiteral("mimeData("));
    CHECK(assignPos > 0, "the mimeData() assignment was not found in "
                         "pasteFromClipboard — restructured?");
    if (assignPos <= 0) return;

    const int guardPos = body.indexOf(QStringLiteral("if (!mime)"), assignPos);
    const int firstDeref = body.indexOf(QStringLiteral("mime->"), assignPos);

    CHECK(guardPos > 0,
          "ANTS-3831: mimeData() is documented nullable, so pasteFromClipboard "
          "must guard the pointer before using it");
    if (guardPos > 0 && firstDeref > 0) {
        CHECK(guardPos < firstDeref,
              "ANTS-3831: the null guard must come BEFORE the first mime-> "
              "dereference, or it guards nothing");
    }
}

// INV-7 (ANTS-5077) — the raster branch, now in pasteFromClipboard, saves the
// screenshot on a worker, and only the delivery that checks the worker's
// result pastes and announces it.
TEST(ImagePasteUriList, RasterPasteIsPrivateQuotedAndAnnouncedOnSave) {
    const QString src = readTerminalWidgetSource();
    if (src.isEmpty()) return;
    const QString body = bodyOf(src, kPasteFn);
    if (body.isEmpty()) return;

    const int save = body.indexOf(QStringLiteral("img.save(filename)"));
    CHECK(save > 0, "pasteFromClipboard's img.save(filename) call was not "
                    "found — raster branch moved or restructured?");
    if (save <= 0) return;
    const int worker = body.lastIndexOf(QStringLiteral("QThread::create("), save);
    CHECK(worker > 0 && save - worker < 400,
          "ANTS-5077: the screenshot must be saved inside a QThread::create "
          "worker, off the GUI thread");
    const int announce = body.indexOf(QStringLiteral("emit imagePasted(img);"), save);
    CHECK(announce > save, "ANTS-5077: imagePasted must be emitted after the save");
    if (announce <= save) return;
    const QString path = body.mid(save, announce - save);

    CHECK(path.contains(QStringLiteral("setOwnerOnlyPerms(filename)")),
          "ANTS-5077: a pasted screenshot must be narrowed to owner-only");
    CHECK(path.contains(QStringLiteral("shellQuote(filename)")),
          "ANTS-5077: the pasted screenshot path must go through shellQuote()");
    CHECK(path.contains(QStringLiteral("if (*saved)")),
          "ANTS-5077: imagePasted must be emitted only after checking the "
          "worker's result");
    CHECK(src.count(QStringLiteral("emit imagePasted(")) == 1,
          "ANTS-5077: imagePasted must be emitted only once, after a good save");
}

}  // namespace
