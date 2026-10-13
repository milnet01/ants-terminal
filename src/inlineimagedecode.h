#pragma once

// Inline-image decoding shared by TerminalGrid and VtStream (ANTS-1061).
//
// Decoding a large PNG or JPEG takes about 100 ms. TerminalGrid applies
// parsed actions on the GUI thread, so it used to freeze the window for that
// long per image. VtStream's parse worker now decodes iTerm2 (OSC 1337) and
// Kitty PNG (APC, f=100) images as it parses, and ships each decoded image
// beside the batch. The grid uses it when it was decoded from exactly the
// text the grid would have decoded, and otherwise decodes as before. The
// synchronous path (tests, benches, session replay) attaches nothing.
//
// Nothing here touches grid state: the image budget, placement and replies
// stay in TerminalGrid.

#include <QByteArray>
#include <QImage>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

struct VtAction;

// An image the parse worker decoded, keyed to the base64 text it came from.
struct PredecodedImage {
    QImage image;               // null when the bytes did not decode
    std::size_t sourceSize = 0;
    std::size_t sourceHash = 0;

    // True when this image was decoded from exactly `b64`.
    bool matches(std::string_view b64) const;
};

namespace inlineimage {

// The decoded-dimension cap; TerminalGrid::MAX_IMAGE_DIM asserts it equals this.
constexpr int kMaxImageDim = 4096;
// OSC 1337 base64 cap, about 9 MiB decoded (ANTS-1829).
constexpr std::size_t kMaxInlineImageB64Bytes = 12ULL * 1024ULL * 1024ULL;
// Kitty base64 cap, about 10 MiB decoded.
constexpr std::size_t kMaxKittyB64Bytes = 15ULL * 1024ULL * 1024ULL;
// Kitty chunk-staging cap; TerminalGrid::MAX_KITTY_CHUNK_BYTES asserts it.
constexpr std::size_t kMaxKittyChunkBytes = 32ULL * 1024ULL * 1024ULL;

using KittyKeys = std::unordered_map<char, std::string>;

// Strict base64 (AbortOnBase64DecodingErrors): a corrupt payload decodes to
// an empty array rather than to a garbage prefix.
QByteArray decodeBase64Strict(std::string_view b64);

// Peek the declared size with QImageReader, decode only when it is within
// `maxDim`, and re-check the result. Null on any failure. `format` may be
// null to let Qt detect it.
QImage decodeBytes(const QByteArray &bytes, const char *format, int maxDim);

// Split an OSC 1337 `File=` payload into its key block and base64 text.
// False when it is not an inline (`inline=1`) image.
bool splitITerm2(const std::string &payload, std::string *keys, std::string *b64);

// Parse a Kitty APC key block (`a=T,f=100,m=1`).
KittyKeys parseKittyKeys(std::string_view keys);

// ANTS-5676 — a continuation chunk (keys only m and q) takes the keys its
// transmission's first chunk carried; a key it repeats wins. Any other frame
// is a command of its own and is left alone.
void mergeContinuationKeys(KittyKeys &keys, const KittyKeys &firstChunk);

// The parse worker's half. Feed it every action in parse order; it returns
// the image an action carries, decoded, or null. It follows Kitty chunking
// the way TerminalGrid does. Where the two disagree (the grid drops staged
// chunks on a resize or a screen switch this cannot see), the image does not
// match and the grid decodes for itself.
class Predecoder {
public:
    std::shared_ptr<const PredecodedImage> onAction(const VtAction &action);

private:
    std::shared_ptr<const PredecodedImage> onApc(const std::string &payload);
    void resetKitty();

    std::string m_kittyBuffer;
    KittyKeys m_kittyFirstKeys;
};

}  // namespace inlineimage
