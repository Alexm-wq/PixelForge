#pragma once
#include "AgentCommands.hpp"
#include "CanvasPack.hpp"
#include "ReferenceStore.hpp"
#include "SessionRecording.hpp"
#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <functional>

namespace pixelforge::qt {
class Canvas;
struct ToolReply { bool success=false; QJsonObject facts; QByteArray image; };

// Same task/controller and exact-pixel semantics as the Windows dynamic bridge.
class AutomationController final : public QObject {
    Q_OBJECT
public:
    explicit AutomationController(Canvas* canvas,QObject* parent=nullptr);
    quint64 begin(const QString& prompt,const QString& outputDirectory);
    void stop(const QString& reason);
    bool acceptReview();
    bool requestChanges(const QString& feedback);
    ToolReply call(const QString& tool,const QJsonObject& arguments);
    QJsonObject state() const;
    const ProjectBundle& project() const { return pack_.bundle(); }
    const QList<quint32>& palette() const {return palette_;}
    QString selectedCanvas() const { return selected_; }
    bool selectCanvas(const QString&, QString* error=nullptr);
    bool saveProject(const QString&,QString* error=nullptr);
    bool loadProject(const QString&,QString* error=nullptr);
    void resetWorkspace();
    void manualChanged();
    bool history(const QString&,QString* error=nullptr);
    bool answerQuestion(const QString& answer);
    bool loadReference(const QString& slot,const QString& path,QString* error=nullptr);
    void clearReference(const QString& slot);
    void setRecordingSource(std::function<QImage()> source){recordingSource_=std::move(source);}
    void setRecordingAllowed(bool allowed){recordingAllowed_=allowed;emit changed();}
    ToolReply recordingControl(const QString& action,int fps=30);
    QJsonObject recordingState() const {return recorder_.status();}
    ToolReply hostPack(QJsonObject arguments,const QString& outputDirectory=QString());
    static QJsonArray tools();
    static QString instructions();
signals:
    void changed();
    void recordingFinished(bool saved,QString error);
    void persistenceFailed(QString error);
private:
    Canvas* canvas_;
    AgentTaskController task_;
    AgentCommandRouter router_;
    QString directory_,selected_;
    CanvasPack pack_;
    ReferenceStore references_;
    SessionRecording recorder_;
    QTimer recordFrames_;
    std::function<QImage()> recordingSource_;
    QSize recordedSize_;
    bool recordingAllowed_=false;
    bool synchronizing_=false;
    QString viewStamp_;
    QByteArray viewImage_;
    ProjectSaveStats lastSave_;
    QMap<QString,QImage> revisionBaseline_;
    bool syncSelected(QString* error=nullptr);
    bool ensurePack(QString* error=nullptr);
    ToolReply packCall(QJsonObject,bool trusted=false);
    quint64 admittedRevision_=0;
    qint64 observedRevision_=-1;
    QList<quint32> palette_{0xff17202e,0xffffffff,0xff8b95a7,0xffeb546c,0xfff4a64c,0xfff9d85c,0xff58c478,0xff32b8b1,0xff4ba6e8,0xff7569e7,0xffc576d7,0xff805742};
    ToolReply reply(const AgentCommandResult& result);
    bool checkpoint(QString* error);
};

// Nonblocking JSONL App Server adapter; no provider call until explicit Generate.
class AutomationClient final : public QObject {
    Q_OBJECT
public:
    explicit AutomationClient(AutomationController* controller,QObject* parent=nullptr);
    ~AutomationClient() override;
    bool start(const QString& prompt,const QString& model,const QString& effort,
               const QString& outputDirectory,const QString& executable=QString());
    void stop();
    void answerQuestion(const QString& answer);
    bool busy() const { return busy_; }
    QString status() const { return status_; }
    QJsonObject evidence() const;
signals:
    void changed();
    void completed(bool success,QString message);
private:
    AutomationController* controller_;
    QProcess process_;
    QMetaObject::Connection startedConnection_;
    QTemporaryDir scratch_;
    QTimer negotiation_,killTimer_;
    QByteArray buffer_;
    QHash<qint64,std::function<void(QJsonObject)>> pending_;
    QHash<QString,QJsonObject> receipts_;
    QHash<QString,QByteArray> callHashes_;
    qint64 receiptBytes_=0;
    qint64 next_=1;
    bool busy_=false,stopping_=false,ended_=false;
    QString status_,model_,effort_,prompt_,thread_,turn_,directory_;
    QJsonArray events_;
    QJsonObject heldQuestion_;
    QByteArray heldHash_;
    void completeTool(QJsonObject request,const QByteArray& hash,ToolReply reply);
    void send(QJsonObject message);
    void request(const QString& method,QJsonObject params,std::function<void(QJsonObject)> done);
    void read();
    void message(QJsonObject message);
    void beginThread();
    void finish(bool success,const QString& reason);
    void retireProcess();
    void setStatus(const QString& status);
    void record(QJsonObject event);
};
}
