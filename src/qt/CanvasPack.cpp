#include "CanvasPack.hpp"
#include "PixelPatch.hpp"
#include "GifExport.hpp"
#include "PixelDocument.hpp"
#include "../win32/PixelProgram.hpp"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <utility>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif

namespace pixelforge::qt {
namespace {
constexpr quint64 HistoryBudget = 128ull * 1024 * 1024;
constexpr quint64 RenderPixels = 16ull * 1024 * 1024;
constexpr qint64 ReplyPngBytes = 32ll * 1024 * 1024;

bool nameOk(const QString& s) {
    static const QRegularExpression expression("^[A-Za-z0-9_.-]{1,240}$");
    static const QRegularExpression device("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\.|$)", QRegularExpression::CaseInsensitiveOption);
    return expression.match(s).hasMatch() && s != "." && s != ".." && !s.endsWith('.') && !device.match(s).hasMatch();
}
int indexOf(const ProjectBundle& b, const QString& name) {
    for (int i = 0; i < b.canvases.size(); ++i)
        if (b.canvases[i].name.compare(name, Qt::CaseInsensitive) == 0) return i;
    return -1;
}
bool number(const QJsonObject& a, const QString& key, qint64 fallback, qint64 low, qint64 high, qint64& result) {
    if (!a.contains(key)) { result = fallback; return result >= low && result <= high; }
    const auto v = a.value(key);
    if (!v.isDouble() || !std::isfinite(v.toDouble()) || std::floor(v.toDouble()) != v.toDouble()) return false;
    result = v.toInteger(std::numeric_limits<qint64>::min());
    return result >= low && result <= high;
}
bool valid(const ProjectBundle& b, QString& error) {
    if (!b.taskId || b.taskId > quint64(std::numeric_limits<qint64>::max()) || b.packRevision > quint64(std::numeric_limits<qint64>::max()) || b.canvases.isEmpty() || b.canvases.size() > ProjectStore::MaxCanvases || b.projectName.size() > 1024 || b.projectName.contains(QChar(0)) || b.projectBrief.size() > 8192 || b.projectBrief.contains(QChar(0)) || b.changeTrackingFloor > b.packRevision) {
        error = "Invalid task identity, project name or canvas count."; return false;
    }
    QSet<QString> names;
    quint64 total = 0;
    for (const auto& c : b.canvases) {
        const quint64 pixels = quint64(c.image.width()) * quint64(c.image.height());
        if (!nameOk(c.name) || (!c.group.isEmpty() && !nameOk(c.group)) || c.frame < -1 || names.contains(c.name.toUpper()) || c.image.isNull() || c.image.width() > ProjectStore::MaxDimension || c.image.height() > ProjectStore::MaxDimension || pixels > ProjectStore::MaxCanvasPixels || total > ProjectStore::MaxTotalPixels - pixels) {
            error = "Invalid/duplicate name, frame, image or resource budget."; return false;
        }
        total += pixels; names.insert(c.name.toUpper());
    }
    return true;
}
bool specs(const QString& text, ProjectBundle& target, QString& error) {
    if (text.isEmpty() || text.size() > 256 * 1024) { error = "Empty or oversized canvas specification."; return false; }
    const auto rows = text.split('|');
    if (rows.size() + target.canvases.size() > ProjectStore::MaxCanvases) { error = "Canvas count limit exceeded."; return false; }
    quint64 total = 0;
    for (const auto& c : target.canvases) total += quint64(c.image.width()) * c.image.height();
    struct Spec { QString name, group; int w, h, frame; };
    QList<Spec> parsed;
    QSet<QString> names;
    for (const auto& c : target.canvases) names.insert(c.name.toUpper());
    for (const auto& row : rows) {
        const auto f = row.split(',');
        if (f.size() != 4 && f.size() != 5) { error = "Each specification needs name,group,width,height[,frame]."; return false; }
        bool ow, oh, of = true;
        const int w = f[2].trimmed().toInt(&ow), h = f[3].trimmed().toInt(&oh), frame = f.size() == 5 && !f[4].trimmed().isEmpty() ? f[4].trimmed().toInt(&of) : -1;
        const QString name = f[0].trimmed(), group = f[1].trimmed();
        const quint64 pixels = quint64(std::max(0,w)) * std::max(0,h);
        if (!nameOk(name) || (!group.isEmpty() && !nameOk(group)) || names.contains(name.toUpper()) || !ow || !oh || !of || w < 1 || h < 1 || w > ProjectStore::MaxDimension || h > ProjectStore::MaxDimension || frame < -1 || pixels > ProjectStore::MaxCanvasPixels || total > ProjectStore::MaxTotalPixels - pixels) {
            error = "Invalid/duplicate canvas specification or dimensions."; return false;
        }
        names.insert(name.toUpper()); total += pixels; parsed.append({name,group,w,h,frame});
    }
    for (const auto& s : parsed) {
        QImage image(s.w,s.h,QImage::Format_ARGB32);
        if (image.isNull()) { error = "Canvas allocation failed."; return false; }
        image.fill(Qt::transparent); target.canvases.append({s.name,s.group,s.frame,image});
    }
    return true;
}
bool document(const QImage& source, PixelDocument& doc, QString& error) {
    const auto image = source.convertToFormat(QImage::Format_ARGB32);
    if (image.isNull()) { error = "Cannot convert canvas pixels."; return false; }
    std::vector<std::uint32_t> pixels(size_t(image.width()) * image.height());
    for (int y=0;y<image.height();++y) std::copy_n(reinterpret_cast<const QRgb*>(image.constScanLine(y)),image.width(),pixels.begin()+size_t(y)*image.width());
    std::string reason;
    if (!doc.replace_pixels(image.width(),image.height(),std::move(pixels),&reason)) { error=QString::fromStdString(reason); return false; }
    return true;
}
QImage imageOf(const PixelDocument& doc) {
    QImage image(doc.width(),doc.height(),QImage::Format_ARGB32);
    if (!image.isNull()) for (int y=0;y<doc.height();++y) std::copy_n(doc.pixels().data()+size_t(y)*doc.width(),doc.width(),reinterpret_cast<QRgb*>(image.scanLine(y)));
    return image;
}
bool program(ProjectBundle& b, const QJsonObject& a, const QList<quint32>& palette, QString& error) {
    QMap<QString,QString> sections;
    QString target = a.value("canvas").toString();
    if(target.isEmpty()&&!b.canvases.isEmpty())target=b.canvases.first().name;
    if (a.value("action").toString().compare("edit",Qt::CaseInsensitive)==0) {
        if (indexOf(b,target)<0) { error="Unknown edit canvas.";return false; }
        sections[target]=a.value("patch").toString();
    } else {
        const auto lines=a.value("program").toString().split(QRegularExpression("[;\\n]"),Qt::SkipEmptyParts);
        for (auto line:lines) {
            line=line.trimmed(); if(line.isEmpty())continue;
            const auto head=line.section(QRegularExpression("[ ,\\t]"),0,0);
            if(head.compare("CANVAS",Qt::CaseInsensitive)==0) {
                target=line.mid(head.size()).trimmed(); if(target.startsWith(','))target=target.mid(1).trimmed();
                if(indexOf(b,target)<0) { error="Unknown CANVAS name."; return false; }
            } else {
                const int i=indexOf(b,target); if(i<0){error="Program needs a valid canvas or CANVAS directive.";return false;}
                sections[b.canvases[i].name]+=(sections.value(b.canvases[i].name).isEmpty()?QString():QString(";"))+line;
            }
        }
    }
    if(sections.isEmpty()){error="No drawing commands.";return false;}
    for(auto it=sections.cbegin();it!=sections.cend();++it){
        const int i=indexOf(b,it.key());
        PixelDocument doc({1,1,ProjectStore::MaxDimension,ProjectStore::MaxDimension,ProjectStore::MaxCanvasPixels});
        if(!document(b.canvases[i].image,doc,error))return false;
        QString patch=it.value();
        if(a.value("action").toString().compare("edit",Qt::CaseInsensitive)!=0){
            const auto compiled=win32::compile_pixel_program(patch.toStdString(),doc.width(),doc.height(),doc.pixels());
            if(!compiled.ok){error=QString::fromStdString(compiled.error);return false;}
            patch=QString::fromStdString(compiled.patch);
        }
        if(!patch.isEmpty() && !applyPixelPatch(doc,patch,palette,&error))return false;
        auto result=imageOf(doc);if(result.isNull()){error="Cannot allocate edited image.";return false;}
        b.canvases[i].image=std::move(result);
    }
    return true;
}
QList<int> selection(const ProjectBundle& b,const QJsonObject& a,QString& error){
    QList<int> result;
    const auto names=a.value("canvases").toString();
    const auto single=a.value("canvas").toString();
    const auto group=a.value("group").toString();
    if(!single.isEmpty()||!names.isEmpty()){
        const auto requested=single.isEmpty()?names.split('|',Qt::SkipEmptyParts):QStringList{single};
        for(const auto& name:requested){const int i=indexOf(b,name);if(i<0){error="Unknown selected canvas.";return {}; }if(!result.contains(i))result.append(i);}
    }else for(int i=0;i<b.canvases.size();++i)if(group.isEmpty()||b.canvases[i].group.compare(group,Qt::CaseInsensitive)==0)result.append(i);
    const auto query=a.value("query").toString(),prefix=a.value("prefix").toString();
    result.erase(std::remove_if(result.begin(),result.end(),[&](int i){const auto& c=b.canvases[i];return
        (a.contains("group")&&c.group.compare(group,Qt::CaseInsensitive)!=0)||
        (!query.isEmpty()&&!c.name.contains(query,Qt::CaseInsensitive)&&!c.group.contains(query,Qt::CaseInsensitive))||
        (!prefix.isEmpty()&&!c.name.startsWith(prefix,Qt::CaseInsensitive))||
        (a.contains("changed_since")&&c.changedRevision<=quint64(a.value("changed_since").toInteger()));}),result.end());
    std::stable_sort(result.begin(),result.end(),[&](int x,int y){const auto& a=b.canvases[x];const auto& c=b.canvases[y];if(a.group!=c.group)return a.group<c.group;if(a.frame>=0&&c.frame>=0&&a.frame!=c.frame)return a.frame<c.frame;if((a.frame>=0)!=(c.frame>=0))return a.frame>=0;return a.name<c.name;});
    return result;
}
QJsonObject filters(const QJsonObject& a){QJsonObject out;for(const auto& key:{"canvas","canvases","group","query","prefix","changed_since"})if(a.contains(key))out[key]=a.value(key);return out;}
QString renderMode(const QJsonObject& a){return a.value("mode").toString(a.value("canvas").toString().isEmpty()?"sheet":"canvas").toLower();}
QByteArray png(const QImage& image){QByteArray bytes;QBuffer buffer(&bytes);buffer.open(QIODevice::WriteOnly);if(!image.save(&buffer,"PNG")||bytes.size()>ReplyPngBytes)return {};return bytes;}
PackReply rendered(const ProjectBundle& b,const QJsonObject& a){
    QString error;auto selected=selection(b,a,error);if(!error.isEmpty())return {false,{{"error","empty_selection"},{"message",error}}, {}};
    const auto mode=renderMode(a);
    if(mode!="canvas"&&mode!="sheet"&&mode!="strip"&&mode!="timeline"&&mode!="animation"&&mode!="gif")return {false,{{"error","unsupported_render_mode"}}, {}};
    const bool hasRegion=a.contains("x")||a.contains("y")||a.contains("width")||a.contains("height");
    if(hasRegion&&mode!="canvas")return {false,{{"error","region_requires_canvas_mode"},{"message","Region bounds require canvas mode; keep sheet/strip/animation scope or explicitly choose one canvas region."}}, {}};
    qint64 scale,columns,offset,limit;
    if(!number(a,"scale",1,1,16,scale)||!number(a,"columns",4,1,128,columns)||!number(a,"offset",0,0,ProjectStore::MaxCanvases,offset)||!number(a,"limit",32,1,128,limit))return {false,{{"error","invalid_render_bounds"}}, {}};
    const int total=selected.size();selected=selected.mid(int(offset),int(limit));if(mode=="canvas"&&!selected.isEmpty())selected={selected.first()};
    if(selected.isEmpty())return {false,{{"error","empty_page"}}, {}};
    QMap<int,QImage> viewImages;
    QJsonObject region;
    if(hasRegion){
        const auto& source=b.canvases[selected.first()].image;qint64 x,y,w,h;
        if(!number(a,"x",0,std::numeric_limits<int>::min(),std::numeric_limits<int>::max(),x)||!number(a,"y",0,std::numeric_limits<int>::min(),std::numeric_limits<int>::max(),y)||!number(a,"width",source.width(),1,std::numeric_limits<int>::max(),w)||!number(a,"height",source.height(),1,std::numeric_limits<int>::max(),h))return {false,{{"error","invalid_region"}}, {}};
        const qint64 left=std::max<qint64>(0,x),top=std::max<qint64>(0,y),right=std::min<qint64>(source.width(),x+w),bottom=std::min<qint64>(source.height(),y+h);
        if(right<=left||bottom<=top)return {false,{{"error","empty_region"}}, {}};
        viewImages[selected.first()]=source.copy(int(left),int(top),int(right-left),int(bottom-top));
        region={{"x",left},{"y",top},{"width",right-left},{"height",bottom-top},{"clipped",left!=x||top!=y||right-left!=w||bottom-top!=h}};
    }
    auto imageFor=[&](int i){return viewImages.contains(i)?viewImages.value(i):b.canvases[i].image;};
    if(mode=="animation"||mode=="gif"){
        qint64 fps;if(!number(a,"fps",12,1,60,fps))return {false,{{"error","invalid_fps"}}, {}};
        QList<QImage> frames;QJsonArray names;for(int i:selected){frames.append(b.canvases[i].image);names.append(b.canvases[i].name);}const auto gif=encodeGif(frames,int(scale),int(fps));
        if(!gif.success)return {false,{{"error","gif_encode_failed"},{"message",gif.error}}, {}};
        return {true,{{"mime","image/gif"},{"width",gif.width},{"height",gif.height},{"names",names},{"total_count",total},{"returned_count",int(selected.size())},{"next_offset",offset+selected.size()<total?QJsonValue(offset+selected.size()):QJsonValue()},{"quantized",gif.quantized},{"partial_alpha_flattened",gif.partialAlphaFlattened},{"delay_centiseconds",gif.delayCentiseconds},{"sha256",QString::fromLatin1(QCryptographicHash::hash(gif.bytes,QCryptographicHash::Sha256).toHex())}},gif.bytes};
    }
    int width=0,height=0;for(int i:selected){const auto image=imageFor(i);width=std::max(width,image.width());height=std::max(height,image.height());}
    const qint64 cols=(mode=="strip"||mode=="timeline")?selected.size():std::min<qint64>(columns,selected.size());
    const qint64 rows=(selected.size()+cols-1)/cols,w=qint64(width)*scale*cols,h=qint64(height)*scale*rows;
    if(w>32768||h>32768||quint64(w)*h>RenderPixels)return {false,{{"error","render_budget"},{"message","Select fewer frames or a smaller scale."}}, {}};
    QImage result(int(w),int(h),QImage::Format_ARGB32);if(result.isNull())return {false,{{"error","allocation_failed"}}, {}};result.fill(Qt::transparent);
    QPainter painter(&result);painter.setCompositionMode(QPainter::CompositionMode_Source);painter.setRenderHint(QPainter::SmoothPixmapTransform,false);
    QJsonArray names;for(int k=0;k<selected.size();++k){const auto& c=b.canvases[selected[k]];const auto image=imageFor(selected[k]);painter.drawImage(QRect(int(k%cols*width*scale),int(k/cols*height*scale),int(image.width()*scale),int(image.height()*scale)),image);names.append(c.name);}painter.end();
    auto bytes=png(result);if(bytes.isEmpty())return {false,{{"error","png_budget_or_encode_failed"}}, {}};
    return {true,{{"mime","image/png"},{"width",result.width()},{"height",result.height()},{"region",region},{"names",names},{"total_count",total},{"returned_count",int(selected.size())},{"next_offset",offset+selected.size()<total?QJsonValue(offset+selected.size()):QJsonValue()},{"sha256",QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex())}},bytes};
}
bool safePath(const QString& path){return ProjectStore::safeAbsolutePath(path);}

bool exportName(const QString& name){return ProjectStore::safeExportFileName(name); }
} // namespace

