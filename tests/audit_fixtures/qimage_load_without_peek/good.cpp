// Fixture: qimage_load_without_peek — safe image decoding paths.
//
// The raw grep matches the loadFromData call shape; the OutputFilter drops
// matches that appear within +/-5 lines of QImageReader or that carry an
// `image-peek-ok` tag. The self-test runs the grep alone, so this file
// must not contain the matched token at all — safe paths use
// QImageReader::read() directly.

#include <QImage>
#include <QImageReader>
#include <QByteArray>
#include <QBuffer>

// Safe: use QImageReader::read() directly — dimension peek + decode are
// both handled by QImageReader with bounds applied via setAutoDetectImageFormat
// and the format probed from the header.
static void decodeViaReader(const QByteArray &buf) {
    QByteArray work = buf;
    QBuffer bufDev(&work);
    bufDev.open(QIODevice::ReadOnly);
    QImageReader reader(&bufDev);
    const QSize size = reader.size();
    constexpr int MAX_IMAGE_DIM = 4096;
    if (size.width() > MAX_IMAGE_DIM || size.height() > MAX_IMAGE_DIM)
        return;
    QImage img = reader.read();
    (void)img;
}
