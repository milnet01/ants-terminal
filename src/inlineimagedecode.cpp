#include "inlineimagedecode.h"

#include "vtparser.h"

#include <QBuffer>
#include <QImageReader>

#include <algorithm>
#include <functional>

bool PredecodedImage::matches(std::string_view b64) const {
    return b64.size() == sourceSize &&
           std::hash<std::string_view>{}(b64) == sourceHash;
}

namespace inlineimage {

namespace {

std::shared_ptr<const PredecodedImage> predecoded(std::string_view b64, QImage image) {
    auto out = std::make_shared<PredecodedImage>();
    out->image      = std::move(image);
    out->sourceSize = b64.size();
    out->sourceHash = std::hash<std::string_view>{}(b64);
    return out;
}

}  // namespace

QByteArray decodeBase64Strict(std::string_view b64) {
    auto res = QByteArray::fromBase64Encoding(
        QByteArray::fromRawData(b64.data(), static_cast<qsizetype>(b64.size())),
        QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
    if (res.decodingStatus != QByteArray::Base64DecodingStatus::Ok) return QByteArray();
    return res.decoded;
}

QImage decodeBytes(const QByteArray &bytes, const char *format, int maxDim) {
    if (bytes.isEmpty()) return QImage();
    // A "compression bomb" can declare 100k x 100k in a header under 1 KB;
    // QImage::loadFromData would allocate for that before any check ran.
    // QImageReader::size() reads the header only.
    QByteArray copy = bytes;  // QBuffer needs a non-const array; shares data
    QBuffer buf(&copy);
    buf.open(QIODevice::ReadOnly);
    QImageReader reader(&buf, format);
    const QSize declared = reader.size();
    if (declared.width() > maxDim || declared.height() > maxDim) return QImage();
    QImage image;
    if (!image.loadFromData(bytes, format)) return QImage();  // image-peek-ok: size peeked above
    if (image.width() > maxDim || image.height() > maxDim) return QImage();
    return image;
}

bool splitITerm2(const std::string &payload, std::string *keys, std::string *b64) {
    // Format: 1337;File=[keys]:BASE64
    if (payload.compare(0, 5, "1337;") != 0) return false;
    const auto colon = payload.find(':', 5);
    if (colon == std::string::npos) return false;
    *keys = payload.substr(5, colon - 5);
    if (keys->find("inline=1") == std::string::npos) return false;
    *b64 = payload.substr(colon + 1);
    return true;
}

KittyKeys parseKittyKeys(std::string_view keys) {
    KittyKeys out;
    size_t pos = 0;
    while (pos < keys.size()) {
        const char key = keys[pos];
        if (pos + 1 >= keys.size() || keys[pos + 1] != '=') break;
        pos += 2;
        std::string value;
        while (pos < keys.size() && keys[pos] != ',') {
            value += keys[pos];
            ++pos;
        }
        out[key] = value;
        if (pos < keys.size() && keys[pos] == ',') ++pos;
    }
    return out;
}

void mergeContinuationKeys(KittyKeys &keys, const KittyKeys &firstChunk) {
    const bool continuation = std::all_of(keys.begin(), keys.end(),
        [](const auto &kv) { return kv.first == 'm' || kv.first == 'q'; });
    if (!continuation) return;
    for (const auto &[key, value] : firstChunk) keys.emplace(key, value);
}

std::shared_ptr<const PredecodedImage> Predecoder::onAction(const VtAction &action) {
    if (action.type == VtAction::OscEnd) {
        if (action.truncated) return nullptr;
        std::string keys, b64;
        if (!splitITerm2(action.oscString, &keys, &b64)) return nullptr;
        if (b64.size() > kMaxInlineImageB64Bytes) return nullptr;
        return predecoded(b64, decodeBytes(decodeBase64Strict(b64), nullptr, kMaxImageDim));
    }
    if (action.type == VtAction::ApcEnd) {
        if (action.truncated) return nullptr;  // the grid refuses it outright
        return onApc(action.oscString);
    }
    return nullptr;
}

std::shared_ptr<const PredecodedImage> Predecoder::onApc(const std::string &payload) {
    if (payload.empty() || payload[0] != 'G') return nullptr;
    const auto semicolon = payload.find(';');
    const std::string_view all(payload);
    KittyKeys keys = parseKittyKeys(semicolon != std::string::npos
                                        ? all.substr(1, semicolon - 1) : all.substr(1));
    const std::string_view data =
        semicolon != std::string::npos ? all.substr(semicolon + 1) : std::string_view();
    mergeContinuationKeys(keys, m_kittyFirstKeys);

    const auto value = [&keys](char k) {
        const auto it = keys.find(k);
        return it == keys.end() ? std::string() : it->second;
    };
    if (value('m') == "1") {
        if (m_kittyFirstKeys.empty()) {
            m_kittyFirstKeys = keys;
            m_kittyFirstKeys.erase('m');
        }
        if (m_kittyBuffer.size() + data.size() > kMaxKittyChunkBytes) {
            resetKitty();
            return nullptr;
        }
        m_kittyBuffer.append(data);
        return nullptr;
    }

    std::string full = std::move(m_kittyBuffer);
    full.append(data);
    resetKitty();
    // Only a transmission of a PNG is worth decoding here; every other
    // action and format is cheap, or carries no image.
    const std::string action = value('a');
    if (value('f') != "100") return nullptr;
    if (!action.empty() && action != "T" && action != "t") return nullptr;
    if (full.empty() || full.size() > kMaxKittyB64Bytes) return nullptr;
    return predecoded(full, decodeBytes(decodeBase64Strict(full), "PNG", kMaxImageDim));
}

void Predecoder::resetKitty() {
    m_kittyBuffer.clear();
    m_kittyBuffer.shrink_to_fit();
    m_kittyFirstKeys.clear();
}

}  // namespace inlineimage
