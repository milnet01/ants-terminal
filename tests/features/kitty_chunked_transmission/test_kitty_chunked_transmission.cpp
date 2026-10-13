// Feature test: Kitty chunked transmission keeps the first chunk's keys.
// See spec.md (ANTS-5676). Drives TerminalGrid through VtParser and reads
// the placed inline images.

#include "terminalgrid.h"
#include "vtparser.h"

#include <QBuffer>
#include <QByteArray>
#include <QImage>

#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

void feed(VtParser &parser, const std::string &bytes) {
    parser.feed(bytes.data(), static_cast<int>(bytes.size()));
}

std::string apc(const std::string &keys, const std::string &data) {
    return "\x1b_G" + keys + ";" + data + "\x1b\\";
}

// A small PNG, base64-encoded.
std::string pngBase64(int w, int h) {
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(qRgb(10, 200, 30));
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return bytes.toBase64().toStdString();
}

// Split `b64` into frames of `chunk` bytes: the first carries `firstKeys`,
// the rest only `m=`, and the last also `lastExtraKeys` when given.
std::string chunked(const std::string &b64, const std::string &firstKeys,
                    size_t chunk, const std::string &lastExtraKeys = "") {
    std::string out;
    for (size_t off = 0; off < b64.size(); off += chunk) {
        const bool last = off + chunk >= b64.size();
        std::string keys;
        if (off == 0) keys = firstKeys + ",";
        if (last && !lastExtraKeys.empty()) keys += lastExtraKeys + ",";
        keys += last ? "m=0" : "m=1";
        out += apc(keys, b64.substr(off, chunk));
    }
    return out;
}

struct Fixture {
    TerminalGrid grid{24, 80};
    VtParser parser{[this](const VtAction &a) { grid.processAction(a); }};
};

}  // namespace

// INV-1
TEST(KittyChunkedTransmission, ChunkedPngIsPlaced) {
    Fixture f;
    const std::string b64 = pngBase64(7, 5);
    ASSERT_GT(b64.size(), 32u);
    feed(f.parser, chunked(b64, "a=T,f=100,i=5", 16));
    ASSERT_EQ(f.grid.inlineImages().size(), 1u)
        << "a chunked PNG must be placed with the first chunk's keys";
    EXPECT_EQ(f.grid.inlineImages().back().image.width(), 7);
    EXPECT_EQ(f.grid.inlineImages().back().image.height(), 5);
}

// INV-2
TEST(KittyChunkedTransmission, ChunkedTransmitThenPlace) {
    Fixture f;
    feed(f.parser, chunked(pngBase64(6, 4), "a=t,f=100,i=9", 16));
    EXPECT_TRUE(f.grid.inlineImages().empty()) << "a=t transmits without placing";
    feed(f.parser, apc("a=p,i=9", ""));
    ASSERT_EQ(f.grid.inlineImages().size(), 1u)
        << "the chunked image must have been stored under i=9";
    EXPECT_EQ(f.grid.inlineImages().back().image.width(), 6);
}

// INV-3
TEST(KittyChunkedTransmission, KeysRepeatedOnLastChunkStillWork) {
    Fixture f;
    feed(f.parser, chunked(pngBase64(5, 3), "a=T,f=100,i=2", 16, "f=100"));
    ASSERT_EQ(f.grid.inlineImages().size(), 1u);
    EXPECT_EQ(f.grid.inlineImages().back().image.width(), 5);
}

// INV-4
TEST(KittyChunkedTransmission, FirstChunkKeysDoNotOutliveTheTransmission) {
    Fixture f;
    feed(f.parser, chunked(pngBase64(4, 4), "a=T,f=100,i=3", 16));
    ASSERT_EQ(f.grid.inlineImages().size(), 1u);
    // One 2x1 RGB pixel pair, raw (f=24), single frame. Read with the PNG's
    // f=100 it would fail to decode and place nothing.
    const std::string raw = QByteArray("\x01\x02\x03\x04\x05\x06", 6).toBase64().toStdString();
    feed(f.parser, apc("a=T,f=24,s=2,v=1", raw));
    ASSERT_EQ(f.grid.inlineImages().size(), 2u);
    EXPECT_EQ(f.grid.inlineImages().back().image.width(), 2);
}

// INV-5
TEST(KittyChunkedTransmission, QueryMidTransmissionIsAnswered) {
    Fixture f;
    std::vector<std::string> replies;
    f.grid.setResponseCallback([&replies](const std::string &r) { replies.push_back(r); });
    const std::string b64 = pngBase64(4, 4);
    feed(f.parser, apc("a=T,f=100,i=7,m=1", b64.substr(0, 16)));
    // The usual Kitty support probe, which carries no m= key.
    feed(f.parser, apc("i=31,s=1,v=1,a=q,t=d,f=24", "AAAA"));
    ASSERT_EQ(replies.size(), 1u)
        << "a query mid-transmission must be answered, not taken as a chunk";
    EXPECT_NE(replies.front().find("i=31"), std::string::npos) << replies.front();
}
