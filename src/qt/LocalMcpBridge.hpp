#pragma once
#include "Automation.hpp"
#include <QLocalServer>
#include <QJsonObject>
#include <QPointer>
#include <QLocalSocket>
#include <functional>
namespace pixelforge::qt {
// Local user-only Qt IPC replaces the Windows pipe; never listens on TCP.
class LocalMcpBridge final:public QObject {
    Q_OBJECT
public:
    LocalMcpBridge(AutomationController*,std::function<bool()> busy,QObject* parent=nullptr);
    bool start(QString* error=nullptr);
    void stop();
    void answerQuestion(const QString&);
    bool awaitingAnswer() const {return !questionSocket_.isNull();}
    QString name() const {return server_.serverName();}
    bool enabled() const {return server_.isListening();}
    static int forwardStdio(const QString& socket);
private:
    AutomationController* controller_;
    std::function<bool()> busy_;
    QLocalServer server_;
    QPointer<QLocalSocket> questionSocket_;
    QJsonValue questionId_;
    QLocalSocket* currentSocket_=nullptr;
    QJsonObject dispatch(const QJsonObject&);
};
}
