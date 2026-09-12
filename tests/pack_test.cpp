#include "CanvasPack.hpp"
#include <QCoreApplication>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QJsonArray>
#include <QBuffer>
#include <QImageReader>
#include "GifExport.hpp"
#include <iostream>

using namespace pixelforge::qt;
#define CHECK(c) do { if(!(c)){std::cerr<<"Pack check failed line "<<__LINE__<<'\n';return 1;} } while(false)
int main(int argc,char** argv){
    QCoreApplication app(argc,argv);QTemporaryDir tmp;CHECK(tmp.isValid());
    const QString output=QFileInfo(tmp.path()).canonicalFilePath();CanvasPack pack;
    auto call=[&](QJsonObject a){a["task_id"]=7;a["pack_revision"]=QJsonValue(qint64(pack.revision()));return pack.call(a,7,output);};
    auto r=call({{"action","create"},{"canvases","first,walk,4,3,2|second,walk,4,3,0"},{"project_name","My pack"}});
    CHECK(r.success&&pack.revision()==1&&pack.bundle().canvases.size()==2);
    CHECK(pack.bundle().canvases[0].name=="first"&&pack.bundle().canvases[0].frame==2);
    r=call({{"action","pass"},{"program","CANVAS first;P 0 0 #FF112233;CANVAS second;H 0 1 3 #FF445566"},{"inspect_before","first,0,0,1,1"},{"inspect_after","first,0,0,1,1"},{"render_mode","strip"},{"render_group","walk"}});
    CHECK(r.success&&!r.image.isEmpty()&&pack.revision()==2);
    CHECK(r.facts["inspect_before"].toArray().size()==1&&r.facts["inspect_after"].toArray().size()==1);
    CHECK(pack.bundle().canvases[0].image.pixel(0,0)==0xff112233);
    CHECK(pack.bundle().canvases[1].image.pixel(2,1)==0xff445566);
    const auto noopRevision=pack.revision();CHECK(call({{"action","edit"},{"canvas","first"},{"patch","P,0,0,#FF112233"}}).success);CHECK(pack.revision()==noopRevision);
    const auto prior=pack.bundle();const auto revision=pack.revision();
    r=call({{"action","program"},{"program","CANVAS first;P 0 0 #FFFFFFFF;CANVAS missing;P 0 0 #FFFFFFFF"}});
    CHECK(!r.success&&pack.revision()==revision&&pack.bundle().canvases[0].image==prior.canvases[0].image);
    r=call({{"action","program"},{"program","CANVAS first;P 0 0 #FFFFFFFF;CANVAS second;THIS_IS_INVALID"}});
    CHECK(!r.success&&pack.revision()==revision&&pack.bundle().canvases[0].image==prior.canvases[0].image);
    r=pack.call({{"action","edit"},{"task_id",7},{"pack_revision",1},{"canvas","first"},{"patch","P,0,0,#FFFFFFFF"}},7,output);
    CHECK(!r.success&&pack.revision()==revision);
    r=pack.call({{"action","list"},{"task_id",8}},7,output);CHECK(!r.success);
    CHECK(call({{"action","history"},{"direction","undo"}}).success);
    CHECK(pack.bundle().canvases[0].image.pixel(0,0)==0&&pack.bundle().canvases[1].image.pixel(2,1)==0);
    CHECK(call({{"action","history"},{"direction","redo"}}).success);
    CHECK(pack.bundle().canvases[0].image.pixel(0,0)==0xff112233&&pack.bundle().canvases[1].image.pixel(2,1)==0xff445566);
    CHECK(call({{"action","add"},{"canvases","third,walk,4,3,3"},{"source","first"}}).success);
    CHECK(pack.bundle().canvases.size()==3);
    CHECK(call({{"action","history"},{"direction","undo"}}).success);
    CHECK(pack.bundle().canvases.size()==3&&pack.bundle().canvases[2].image.pixel(0,0)==0xff112233);
    CHECK(call({{"action","history"},{"direction","redo"}}).success);
    CHECK(call({{"action","clone"},{"source","first"},{"dest","second"}}).success);
    CHECK(pack.bundle().canvases[0].image==pack.bundle().canvases[1].image);
    CHECK(call({{"action","copy"},{"source","first"},{"dest","first"},{"sx",0},{"sy",0},{"dx",1},{"dy",0},{"width",4},{"height",3}}).success);
    CHECK(pack.bundle().canvases[0].image.pixel(1,0)==0xff112233);
    r=call({{"action","inspect"},{"canvas","first"},{"x",-1},{"y",0},{"width",3},{"height",1},{"pixel_limit",1}});
    CHECK(r.success&&r.facts["clipped"].toBool()&&r.facts["pixels"].toArray().size()==1&&!r.facts["next_pixel_offset"].isNull());
    r=call({{"action","view"},{"group","walk"},{"mode","strip"},{"scale",2}});
    CHECK(r.success&&QImage::fromData(r.image,"PNG").size()==QSize(24,6));
    CHECK(r.facts["names"].toArray()[0].toString()=="second"); // Stable frame ordering for group view.
    r=call({{"action","view"},{"mode","animation"},{"group","walk"},{"fps",12}});CHECK(r.success&&r.facts["mime"].toString()=="image/gif"&&r.image.startsWith("GIF89a"));
    QBuffer gifBuffer(&r.image);CHECK(gifBuffer.open(QIODevice::ReadOnly));QImageReader gifReader(&gifBuffer,"GIF");CHECK(gifReader.canRead()&&gifReader.imageCount()==3);CHECK(!gifReader.read().isNull());
    CHECK(call({{"action","export"},{"scope","animation"},{"group","walk"},{"path","walk.gif"}}).success);CHECK(QImage(output+"/walk.gif").size()==QSize(4,3));
    QImage many(256,1,QImage::Format_ARGB32);for(int x=0;x<256;++x)many.setPixel(x,0,qRgba(x,0,0,255));const auto quantized=encodeGif({many},1,10);CHECK(quantized.success&&quantized.quantized);
    QImage alpha(1,1,QImage::Format_ARGB32);alpha.setPixel(0,0,qRgba(1,2,3,128));CHECK(encodeGif({alpha},1,12).partialAlphaFlattened);
    CHECK(!encodeGif({many},17,12).success);
    r=call({{"action","analyze"},{"group","walk"},{"offset",1},{"limit",1}});CHECK(r.success&&r.facts["returned_count"].toInt()==1);auto metric=r.facts["items"].toArray()[0].toObject();CHECK(metric["name"].toString()=="first"&&metric["previous"].toString()=="second"&&metric["changed_from_previous"].toInteger()>=0&&metric["visible"].toInteger()==2);
    r=call({{"action","analyze"},{"group","walk"},{"offset",1},{"limit",1}});CHECK(r.success&&r.facts["scanned_frames"].toInt()==0&&r.facts["compared_pairs"].toInt()==0);
    r=call({{"action","analyze"},{"group","walk"},{"offset",0},{"limit",1}});CHECK(r.success&&r.facts["items"].toArray()[0].toObject()["loop_transition"].toBool());
    r=call({{"action","analyze"},{"summary",true}});CHECK(r.success&&r.facts["group_count"].toInt()==1&&r.facts["items"].toArray()[0].toObject()["frames"].toInt()==3);
    CHECK(!call({{"action","export"},{"scope","canvas"},{"canvas","first"},{"path","../escape.png"}}).success);
    CHECK(call({{"action","export"},{"scope","canvas"},{"canvas","first"},{"path","first.png"}}).success);
    CHECK(QImage(output+"/first.png")==pack.bundle().canvases[0].image);
    CHECK(call({{"action","export"},{"scope","group"},{"group","walk"},{"path","walk-export"}}).success);
    CHECK(call({{"action","export"},{"scope","timeline"},{"group","walk"},{"path","walk-timeline.png"}}).success);
    CHECK(QImage(output+"/walk-timeline.png").size()==QSize(12,3));
    CHECK(QFileInfo::exists(output+"/walk-export/third.png"));
    QString error;auto manual=pack.bundle().canvases[0].image;manual.setPixel(3,2,0xffabcdef);
    const auto beforeManual=pack.revision();CHECK(!pack.replaceCanvasImage("first",manual,beforeManual-1,&error));
    CHECK(pack.replaceCanvasImage("first",manual,beforeManual,&error));
    CHECK(pack.revision()==beforeManual+1&&pack.bundle().canvases[0].image.pixel(3,2)==0xffabcdef);
    CHECK(call({{"action","history"},{"direction","undo"}}).success);
    CHECK(pack.bundle().canvases[0].image.pixel(3,2)!=0xffabcdef);
    CHECK(call({{"action","save"}}).success);
    ProjectBundle disk;CHECK(ProjectStore::load(output,disk,&error));CHECK(disk.projectName=="My pack"&&disk.packRevision==pack.revision());
    CanvasPack restored;CHECK(restored.restore(disk,&error));CHECK(restored.bundle().canvases[2].name=="third");
    auto invalid=disk;invalid.canvases[1].name="FIRST";CHECK(!restored.restore(invalid,&error));CHECK(restored.bundle().canvases[1].name=="second");
    CHECK(ProjectStore::MaxCanvases == 4096);
    r=call({{"action","list"},{"query","WALK"},{"prefix","fi"},{"limit",1}});
    CHECK(r.success&&r.facts["items"].toArray().size()==1&&r.facts["items"].toArray()[0].toObject()["name"].toString()=="first");
    CHECK(!r.facts.contains("canvases")&&!r.facts.contains("rows")&&r.facts["filters"].toObject()["prefix"].toString()=="fi");
    r=call({{"action","list"},{"query","no-match"}});CHECK(r.success&&r.facts["returned_count"].toInt()==0);
    r=call({{"action","list"},{"format","legacy"}});CHECK(r.success&&r.facts.contains("canvases")&&!r.facts.contains("items"));
    r=call({{"action","index"}});CHECK(r.success&&r.facts["group_count"].toInt()==1&&r.facts["items"].toArray()[0].toObject()["frames"].toInt()==3);
    const auto anchor=pack.revision();
    r=pack.call({{"action","list"},{"task_id",7},{"offset",1},{"limit",1}},7,output);CHECK(!r.success&&r.facts["error"].toString()=="page_anchor_required");
    r=call({{"action","list"},{"offset",1},{"limit",1}});CHECK(r.success&&r.facts["items"].toArray()[0].toObject()["name"].toString()=="first");
    r=call({{"action","view"},{"mode","canvas"},{"canvas","first"},{"x",1},{"y",0},{"width",2},{"height",1},{"scale",2}});
    CHECK(r.success&&QImage::fromData(r.image,"PNG").size()==QSize(4,2));
    // Exact named view without mode must honor requested ROI, not silently use
    // the default sheet renderer and return the full scaled canvas.
    r=call({{"action","view"},{"canvas","first"},{"x",1},{"y",0},{"width",2},{"height",1},{"scale",2}});
    CHECK(r.success&&r.facts["mode"].toString()=="canvas"&&QImage::fromData(r.image,"PNG").size()==QSize(4,2));
    auto incompatible=call({{"action","view"},{"canvas","first"},{"mode","sheet"},{"x",1},{"width",2}});
    CHECK(!incompatible.success&&incompatible.facts["error"].toString()=="region_requires_canvas_mode");
    const auto observation=r.facts["observation"].toString();
    const auto renders=r.facts["render_count"].toInteger();
    r=call({{"action","view"},{"mode","canvas"},{"canvas","first"},{"x",1},{"y",0},{"width",2},{"height",1},{"scale",2}});
    CHECK(r.success&&r.facts["render_count"].toInteger()==renders&&r.facts["render_cache_hits"].toInteger()>0);
    CHECK(call({{"action","edit"},{"canvas","second"},{"patch","P,3,2,#FF010203"}}).success);
    r=call({{"action","view"},{"mode","canvas"},{"canvas","first"},{"x",1},{"y",0},{"width",2},{"height",1},{"scale",2},{"known_observation",observation}});
    CHECK(r.success&&r.image.isEmpty()&&r.facts["unchanged"].toBool()); // Unrelated edit does not resend.
    r=call({{"action","view"},{"mode","canvas"},{"canvas","first"},{"x",1},{"y",0},{"width",2},{"height",1},{"scale",2},{"known_observation",observation},{"resend_image",true}});CHECK(r.success&&!r.image.isEmpty());
    r=call({{"action","list"},{"changed_since",QJsonValue(qint64(anchor))}});
    CHECK(r.success&&r.facts["returned_count"].toInt()==1&&r.facts["items"].toArray()[0].toObject()["name"].toString()=="second");
    r=pack.call({{"action","list"},{"task_id",7},{"pack_revision",QJsonValue(qint64(anchor))},{"offset",1}},7,output);CHECK(!r.success&&r.facts["error"].toString()=="revision_conflict");
    CHECK(pack.setProjectBrief("Accepted: bounded walking animation",&error));
    CHECK(call({{"action","save"}}).success);
    r=call({{"action","save"}});CHECK(r.success&&r.facts["encoded_images"].toInt()==0&&r.facts["reused_images"].toInt()==3);
    CHECK(ProjectStore::load(output,disk,&error)&&disk.projectBrief=="Accepted: bounded walking animation");
    CHECK(disk.canvases[1].changedRevision==anchor+1);
    CanvasPack seeded;QImage seed(2,2,QImage::Format_ARGB32);seed.fill(0xffaabbcc);seeded.setSourceImage(seed);
    auto seededReply=seeded.call({{"action","pass"},{"task_id",9},{"create_canvases","match,g,2,2|other,g,3,2"}},9,QString());
    CHECK(seededReply.success&&seededReply.facts["seeded_canvases"].toArray()==QJsonArray{QString("match")});
    CHECK(seeded.bundle().canvases[0].image==seed&&seeded.bundle().canvases[1].image.pixel(0,0)==0);
    CanvasPack regionPack;ProjectBundle regionBundle;regionBundle.taskId=10;QImage largeFrame(64,64,QImage::Format_ARGB32);largeFrame.fill(0xff112233);regionBundle.canvases.append({"frame_0799","frames",799,largeFrame});CHECK(regionPack.restore(regionBundle,&error));
    const auto directRegion=regionPack.call({{"action","view"},{"task_id",10},{"canvas","frame_0799"},{"x",8},{"y",8},{"width",8},{"height",8},{"scale",4}},10,QString());
    CHECK(directRegion.success&&QImage::fromData(directRegion.image,"PNG").size()==QSize(32,32));
    const auto fullFrame=regionPack.call({{"action","view"},{"task_id",10},{"canvas","frame_0799"},{"scale",4}},10,QString());
    CHECK(fullFrame.success&&QImage::fromData(fullFrame.image,"PNG").size()==QSize(256,256));
    std::cout<<"CanvasPack detached edits/CAS/history/render/export/restore checks passed\n";return 0;
}
