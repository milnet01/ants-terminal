// Fixture: qimage_load_without_peek — loadFromData() call with no
// QImageReader dimension peek. Image-bomb DoS vector: a malicious PNG
// can encode 65535x65535 in the IHDR while the compressed payload stays
// under 1 KB; plain QImage::loadFromData then allocates ~17 GB before
// the dimension sanity check fires.

#include <QImage>
#include <QPixmap>
#include <QByteArray>

static void decodeBadPng(const QByteArray &buf) {
    QImage img;
    img.loadFromData(buf, "PNG");             // @expect qimage_load_without_peek
    (void)img;
}

static void decodeBadPixmap(const QByteArray &buf) {
    QPixmap px;
    px.loadFromData(buf);                     // @expect qimage_load_without_peek
    (void)px;
}
