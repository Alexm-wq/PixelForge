#pragma once
#include "AgentCommands.hpp"
#include <QList>
#include <QString>
#include <cmath>

namespace pixelforge::qt {
// Shared wire grammar; parse completely before any document mutation.
inline bool parsePixelPatch(const QString& patch,const QList<quint32>& palette,
                            std::vector<AgentPixelOp>& output,QString* error=nullptr) {
    auto fail=[&](QString reason){if(error)*error=reason;return false;};
    if(patch.isEmpty()||patch.size()>16*1024*1024)return fail("Empty or oversized pixel patch.");
    std::vector<AgentPixelOp> ops;quint64 touchedPixels=0;
    for(const auto& line:patch.split(';',Qt::SkipEmptyParts)) {
        const auto p=line.trimmed().split(',');if(p.size()<4)return fail("Malformed pixel patch.");
        AgentPixelOp op;const auto kind=p[0].trimmed();int count=0;
        if(kind=="P"){op.kind=AgentPixelOpKind::SetPixel;count=4;}
        else if(kind=="H"){op.kind=AgentPixelOpKind::HorizontalRun;count=5;}
        else if(kind=="V"){op.kind=AgentPixelOpKind::VerticalRun;count=5;}
        else if(kind=="R"){op.kind=AgentPixelOpKind::FillRect;count=6;}
        else if(kind=="L"){op.kind=AgentPixelOpKind::Line;count=6;}
        else return fail("Unknown pixel patch operation.");
        if(p.size()!=count)return fail("Wrong pixel operation field count.");
        QList<int> nums;for(int n=1;n<count-1;++n){bool ok=false;const int x=p[n].trimmed().toInt(&ok);if(!ok||x<0||x>65536)return fail("Invalid pixel geometry.");nums<<x;}
        op.x=nums[0];op.y=nums[1];
        if(kind=="H")op.width=nums[2];if(kind=="V")op.height=nums[2];
        if(kind=="R"){op.width=nums[2];op.height=nums[3];}
        if(kind=="L"){op.x2=nums[2];op.y2=nums[3];}
        auto color=p.last().trimmed();bool ok=false;
        if(color.startsWith('#')){color.remove(0,1);op.argb=color.toUInt(&ok,16);ok=ok&&color.size()==8;}
        else {const int i=color.toInt(&ok);ok=ok&&i>=0&&i<palette.size();if(ok)op.argb=palette[i];}
        if(!ok)return fail("Invalid colour: use #AARRGGBB or a palette index.");
        const quint64 work=kind=="P"?1:kind=="L"?quint64(std::max(std::abs(op.x2-op.x),std::abs(op.y2-op.y)))+1:quint64(op.width)*op.height;
        if(work>64ull*1024*1024-touchedPixels)return fail("Pixel transaction exceeds the 64 Mi touched-pixel work budget; nothing applied.");
        touchedPixels+=work;ops.push_back(op);if(ops.size()>262144)return fail("Pixel transaction exceeds the 262,144-operation host budget; nothing applied.");
    }
    if(ops.empty())return fail("Empty pixel patch.");
    output=std::move(ops);return true;
}
inline bool applyPixelPatch(PixelDocument& document,const QString& patch,
                            const QList<quint32>& palette,QString* error=nullptr) {
    std::vector<AgentPixelOp> ops;if(!parsePixelPatch(patch,palette,ops,error))return false;
    auto inside=[&](int x,int y){return x>=0&&y>=0&&x<document.width()&&y<document.height();};
    for(const auto& op:ops) {
        bool valid=inside(op.x,op.y);
        if(op.kind==AgentPixelOpKind::Line)valid=valid&&inside(op.x2,op.y2);
        else if(op.kind!=AgentPixelOpKind::SetPixel)valid=valid&&op.width>0&&op.height>0&&qint64(op.x)+op.width<=document.width()&&qint64(op.y)+op.height<=document.height();
        if(!valid){if(error)*error="Patch geometry lies outside the canvas; nothing applied.";return false;}
    }
    auto tx=document.begin_transaction();
    for(const auto& op:ops) {
        if(op.kind==AgentPixelOpKind::Line) {
            int x=op.x,y=op.y,dx=std::abs(op.x2-x),sx=x<op.x2?1:-1,dy=-std::abs(op.y2-y),sy=y<op.y2?1:-1,e=dx+dy;
            for(;;){tx.set_pixel(x,y,op.argb);if(x==op.x2&&y==op.y2)break;const int twice=2*e;if(twice>=dy){e+=dy;x+=sx;}if(twice<=dx){e+=dx;y+=sy;}}
        } else if(op.kind==AgentPixelOpKind::SetPixel)tx.set_pixel(op.x,op.y,op.argb);
        else tx.fill_rect(op.x,op.y,op.width,op.height,op.argb);
    }
    if(tx.pending_changes()==0){tx.cancel();return true;}
    return tx.commit();
}
}
