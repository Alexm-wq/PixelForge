#include "LocalMcpBridge.hpp"
#include "Editor.hpp"
#include <QtTest>
#include <QJsonDocument>
#include <QTemporaryDir>
using namespace pixelforge::qt;
class McpTest final:public QObject{
    Q_OBJECT
private slots:
    void actualUserLocalTransport(){
        QTemporaryDir folder;qputenv("XDG_DATA_HOME",folder.path().toUtf8());Canvas canvas;AutomationController control(&canvas);bool busy=false;LocalMcpBridge bridge(&control,[&]{return busy;});QString why;QVERIFY2(bridge.start(&why),qPrintable(why));
        QLocalSocket peer;peer.connectToServer(bridge.name());QTRY_COMPARE(peer.state(),QLocalSocket::ConnectedState);int id=0;
        auto call=[&](QString method,QJsonObject params){const int requestId=++id;peer.write(QJsonDocument(QJsonObject{{"jsonrpc","2.0"},{"id",requestId},{"method",method},{"params",params}}).toJson(QJsonDocument::Compact)+'\n');QByteArray data;QElapsedTimer timer;timer.start();while(!data.contains('\n')&&timer.elapsed()<3000){QCoreApplication::processEvents();data+=peer.readAll();QTest::qWait(1);}return QJsonDocument::fromJson(data).object();};
        QVERIFY(call("initialize",{})["result"].toObject().contains("capabilities"));QVERIFY(call("tools/list",{})["result"].toObject()["tools"].toArray().size()>=10);
        auto tool=[&](QString name,QJsonObject args){return call("tools/call",{{"name",name},{"arguments",args}});};
        auto r=tool("pixelforge_task",{{"action","begin"},{"prompt","Offline external MCP fixture"}});QVERIFY(!r.contains("error"));
        auto facts=QJsonDocument::fromJson(r["result"].toObject()["content"].toArray()[0].toObject()["text"].toString().toUtf8()).object();const auto task=facts["task_id"];
        r=tool("pixelforge_task",{{"action","accept"},{"task_id",task},{"width",16},{"height",16}});QVERIFY(!r["result"].toObject()["isError"].toBool());
        busy=true;r=tool("pixelforge_edit",{{"task_id",task},{"expected_revision",qint64(canvas.document().revision())},{"patch","P,1,1,#FF112233"}});QVERIFY(r.contains("error"));QCOMPARE(canvas.image().pixel(1,1),0u);busy=false;
        r=tool("pixelforge_edit",{{"task_id",task},{"expected_revision",qint64(canvas.document().revision())},{"patch","P,1,1,#FF112233"}});QVERIFY(!r["result"].toObject()["isError"].toBool());QCOMPARE(canvas.image().pixel(1,1),0xff112233u);
        r=tool("pixelforge_view",{{"action","render"},{"task_id",task},{"expected_revision",qint64(canvas.document().revision())}});QCOMPARE(r["result"].toObject()["content"].toArray()[1].toObject()["mimeType"].toString(),"image/png");
        bridge.stop();QTRY_VERIFY(!bridge.enabled());peer.abort();
    }
};
QTEST_MAIN(McpTest)
#include "qt_mcp_test.moc"