PackReply CanvasPack::respond(bool success,QJsonObject facts,QByteArray image) const {
    facts["ok"]=success;facts["task_id"]=QJsonValue(qint64(bundle_.taskId));facts["revision"]=QJsonValue(qint64(revision_));facts["pack_revision"]=QJsonValue(qint64(revision_));
    facts["can_undo"]=!undo_.isEmpty();facts["can_redo"]=!redo_.isEmpty();facts["change_tracking_floor"]=QJsonValue(qint64(bundle_.changeTrackingFloor));return {success,std::move(facts),std::move(image)};
}
PackReply CanvasPack::reject(const QString& code,const QString& message)const{return respond(false,{{"error",code},{"message",message}});}
void CanvasPack::trimHistory(){
    quint64 bytes=0;for(const auto& h:undo_)bytes+=h.bytes;for(const auto& h:redo_)bytes+=h.bytes;
    while((undo_.size()+redo_.size()>32||bytes>HistoryBudget)&&!undo_.isEmpty()){bytes-=undo_.first().bytes;undo_.removeFirst();}
    while(bytes>HistoryBudget&&!redo_.isEmpty()){bytes-=redo_.first().bytes;redo_.removeFirst();}
}
QList<int> CanvasPack::select(const QJsonObject& args,QString& error){
    if(selectionRevision_!=revision_){selections_.clear();selectionRevision_=revision_;}
    const QString key=QString::fromUtf8(QJsonDocument(filters(args)).toJson(QJsonDocument::Compact));
    const auto found=selections_.constFind(key);if(found!=selections_.cend()){++selectionCacheHits_;return found.value();}
    auto result=selection(bundle_,args,error);if(error.isEmpty()){if(selections_.size()>=16)selections_.clear();selections_.insert(key,result);}return result;
}
bool CanvasPack::setProjectBrief(const QString& brief,QString* error){
    if(brief.size()>8192||brief.contains(QChar(0))||bundle_.canvases.isEmpty()){if(error)*error="Invalid project brief or no current pack.";return false;}
    if(brief!=bundle_.projectBrief){if(revision_>=quint64(std::numeric_limits<qint64>::max())){if(error)*error="Pack revision exhausted.";return false;}bundle_.projectBrief=brief;bundle_.packRevision=++revision_;}
    if(error)error->clear();return true;
}
bool CanvasPack::restore(const ProjectBundle& value,QString* error){
    QString reason;if(!valid(value,reason)){if(error)*error=reason;return false;}
    ProjectBundle staged=value;for(auto& c:staged.canvases){c.image=c.image.convertToFormat(QImage::Format_ARGB32);if(c.image.isNull()){if(error)*error="Canvas conversion failed.";return false;}}
    for(auto& c:staged.canvases)if(c.changedRevision>staged.packRevision){if(error)*error="Invalid canvas change revision.";return false;}
    bundle_=std::move(staged);revision_=bundle_.packRevision;undo_.clear();redo_.clear();analysis_.clear();deltas_.clear();selections_.clear();selectionRevision_=~quint64(0);renderedIdentity_.clear();renderedCache_={};if(error)error->clear();return true;
}

