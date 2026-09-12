#include "Editor.hpp"
#include "Automation.hpp"
#include "../src/win32/PixelProgram.hpp"
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
using namespace pixelforge::qt;
class AutomationTest final:public QObject {
    Q_OBJECT
private slots:
    void cleanup(){qunsetenv("PIXELFORGE_TEST_HOLD");qunsetenv("PIXELFORGE_TEST_QUESTION");}
    void guardedControllerAndImages() {
        QTemporaryDir folder;Canvas canvas;AutomationController control(&canvas);
        const auto id=control.begin("Synthetic fixture",folder.path());
        auto get=control.call("pixelforge_task",{{"action","get"}});QVERIFY(get.success);
        auto accept=control.call("pixelforge_task",{{"action","accept"},{"task_id",qint64(id)},{"width",16},{"height",16}});QVERIFY(accept.success);
        const auto rev=canvas.document().revision();
        auto bound=[&](QString action=QString()){QJsonObject o{{"task_id",qint64(id)},{"expected_revision",qint64(canvas.document().revision())}};if(!action.isEmpty())o["action"]=action;return o;};
        auto bad=bound();bad["patch"]="P,1,1,#FFFFFFFF;R,15,15,5,5,#FF000000";
        QVERIFY(!control.call("pixelforge_edit",bad).success);QCOMPARE(canvas.document().revision(),rev);QCOMPARE(canvas.document().pixel(1,1),0u);
        bad=bound();bad["program"]="R 2 2 5 4 #FFEB546C";auto applied=control.call("pixelforge_program",bad);QVERIFY2(applied.success,qPrintable(QJsonDocument(applied.facts).toJson()));QCOMPARE(canvas.document().pixel(3,3),0xffeb546cu);
        QVERIFY(!control.call("pixelforge_program",bad).success); // old revision
        auto end=bound("finish");end["summary"]="fixture";QVERIFY(!control.call("pixelforge_task",end).success);
        auto render=bound("render");render["scale"]=4;const auto image=control.call("pixelforge_view",render);QVERIFY(image.success);QCOMPARE(QImage::fromData(image.image).size(),QSize(64,64));
        end=bound("finish");end["summary"]="fixture";QVERIFY(control.call("pixelforge_task",end).success);QVERIFY(control.state()["awaiting_user_review"].toBool());
        QVERIFY(control.acceptReview());QVERIFY(!control.state()["awaiting_user_review"].toBool());
        QCOMPARE(QImage(folder.filePath("canvas.png")).convertToFormat(QImage::Format_ARGB32),canvas.image());
    }
    void multiCanvasControllerPersistenceAndRevision() {
        QTemporaryDir folder;Canvas canvas;AutomationController control(&canvas);const auto id=control.begin("two synthetic frames",folder.path());
        QVERIFY(control.call("pixelforge_task",{{"action","accept"},{"task_id",qint64(id)},{"width",16},{"height",16}}).success);
        auto call=[&](QJsonObject a){a["task_id"]=qint64(id);a["pack_revision"]=control.state()["pack_revision"];return control.call("pixelforge_pack",a);};
        auto r=call({{"action","create"},{"canvases","one,walk,16,16,0|two,walk,16,16,1"}});QVERIFY2(r.success,qPrintable(QJsonDocument(r.facts).toJson()));
        r=call({{"action","pass"},{"program","CANVAS one;P 1 1 #FF112233;CANVAS two;P 2 2 #FF445566"},{"render_mode","strip"}});QVERIFY2(r.success,qPrintable(QJsonDocument(r.facts).toJson()));QCOMPARE(control.project().canvases.size(),2);
        QVERIFY(control.selectCanvas("two"));QCOMPARE(canvas.image().pixel(2,2),0xff445566u);
        auto edit=QJsonObject{{"task_id",qint64(id)},{"expected_revision",qint64(canvas.document().revision())},{"patch","P,3,3,#FFAABBCC"}};
        QVERIFY(control.call("pixelforge_edit",edit).success);QCOMPARE(control.project().canvases[1].image.pixel(3,3),0xffaabbccu);
        QVERIFY(control.history("undo"));QCOMPARE(canvas.image().pixel(3,3),0u);QVERIFY(control.history("redo"));
        QVERIFY(control.saveProject(folder.path()));Canvas other;AutomationController restored(&other);QString why;QVERIFY2(restored.loadProject(folder.filePath("project.json"),&why),qPrintable(why));
        QCOMPARE(restored.state()["task_id"].toInteger(),qint64(id));QVERIFY(restored.selectCanvas("two"));QCOMPARE(other.image(),canvas.image());
        const auto retained=restored.project();QVERIFY(!restored.loadProject(folder.filePath("missing.json"),&why));QCOMPARE(restored.project().canvases[1].image,retained.canvases[1].image);
        r=call({{"action","export"},{"scope","animation"},{"group","walk"}});QVERIFY2(r.success,qPrintable(QJsonDocument(r.facts).toJson()));QVERIFY(QFileInfo::exists(folder.filePath("animation.gif")));
        QVERIFY(call({{"action","view"},{"mode","sheet"}}).success);auto strip=call({{"action","view"},{"mode","animation"},{"group","walk"}});QVERIFY(strip.success);QVERIFY(strip.image.startsWith("\x89PNG"));QCOMPARE(strip.facts["agent_observation"].toString(),"ordered_frame_strip");QVERIFY(!strip.facts["animation_preview_generated"].toBool());QCOMPARE(QImage::fromData(strip.image).size(),QSize(32,16));
        QVERIFY(control.call("pixelforge_task",{{"action","finish"},{"task_id",qint64(id)},{"expected_revision",qint64(canvas.document().revision())},{"summary","PROJECT_NAME: Frames"}}).success);
        QVERIFY(control.requestChanges("Adjust one corner"));const auto resumed=control.begin("Adjust one corner",folder.path());QCOMPARE(resumed,id);
        QVERIFY(!call({{"action","create"},{"canvases","replacement,,16,16,-1"}}).success);QCOMPARE(control.project().canvases.size(),2);
    }
    void referenceSeedMirrorsSelectedPackWithoutEditorSignals(){
        QTemporaryDir folder;Canvas canvas;AutomationController control(&canvas);const auto id=control.begin("Two reference frames",folder.path());
        QVERIFY(control.call("pixelforge_task",{{"action","accept"},{"task_id",qint64(id)}}).success);
        QVERIFY(control.call("pixelforge_pack",{{"action","create"},{"task_id",qint64(id)},{"canvases","one,,16,16,-1|two,,16,16,-1"}}).success);
        QVERIFY(control.selectCanvas("two"));QImage ref(16,16,QImage::Format_ARGB32);ref.fill(0);ref.setPixel(2,2,0x80ff2244);QVERIFY(ref.save(folder.filePath("source.png")));QString why;QVERIFY(control.loadReference("source",folder.filePath("source.png"),&why));
        const auto r=control.call("pixelforge_reference",{{"action","seed"},{"reference","source"},{"task_id",qint64(id)},{"expected_revision",qint64(canvas.document().revision())}});QVERIFY(r.success);QCOMPARE(control.project().canvases[1].image,ref);QCOMPARE(control.project().canvases[0].image.pixel(2,2),0u);
        ProjectBundle restored;QVERIFY(ProjectStore::load(folder.path(),restored,&why));QCOMPARE(restored.canvases[1].image,ref);
    }
    void legacyManifestAssetsSurviveCheckpointFailure() {
        QTemporaryDir folder;QImage first(4,4,QImage::Format_ARGB32),second=first;first.fill(0xff112233);second.fill(0xff445566);
        QVERIFY(first.save(folder.filePath("canvas.png")));QVERIFY(second.save(folder.filePath("second.png")));
        QFile manifest(folder.filePath("project.json"));QVERIFY(manifest.open(QIODevice::WriteOnly));manifest.write(R"({"version":1,"task_id":77,"pack_revision":0,"canvases":[{"name":"first","group":"","frame":-1,"width":4,"height":4,"file":"canvas.png"},{"name":"second","group":"","frame":-1,"width":4,"height":4,"file":"second.png"}]})");manifest.close();
        Canvas canvas;AutomationController control(&canvas);QString why;QVERIFY2(control.loadProject(folder.path(),&why),qPrintable(why));QVERIFY(control.selectCanvas("second"));
        // Agent exports cannot replace a PNG referenced by the current legacy manifest.
        auto io=[&](const QString& name){return control.call("pixelforge_io",{{"action","export"},{"task_id",77},{"expected_revision",qint64(canvas.document().revision())},{"path",name}});};
        QVERIFY(!io("canvas.png").success);QVERIFY(!io("CON.png").success);
        auto denied=control.call("pixelforge_pack",{{"action","export"},{"task_id",77},{"canvas","second"},{"scope","canvas"},{"path","canvas.png"}});QVERIFY(!denied.success);QCOMPARE(denied.facts["error"].toString(),"export_exists");
        QCOMPARE(QImage(folder.filePath("canvas.png")).convertToFormat(QImage::Format_ARGB32),first);QVERIFY(io("new-second.png").success);QCOMPARE(QImage(folder.filePath("new-second.png")).convertToFormat(QImage::Format_ARGB32),second);
        QVERIFY(!io("new-second.png").success);QVERIFY(QFile::link(folder.filePath("canvas.png"),folder.filePath("linked.png")));QVERIFY(!io("linked.png").success);QCOMPARE(QImage(folder.filePath("canvas.png")).convertToFormat(QImage::Format_ARGB32),first);
        QVERIFY(first.save(folder.filePath("explicit-ui-copy.png")));auto trusted=control.hostPack({{"action","export"},{"canvas","second"},{"scope","canvas"},{"path","explicit-ui-copy.png"}},folder.path());QVERIFY(trusted.success);QCOMPARE(QImage(folder.filePath("explicit-ui-copy.png")).convertToFormat(QImage::Format_ARGB32),second);
        // A occupied manifest destination forces atomic publication refusal.
        QVERIFY(QFile::rename(folder.filePath("project.json"),folder.filePath("prior.json")));QVERIFY(QDir().mkdir(folder.filePath("project.json")));
        auto r=control.call("pixelforge_edit",{{"task_id",77},{"expected_revision",qint64(canvas.document().revision())},{"patch","P,0,0,#FFAABBCC"}});
        QVERIFY(r.success);QVERIFY(r.facts.contains("persistence_error"));QCOMPARE(canvas.image().pixel(0,0),0xffaabbccu);
        QCOMPARE(QImage(folder.filePath("canvas.png")).convertToFormat(QImage::Format_ARGB32),first);
        QCOMPARE(QImage(folder.filePath("second.png")).convertToFormat(QImage::Format_ARGB32),second);QSignalSpy persistence(&control,&AutomationController::persistenceFailed);control.manualChanged();QCOMPARE(persistence.size(),1);
    }
    void generatedCompilerAndHostCapacity(){
        QTemporaryDir folder;Canvas canvas;AutomationController control(&canvas);const auto id=control.begin("Large artistic pass",folder.path());
        QVERIFY(control.call("pixelforge_task",{{"action","accept"},{"task_id",qint64(id)},{"width",160},{"height",160}}).success);
        auto bound=[&]{return QJsonObject{{"task_id",qint64(id)},{"expected_revision",qint64(canvas.document().revision())}};};
        QStringList colors;for(int i=0;i<512;++i)colors<<QString("%1").arg(0xff000000u+quint32(i),8,16,QChar('0'));auto palette=bound();palette["action"]="set";palette["colors"]=colors.join(',');QVERIFY(control.call("pixelforge_palette",palette).success);QCOMPARE(control.palette().size(),512);
        QString program;for(int i=0;i<70;++i)program+=QString("MASKRECT m%1 0 0 160 160;").arg(i);const QString longName(80,'a');program+="MASKRECT "+longName+" 0 0 160 160;CLUSTERS m0 300 0.01 257 257 3;FILLMASK "+longName+" 300;DITHER "+longName+" 300 301 0.5 CHECKER 0";
        const auto compiled=pixelforge::win32::compile_pixel_program(program.toStdString(),160,160,std::vector<std::uint32_t>(160*160));QVERIFY2(compiled.ok,compiled.error.c_str());QVERIFY(compiled.patch_operations>20000);
        auto args=bound();args["program"]=program;auto result=control.call("pixelforge_program",args);QVERIFY2(result.success,qPrintable(QJsonDocument(result.facts).toJson()));QVERIFY(canvas.image().pixel(0,0)!=canvas.image().pixel(1,0));QVERIFY(canvas.image().pixel(0,0)==0xff00012cu||canvas.image().pixel(0,0)==0xff00012du);
        QVERIFY(control.call("pixelforge_pack",{{"action","create"},{"task_id",qint64(id)},{"canvases","named,,160,160,-1"}}).success);
        result=control.call("pixelforge_pack",{{"action","program"},{"task_id",qint64(id)},{"canvas","named"},{"program",program}});QVERIFY2(result.success,qPrintable(QJsonDocument(result.facts).toJson()));QVERIFY(control.project().canvases[0].image.pixel(0,0)!=control.project().canvases[0].image.pixel(1,0));
        const auto retained=canvas.image();const auto rev=canvas.document().revision();args=bound();args["patch"]=QString("R,0,0,160,160,300;").repeated(3000);result=control.call("pixelforge_edit",args);QVERIFY(!result.success);QVERIFY(result.facts["error"].toString().contains("work budget"));QCOMPARE(canvas.document().revision(),rev);QCOMPARE(canvas.image(),retained);
        args=bound();args["program"]="COPY 0 0 2147483647 2147483647 0 0";result=control.call("pixelforge_program",args);QVERIFY(!result.success);QCOMPARE(canvas.image(),retained);
        const auto excess=pixelforge::win32::compile_pixel_program("MASKRECT m 0 0 514 514;FILLMASK m 300;DITHER m 300 301 0.5 CHECKER 0",514,514,std::vector<std::uint32_t>(514*514));QVERIFY(!excess.ok);QVERIFY(QString::fromStdString(excess.error).contains("expanded patch budget"));
        args=bound();args["program"]="MASKRECT noop 0 0 1 1";result=control.call("pixelforge_program",args);QVERIFY2(result.success,qPrintable(QJsonDocument(result.facts).toJson()));QCOMPARE(canvas.document().revision(),rev);
        qInfo()<<"capacity:70 masks,80-character mask name,palette512/index301,cluster257,compiled operations"<<compiled.patch_operations<<"; early copy/work/operation refusals";
    }
    void actualDeferredQuestionSameTurn() {
        qputenv("PIXELFORGE_TEST_QUESTION","1");QTemporaryDir folder;Canvas canvas;AutomationController control(&canvas);AutomationClient client(&control);QSignalSpy complete(&client,&AutomationClient::completed);
        QVERIFY(client.start("Question fixture", "gpt-6-astra","medium",folder.path(),QStringLiteral(PIXELFORGE_FAKE_SERVER)));
        QTRY_VERIFY_WITH_TIMEOUT(control.state()["awaiting_user_input"].toBool(),10000);QVERIFY(client.busy());QCOMPARE(complete.size(),0);
        client.answerQuestion("Keep a transparent background");QTRY_COMPARE_WITH_TIMEOUT(complete.size(),1,10000);QVERIFY2(complete[0][0].toBool(),qPrintable(client.status()));QTRY_VERIFY_WITH_TIMEOUT(!client.busy(),5000);
        bool answer=false;int turns=0;for(const auto& e:client.evidence()["events"].toArray()){const auto o=e.toObject();turns+=o["method"]=="turn/start";if(o["tool"]=="pixelforge_dialog")answer=o["result"].toObject()["answer"]=="Keep a transparent background";}QVERIFY(answer);QCOMPARE(turns,1);qunsetenv("PIXELFORGE_TEST_QUESTION");
    }
    void actualJsonlTransport() {
        QTemporaryDir folder;Canvas canvas;AutomationController control(&canvas);AutomationClient client(&control);
        QSignalSpy complete(&client,&AutomationClient::completed);
        QVERIFY(client.start("Explicit scripted peer fixture", "gpt-6-astra","medium",folder.path(),QStringLiteral(PIXELFORGE_FAKE_SERVER)));
        QTRY_COMPARE_WITH_TIMEOUT(complete.size(),1,15000);QVERIFY2(complete[0][0].toBool(),qPrintable(client.status()));QTRY_VERIFY_WITH_TIMEOUT(!client.busy(),5000);
        QCOMPARE(canvas.document().pixel(2,2),0xffeb546cu);QCOMPARE(canvas.document().pixel(12,8),0xff32b8b1u);
        QVERIFY(control.state()["awaiting_user_review"].toBool());
        QFile receipt(folder.filePath("automation.json"));QVERIFY(receipt.open(QIODevice::ReadOnly));const auto facts=QJsonDocument::fromJson(receipt.readAll()).object();
        QCOMPARE(facts["thread_id"].toString(),"synthetic-thread");QCOMPARE(facts["turn_id"].toString(),"synthetic-turn");
        bool refused=false,rendered=false;for(const auto& e:facts["events"].toArray()){const auto o=e.toObject();if(o["tool"]=="pixelforge_edit"&&!o["success"].toBool())refused=true;if(o["image_bytes"].toInt()>0)rendered=true;}QVERIFY(refused&&rendered);
    }
    void failedExecutableThenSingleCleanNegotiation(){
        QTemporaryDir folder;Canvas canvas;AutomationController control(&canvas);AutomationClient client(&control);QSignalSpy complete(&client,&AutomationClient::completed);
        QVERIFY(client.start("Failed executable fixture","gpt-6-astra","medium",folder.path(),folder.filePath("missing-executable")));
        QTRY_COMPARE_WITH_TIMEOUT(complete.size(),1,5000);QTRY_VERIFY_WITH_TIMEOUT(!client.busy(),5000);
        QVERIFY(client.start("Clean offline recovery","gpt-6-astra","medium",folder.path(),QStringLiteral(PIXELFORGE_FAKE_SERVER)));
        QTRY_COMPARE_WITH_TIMEOUT(complete.size(),2,10000);QVERIFY2(complete[1][0].toBool(),qPrintable(client.status()));QTRY_VERIFY_WITH_TIMEOUT(!client.busy(),5000);
        int initializes=0,turns=0;for(const auto& e:client.evidence()["events"].toArray()){initializes+=e.toObject()["method"]=="initialize";turns+=e.toObject()["method"]=="turn/start";}QCOMPARE(initializes,1);QCOMPARE(turns,1);
    }
    void stopRetainsCommittedPixelsAndRejectsLateCall() {
        qputenv("PIXELFORGE_TEST_HOLD","1");QTemporaryDir folder;Canvas canvas;AutomationController control(&canvas);AutomationClient client(&control);QSignalSpy complete(&client,&AutomationClient::completed);
        QVERIFY(client.start("Explicit Stop fixture", "gpt-6-astra","medium",folder.path(),QStringLiteral(PIXELFORGE_FAKE_SERVER)));
        QTRY_VERIFY_WITH_TIMEOUT(canvas.document().pixel(2,2)==0xffeb546cu,15000);
        client.stop();QTRY_COMPARE_WITH_TIMEOUT(complete.size(),1,8000);QVERIFY(!complete[0][0].toBool());QTRY_VERIFY_WITH_TIMEOUT(!client.busy(),5000);
        QCOMPARE(canvas.document().pixel(2,2),0xffeb546cu);QCOMPARE(canvas.document().pixel(0,0),0u);QCOMPARE(control.state()["state"].toString(),"ABORTED");
        qunsetenv("PIXELFORGE_TEST_HOLD");
    }
};
QTEST_MAIN(AutomationTest)
#include "qt_automation_test.moc"
