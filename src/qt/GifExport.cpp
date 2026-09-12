#include "GifExport.hpp"
#include <QHash>
#include <algorithm>
#include <array>
#include <cstdint>

namespace pixelforge::qt {
namespace {
constexpr qint64 MaxBytes=32ll*1024*1024;
void u16(QByteArray& bytes,int n){bytes.append(char(n&255));bytes.append(char((n>>8)&255));}
void blocks(QByteArray& out,const QByteArray& payload){for(qsizetype i=0;i<payload.size();){const auto n=std::min(qsizetype(255),payload.size()-i);out.append(char(n));out.append(payload.constData()+i,n);i+=n;}out.append(char(0));}
}
GifReply encodeGif(const QList<QImage>& frames,int scale,int fps){
    GifReply result;
    auto fail=[&](const QString& reason){result.bytes.clear();result.error=reason;return result;};
    if(frames.isEmpty()||frames.size()>512||scale<1||scale>16||fps<1||fps>60)return fail("Invalid bounded GIF frame count, scale or fps.");
    int w=0,h=0;for(const auto& image:frames){if(image.isNull()||qint64(image.width())*scale>65535||qint64(image.height())*scale>65535)return fail("Invalid GIF dimensions.");w=std::max(w,image.width()*scale);h=std::max(h,image.height()*scale);}
    const qint64 pixels=qint64(w)*h;
    // Literal LZW is deliberately identical to the original codec. Bound its
    // exact worst-case byte count before allocating or scanning any frame.
    const qint64 payload=(pixels*18+9+7)/8;
    const qint64 predicted=801+(payload+(payload+254)/255+30)*frames.size();
    if(pixels>16ll*1024*1024||predicted>MaxBytes)return fail("GIF exceeds the32MiB encoded budget; select fewer/smaller frames or reduce scale.");
    QList<quint32> unique;QHash<quint32,quint8> exact;
    bool paletteExact=true;
    for(const auto& image:frames){
        for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x){
            const auto color=image.pixel(x,y);const auto alpha=qAlpha(color);if(alpha>0&&alpha<255)result.partialAlphaFlattened=true;
            if(alpha==0||!paletteExact)continue;const auto rgb=color&0x00ffffffu;
            if(exact.contains(rgb))continue;
            if(unique.size()==255){paletteExact=false;continue;}
            unique.append(rgb);exact.insert(rgb,quint8(unique.size()));
        }
    }
    QByteArray bytes;bytes.reserve(int(predicted));bytes.append("GIF89a",6);u16(bytes,w);u16(bytes,h);bytes.append(char(0xf7));bytes.append(char(0));bytes.append(char(0));
    std::array<std::array<quint8,3>,256> table{};
    if(paletteExact){for(int i=0;i<unique.size();++i)table[i+1]={quint8((unique[i]>>16)&255),quint8((unique[i]>>8)&255),quint8(unique[i]&255)};}
    else for(int i=1;i<256;++i)table[i]={quint8((((i>>5)&7)*255)/7),quint8((((i>>2)&7)*255)/7),quint8(((i&3)*255)/3)};
    for(const auto& rgb:table)for(auto c:rgb)bytes.append(char(c));
    const unsigned char loop[]={0x21,0xff,0x0b,'N','E','T','S','C','A','P','E','2','.','0',0x03,0x01,0,0,0};bytes.append(reinterpret_cast<const char*>(loop),sizeof(loop));
    const int delay=std::max(1,int(100.0/fps+0.5));
    auto paletteIndex=[&](QRgb color)->int{if(qAlpha(color)==0)return 0;const auto rgb=color&0x00ffffffu;if(paletteExact)return exact.value(rgb);const int r=qRed(color)*7/255,g=qGreen(color)*7/255,b=qBlue(color)*3/255;return std::max(1,(r<<5)|(g<<2)|b);};
    for(const auto& image:frames){
        bytes.append(char(0x21));bytes.append(char(0xf9));bytes.append(char(4));bytes.append(char(9));u16(bytes,delay);bytes.append(char(0));bytes.append(char(0));bytes.append(char(0x2c));u16(bytes,0);u16(bytes,0);u16(bytes,w);u16(bytes,h);bytes.append(char(0));bytes.append(char(8));
        QByteArray data;data.reserve(int(payload));quint32 accumulator=0;int bits=0;
        auto code=[&](int n){accumulator|=quint32(n)<<bits;bits+=9;while(bits>=8){data.append(char(accumulator&255));accumulator>>=8;bits-=8;}};
        const int fw=image.width()*scale,fh=image.height()*scale,ox=(w-fw)/2,oy=(h-fh)/2;
        for(int y=0;y<h;++y)for(int x=0;x<w;++x){int index=0;if(x>=ox&&y>=oy&&x<ox+fw&&y<oy+fh)index=paletteIndex(image.pixel((x-ox)/scale,(y-oy)/scale));code(256);code(index);}
        code(257);if(bits)data.append(char(accumulator&255));blocks(bytes,data);
    }
    bytes.append(char(0x3b));if(bytes.size()>MaxBytes)return fail("GIF encoding exceeded byte limit.");
    result.success=true;result.bytes=std::move(bytes);result.width=w;result.height=h;result.delayCentiseconds=delay;result.quantized=!paletteExact;return result;
}
}