bool CanvasPack::replaceCanvasImage(const QString& name,const QImage& image,quint64 expected,QString* error){
    auto fail=[&](const QString& text){if(error)*error=text;return false;};
    const int i=indexOf(bundle_,name);
    if(expected!=revision_||i<0||image.isNull()||image.size()!=bundle_.canvases[i].image.size())return fail("Manual edit has stale pack revision, unknown canvas or mismatched dimensions.");
    const auto normalized=image.convertToFormat(QImage::Format_ARGB32);if(normalized.isNull())return fail("Cannot allocate manual image.");
    if(normalized==bundle_.canvases[i].image){if(error)error->clear();return true;}
    if(revision_>=quint64(std::numeric_limits<qint64>::max()))return fail("Pack revision exhausted.");
    History history;history.before[bundle_.canvases[i].name]=bundle_.canvases[i].image;history.after[bundle_.canvases[i].name]=normalized;history.bytes=quint64(image.width())*image.height()*8;
    ProjectBundle staged=bundle_;staged.canvases[i].image=normalized;staged.canvases[i].changedRevision=revision_+1;
    auto undo=undo_;undo.append(history);undo_=std::move(undo);redo_.clear();trimHistory();bundle_=std::move(staged);bundle_.packRevision=++revision_;
    if(error)error->clear();return true;
}

