#include "LocalMcpBridge.hpp"
#include <QLocalSocket>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QDir>
#include <QUuid>
#include <QCoreApplication>
#include <iostream>
namespace pixelforge::qt {
namespace { constexpr int MaxMessage=4*1024*1024; }
LocalMcpBridge::LocalMcpBridge(AutomationController* c,std::function<bool()> busy,QObject* parent):QObject(parent),controller_(c),busy_(std::move(busy)) {
    connect(controller_,&AutomationController::recordingFinished,this,[this](bool saved,const QString&){
        if(!questionSocket_||controller_->state()["awaiting_user_input"].toBool())return;
        const QJsonObject response{{"jsonrpc","2.0"},{"id",questionId_},{"result",QJsonObject{{"isError",!saved},{"content",QJsonArray{QJsonObject{{"type","text"},{"text",QString::fromUtf8(QJsonDocument(controller_->recordingState()).toJson(QJsonDocument::Compact))}}}}}}};
        questionSocket_->write(QJsonDocument(response).toJson(QJsonDocument::Compact)+'\n');questionSocket_.clear();questionId_={};
    });
    server_.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&server_,&QLocalServer::newConnection,this,[this]{
        while(auto* socket=server_.nextPendingConnection()){
            socket->setReadBufferSize(MaxMessage+1);auto* buffer=new QByteArray;
            connect(socket,&QLocalSocket::disconnected,socket,&QObject::deleteLater);
            connect(socket,&QObject::destroyed,this,[buffer]{delete buffer;});
            connect(socket,&QLocalSocket::readyRead,this,[this,socket,buffer]{
                *buffer+=socket->readAll();if(buffer->size()>MaxMessage){socket->abort();return;}
                int count=0;while(buffer->contains('\n')&&count++<16){const int at=buffer->indexOf('\n');const auto line=buffer->left(at);buffer->remove(0,at+1);
                    QJsonParseError why;const auto message=QJsonDocument::fromJson(line,&why);
                    if(why.error!=QJsonParseError::NoError||!message.isObject()){socket->abort();return;}
                    const auto request=message.object();if(!request.contains("id"))continue;
                    currentSocket_=socket;const auto result=dispatch(request);currentSocket_=nullptr;if(result.isEmpty())continue;const auto bytes=QJsonDocument(result).toJson(QJsonDocument::Compact)+'\n';
                    if(bytes.size()>32*1024*1024||socket->bytesToWrite()>32*1024*1024){socket->abort();return;}
                    socket->write(bytes);
                }
                if(buffer->contains('\n'))QMetaObject::invokeMethod(socket,"readyRead",Qt::QueuedConnection);
            });
        }
    });
}
bool LocalMcpBridge::start(QString* why){
    if(server_.isListening())return true;
    const auto name="pixelforge-"+QString::number(QCoreApplication::applicationPid())+"-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
    if(!server_.listen(name)){if(why)*why=server_.errorString();return false;}return true;
}
void LocalMcpBridge::stop(){if(questionSocket_)controller_->stop("External MCP disconnected; pending question cancelled.");questionSocket_.clear();for(auto* c:server_.findChildren<QLocalSocket*>())c->abort();server_.close();}
QJsonObject LocalMcpBridge::dispatch(const QJsonObject& request){
    const auto id=request["id"];const auto method=request["method"].toString();const auto params=request["params"].toObject();
    auto ok=[&](QJsonObject r){return QJsonObject{{"jsonrpc","2.0"},{"id",id},{"result",r}};};
    auto fail=[&](int code,QString message){return QJsonObject{{"jsonrpc","2.0"},{"id",id},{"error",QJsonObject{{"code",code},{"message",message}}}};};
    if(method=="initialize")return ok({{"protocolVersion","2024-11-05"},{"capabilities",QJsonObject{{"tools",QJsonObject{}}}},{"serverInfo",QJsonObject{{"name","PixelForge Qt"},{"version","0.4.2"}}}});
    if(method=="ping")return ok({});
    if(method=="tools/list"){
        auto tools=AutomationController::tools();for(int i=0;i<tools.size();++i){auto t=tools[i].toObject();if(t["name"]=="pixelforge_task"){auto schema=t["inputSchema"].toObject();auto props=schema["properties"].toObject();auto action=props["action"].toObject();auto values=action["enum"].toArray();values.append("begin");action["enum"]=values;props["action"]=action;props["prompt"]=QJsonObject{{"type","string"}};schema["properties"]=props;t["inputSchema"]=schema;tools[i]=t;}}
        return ok({{"tools",tools}});
    }
    if(method!="tools/call")return fail(-32601,"Unknown MCP method.");
    if(questionSocket_)return fail(-32001,"Answer or cancel the original pending user question first.");
    if(busy_())return fail(-32001,"The original in-app agent turn owns this document; Stop it before external MCP edits.");
    const auto tool=params["name"].toString();if(!params["arguments"].isObject())return fail(-32602,"Expected tool arguments object.");
    auto args=params["arguments"].toObject();ToolReply reply;
    if(tool=="pixelforge_task"&&args["action"]=="begin"){
        const auto prompt=args["prompt"].toString();if(prompt.trimmed().isEmpty()||prompt.size()>16000)return fail(-32602,"Provide a bounded prompt.");
        const auto dir=QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)+"/manual-mcp/"+QUuid::createUuid().toString(QUuid::WithoutBraces);
        if(!QDir().mkpath(dir))return fail(-32002,"Cannot create project directory.");
        if(!controller_->begin(prompt,dir))return fail(-32001,"Resolve the current review or question before a new task.");reply={true,controller_->state(),{}};
    }else reply=controller_->call(tool,args);
    if(reply.success&&(reply.facts["pending_user_answer"].toBool()||reply.facts["pending_recording"].toBool())){questionSocket_=currentSocket_;questionId_=id;QMetaObject::invokeMethod(controller_,"changed",Qt::QueuedConnection);return {};}
    QJsonArray content{QJsonObject{{"type","text"},{"text",QString::fromUtf8(QJsonDocument(reply.facts).toJson(QJsonDocument::Compact))}}};
    if(!reply.image.isEmpty())content.append(QJsonObject{{"type","image"},{"mimeType",reply.image.startsWith("GIF")?"image/gif":"image/png"},{"data",QString::fromLatin1(reply.image.toBase64())}});
    return ok({{"isError",!reply.success},{"content",content}});
}
void LocalMcpBridge::answerQuestion(const QString& answer){
    if(!questionSocket_||!controller_->answerQuestion(answer))return;
    auto facts=controller_->state();facts["answer"]=answer;facts["ok"]=true;
    const QJsonObject result{{"jsonrpc","2.0"},{"id",questionId_},{"result",QJsonObject{{"isError",false},{"content",QJsonArray{QJsonObject{{"type","text"},{"text",QString::fromUtf8(QJsonDocument(facts).toJson(QJsonDocument::Compact))}}}}}}};
    questionSocket_->write(QJsonDocument(result).toJson(QJsonDocument::Compact)+'\n');questionSocket_.clear();questionId_={};
}
int LocalMcpBridge::forwardStdio(const QString& name){
    QLocalSocket socket;socket.connectToServer(name);if(!socket.waitForConnected(5000)){std::cerr<<"PixelForge local bridge unavailable; enable it in the editor.\n";return 2;}
    std::string line;QByteArray reply;
    while(std::getline(std::cin,line)){
        if(line.size()>MaxMessage)return 3;const auto request=QJsonDocument::fromJson(QByteArray::fromStdString(line));if(!request.isObject())return 3;
        const auto bytes=QByteArray::fromStdString(line)+'\n';socket.write(bytes);while(socket.bytesToWrite())if(!socket.waitForBytesWritten(5000))return 4;
        if(!request.object().contains("id"))continue;
        while(!reply.contains('\n')){if(!socket.waitForReadyRead(30000)){if(socket.state()==QLocalSocket::ConnectedState)continue;return 4;}reply+=socket.readAll();if(reply.size()>32*1024*1024)return 3;}
        const auto at=reply.indexOf('\n');std::cout<<reply.left(at).constData()<<std::endl;reply.remove(0,at+1);
    }
    socket.disconnectFromServer();return 0;
}
}
