#include "Automation.hpp"
#include "Editor.hpp"
#include <QtTest>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QElapsedTimer>
#include <QFile>
using namespace pixelforge::qt;
class LargeProjectTest final:public QObject{
    Q_OBJECT
private slots:
    void actual1600FrameControllerNavigationAndDeltaSave(){
        QElapsedTimer elapsed;elapsed.start();QTemporaryDir folder;ProjectBundle project;project.taskId=16001;project.packRevision=1;project.projectName="Synthetic movement library";project.projectBrief="Supplied synthetic1600frame fixture: walk and run groups; preserve identities and request targeted regions.";
        for(int i=0;i<1600;++i){QImage image(64,64,QImage::Format_ARGB32);image.fill(0);image.setPixel(i%64,(i/64)%64,0xff000000u|quint32(i+1));project.canvases.append({QString("frame_%1").arg(i,4,10,QChar('0')),i<800?"walk":"run",i%800,image,1});}
        QString why;ProjectSaveStats initial;QVERIFY2(ProjectStore::save(folder.path(),project,&why,&initial),qPrintable(why));QCOMPARE(initial.encodedImages,1600);
        Canvas canvas;AutomationController control(&canvas);QVERIFY2(control.loadProject(folder.path(),&why),qPrintable(why));
        const auto overview=control.call("pixelforge_task",{{"action","get"}});const auto overviewBytes=QJsonDocument(overview.facts).toJson(QJsonDocument::Compact).size();QVERIFY(overviewBytes<=4096);QCOMPARE(control.project().canvases.size(),1600);
        auto call=[&](QJsonObject a){a["task_id"]=16001;return control.call("pixelforge_pack",a);};
        QSet<QString> names;int offset=0,maxPage=0,pages=0;const QJsonValue anchor=control.state().value("pack_revision");
        do {const auto r=call({{"action","list"},{"limit",24},{"offset",offset},{"pack_revision",anchor}});QVERIFY2(r.success,qPrintable(QJsonDocument(r.facts).toJson()));QVERIFY(r.image.isEmpty());const auto bytes=QJsonDocument(r.facts).toJson(QJsonDocument::Compact).size();maxPage=std::max(maxPage,int(bytes));QVERIFY(bytes<=8192);for(const auto& item:r.facts["items"].toArray())names.insert(item.toObject()["name"].toString());offset=r.facts["next_offset"].toInt(-1);++pages;QVERIFY(pages<80);}while(offset>=0);
        QCOMPARE(names.size(),1600);auto broad=call({{"action","index"}});QVERIFY(broad.success);QCOMPARE(broad.facts["canvas_count"].toInt(),1600);auto rich=call({{"action","list"},{"limit",2},{"format","legacy"}});QVERIFY(rich.success);QVERIFY(rich.facts.contains("rows")||rich.facts.contains("canvases"));QVERIFY(!call({{"action","list"},{"offset",24},{"limit",24}}).success);
        auto a=call({{"action","analyze"},{"canvas","frame_0799"}});QVERIFY(a.success);auto cached=call({{"action","analyze"},{"canvas","frame_0799"}});QVERIFY(cached.success);QCOMPARE(cached.facts["scanned_frames"].toInt(),0);QCOMPARE(cached.facts["compared_pairs"].toInt(),0);
        const QJsonObject view{{"action","view"},{"canvas","frame_0799"},{"x",8},{"y",8},{"width",8},{"height",8},{"scale",4}};a=call(view);QVERIFY(a.success);QCOMPARE(QImage::fromData(a.image).size(),QSize(32,32));auto known=view;known["known_observation"]=a.facts["observation"];cached=call(known);QVERIFY(cached.success);QVERIFY(cached.image.isEmpty());auto fresh=known;fresh["resend_image"]=true;auto resent=call(fresh);QVERIFY(resent.success);QVERIFY(!resent.image.isEmpty());auto full=call({{"action","view"},{"canvas","frame_0799"}});QVERIFY(full.success);QCOMPARE(QImage::fromData(full.image).size(),QSize(64,64));
        QVERIFY2(control.saveProject(folder.path(),&why),qPrintable(why));QCOMPARE(control.state()["last_save"].toObject()["encoded_images"].toInt(),0);
        a=call({{"action","edit"},{"canvas","frame_0001"},{"patch","P,5,5,#FFAABBCC"},{"pack_revision",anchor}});QVERIFY2(a.success,qPrintable(QJsonDocument(a.facts).toJson()));QCOMPARE(control.state()["last_save"].toObject()["encoded_images"].toInt(),1);QCOMPARE(control.state()["last_save"].toObject()["reused_images"].toInt(),1599);
        QVERIFY(!call({{"action","list"},{"offset",24},{"limit",24},{"pack_revision",anchor}}).success);
        a=call({{"action","list"},{"changed_since",anchor},{"limit",24},{"pack_revision",control.state()["pack_revision"]}});QVERIFY(a.success);QCOMPARE(a.facts["items"].toArray().size(),1);QCOMPARE(a.facts["items"].toArray()[0].toObject()["name"].toString(),"frame_0001");
        cached=call(known);QVERIFY(cached.success);QVERIFY(cached.image.isEmpty()); // unrelated edit does not invalidate selected content identity
        ProjectBundle reopened;QVERIFY2(ProjectStore::load(folder.path(),reopened,&why),qPrintable(why));QCOMPARE(reopened.canvases.size(),1600);QCOMPARE(reopened.projectBrief,project.projectBrief);QCOMPARE(reopened.canvases[1].image.pixel(5,5),0xffaabbccu);
        const QJsonObject result{{"frames",1600},{"frame_size","64x64"},{"overview_bytes",overviewBytes},{"max_page_bytes",maxPage},{"pages",pages},{"complete_unique_ids",names.size()},{"repeat_scanned_frames",0},{"repeat_compared_pairs",0},{"one_edit_encoded",1},{"one_edit_reused",1599},{"elapsed_ms",elapsed.elapsed()},{"provider_calls",0},{"eager_load",true}};
        qInfo().noquote()<<QJsonDocument(result).toJson(QJsonDocument::Compact);const auto evidence=qEnvironmentVariable("PIXELFORGE_LARGE_RESULT");if(!evidence.isEmpty()){QFile file(evidence);QVERIFY(file.open(QIODevice::WriteOnly));file.write(QJsonDocument(result).toJson());}
    }
};
QTEST_MAIN(LargeProjectTest)
#include "qt_large_project_test.moc"