PackReply CanvasPack::call(const QJsonObject& a,quint64 currentTaskId,const QString& outputDirectory) {
 try {
    if(QJsonDocument(a).toJson(QJsonDocument::Compact).size()>2*1024*1024)return reject("argument_budget","Pack arguments exceed 2 MiB.");
    qint64 task;
    if(!number(a,"task_id",0,1,std::numeric_limits<qint64>::max(),task)||quint64(task)!=currentTaskId)return reject("stale_task","Use the exact currently admitted task_id.");
    if(!a.value("action").isString())return reject("invalid_action","Pack action must be a string.");
    const bool indexMode=a.value("action").toString().compare("index",Qt::CaseInsensitive)==0;
    const auto action=indexMode?QString("list"):a.value("action").toString().toLower();
    for(const auto& key:{QString("canvas"),QString("canvases"),QString("group"),QString("source"),QString("dest"),QString("program"),QString("patch"),QString("mode"),QString("scope"),QString("path"),QString("direction"),QString("project_name"),QString("create_canvases"),QString("inspect_before"),QString("inspect_after"),QString("render_mode"),QString("render_canvas"),QString("render_canvases"),QString("render_group")})if(a.contains(key)&&!a.value(key).isString())return reject("invalid_string",key+" must be a string.");
    for(const auto& key:{QString("expected_revision"),QString("pack_revision")})if(a.contains(key)){
        qint64 expected;if(!number(a,key,-1,0,std::numeric_limits<qint64>::max(),expected)||quint64(expected)!=revision_)return reject("revision_conflict","Read pack list and use its current pack_revision; no edit applied.");
    }
    for(const auto& key:{"query","prefix","format","project_brief","known_observation"})if(a.contains(key)&&!a.value(key).isString())return reject("invalid_string",QString(key)+" must be a string.");
    if(a.value("query").toString().size()>240||a.value("prefix").toString().size()>240||a.value("known_observation").toString().size()>256)return reject("filter_budget","Query, prefix or observation is too long.");
    if(a.contains("resend_image")&&!a.value("resend_image").isBool())return reject("invalid_resend","resend_image must be boolean.");
    const auto format=a.value("format").toString("compact");if(format!="compact"&&format!="legacy")return reject("invalid_format","Use compact or legacy metadata format.");
    const bool legacy=format=="legacy";
    const bool readPage=action=="list"||action=="analyze"||action=="view"||action=="inspect";
    if(readPage&&QJsonDocument(filters(a)).toJson(QJsonDocument::Compact).size()>2048)return reject("filter_budget","Use a smaller targeted filter; echoed filters are bounded to 2 KiB.");
    if(readPage&&((a.contains("offset")&&a.value("offset").toDouble()>0)||(a.contains("pixel_offset")&&a.value("pixel_offset").toDouble()>0))&&!a.contains("pack_revision")&&!a.contains("expected_revision"))return reject("page_anchor_required","Continuation requires its exact pack_revision or expected_revision; restart the query after a conflict.");
    if(a.contains("changed_since")){qint64 since;if(!number(a,"changed_since",0,0,qint64(revision_),since)||quint64(since)<bundle_.changeTrackingFloor)return reject("change_baseline_unavailable","changed_since must be within the retained pack baseline and current revision.");}
    const bool replacing=action=="create"||(action=="pass"&&a.contains("create_canvases"));
    if(!replacing&&(bundle_.canvases.isEmpty()||bundle_.taskId!=currentTaskId))return reject("pack_not_created","Restore/create the current task's pack first.");
    auto metadataPage=[&](QJsonObject facts,QJsonArray items,int start,int total)->PackReply{
        if(legacy){auto reply=respond(true,facts);if(QJsonDocument(reply.facts).toJson(QJsonDocument::Compact).size()>8192)return reject("page_byte_budget","Legacy page exceeds 8 KiB; reduce limit or use compact format.");return reply;}
        for(;;){
            facts["items"]=items;facts["returned_count"]=int(items.size());facts["next_offset"]=start+items.size()<total?QJsonValue(start+items.size()):QJsonValue(-1);
            auto reply=respond(true,facts);
            if(QJsonDocument(reply.facts).toJson(QJsonDocument::Compact).size()<=8192)return reply;
            if(items.isEmpty())return reject("page_byte_budget","This filter or item exceeds the bounded metadata page.");
            items.removeLast();
            if(items.isEmpty()&&start<total)return reject("page_byte_budget","One metadata item exceeds 8 KiB; narrow the query.");
        }
    };
    if(action=="list"||action=="analyze"){
        qint64 offset,limit;if(!number(a,"offset",0,0,ProjectStore::MaxCanvases,offset)||!number(a,"limit",64,1,256,limit))return reject("invalid_page","Use a bounded offset and limit.");
        if(a.contains("summary")&&!a.value("summary").isBool())return reject("invalid_summary","summary must be boolean.");
        QString error;auto selected=select(a,error);if(!error.isEmpty())return reject("empty_selection",error);
        int scanned=0,compared=0;
        auto metrics=[&](int index)->QJsonObject {
            const auto& c=bundle_.canvases[index];const auto key=c.image.cacheKey();auto found=analysis_.constFind(c.name);
            if(found!=analysis_.cend()&&found->imageKey==key)return found->facts;
            ++scanned;quint64 visible=0,sx=0,sy=0,hash=14695981039346656037ull;
            int left=c.image.width(),top=c.image.height(),right=-1,bottom=-1;std::vector<quint32> colors;
            colors.reserve(size_t(c.image.width())*c.image.height());
            for(int y=0;y<c.image.height();++y)for(int x=0;x<c.image.width();++x){const auto pixel=c.image.pixel(x,y);hash^=pixel;hash*=1099511628211ull;if(!qAlpha(pixel))continue;colors.push_back(pixel);++visible;sx+=x;sy+=y;left=std::min(left,x);right=std::max(right,x);top=std::min(top,y);bottom=std::max(bottom,y);}
            std::sort(colors.begin(),colors.end());const auto distinct=std::unique(colors.begin(),colors.end())-colors.begin();
            QJsonObject result{{"name",c.name},{"group",c.group},{"frame",c.frame},{"width",c.image.width()},{"height",c.image.height()},{"order",index},{"visible",QJsonValue(qint64(visible))},{"left",visible?left:-1},{"top",visible?top:-1},{"right",right},{"bottom",bottom},{"centroid_x",visible?double(sx)/visible:-1.0},{"centroid_y",visible?double(sy)/visible:-1.0},{"colors",int(distinct)},{"hash",QString::number(hash,16)},{"hash_algorithm","fnv1a64_argb_words"}};
            analysis_[c.name]={key,result};return result;
        };
        auto delta=[&](int index,int previous)->qint64{
            const auto& c=bundle_.canvases[index];const auto& p=bundle_.canvases[previous];if(c.image.size()!=p.image.size())return -1;
            const auto stamp=QString::number(c.image.cacheKey())+":"+QString::number(p.image.cacheKey())+":"+p.name;const auto old=deltas_.constFind(c.name);if(old!=deltas_.cend()&&old->first==stamp)return old->second;
            ++compared;qint64 count=0;for(int y=0;y<c.image.height();++y)for(int x=0;x<c.image.width();++x)count+=c.image.pixel(x,y)!=p.image.pixel(x,y);deltas_[c.name]={stamp,count};return count;
        };
        if(a.value("summary").toBool(indexMode)){
            QMap<QString,QList<int>> groups;for(int i:selected)groups[bundle_.canvases[i].group].append(i);
            const auto keys=groups.keys();const int end=std::min<qint64>(keys.size(),offset+limit);QString rows;QJsonArray items;
            for(int n=int(offset);n<end;++n){const auto& name=keys[n];const auto frames=groups.value(name);QJsonObject facts{{"group",name},{"frames",int(frames.size())}};QString row=name+","+QString::number(frames.size());
                if(action=="analyze"){
                    int empty=0,duplicates=0,mismatch=0; qint64 low=std::numeric_limits<qint64>::max(),high=0,worst=0;QString worstName;QSet<QString> hashes;
                    for(int k=0;k<frames.size();++k){const auto m=metrics(frames[k]);const auto visible=m.value("visible").toInteger();empty+=visible==0;low=std::min(low,visible);high=std::max(high,visible);const auto identity=QString::number(m["width"].toInt())+"x"+QString::number(m["height"].toInt())+":"+m["hash"].toString();duplicates+=hashes.contains(identity);hashes.insert(identity);const auto count=delta(frames[k],frames[(k+frames.size()-1)%frames.size()]);if(count<0){++mismatch;continue;}if(count>worst){worst=count;worstName=bundle_.canvases[frames[k]].name;}}
                    facts["empty"]=empty;facts["duplicates"]=duplicates;facts["dimension_mismatches"]=mismatch;facts["min_visible"]=low;facts["max_visible"]=high;facts["max_changed"]=worst;facts["transition_to"]=worstName;
                    row+=","+QString::number(empty)+","+QString::number(duplicates)+","+QString::number(mismatch)+","+QString::number(low)+","+QString::number(high)+","+QString::number(worst)+","+worstName;
                }
                if(!rows.isEmpty())rows+='|';rows+=row;items.append(facts);
            }
            QJsonObject result{{"summary",true},{"group_count",int(keys.size())},{"canvas_count",int(bundle_.canvases.size())},{"matched_canvas_count",int(selected.size())},{"returned_count",int(items.size())},{"offset",offset},{"next_offset",end<keys.size()?QJsonValue(end):QJsonValue(-1)},{"scanned_frames",scanned},{"compared_pairs",compared},{"filters",filters(a)},{"order","group,frame,name"},{"format",format},{"selection_cache_hits",selectionCacheHits_}};
            if(legacy){result["columns"]=action=="list"?"group,frames":"group,frames,empty,duplicates,dimension_mismatches,min_visible,max_visible,max_changed,transition_to";result["rows"]=rows;}else result["items"]=items;
            return metadataPage(result,items,int(offset),int(keys.size()));
        }
        const int end=std::min<qint64>(selected.size(),offset+limit);QString rows,versions;QJsonArray entries;
        for(int k=int(offset);k<end;++k){const int i=selected[k];const auto& c=bundle_.canvases[i];QJsonObject facts;QString row;
            if(action=="list"){facts={{"name",c.name},{"group",c.group},{"frame",c.frame},{"width",c.image.width()},{"height",c.image.height()},{"order",i}};row=c.name+","+c.group+","+QString::number(c.image.width())+","+QString::number(c.image.height())+","+QString::number(c.frame);}
            else{
                facts=metrics(i);int previous=-1;if(c.frame>=0&&!c.group.isEmpty()){if(k>0&&bundle_.canvases[selected[k-1]].group==c.group)previous=selected[k-1];else for(int p=selected.size()-1;p>=0;--p)if(bundle_.canvases[selected[p]].group==c.group){previous=selected[p];break;}}
                const qint64 difference=previous>=0?delta(i,previous):-1;facts["previous"]=difference>=0?bundle_.canvases[previous].name:QString();facts["changed_from_previous"]=difference;facts["loop_transition"]=previous>=0&&(k==0||bundle_.canvases[selected[k-1]].group!=c.group);
                row=c.name+","+c.group+","+QString::number(c.frame)+","+QString::number(c.image.width())+","+QString::number(c.image.height());
                for(const auto& field:{QString("visible"),QString("left"),QString("top"),QString("right"),QString("bottom")})row+=","+QString::number(facts[field].toInteger());
                row+=","+QString::number(facts["centroid_x"].toDouble(),'g',5)+","+QString::number(facts["centroid_y"].toDouble(),'g',5)+","+QString::number(facts["colors"].toInt())+","+facts["hash"].toString()+","+facts["previous"].toString()+","+QString::number(difference);
            }
            facts["changed_revision"]=QJsonValue(qint64(c.changedRevision));
            if(!rows.isEmpty()){rows+='|';versions+='|';}rows+=row;versions+=c.name+","+QString::number(c.changedRevision);entries.append(facts);
        }
        QJsonObject facts{{"total_count",int(selected.size())},{"offset",offset},{"canvas_count",int(bundle_.canvases.size())},{"returned_count",int(entries.size())},{"next_offset",end<selected.size()?QJsonValue(end):QJsonValue(-1)},{"filters",filters(a)},{"order","group,frame,name"},{"format",format},{"selection_cache_hits",selectionCacheHits_}};
        if(!legacy)facts["items"]=entries;
        if(action=="list"&&legacy){facts["canvases"]=rows;facts["canvas_revisions"]=versions;}
        if(action=="analyze"){if(legacy){facts["rows"]=rows;facts["columns"]="name,group,frame,width,height,visible,left,top,right,bottom,centroid_x,centroid_y,colors,hash,previous,changed_from_previous";}facts["scanned_frames"]=scanned;facts["compared_pairs"]=compared;}
        return metadataPage(facts,entries,int(offset),int(selected.size()));
    }
    if(action=="view") {
        QString error;auto selected=select(a,error);if(!error.isEmpty())return reject("empty_selection",error);
        qint64 offset,limit;if(!number(a,"offset",0,0,ProjectStore::MaxCanvases,offset)||!number(a,"limit",32,1,128,limit))return reject("invalid_page","Invalid image page.");
        const int total=selected.size();selected=selected.mid(int(offset),int(limit));if(renderMode(a)=="canvas"&&!selected.isEmpty())selected={selected.first()};
        QJsonArray identities;int hashed=0;for(int i:selected){const auto& c=bundle_.canvases[i];bool fresh=false;const auto hash=ProjectStore::pixelIdentity(c.image,&fresh);if(hash.isEmpty())return reject("image_identity_failed","Cannot identify selected image.");hashed+=fresh;identities.append(QJsonObject{{"name",c.name},{"pixels",hash}});}
        QJsonObject options;for(const auto& key:{"mode","scale","columns","fps","x","y","width","height"})if(a.contains(key))options[key]=a.value(key);
        options["mode"]=renderMode(a);
        const QJsonObject descriptor{{"task",QJsonValue(qint64(bundle_.taskId))},{"images",identities},{"options",options},{"offset",offset},{"total",total},{"renderer","pack-view-v3"}};
        const QString observation=QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(descriptor).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
        if(renderedIdentity_==observation&&renderedCache_.success&&a.value("known_observation").toString()==observation&&!a.value("resend_image").toBool())return respond(true,{{"observation",observation},{"unchanged",true},{"image_omitted",true},{"images_hashed",hashed},{"filters",filters(a)},{"total_count",total}});
        PackReply r;
        if(renderedIdentity_==observation&&renderedCache_.success){r=renderedCache_;++renderCacheHits_;}
        else{r=rendered(bundle_,a);if(r.success){++renders_;renderedIdentity_=observation;renderedCache_=r;}}
        r.facts["observation"]=observation;r.facts["images_hashed"]=hashed;r.facts["render_count"]=renders_;r.facts["render_cache_hits"]=renderCacheHits_;r.facts["filters"]=filters(a);r.facts["mode"]=renderMode(a);
        if(r.success&&a.value("known_observation").toString()==observation&&!a.value("resend_image").toBool()){r.image.clear();r.facts["unchanged"]=true;r.facts["image_omitted"]=true;}
        return respond(r.success,r.facts,r.image);
    }
    if(action=="inspect"){
        const int i=indexOf(bundle_,a.value("canvas").toString());if(i<0)return reject("unknown_canvas","Inspect requires an exact existing canvas.");
        const auto& image=bundle_.canvases[i].image;qint64 x,y,w,h,offset,limit;
        if(!number(a,"x",0,std::numeric_limits<int>::min(),std::numeric_limits<int>::max(),x)||!number(a,"y",0,std::numeric_limits<int>::min(),std::numeric_limits<int>::max(),y)||!number(a,"width",image.width(),1,std::numeric_limits<int>::max(),w)||!number(a,"height",image.height(),1,std::numeric_limits<int>::max(),h)||!number(a,"pixel_offset",0,0,ProjectStore::MaxCanvasPixels,offset)||!number(a,"pixel_limit",4096,1,4096,limit))return reject("invalid_inspection","Invalid region or pixel page.");
        const qint64 left=std::max<qint64>(0,x),top=std::max<qint64>(0,y),right=std::min<qint64>(image.width(),x+w),bottom=std::min<qint64>(image.height(),y+h);
        const qint64 actualW=std::max<qint64>(0,right-left),actualH=std::max<qint64>(0,bottom-top),total=actualW*actualH,end=std::min(total,offset+limit);
        QJsonArray colors;QString rle,last;int run=0;
        for(qint64 p=offset;p<end;++p){const QString color=QString("%1").arg(image.pixel(int(left+p%actualW),int(top+p/actualW)),8,16,QChar('0')).toUpper();colors.append("#"+color);if(color==last)++run;else{if(run){if(!rle.isEmpty())rle+=',';rle+=last+'x'+QString::number(run);}last=color;run=1;}}
        if(run){if(!rle.isEmpty())rle+=',';rle+=last+'x'+QString::number(run);}
        return respond(true,{{"canvas",bundle_.canvases[i].name},{"x",left},{"y",top},{"width",actualW},{"height",actualH},{"clipped",left!=x||top!=y||actualW!=w||actualH!=h},{"total_pixels",total},{"pixels",colors},{"rle",rle},{"next_pixel_offset",end<total?QJsonValue(end):QJsonValue()}});
    }
    if(action=="save"){
        if(outputDirectory.isEmpty()||!QDir::isAbsolutePath(outputDirectory))return reject("output_directory_required","Save requires the controller's absolute project directory.");
        QString error;ProjectBundle saved=bundle_;saved.packRevision=revision_;ProjectSaveStats stats;
        if(!ProjectStore::save(outputDirectory,saved,&error,&stats))return reject("save_failed",error);
        return respond(true,{{"manifest",QDir(outputDirectory).filePath("project.json")},{"pixels_edited",false},{"reused_images",stats.reusedImages},{"encoded_images",stats.encodedImages},{"hashed_images",stats.hashedImages},{"verified_images",stats.verifiedImages},{"referenced_bytes",stats.referencedBytes}});
    }
    if(action=="export"){
        if(outputDirectory.isEmpty()||!QDir::isAbsolutePath(outputDirectory)||!safePath(outputDirectory))return reject("unsafe_export_root","Export requires a safe absolute controller-owned directory.");
        const auto scope=a.value("scope").toString("canvas").toLower();
        if(scope!="canvas"&&scope!="sheet"&&scope!="strip"&&scope!="timeline"&&scope!="group"&&scope!="all"&&scope!="animation"&&scope!="gif")return reject("unsupported_export_scope","Use canvas/group/all/sheet/strip/timeline PNG or animation/GIF export.");
        if(!QDir().mkpath(outputDirectory)||!safePath(outputDirectory))return reject("export_directory_failed","Cannot create a safe export directory.");
        QString error;auto selected=selection(bundle_,a,error);if(!error.isEmpty()||selected.isEmpty())return reject("empty_selection",error.isEmpty()?"No canvases matched export selection.":error);
        if(scope=="animation"||scope=="gif"){
            qint64 scale,fps;if(!number(a,"scale",1,1,16,scale)||!number(a,"fps",12,1,60,fps))return reject("invalid_gif_options","Invalid scale or fps.");
            QList<QImage> frames;for(int i:selected)frames.append(bundle_.canvases[i].image);const auto gif=encodeGif(frames,int(scale),int(fps));if(!gif.success)return reject("gif_encode_failed",gif.error);
            const auto requested=a.value("path").toString("animation.gif");if(!exportName(requested)||QFileInfo(requested).suffix().compare("gif",Qt::CaseInsensitive)!=0)return reject("unsafe_export_path","GIF needs a safe single filename in the output directory.");const auto path=QDir(outputDirectory).filePath(requested);if(!safePath(path))return reject("unsafe_export_path","Symlink GIF destination refused.");if(!exportReplacementAllowed_&&QFileInfo::exists(path))return reject("export_exists","Agent exports preserve existing files; choose a new filename.");QSaveFile file(path);file.setDirectWriteFallback(false);if(!file.open(QIODevice::WriteOnly)||file.write(gif.bytes)!=gif.bytes.size()||!file.commit())return reject("export_failed","Atomic GIF write failed.");return respond(true,{{"path",path},{"mime","image/gif"},{"frames",int(selected.size())},{"quantized",gif.quantized},{"partial_alpha_flattened",gif.partialAlphaFlattened},{"delay_centiseconds",gif.delayCentiseconds},{"pixels_edited",false}});
        }
        if(scope=="canvas"||scope=="sheet"||scope=="strip"||scope=="timeline"){
            QJsonObject args=a;args["mode"]=scope;args["offset"]=0;args["limit"]=128;
            if(selected.size()>128&&scope!="canvas")return reject("export_budget","Select at most 128 images for one bounded sheet/strip export.");
            auto renderedImage=rendered(bundle_,args);if(!renderedImage.success)return respond(false,renderedImage.facts);
            const auto requested=a.value("path").toString(scope=="canvas"?bundle_.canvases[selected.first()].name+".png":scope+".png");
            if(!exportName(requested)||QFileInfo(requested).suffix().compare("png",Qt::CaseInsensitive)!=0)return reject("unsafe_export_path","Export path must be a single safe PNG filename below the output directory.");
            const auto path=QDir(outputDirectory).filePath(requested);if(!safePath(path))return reject("unsafe_export_path","Symlink export destination refused.");
            if(!exportReplacementAllowed_&&QFileInfo::exists(path))return reject("export_exists","Agent exports preserve existing files; choose a new filename.");
            QSaveFile file(path);file.setDirectWriteFallback(false);if(!file.open(QIODevice::WriteOnly)||file.write(renderedImage.image)!=renderedImage.image.size()||!file.commit())return reject("export_failed","Atomic PNG write failed.");
            return respond(true,{{"path",path},{"mime","image/png"},{"pixels_edited",false}});
        }
        const auto requested=a.value("path").toString(scope=="group"?QString("group-")+a.value("group").toString():QString("all-canvases"));
        if(!exportName(requested))return reject("unsafe_export_path","Group export needs one safe new directory name.");
        const auto destination=QDir(outputDirectory).filePath(requested);if(QFileInfo::exists(destination)||!safePath(destination))return reject("export_exists","Refusing to replace an existing export directory.");
        QTemporaryDir staged(QDir(outputDirectory).filePath(".pack-export-XXXXXX"));if(!staged.isValid())return reject("export_failed","Cannot stage group export.");
        QJsonArray paths;for(int i:selected){const auto bytes=png(bundle_.canvases[i].image);if(bytes.isEmpty())return reject("export_budget","PNG exceeds the portable per-image export budget.");QSaveFile file(QDir(staged.path()).filePath(bundle_.canvases[i].name+".png"));file.setDirectWriteFallback(false);if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size()||!file.commit())return reject("export_failed","PNG group write failed.");paths.append(QDir(destination).filePath(bundle_.canvases[i].name+".png"));}
        if(!safePath(outputDirectory)||!QDir().rename(staged.path(),destination))return reject("export_failed","Cannot publish complete export directory.");staged.setAutoRemove(false);return respond(true,{{"paths",paths},{"directory",destination},{"pixels_edited",false}});
    }
    if(action=="history"){
        const auto direction=a.value("direction").toString().toLower();if(direction!="undo"&&direction!="redo")return reject("invalid_direction","Use undo or redo.");
        auto& source=direction=="undo"?undo_:redo_;auto& destination=direction=="undo"?redo_:undo_;
        if(source.isEmpty())return reject("history_empty","No retained whole-pass history in this direction.");
        if(revision_>=quint64(std::numeric_limits<qint64>::max()))return reject("revision_exhausted","Cannot advance pack revision.");
        const auto entry=source.last();const auto& images=direction=="undo"?entry.before:entry.after;ProjectBundle staged=bundle_;
        for(auto it=images.cbegin();it!=images.cend();++it){const int i=indexOf(staged,it.key());if(i<0||staged.canvases[i].image.size()!=it.value().size())return reject("history_conflict","Changed canvas structure prevents this history operation.");staged.canvases[i].image=it.value();staged.canvases[i].changedRevision=revision_+1;}
        auto nextSource=source;auto nextDestination=destination;nextSource.removeLast();nextDestination.append(entry);source=std::move(nextSource);destination=std::move(nextDestination);bundle_=std::move(staged);bundle_.packRevision=++revision_;return respond(true,{{"direction",direction},{"changed_canvases",int(images.size())}});
    }
    if(action!="create"&&action!="add"&&action!="program"&&action!="edit"&&action!="copy"&&action!="clone"&&action!="pass")return reject("unsupported_action","This portable pack action is not implemented.");
    if(revision_>=quint64(std::numeric_limits<qint64>::max()))return reject("revision_exhausted","Cannot advance pack revision.");
    ProjectBundle staged=bundle_;QString error;QJsonObject extra;
    if(replacing){staged={};staged.taskId=currentTaskId;staged.packRevision=revision_;staged.projectName=a.value("project_name").toString();staged.projectBrief=a.value("project_brief").toString();if(!specs(a.value(action=="pass"?"create_canvases":"canvases").toString(),staged,error))return reject("invalid_canvases",error);}
    if(action=="add"){
        const int previous=staged.canvases.size();const auto sourceName=a.value("source").toString();const int seed=indexOf(bundle_,sourceName);
        if(!sourceName.isEmpty()&&seed<0)return reject("unknown_canvas","Unknown source seed canvas.");
        if(!specs(a.value("canvases").toString(),staged,error))return reject("invalid_canvases",error);
        if(seed>=0)for(int i=previous;i<staged.canvases.size();++i){if(staged.canvases[i].image.size()!=bundle_.canvases[seed].image.size())return reject("source_size_mismatch","All seeded added canvases must match source dimensions.");staged.canvases[i].image=bundle_.canvases[seed].image;}
    }
    auto inspections=[&](const QString& specification,const ProjectBundle& b,QJsonArray& result)->bool {
        if(specification.isEmpty())return true;
        const auto rows=specification.split('|');if(rows.size()>4){error="At most four bounded inspections per side of a pass.";return false;}
        CanvasPack observer;if(!observer.restore(b,&error))return false;
        for(const auto& row:rows){const auto f=row.split(',');if(f.size()!=5){error="Inspection needs canvas,x,y,width,height.";return false;}QJsonObject request{{"action","inspect"},{"task_id",QJsonValue(qint64(b.taskId))},{"canvas",f[0].trimmed()},{"pixel_limit",4096}};const QStringList keys{"x","y","width","height"};for(int n=1;n<5;++n){bool ok=false;const int value=f[n].trimmed().toInt(&ok);if(!ok){error="Invalid inspection integer.";return false;}request[keys[n-1]]=value;}const auto reply=observer.call(request,b.taskId,QString());if(!reply.success){error=reply.facts.value("message").toString("Inspection failed.");return false;}result.append(reply.facts);}
        return true;
    };
    if(a.contains("seed_from_source")&&!a["seed_from_source"].isBool())return reject("invalid_seed","seed_from_source must be boolean.");
    if(action=="pass"&&replacing&&a.value("seed_from_source").toBool(true)&&!sourceImage_.isNull()){
        QJsonArray seeded;for(auto& c:staged.canvases)if(c.image.size()==sourceImage_.size()){c.image=sourceImage_.convertToFormat(QImage::Format_ARGB32);seeded.append(c.name);}extra["seeded_canvases"]=seeded;
    }
    if(action=="pass"&&a.contains("inspect_before")){QJsonArray facts;if(!inspections(a.value("inspect_before").toString(),staged,facts))return reject("inspection_rejected",error);extra["inspect_before"]=facts;}
    if(action=="program"||action=="edit"||action=="pass"){
        if(action!="pass"||!a.value("program").toString().isEmpty())if(!program(staged,a,palette_,error))return reject("program_rejected",error);
    }
    if(action=="copy"||action=="clone"){
        const int source=indexOf(staged,a.value("source").toString()),dest=indexOf(staged,a.value("dest").toString());
        if(source<0||dest<0)return reject("unknown_canvas","Source and destination must exist.");
        const QImage original=staged.canvases[source].image;auto& target=staged.canvases[dest].image;
        if(action=="clone"){if(original.size()!=target.size())return reject("size_mismatch","Clone canvases must have matching dimensions.");target=original;}
        else{
            qint64 sx,sy,dx,dy,w,h;const auto min=std::numeric_limits<int>::min(),max=std::numeric_limits<int>::max();
            if(!number(a,"sx",0,min,max,sx)||!number(a,"sy",0,min,max,sy)||!number(a,"dx",0,min,max,dx)||!number(a,"dy",0,min,max,dy)||!number(a,"width",original.width(),0,max,w)||!number(a,"height",original.height(),0,max,h))return reject("invalid_copy","Copy geometry must be bounded integers.");
            const auto left=std::max({qint64(0),-sx,-dx}),top=std::max({qint64(0),-sy,-dy});
            const auto right=std::min({w,qint64(original.width())-sx,qint64(target.width())-dx}),bottom=std::min({h,qint64(original.height())-sy,qint64(target.height())-dy});
            const auto aw=std::max(qint64(0),right-left),ah=std::max(qint64(0),bottom-top);
            if(aw&&ah){target=target.copy();if(target.isNull())return reject("allocation_failed","Copy allocation failed.");for(int y=0;y<ah;++y)for(int x=0;x<aw;++x)target.setPixel(int(dx+left+x),int(dy+top+y),original.pixel(int(sx+left+x),int(sy+top+y)));}
            extra={{"clipped",aw!=w||ah!=h||left!=0||top!=0},{"empty",aw==0||ah==0},{"width",aw},{"height",ah}};
        }
    }
    if(!valid(staged,error))return reject("pack_validation",error);
    if(action=="pass"&&a.contains("inspect_after")){QJsonArray facts;if(!inspections(a.value("inspect_after").toString(),staged,facts))return reject("inspection_rejected",error);extra["inspect_after"]=facts;}
    PackReply preview;
    if(action=="pass"&&a.contains("render_mode")){
        QJsonObject args=a;args["mode"]=a.value("render_mode");args.remove("canvases");args.remove("canvas");args.remove("group");
        if(a.contains("render_canvas"))args["canvas"]=a.value("render_canvas");if(a.contains("render_canvases"))args["canvases"]=a.value("render_canvases");if(a.contains("render_group"))args["group"]=a.value("render_group");
        preview=rendered(staged,args);if(!preview.success)return respond(false,preview.facts);
    }
    History entry;quint64 changedPixels=0;QJsonArray changedNames;
    if(!replacing)for(const auto& c:staged.canvases){const int i=indexOf(bundle_,c.name);if(i>=0&&bundle_.canvases[i].image!=c.image){entry.before[c.name]=bundle_.canvases[i].image;entry.after[c.name]=c.image;entry.bytes+=quint64(c.image.width())*c.image.height()*8;changedNames.append(c.name);for(int y=0;y<c.image.height();++y)for(int x=0;x<c.image.width();++x)if(c.image.pixel(x,y)!=bundle_.canvases[i].image.pixel(x,y))++changedPixels;}}
    if(entry.bytes>HistoryBudget)return reject("history_budget","This pass exceeds the 128 MiB retained-history budget; use smaller atomic passes. No changes applied.");
    const bool changed=replacing||action=="add"||!entry.before.isEmpty();
    if(changed){
        for(auto& c:staged.canvases){const int old=indexOf(bundle_,c.name);if(replacing||old<0||entry.after.contains(c.name))c.changedRevision=revision_+1;}
        if(replacing){staged.changeTrackingFloor=revision_+1;undo_.clear();redo_.clear();analysis_.clear();deltas_.clear();}else if(!entry.before.isEmpty()){undo_.append(entry);redo_.clear();trimHistory();}bundle_=std::move(staged);bundle_.packRevision=++revision_;
    }
    extra["changed"]=changed;extra["changed_pixels"]=QJsonValue(qint64(changedPixels));extra["changed_canvases"]=changedNames;extra["canvas_count"]=int(bundle_.canvases.size());extra["history_retained"]=!entry.before.isEmpty()&&!undo_.isEmpty();
    if(preview.success){for(auto it=preview.facts.begin();it!=preview.facts.end();++it)extra[it.key()]=it.value();return respond(true,extra,preview.image);}
    return respond(true,extra);
 } catch(const std::bad_alloc&){return reject("allocation_failed","Pack operation exceeded available memory; retry only after inspecting current state.");}
}
} // namespace pixelforge::qt
