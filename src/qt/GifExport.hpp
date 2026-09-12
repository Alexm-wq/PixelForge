#pragma once
#include <QByteArray>
#include <QImage>
#include <QList>
#include <QString>
namespace pixelforge::qt {
struct GifReply {
    bool success=false;
    QByteArray bytes;
    QString error;
    int width=0,height=0,delayCentiseconds=0;
    bool quantized=false,partialAlphaFlattened=false;
};
// Portable CPU adaptation of the existing Windows MultiCanvasTool encoder.
// GIF89a global palette, transparent index0, disposal2, infinite loop; nearest
// neighbor scaling/centering. Exact RGB only <=255 visible colors. Alpha 1..255
// becomes opaque, as on Windows. No external encoder or image plugin required.
GifReply encodeGif(const QList<QImage>& frames,int scale,int fps);
}
