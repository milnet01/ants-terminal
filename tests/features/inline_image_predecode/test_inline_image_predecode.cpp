// Feature test: inline images decoded off the GUI thread (ANTS-1061).
// See spec.md. Drives inlineimage::Predecoder and TerminalGrid through
// VtParser.

#include "inlineimagedecode.h"
#include "terminalgrid.h"
#include "vtparser.h"

#include <QBuffer>
#include <QByteArray>
#include <QImage>

#include <gtest/gtest.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {

std::string pngBase64(int w, int h) {
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(qRgb(200, 40, 90));
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return bytes.toBase64().toStdString();
}

// Valid base64 whose bytes are not an image.
std::string notAnImageBase64() {
    return QByteArray("these bytes are not an image").toBase64().toStdString();
}

std::string iterm2(const std::string &b64) {
    return "\x1b]1337;File=inline=1:" + b64 + "\x07";
}

std::string apc(const std::string &keys, const std::string &data) {
    return "\x1b_G" + keys + ";" + data + "\x1b\\";
}

// Every action the bytes parse to, in order.
std::vector<VtAction> parse(const std::string &bytes) {
    std::vector<VtAction> out;
    VtParser parser([&out](const VtAction &a) {
        VtAction copy = a;
        copy.printRun = nullptr;  // not needed here; never dereferenced
        out.push_back(copy);
    });
    parser.feed(bytes.data(), static_cast<int>(bytes.size()));
    return out;
}

// The predecoder's result for each action, in order.
std::vector<std::shared_ptr<const PredecodedImage>> predecode(const std::string &bytes) {
    inlineimage::Predecoder pre;
    std::vector<std::shared_ptr<const PredecodedImage>> out;
    for (const VtAction &a : parse(bytes)) out.push_back(pre.onAction(a));
    return out;
}

// A pre-decoded image keyed to `b64`, holding `image`.
PredecodedImage keyedTo(const std::string &b64, QImage image) {
    PredecodedImage p;
    p.image      = std::move(image);
    p.sourceSize = b64.size();
    p.sourceHash = std::hash<std::string_view>{}(b64);
    return p;
}

// Apply `bytes` to a fresh grid, handing `pre` with the last action.
std::unique_ptr<TerminalGrid> applyWithLast(const std::string &bytes,
                                            const PredecodedImage *pre) {
    auto grid = std::make_unique<TerminalGrid>(24, 80);
    const std::vector<VtAction> actions = parse(bytes);
    for (size_t i = 0; i < actions.size(); ++i)
        grid->processAction(actions[i], i + 1 == actions.size() ? pre : nullptr);
    return grid;
}

}  // namespace

// INV-1
TEST(InlineImagePredecode, DecodesAnITerm2Image) {
    const std::string b64 = pngBase64(6, 4);
    const auto results = predecode(iterm2(b64));
    ASSERT_EQ(results.size(), 1u);
    ASSERT_TRUE(results[0]);
    EXPECT_EQ(results[0]->image.size(), QSize(6, 4));
    EXPECT_TRUE(results[0]->matches(b64));
}

// INV-2
TEST(InlineImagePredecode, FollowsKittyChunking) {
    const std::string b64 = pngBase64(7, 3);
    std::string bytes;
    for (size_t off = 0; off < b64.size(); off += 20) {
        const bool last = off + 20 >= b64.size();
        bytes += apc(std::string(off == 0 ? "a=T,f=100,i=4," : "") + (last ? "m=0" : "m=1"),
                     b64.substr(off, 20));
    }
    const auto results = predecode(bytes);
    ASSERT_GE(results.size(), 2u);
    for (size_t i = 0; i + 1 < results.size(); ++i)
        EXPECT_FALSE(results[i]) << "an m=1 frame must not decode (frame " << i << ")";
    ASSERT_TRUE(results.back());
    EXPECT_EQ(results.back()->image.size(), QSize(7, 3));
    EXPECT_TRUE(results.back()->matches(b64));
}

// INV-3
TEST(InlineImagePredecode, SkipsFramesWithNoPngToTransmit) {
    const std::string raw = QByteArray("\x01\x02\x03", 3).toBase64().toStdString();
    const auto results = predecode(apc("a=p,i=4", "") +
                                   apc("a=T,f=24,s=1,v=1", raw) +
                                   apc("a=q,i=31,f=100", pngBase64(2, 2)));
    ASSERT_EQ(results.size(), 3u);
    for (size_t i = 0; i < results.size(); ++i)
        EXPECT_FALSE(results[i]) << "frame " << i;
}

// INV-4 (iTerm2)
TEST(InlineImagePredecode, GridPlacesAMatchingITerm2Image) {
    const std::string b64 = notAnImageBase64();
    ASSERT_TRUE(applyWithLast(iterm2(b64), nullptr)->inlineImages().empty())
        << "precondition: the grid alone cannot decode this payload";
    const PredecodedImage pre = keyedTo(b64, QImage(3, 2, QImage::Format_ARGB32));
    const auto grid = applyWithLast(iterm2(b64), &pre);
    ASSERT_EQ(grid->inlineImages().size(), 1u)
        << "the grid must use the matching pre-decoded image";
    EXPECT_EQ(grid->inlineImages().back().image.size(), QSize(3, 2));
}

// INV-4 (Kitty)
TEST(InlineImagePredecode, GridPlacesAMatchingKittyImage) {
    const std::string b64 = notAnImageBase64();
    const std::string bytes = apc("a=T,f=100,i=6", b64);
    ASSERT_TRUE(applyWithLast(bytes, nullptr)->inlineImages().empty())
        << "precondition: the grid alone cannot decode this payload";
    const PredecodedImage pre = keyedTo(b64, QImage(4, 2, QImage::Format_ARGB32));
    const auto grid = applyWithLast(bytes, &pre);
    ASSERT_EQ(grid->inlineImages().size(), 1u)
        << "the grid must use the matching pre-decoded image";
    EXPECT_EQ(grid->inlineImages().back().image.size(), QSize(4, 2));
}

// INV-5
TEST(InlineImagePredecode, GridIgnoresAMismatchedImage) {
    const std::string b64 = pngBase64(5, 5);
    const PredecodedImage pre =
        keyedTo(notAnImageBase64(), QImage(3, 2, QImage::Format_ARGB32));
    const auto grid = applyWithLast(iterm2(b64), &pre);
    ASSERT_EQ(grid->inlineImages().size(), 1u);
    EXPECT_EQ(grid->inlineImages().back().image.size(), QSize(5, 5))
        << "a pre-decoded image keyed to other text must be ignored";
}
