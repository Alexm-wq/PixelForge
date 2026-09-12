#include "Automation.hpp"
#include "Editor.hpp"
#include "PixelProgram.hpp"
#include "PixelPatch.hpp"
#include <QBuffer>
#include <QPainter>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <cmath>
#include <limits>
#ifdef Q_OS_UNIX
#include <signal.h>
#include <unistd.h>
#endif

namespace pixelforge::qt {
namespace {
bool integer(const QJsonObject& a,const QString& key,qint64& result,qint64 min=0,qint64 max=std::numeric_limits<qint64>::max()) {
    const auto v=a.value(key);const auto d=v.toDouble(-1);
    if(!v.isDouble() || !std::isfinite(d) || std::floor(d)!=d) return false;
    const auto value=v.toInteger(-1);if(value<min||value>max)return false;result=value;return true;
}
ToolReply error(const QString& message) {return {false,{{"ok",false},{"error",message}}, {}};}
QJsonObject schema(const QJsonObject& properties,const QJsonArray& required) {
    return {{"type","object"},{"properties",properties},{"required",required},{"additionalProperties",false}};
}
QJsonObject str() {return {{"type","string"}};}
QJsonObject num() {return {{"type","integer"},{"minimum",0}};}
QJsonObject action(QStringList values) {return {{"type","string"},{"enum",QJsonArray::fromStringList(values)}};}
}
AutomationController::AutomationController(Canvas* canvas,QObject* parent)
    :QObject(parent),canvas_(canvas),task_(canvas->document_),router_(canvas->document_,task_) {
    connect(&recordFrames_,&QTimer::timeout,this,[this]{
        if(!recordingSource_||recorder_.status()["state"]!="recording")return;
        const auto image=recordingSource_();if(image.isNull())return;
        QImage frame(recordedSize_,QImage::Format_ARGB32);frame.fill(Qt::black);QPainter painter(&frame);
        const auto scaled=image.scaled(recordedSize_,Qt::KeepAspectRatio,Qt::SmoothTransformation);painter.drawImage(QPoint((frame.width()-scaled.width())/2,(frame.height()-scaled.height())/2),scaled);painter.end();recorder_.append(frame);
    });
    connect(&recorder_,&SessionRecording::changed,this,&AutomationController::changed);
    connect(&recorder_,&SessionRecording::finished,this,[this](bool saved,const QString& why){recordFrames_.stop();emit recordingFinished(saved,why);emit changed();});
}
quint64 AutomationController::begin(const QString& prompt,const QString& directory) {
    if(task_.awaiting_user_input()||task_.awaiting_user_review())return 0;
    canvas_->cancelStroke();if(directory_.isEmpty()||pack_.bundle().canvases.isEmpty())directory_=directory;observedRevision_=-1;
    admittedRevision_=canvas_->document_.revision();
    if(!pack_.bundle().canvases.isEmpty() && task_.resume_existing_project(prompt.toStdString())) { emit changed();return task_.snapshot().id; }
    const auto id=task_.begin(prompt.toStdString());
    if(!pack_.bundle().canvases.isEmpty()) {auto b=pack_.bundle();b.taskId=id;pack_.restore(b);}
    emit changed();return id;
}
void AutomationController::stop(const QString& reason) {
    recordFrames_.stop();recorder_.stop();
    if(task_.state()==TaskState::AwaitingAgentDecision) task_.reject(reason.toStdString());
    else if(task_.state()==TaskState::Accepted) task_.abort(reason.toStdString());
    emit changed();
}
bool AutomationController::acceptReview() {
    if(!task_.awaiting_user_review()||recorder_.status()["process_running"].toBool())return false;
    QString why;if(!saveProject(directory_,&why)){emit changed();return false;}
    const bool ok=task_.user_accept_review();revisionBaseline_.clear();emit changed();return ok;
}
bool AutomationController::requestChanges(const QString& feedback) {
    const bool ok=task_.user_request_changes(feedback.toStdString());
    if(ok){revisionBaseline_.clear();for(const auto& c:pack_.bundle().canvases)revisionBaseline_[c.name]=c.image;}
    emit changed();return ok;
}
QJsonObject AutomationController::state() const {
    const auto s=task_.snapshot();
    QSet<QString> groups;for(const auto& c:pack_.bundle().canvases)groups.insert(c.group);
    const auto brief=pack_.bundle().projectBrief;
    const auto rec=recorder_.status();
    return {{"task_id",qint64(s.id)},{"state",task_state_name(s.state)},{"revision",qint64(s.document_revision)},
            {"canvas_width",s.canvas_width},{"canvas_height",s.canvas_height},{"status_message",QString::fromStdString(s.status_message).left(400)},
            {"review_summary",QString::fromStdString(s.review_summary).left(600)},{"review_summary_truncated",s.review_summary.size()>600},{"awaiting_user_review",s.awaiting_user_review},
            {"awaiting_user_input",s.awaiting_user_input},{"question",QString::fromStdString(s.user_input_question)},{"question_reason",QString::fromStdString(s.user_input_reason)},
            {"suggestions",QString::fromStdString(s.user_input_suggestions)},{"pack_revision",qint64(pack_.revision())},
            {"recording",QJsonObject{{"state",rec["state"]},{"saved",rec["saved"]},{"dropped_frames",rec["dropped_frames"]}}},{"recording_allowed",recordingAllowed_},{"references",references_.metadata()},{"canvas_count",pack_.bundle().canvases.size()},{"selected_canvas",selected_},{"project_name",pack_.bundle().projectName},{"project_brief",brief.left(600)},
            {"project_brief_truncated",brief.size()>600},{"group_count",groups.size()},
            {"navigation","Choose broad or filtered index/list/analyze pages, full or selected view/inspect, compact or legacy detail. Carry pack_revision for continuation; known_observation reuse is optional, resend_image requests fresh bytes."},
            {"last_save",QJsonObject{{"reused_images",lastSave_.reusedImages},{"encoded_images",lastSave_.encodedImages},{"hashed_images",lastSave_.hashedImages}}}};
}
bool AutomationController::syncSelected(QString* why) {
    if(pack_.bundle().canvases.isEmpty())return true;
    const ProjectCanvas* found=nullptr;for(const auto& c:pack_.bundle().canvases)if(c.name==selected_)found=&c;
    if(!found){found=&pack_.bundle().canvases.first();selected_=found->name;}
    if(canvas_->image_==found->image)return true;
    const auto image=found->image.convertToFormat(QImage::Format_ARGB32);
    std::vector<quint32> pixels(image.width()*image.height());
    for(int y=0;y<image.height();++y)std::copy_n(reinterpret_cast<const quint32*>(image.constScanLine(y)),image.width(),pixels.begin()+y*image.width());
    std::string errorMessage;
    if(!canvas_->document_.replace_pixels(image.width(),image.height(),std::move(pixels),&errorMessage)){if(why)*why=QString::fromStdString(errorMessage);return false;}
    synchronizing_=true;canvas_->undoCount_=canvas_->redoCount_=0;canvas_->syncImage();emit canvas_->documentChanged();synchronizing_=false;return true;
}
bool AutomationController::ensurePack(QString* why) {
    if(!pack_.bundle().canvases.isEmpty())return true;
    ProjectBundle b;b.taskId=task_.snapshot().id;b.canvases.append({"canvas","",-1,canvas_->image_});
    if(!b.taskId){task_.begin("Manual project");task_.accept(canvas_->image_.width(),canvas_->image_.height());b.taskId=task_.snapshot().id;}
    selected_="canvas";if(!pack_.restore(b,why))return false;return syncSelected(why);
}
void AutomationController::resetWorkspace(){
    stop("Workspace replaced by user.");if(task_.awaiting_user_review())task_.user_accept_review();
    pack_=CanvasPack{};selected_.clear();directory_.clear();revisionBaseline_.clear();observedRevision_=-1;emit changed();
}
bool AutomationController::selectCanvas(const QString& name,QString* why){
    bool found=false;for(const auto& c:pack_.bundle().canvases)if(c.name==name)found=true;
    if(!found){if(why)*why="Unknown canvas.";return false;}
    selected_=name;observedRevision_=-1;const bool ok=syncSelected(why);emit changed();return ok;
}
void AutomationController::manualChanged(){
    if(synchronizing_||pack_.bundle().canvases.isEmpty())return;
    QString why;if(pack_.replaceCanvasImage(selected_,canvas_->image_,pack_.revision(),&why)){observedRevision_=-1;if(!directory_.isEmpty()&&!checkpoint(&why))emit persistenceFailed(why);emit changed();}else emit persistenceFailed(why);
}
bool AutomationController::saveProject(const QString& directory,QString* why){
    if(directory.isEmpty()){if(why)*why="Choose a project directory first.";return false;}
    if(!ensurePack(why))return false;
    if(pack_.bundle().projectBrief.isEmpty())pack_.setProjectBrief(QString::fromStdString(task_.snapshot().prompt).left(4096)+"\n"+QString::fromStdString(task_.snapshot().review_summary).left(4095),why);
    auto b=pack_.bundle();b.packRevision=pack_.revision();
    const auto summary=QString::fromStdString(task_.snapshot().review_summary);
    const auto match=QRegularExpression("(?:^|\\n)PROJECT_NAME:\\s*([^\\n]{1,100})").match(summary);
    if(match.hasMatch())b.projectName=match.captured(1).trimmed();
    if(b.projectBrief.isEmpty())b.projectBrief=QString::fromStdString(task_.snapshot().prompt).left(4096)+"\n"+summary.left(4096);
    if(!ProjectStore::save(directory,b,why,&lastSave_))return false;directory_=directory;return true;
}
bool AutomationController::loadProject(const QString& path,QString* why){
    ProjectBundle b;if(!ProjectStore::load(path,b,why))return false;
    for(const auto& c:b.canvases){std::string errorMessage;if(!canvas_->document_.can_resize(c.image.width(),c.image.height(),&errorMessage)){if(why)*why=QString::fromStdString(errorMessage);return false;}}
    CanvasPack prepared;if(!prepared.restore(b,why))return false;
    resetWorkspace();task_.set_next_task_id_for_restore(b.taskId);task_.begin("Restored project");
    if(!task_.accept(b.canvases.first().image.width(),b.canvases.first().image.height()))return false;
    pack_=std::move(prepared);selected_=b.canvases.first().name;
    directory_=QFileInfo(path).isDir()?path:QFileInfo(path).absolutePath();
    const bool ok=syncSelected(why);emit changed();return ok;
}
bool AutomationController::history(const QString& direction,QString* why){
    const auto r=hostPack({{"action","history"},{"direction",direction}});
    if(!r.success&&why)*why=r.facts["message"].toString(r.facts["error"].toString());return r.success;
}
ToolReply AutomationController::hostPack(QJsonObject args,const QString& output){
    QString why;if(!ensurePack(&why))return error(why);
    args["task_id"]=qint64(task_.snapshot().id);args["pack_revision"]=qint64(pack_.revision());
    if(!output.isEmpty()) {auto exporting=pack_;exporting.setExportReplacementAllowed(true);const auto r=exporting.call(args,task_.snapshot().id,output);return {r.success,r.facts,r.image};}
    return packCall(args,true);
}
ToolReply AutomationController::packCall(QJsonObject args,bool trusted){
    if(!trusted&&(task_.state()!=TaskState::Accepted||task_.awaiting_user_input()))return error("Pack operations require the current accepted task with no unanswered question.");
    const auto verb=args["action"].toString();
    QString requestedMotion;
    if(!trusted){
        const auto key=verb=="pass"?QString("render_mode"):QString("mode");
        const auto requested=args.value(key).toString().toLower();
        if((verb=="view"||verb=="pass")&&(requested=="animation"||requested=="timeline"||requested=="gif")){requestedMotion=requested;args[key]="strip";}
    }
    if(!revisionBaseline_.isEmpty()&&(verb=="create"||args.contains("create_canvases")))return error("A revision must preserve the existing pack; add or edit named canvases instead.");
    CanvasPack candidate=pack_;candidate.setExportReplacementAllowed(trusted);candidate.setPalette(palette_);candidate.setSourceImage(references_.image("source"));const auto before=candidate.revision();
    auto r=candidate.call(args,task_.snapshot().id,directory_);if(!r.success)return {false,r.facts,r.image};
    if(!requestedMotion.isEmpty()){r.facts["agent_observation"]="ordered_frame_strip";r.facts["requested_render_mode"]=requestedMotion;r.facts["fps"]=args.value("fps").toInt(12);r.facts["frame_order"]="group,frame,name";r.facts["animation_preview_generated"]=false;r.facts["guidance"]="This PNG shows ordered frames. Use native Play frames for motion or export scope=animation for a GIF file.";}
    if(candidate.revision()!=before){
        for(const auto& c:candidate.bundle().canvases){
            std::string why;if(!canvas_->document_.can_resize(c.image.width(),c.image.height(),&why))return error(QString::fromStdString(why));
            if(revisionBaseline_.contains(c.name)&&!trusted){const auto& base=revisionBaseline_[c.name];
                if(base.size()!=c.image.size())return error("Revision cannot resize retained artwork.");
                qint64 changed=0,opaque=0,erased=0;for(int y=0;y<base.height();++y)for(int x=0;x<base.width();++x){const auto a=base.pixel(x,y),b=c.image.pixel(x,y);changed+=a!=b;opaque+=qAlpha(a)>0;erased+=qAlpha(a)>0&&qAlpha(b)==0;}
                if(changed>qint64(base.width())*base.height()*.72||(opaque&&erased>opaque*.45))return error("Revision replaces too much existing artwork; preserve it and apply incremental changes.");
            }
        }
        pack_=std::move(candidate);observedRevision_=-1;QString why;
        if(!syncSelected(&why))return error(why);
        auto facts=r.facts;if(!directory_.isEmpty()&&!checkpoint(&why))facts["persistence_error"]=why;
        facts["document_revision"]=qint64(canvas_->document_.revision());if(!r.image.isEmpty())observedRevision_=canvas_->document_.revision();emit changed();return {true,facts,r.image};
    }
    pack_=std::move(candidate); // retain read-only analysis/observation caches without publishing a new revision
    if(!r.image.isEmpty())observedRevision_=canvas_->document_.revision();return {true,r.facts,r.image};
}
ToolReply AutomationController::recordingControl(const QString& verb,int fps){
    if(verb=="status")return {true,recorder_.status(),{}};
    if(verb=="stop"){
        recordFrames_.stop();recorder_.stop();auto facts=recorder_.status();facts["pending_recording"]=facts["process_running"].toBool();return {true,facts,{}};
    }
    if(verb!="start")return error("Unknown recording action.");
    if(!recordingAllowed_)return error("Recording is disabled. The user must explicitly enable session recording in Automation settings; a model request cannot grant it.");
    if(!recordingSource_||directory_.isEmpty())return error("No recording frame source or current task folder is available.");
    if(recorder_.status()["recording"].toBool())return {true,recorder_.status(),{}};
    const auto image=recordingSource_();if(image.isNull())return error("The editor could not provide a recording frame.");recordedSize_=image.size();
    const auto path=QDir(directory_).filePath("session-"+QDateTime::currentDateTimeUtc().toString("yyyyMMdd-hhmmsszzz")+".mp4");
    QString why;if(!recorder_.start(path,image.width(),image.height(),fps,&why))return error(why);
    recordFrames_.start(std::max(1,1000/fps));return {true,recorder_.status(),{}};
}
bool AutomationController::loadReference(const QString& slot,const QString& path,QString* why){const bool ok=references_.load(slot,path,why);emit changed();return ok;}
void AutomationController::clearReference(const QString& slot){references_.clear(slot);emit changed();}
bool AutomationController::answerQuestion(const QString& answer){
    if(answer.trimmed().isEmpty()||answer.size()>8000)return false;
    const bool ok=task_.user_answer_input(answer.toStdString());emit changed();return ok;
}
ToolReply AutomationController::reply(const AgentCommandResult& r) {
    auto facts=state();facts["ok"]=r.ok;facts["error"]=agent_error_code_name(r.error);
    facts["message"]=QString::fromStdString(r.message);facts["changed_pixels"]=qint64(r.changed_pixels);
    emit changed();return {r.ok,facts,{}};
}
bool AutomationController::checkpoint(QString* errorMessage) {
    if(directory_.isEmpty()) return true;
    QDir d(directory_); if(!d.exists()){if(errorMessage)*errorMessage="Project output directory is missing; committed pixels are retained.";return false;}
    // Loaded manifests may themselves reference canvas.png. Never overwrite a
    // legacy asset before the new immutable-generation manifest commits.
    if(!pack_.bundle().canvases.isEmpty())return ProjectStore::save(directory_,pack_.bundle(),errorMessage,&lastSave_);
    return canvas_->writePng(d.filePath("canvas.png"),errorMessage);
}
ToolReply AutomationController::call(const QString& tool,const QJsonObject& a) {
    if(QJsonDocument(a).toJson(QJsonDocument::Compact).size()>1024*1024) return error("Tool input exceeds the bounded host message size.");
    QSet<QString> permitted;
    bool known=false;
    for(const auto& entry:tools()) if(entry.toObject()["name"].toString()==tool) {
        known=true;for(const auto& k:entry.toObject()["inputSchema"].toObject()["properties"].toObject().keys()) permitted.insert(k);
    }
    if(!known) return error("Unknown PixelForge tool.");
    for(const auto& k:a.keys()) if(!permitted.contains(k)) return error("Unknown field: "+k);
    const auto verb=a["action"].toString();
    if(tool=="pixelforge_task" && verb=="get") return reply(router_.task_get());
    qint64 id=0,rev=0;
    if(!integer(a,"task_id",id) || quint64(id)!=task_.snapshot().id) return error("Missing or stale task_id.");
    if(tool=="pixelforge_task") {
        if(verb=="accept") {
            if(task_.state()==TaskState::Accepted && !pack_.bundle().canvases.isEmpty())return reply(router_.task_get());
            if(canvas_->document_.revision()!=admittedRevision_) return error("Canvas changed after Generate; accept refused to preserve newer work.");
            qint64 w=canvas_->document_.width(),h=canvas_->document_.height();
            if((a.contains("width")&&!integer(a,"width",w,1,4096))||(a.contains("height")&&!integer(a,"height",h,1,4096))) return error("Invalid canvas dimensions.");
            auto r=router_.task_accept(id,w,h);
            if(r.ok) {canvas_->undoCount_=canvas_->redoCount_=0;canvas_->syncImage();emit canvas_->documentChanged();}
            return reply(r);
        }
        if(verb=="reject") return reply(router_.task_reject(id,a["reason"].toString().toStdString()));
        if(verb=="abort") return reply(router_.task_abort(id,a["reason"].toString().toStdString()));
    }
    if(tool=="pixelforge_record"){
        qint64 fps=30;if(a.contains("fps")&&!integer(a,"fps",fps,1,60))return error("Recording fps must be 1–60.");
        return recordingControl(verb,int(fps));
    }
    if(tool=="pixelforge_reference" && a["action"]!="seed") {
        const auto r=references_.call(a);return {r.success,r.facts,r.image};
    }
    if(tool=="pixelforge_view"&&(verb=="content_reference"||verb=="style_reference")) {
        const auto r=references_.call({{"action","view"},{"reference",verb=="content_reference"?"content":"style"}});return {r.success,r.facts,r.image};
    }
    if(tool=="pixelforge_pack"||tool=="pixelforge_pass") {
        QJsonObject args=a;if(tool=="pixelforge_pass")args["action"]="pass";
        return packCall(args);
    }
    if(tool=="pixelforge_dialog") {
        if(a["question"].toString().trimmed().isEmpty()||a["question"].toString().size()>2000||a["reason"].toString().size()>2000||a["suggestions"].toString().size()>1000)return error("Question/reason/suggestions exceed supported bounds.");
        std::string why;if(!task_.request_user_input(a["reason"].toString().toStdString(),a["question"].toString().toStdString(),a["suggestions"].toString().toStdString(),&why))return error(QString::fromStdString(why));
        emit changed();auto r=reply(router_.task_get());r.facts["pending_user_answer"]=true;return r;
    }
    if(tool=="pixelforge_reference" && verb=="seed") {
        if(task_.state()!=TaskState::Accepted||task_.awaiting_user_input())return error("Reference seed requires an accepted task without a pending question.");
        if(a.contains("expected_revision")&&a["expected_revision"].toInteger(-1)!=qint64(canvas_->document_.revision()))return error("Stale reference seed revision.");
        const auto image=references_.image(a["reference"].toString());
        if(image.isNull()||image.size()!=canvas_->image_.size())return error("Select a reference with exactly the accepted canvas dimensions before seeding.");
        std::vector<AgentPixelOp> ops;ops.reserve(image.width()*image.height());
        for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x)ops.push_back(AgentPixelOp{AgentPixelOpKind::SetPixel,x,y,0,0,1,1,image.pixel(x,y)});
        const auto before=canvas_->document_.revision();const auto r=router_.edit(id,before,ops);
        if(r.ok&&before!=canvas_->document_.revision()){++canvas_->undoCount_;canvas_->redoCount_=0;observedRevision_=-1;canvas_->syncImage();synchronizing_=true;emit canvas_->documentChanged();synchronizing_=false;QString why;if(!pack_.bundle().canvases.isEmpty())pack_.replaceCanvasImage(selected_,canvas_->image_,pack_.revision(),&why);if(!checkpoint(&why)){auto result=reply(r);result.facts["persistence_error"]=why;return result;}}
        return reply(r);
    }
    if(!integer(a,"expected_revision",rev) || quint64(rev)!=canvas_->document_.revision()) return error("Missing or stale expected_revision; inspect task.get before retrying a new edit.");
    if(tool=="pixelforge_task" && verb=="finish") {
        if(observedRevision_!=rev) return error("Render the exact current revision before submitting it for user review.");
        if(a["summary"].toString().trimmed().isEmpty()) return error("A review summary is required.");
        QString why;if(!checkpoint(&why)) return error("Canvas checkpoint failed; artwork retained: "+why);
        auto result=reply(router_.task_finish(id,rev,a["summary"].toString().toStdString()));if(result.success){recordFrames_.stop();recorder_.stop();}return result;
    }
    if(tool=="pixelforge_view") {
        if(verb=="render") {
            qint64 scale=1;if(a.contains("scale")&&!integer(a,"scale",scale,1,32)) return error("Invalid render scale.");
            if(qint64(canvas_->document_.width())*canvas_->document_.height()*scale*scale>4194304) return error("Render too large; request a smaller scale.");
            const auto stamp=ProjectStore::pixelIdentity(canvas_->image_)+":"+QString::number(scale);
            observedRevision_=rev;auto r=reply(router_.task_get());r.facts["observed_revision"]=rev;r.facts["observation"]=stamp;
            if(a["known_observation"].toString()==stamp&&!a["resend_image"].toBool()){r.facts["unchanged"]=true;return r;}
            if(viewStamp_!=stamp){QByteArray png;QBuffer buffer(&png);buffer.open(QIODevice::WriteOnly);
                if(!canvas_->image_.scaled(canvas_->document_.width()*scale,canvas_->document_.height()*scale,Qt::IgnoreAspectRatio,Qt::FastTransformation).save(&buffer,"PNG")) return error("PNG render failed.");viewStamp_=stamp;viewImage_=png;}
            r.image=viewImage_;return r;
        }
        if(verb=="inspect") {
            qint64 x,y,w,h;if(!integer(a,"x",x,0,4095)||!integer(a,"y",y,0,4095)||!integer(a,"width",w,1,4096)||!integer(a,"height",h,1,4096)||w*h>4096) return error("Inspection rectangle exceeds supported bounds.");
            auto r=router_.inspect_region(id,rev,x,y,w,h);if(!r.ok)return error(QString::fromStdString(r.message));
            auto result=reply(router_.task_get());QJsonArray pixels;for(auto c:r.pixels)pixels.append(QString("#%1").arg(c,8,16,QChar('0')));result.facts["pixels"]=pixels;return result;
        }
    }
    if(tool=="pixelforge_palette") {
        if(verb=="set") {
            const auto values=a["colors"].toString().split(',');if(values.isEmpty()||values.size()>65536)return error("Palette requires 1–65,536 AARRGGBB colours within the tool input budget.");
            QList<quint32> palette;for(auto value:values){value=value.trimmed();if(value.startsWith('#'))value.remove(0,1);bool ok=false;auto c=value.toUInt(&ok,16);if(!ok||value.size()!=8)return error("Invalid AARRGGBB palette colour.");palette.append(c);}palette_=palette;
        } else if(verb!="get") return error("Unsupported palette action.");
        auto r=reply(router_.task_get());QStringList values;for(auto c:palette_)values<<QString("%1").arg(c,8,16,QChar('0'));r.facts["colors"]=values.join(',');return r;
    }
    if(tool=="pixelforge_io" && verb=="export") {
        const auto name=a["path"].toString();
        if(!QRegularExpression("^[A-Za-z0-9_-][A-Za-z0-9_.-]{0,100}\\.png$").match(name).hasMatch() || directory_.isEmpty() || !ProjectStore::safeExportFileName(name))return error("Export path must be a safe new PNG filename inside this task's output folder.");
        const auto destination=QDir(directory_).filePath(name);
        if(!ProjectStore::safeAbsolutePath(destination))return error("Export path contains a link or unsafe component.");
        if(QFileInfo::exists(destination))return error("Agent exports preserve existing files; choose a new PNG filename.");
        QString why;if(!canvas_->writePng(destination,&why))return error(why);
        auto r=reply(router_.task_get());r.facts["file"]=name;return r;
    }
    const auto before=canvas_->document_.revision();
    AgentCommandResult mutation;
    if(tool=="pixelforge_history") {
        if(verb=="undo")mutation=router_.history_undo(id,rev);else if(verb=="redo")mutation=router_.history_redo(id,rev);else return error("Unknown history direction.");
        if(mutation.ok){if(verb=="undo"){--canvas_->undoCount_;++canvas_->redoCount_;}else{++canvas_->undoCount_;--canvas_->redoCount_;}}
    } else if(tool=="pixelforge_edit"||tool=="pixelforge_program") {
        QString patch=a["patch"].toString();
        if(tool=="pixelforge_program") {
            const auto compiled=win32::compile_pixel_program(a["program"].toString().toStdString(),canvas_->document_.width(),canvas_->document_.height(),canvas_->document_.pixels());
            if(!compiled.ok)return error(QString::fromStdString(compiled.error));patch=QString::fromStdString(compiled.patch);if(patch.isEmpty())return reply(router_.task_get());
        }
        std::vector<AgentPixelOp> operations;QString parseError;
        if(!parsePixelPatch(patch,palette_,operations,&parseError))return error(parseError);
        mutation=router_.edit(id,rev,operations);
        if(mutation.ok && canvas_->document_.revision()!=before){++canvas_->undoCount_;canvas_->redoCount_=0;}
    } else return error("Unsupported tool action.");
    if(mutation.ok && canvas_->document_.revision()!=before) {
        observedRevision_=-1;canvas_->syncImage();synchronizing_=true;emit canvas_->documentChanged();synchronizing_=false;
        if(!pack_.bundle().canvases.isEmpty()){QString why;pack_.replaceCanvasImage(selected_,canvas_->image_,pack_.revision(),&why);}
        QString why;if(!checkpoint(&why)){auto r=reply(mutation);r.facts["persistence_error"]=why;r.facts["applied"]=true;return r;}
    }
    return reply(mutation);
}
QJsonArray AutomationController::tools() {
    QJsonArray result;
    auto add=[&](QString name,QString description,QJsonObject props,QJsonArray required){result.append(QJsonObject{{"name",name},{"description",description},{"inputSchema",schema(props,required)}});};
    const QJsonObject binding{{"task_id",num()},{"expected_revision",num()}};
    auto fields=[&](QJsonObject extra){auto p=binding;for(auto i=extra.begin();i!=extra.end();++i)p[i.key()]=i.value();return p;};
    add("pixelforge_task","Manage the actual PixelForge task. get provides optional current orientation. accept preserves the requested canvas size unless explicit dimensions are supplied. finish requires exact current rendered revision and produces user review, not automatic user acceptance.",fields({{"action",action({"get","accept","reject","abort","finish"})},{"width",num()},{"height",num()},{"reason",str()},{"summary",str()}}),{"action"});
    add("pixelforge_edit","Atomic exact pixel patch: P,x,y,c; H,x,y,length,c; V,x,y,length,c; R,x,y,width,height,c; L,x0,y0,x1,y1,c. Colour #AARRGGBB or palette index. Use current task_id/expected_revision.",fields({{"patch",str()}}),{"task_id","expected_revision","patch"});
    add("pixelforge_program","Existing PixelForge raster language. Commands one per line: CLEAR color; P x y color; R x y width height color; H x y length color; V x y length color; L x0 y0 x1 y1 color; FCIRCLE cx cy radius color; FELLIPSE x y width height color. Also masks, SHADE, DITHER, OUTLINE and other existing PixelProgram commands; no executable code.",fields({{"program",str()}}),{"task_id","expected_revision","program"});
    add("pixelforge_view","Receive an actual current PNG image with render, or exact ARGB pixels with inspect. Render the final revision before finish.",fields({{"action",action({"render","inspect","content_reference","style_reference"})},{"scale",num()},{"x",num()},{"y",num()},{"width",num()},{"height",num()},{"known_observation",str()},{"resend_image",QJsonObject{{"type","boolean"}}}}),{"action","task_id","expected_revision"});
    add("pixelforge_palette","Get or set up to 65,536 comma-separated AARRGGBB palette colours within the message budget.",fields({{"action",action({"get","set"})},{"colors",str()}}),{"action","task_id","expected_revision"});
    add("pixelforge_history","Undo/redo an atomic committed drawing pass.",fields({{"action",action({"undo","redo"})}}),{"action","task_id","expected_revision"});
    add("pixelforge_io","Export the exact native PNG into this task's output folder. path is a new PNG filename, not an arbitrary external filesystem path; existing files and linked paths are refused.",fields({{"action",action({"export"})},{"path",str()}}),{"action","task_id","expected_revision","path"});
    QJsonObject packFields{{"action",str()},{"task_id",num()},{"expected_revision",num()},{"pack_revision",num()}};
    for(const auto& k:QStringList{"canvas","canvases","group","source","dest","program","patch","mode","scope","path","direction","project_name","create_canvases","inspect_before","inspect_after","render_mode","render_canvas","render_canvases","render_group","query","prefix","format","project_brief","known_observation"})packFields[k]=str();
    for(const auto& k:QStringList{"x","y","width","height","sx","sy","dx","dy","scale","columns","fps","offset","limit","pixel_offset","pixel_limit","max_colors","changed_since"})packFields[k]=num();
    for(const auto& k:QStringList{"sx","sy","dx","dy","x","y"})packFields[k]=QJsonObject{{"type","integer"}};
    packFields["resend_image"]=QJsonObject{{"type","boolean"}};packFields["seed_from_source"]=QJsonObject{{"type","boolean"}};packFields["summary"]=QJsonObject{{"type","boolean"}};packFields["include_loop"]=QJsonObject{{"type","boolean"}};
    add("pixelforge_pack","Actual named multi-canvas pack. index/list default to compact metadata pages; query name/group substring, prefix name, exact group/canvas; carry pack_revision for offset continuation. changed_since returns changed canvases only above tracking floor. known_observation suppresses repeated exact selected view bytes. format=legacy opts into richer text rows. Unfiltered broad scope, explicit ROI, compact/rich metadata and fresh images are agent choices. Omit known_observation for a fresh image, or set resend_image=true. Agent animation/timeline/gif view returns an explicitly labelled ordered PNG strip; native preview and export can produce GIF. create/add canvases='name,group,width,height,frame|...'; create replaces an initial pack, add preserves it. list/analyze support group/filter/pages; program/edit use CANVAS name lines; copy/clone source,dest; history direction undo/redo; view mode canvas/sheet/strip/timeline/animation; inspect region; save; export scope canvas/group/all/sheet/strip/animation. PNG/GIF outputs stay in project folder. Use task_id and pack_revision returned by list; expected_revision here means PACK revision. No arbitrary code. GIF flattens partial alpha and quantizes if needed.",packFields,{"action","task_id"});
    packFields.remove("action");add("pixelforge_pass","Atomic local multi-canvas pass: optional create_canvases, inspect_before/after='name,x,y,width,height|...' (up to4), existing PixelProgram with CANVAS name sections, optional render_mode and render_canvas/canvases/group. One final real PNG/GIF; no intermediate model turn. Use current pack_revision.",packFields,{"task_id"});
    add("pixelforge_dialog","Ask the user a necessary artwork question and wait for their actual answer in this same turn. No answer is invented. Stop remains available. suggestions is optional up to3 choices separated by |.",{{"task_id",num()},{"reason",str()},{"question",str()},{"suggestions",str()}},{"task_id","reason","question"});
    add("pixelforge_reference","Actual user-selected source/content/style slots, separate from the document. view returns the real image; palette extracts observed ARGB colors; seed copies source pixels only into matching accepted canvas with revision guard. A missing slot is unavailable, never invented. No tool accepts a filesystem path. known_observation/resend_image optionally suppress exact repeated image bytes.",{{"task_id",num()},{"expected_revision",num()},{"action",action({"view","palette","seed"})},{"reference",action({"source","content","style"})},{"max_colors",num()},{"scale",num()},{"max_edge",num()},{"known_observation",str()},{"resend_image",QJsonObject{{"type","boolean"}}}},{"task_id","reference","action"});
    add("pixelforge_record","Optional local editor-session MP4. Only when the user explicitly enabled recording, start before mutations; status reports actual encoder state; stop waits for real encoder completion. Video stays local and is never returned to the agent. Captures only the owned editor pixels, excluding desktop/other windows; resizing letterboxes. Disabled recording is not permission to start it.",{{"action",action({"start","status","stop"})},{"task_id",num()},{"fps",num()}},{"action","task_id"});
    return result;
}
QString AutomationController::instructions() {
    return "You are PixelForge's actual pixel artist. Interpret the user's request yourself; the host does not choose artwork. Use only supplied PixelForge tools for artwork. Do not run shell, code, network, or filesystem tools. You may get current task state when needed. A new task must be accepted with the requested dimensions before drawing; a restored task may already be accepted. Each edit needs exact current task_id and expected_revision; refresh after a stale rejection, never blindly replay. Inspect actual PNG with pixelforge_view render, improve if needed, then task.finish with concise honest summary. Finish means awaiting HUMAN review. PNG checkpoints are automatic, project acceptance is separate. If unsupported, reject/abort with honest reason; never describe successful artwork without a committed edit and actual image. Choose your own inspection strategy: task.get orientation, broad unfiltered pack index/list/analyze, or selected parts using query/prefix/group/canvas. Choose compact or legacy richer metadata; bounded pages expose next_offset and require their matching pack_revision on continuation. View the full supported selection by omitting ROI, or choose an explicit rectangle/scale. Images are returned fresh unless you deliberately supply matching known_observation; resend_image=true forces fresh bytes. These choices do not require an overview/search/preview sequence. Keep stable canvas IDs and honor stated resource limits. Agent animation/timeline observations are ordered PNG frame strips; do not claim you watched native playback. GIF is available as an export file and through native Play frames. Use pixelforge_pack for named canvases, groups/frames, paged analysis and GIF/sheet exports, or pixelforge_pass for one atomic local program/inspect/render pass. Pack revision is distinct from task document revision. A restored project can already be ACCEPTED; inspect get instead of assuming resize is needed. Ask necessary questions through pixelforge_dialog and wait for the actual user response. Selected references are exposed only through pixelforge_reference. Read their metadata in task.get; do not claim an unavailable slot. pixelforge_pass seeds newly created matching-size canvases from a selected source by default; seed_from_source=false disables that. Do not claim unadvertised recording tools. No fabrication or canned images.";
}

AutomationClient::AutomationClient(AutomationController* controller,QObject* parent):QObject(parent),controller_(controller) {
    connect(controller_,&AutomationController::recordingFinished,this,[this](bool saved,const QString& why){
        record({{"recording_saved",saved},{"recording_error",why}});
        if(heldQuestion_.isEmpty()||heldQuestion_["params"].toObject()["tool"]!="pixelforge_record"||ended_||stopping_)return;
        const auto request=heldQuestion_;const auto hash=heldHash_;heldQuestion_={};heldHash_.clear();auto facts=controller_->recordingState();facts["ok"]=saved;completeTool(request,hash,{saved,facts,{}});
    });
    negotiation_.setSingleShot(true);negotiation_.setInterval(30000);
    killTimer_.setSingleShot(true);killTimer_.setInterval(2500);
    connect(&negotiation_,&QTimer::timeout,this,[this]{finish(false,"App Server negotiation timed out; no request was replayed.");});
    connect(&killTimer_,&QTimer::timeout,this,[this]{retireProcess();});
    connect(&process_,&QProcess::readyReadStandardOutput,this,&AutomationClient::read);
    connect(&process_,&QProcess::readyReadStandardError,this,[this]{process_.readAllStandardError();});
    connect(&process_,&QProcess::errorOccurred,this,[this](QProcess::ProcessError e){if(busy_&&!ended_)finish(false,stopping_?"Stopped. Committed pixels retained; no turn replayed.":"Local Codex process error ("+QString::number(e)+"). Check the installed Codex executable and sign-in.");});
    connect(&process_,&QProcess::finished,this,[this](int code,QProcess::ExitStatus){
        killTimer_.stop();if(!ended_&&busy_)finish(false,"Local Codex exited before completion ("+QString::number(code)+"). No turn was replayed.");
        busy_=false;emit changed();
    });
#ifdef Q_OS_UNIX
    process_.setChildProcessModifier([]{::setsid();});
#endif
}
AutomationClient::~AutomationClient(){stop();retireProcess();process_.waitForFinished(2000);}
void AutomationClient::setStatus(const QString& s){status_=s;emit changed();}
void AutomationClient::record(QJsonObject event){if(events_.size()<2000){event["at"]=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);events_.append(event);}}
QJsonObject AutomationClient::evidence() const {return {{"thread_id",thread_},{"turn_id",turn_},{"requested_model",model_},{"requested_effort",effort_},{"status",status_},{"events",events_},{"task",controller_->state()}};}
void AutomationClient::send(QJsonObject m){const auto bytes=QJsonDocument(m).toJson(QJsonDocument::Compact)+'\n';if(process_.write(bytes)!=bytes.size()&&!ended_)finish(false,"App Server write failed; side effects may be uncertain. No replay.");}
void AutomationClient::request(const QString& method,QJsonObject p,std::function<void(QJsonObject)> done){const auto id=next_++;pending_[id]=std::move(done);send({{"id",id},{"method",method},{"params",p}});record({{"method",method},{"request_id",id}});}
bool AutomationClient::start(const QString& prompt,const QString& model,const QString& effort,const QString& outputDirectory,const QString& executable) {
    if(busy_||process_.state()!=QProcess::NotRunning||prompt.trimmed().isEmpty()||prompt.size()>16000||!scratch_.isValid())return false;
    if(!QRegularExpression("^[a-zA-Z0-9_.-]{1,96}$").match(model).hasMatch()||!QStringList{"low","medium","high","xhigh","max","ultra"}.contains(effort))return false;
    QString exe=executable;if(exe.isEmpty())exe=qEnvironmentVariable("PIXELFORGE_CODEX_EXE");if(exe.isEmpty())exe=qEnvironmentVariable("CODEX_CLI_PATH");if(exe.isEmpty())exe=QStandardPaths::findExecutable("codex");
    if(exe.isEmpty()){setStatus("Codex CLI is not installed or not on PATH.");return false;}
    if(!QDir().mkpath(outputDirectory)){setStatus("Cannot create this task's output folder.");return false;}
    busy_=true;stopping_=ended_=false;thread_.clear();turn_.clear();buffer_.clear();pending_.clear();receipts_.clear();callHashes_.clear();receiptBytes_=0;events_={};heldQuestion_={};heldHash_.clear();
    model_=model;effort_=effort;prompt_=prompt;directory_=outputDirectory;if(!controller_->begin(prompt,outputDirectory)){busy_=false;setStatus("Resolve the current review or user question before starting another turn.");return false;}
    process_.setWorkingDirectory(scratch_.path());
    QStringList args;for(const auto& value:QStringList{"mcp_servers={}","features.apps=false","features.plugins=false","features.browser_use=false","features.computer_use=false","features.shell_tool=false","features.unified_exec=false","features.agents=false","web_search=\"disabled\""})args<<"-c"<<value;
    args<<"app-server"<<"--listen"<<"stdio://";
    process_.setProgram(exe);process_.setArguments(args);
    QObject::disconnect(startedConnection_);
    startedConnection_=connect(&process_,&QProcess::started,this,[this]{
        request("initialize",{{"clientInfo",QJsonObject{{"name","pixelforge_qt"},{"version","0.4.2"}}},{"capabilities",QJsonObject{{"experimentalApi",true}}}},[this](QJsonObject){
            send({{"method","initialized"},{"params",QJsonObject{}}});
            request("account/read",{{"refreshToken",false}},[this](QJsonObject r){
                if(r["account"].toObject()["type"]!="chatgpt"){finish(false,"Sign in to the installed Codex CLI with ChatGPT before generating.");return;}
                request("model/list",{{"includeHidden",false}},[this](QJsonObject r){
                    bool supported=false;for(const auto& item:r["data"].toArray()){const auto m=item.toObject();if(m["model"]!=model_&&m["id"]!=model_)continue;for(const auto& e:m["supportedReasoningEfforts"].toArray())if(e.toObject()["reasoningEffort"]==effort_)supported=true;}
                    if(!supported){finish(false,"Requested model/effort is not supported by the installed account catalog.");return;}beginThread();
                });
            });
        });
    },Qt::SingleShotConnection);
    setStatus("Starting local Codex App Server…");negotiation_.start();process_.start();return true;
}
void AutomationClient::beginThread(){
    request("thread/start",{{"model",model_},{"cwd",scratch_.path()},{"approvalPolicy","never"},{"sandbox","read-only"},{"ephemeral",true},{"config",QJsonObject{{"model_reasoning_effort",effort_}}},{"baseInstructions",AutomationController::instructions()},{"dynamicTools",AutomationController::tools()},{"serviceName","PixelForge"},{"environments",QJsonArray{}}},[this](QJsonObject r){
        thread_=r["thread"].toObject()["id"].toString();
        if(thread_.isEmpty()||r["model"]!=model_||r["reasoningEffort"]!=effort_||r["cwd"]!=scratch_.path()||r["sandbox"].toObject()["type"]!="readOnly"||r["approvalPolicy"]!="never") {finish(false,"Returned thread settings did not match requested model, effort, cwd and read-only policy. No turn sent.");return;}
        record({{"effective_model",r["model"]},{"effective_effort",r["reasoningEffort"]},{"sandbox","readOnly"}});
        request("turn/start",{{"threadId",thread_},{"model",model_},{"effort",effort_},{"input",QJsonArray{QJsonObject{{"type","text"},{"text",prompt_}}}}},[this](QJsonObject r){
            const auto turn=r["turn"].toObject()["id"].toString();if(turn.isEmpty()||(!turn_.isEmpty()&&turn_!=turn)){finish(false,"Missing or mismatched original turn identity.");return;}turn_=turn;negotiation_.stop();setStatus("Codex is working — canvas updates when real tools commit.");
        });
    });
}
void AutomationClient::read(){
    buffer_+=process_.readAllStandardOutput();if(buffer_.size()>8*1024*1024){finish(false,"App Server message exceeded the host bound.");return;}
    int lines=0;while(buffer_.contains('\n')&&lines++<32){const auto pos=buffer_.indexOf('\n');const auto line=buffer_.left(pos);buffer_.remove(0,pos+1);QJsonParseError e;auto doc=QJsonDocument::fromJson(line,&e);if(e.error!=QJsonParseError::NoError||!doc.isObject()){finish(false,"Invalid App Server JSONL message.");return;}message(doc.object());}
    if(buffer_.contains('\n'))QTimer::singleShot(0,this,&AutomationClient::read);
}
void AutomationClient::message(QJsonObject m){
    const auto method=m["method"].toString();const auto params=m["params"].toObject();
    if(method.isEmpty()&&m.contains("id")) {
        const qint64 id=m["id"].toInteger(-1);auto callback=pending_.take(id);if(!callback)return;
        if(m.contains("error")){finish(false,"Codex RPC rejected request "+QString::number(id)+" (code "+QString::number(m["error"].toObject()["code"].toInt())+"). No request replayed.");return;}
        if(!ended_&&!stopping_)callback(m["result"].toObject());return;
    }
    if(m.contains("id")) {
        if(method=="item/tool/call") {
            const QString call=params["callId"].toString(),tool=params["tool"].toString(),incomingTurn=params["turnId"].toString();
            if(turn_.isEmpty()&&!incomingTurn.isEmpty()&&params["threadId"]==thread_)turn_=incomingTurn;
            const QString key=call;
            if(ended_||stopping_||!busy_||call.isEmpty()||params["threadId"]!=thread_||incomingTurn!=turn_||(!params["namespace"].isNull()&&!params["namespace"].toString().isEmpty())) {
                const QJsonObject item{{"type","inputText"},{"text","Inactive or mismatched original PixelForge task/turn; no action taken."}};
                send({{"id",m["id"]},{"result",QJsonObject{{"success",false},{"contentItems",QJsonArray{item}}}}});return;
            }
            // Exact call IDs are never replayed, including after a lost tool response.
            const auto hash=QCryptographicHash::hash(tool.toUtf8()+QJsonDocument(params["arguments"].toObject()).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256);
            if(!heldQuestion_.isEmpty()){
                if(heldQuestion_["params"].toObject()["callId"]==call&&heldHash_==hash)return;
                finish(false,"Overlapping tool request while the user question is pending; no replay.");return;
            }
            if(receipts_.contains(key)){
                if(callHashes_[key]!=hash){finish(false,"Conflicting reuse of a tool call ID; no action replayed.");return;}
                send({{"id",m["id"]},{"result",receipts_[key]}});return;
            }
            if(receipts_.size()>=1000||receiptBytes_>48*1024*1024){finish(false,"Tool receipt bound reached; no further action admitted.");return;}
            ToolReply reply=params["arguments"].isObject()?controller_->call(tool,params["arguments"].toObject()):error("Tool arguments must be an object.");
            if(reply.success&&(reply.facts["pending_user_answer"].toBool()||reply.facts["pending_recording"].toBool())){heldQuestion_=m;heldHash_=hash;setStatus(reply.facts["pending_recording"].toBool()?"Finalizing the actual local recording…":"Waiting for your answer; this original agent turn remains active.");return;}
            completeTool(m,hash,reply);return;
        }
        QJsonObject response;
        if(method=="item/commandExecution/requestApproval"||method=="item/fileChange/requestApproval")response={{"decision","decline"}};
        else if(method=="item/permissions/requestApproval")response={{"scope","turn"},{"permissions",QJsonObject{}}};
        else {send({{"id",m["id"]},{"error",QJsonObject{{"code",-32601},{"message","Unsupported request in PixelForge automation."}}}});return;}
        send({{"id",m["id"]},{"result",response}});return;
    }
    if(ended_)return;
    if(params.contains("threadId")&&params["threadId"]!=thread_)return;
    if(method=="turn/started") {const auto id=params["turn"].toObject()["id"].toString();if(turn_.isEmpty())turn_=id;else if(id!=turn_)finish(false,"Unexpected turn identity.");}
    else if(method=="turn/completed") {
        const auto t=params["turn"].toObject();if(t["id"]!=turn_)return;
        const bool ok=t["status"]=="completed"&&controller_->state()["awaiting_user_review"].toBool();
        finish(ok,ok?"Artwork ready for your review — accept or request changes.":stopping_?"Stopped. Committed pixels retained; no turn replayed.":"Agent turn ended without a reviewed finished artwork. Committed pixels retained.");
    } else if(method=="item/started") {
        const auto type=params["item"].toObject()["type"].toString();
        if(type=="commandExecution"||type=="fileChange"){stop();setStatus("Unexpected non-pixel operation; stopping without replay.");}
        else if(type=="reasoning")setStatus("Codex is considering the artwork…");
    } else if(method=="item/completed"&&params["item"].toObject()["type"]=="agentMessage") {
        const auto text=params["item"].toObject()["text"].toString().left(4000);record({{"agent_message",text}});setStatus(text);
    }
}
void AutomationClient::completeTool(QJsonObject request,const QByteArray& hash,ToolReply reply){
    const auto params=request["params"].toObject();const auto call=params["callId"].toString(),tool=params["tool"].toString();
    QJsonArray content{QJsonObject{{"type","inputText"},{"text",QString::fromUtf8(QJsonDocument(reply.facts).toJson(QJsonDocument::Compact))}}};
    if(!reply.image.isEmpty())content.append(QJsonObject{{"type","inputImage"},{"imageUrl",QString(reply.image.startsWith("GIF")?"data:image/gif;base64,":"data:image/png;base64,")+QString::fromLatin1(reply.image.toBase64())}});
    QJsonObject response{{"success",reply.success},{"contentItems",content}};receipts_[call]=response;callHashes_[call]=hash;receiptBytes_+=QJsonDocument(response).toJson(QJsonDocument::Compact).size();
    record({{"call_id",call},{"tool",tool},{"action",params["arguments"].toObject()["action"]},{"success",reply.success},{"result",reply.facts},{"image_bytes",reply.image.size()}});
    send({{"id",request["id"]},{"result",response}});setStatus(tool+(reply.success?" applied":" refused")+" · revision "+QString::number(controller_->state()["revision"].toInteger()));
}
void AutomationClient::answerQuestion(const QString& answer){
    if(heldQuestion_.isEmpty()||!busy_||stopping_||ended_||!controller_->answerQuestion(answer))return;
    const auto request=heldQuestion_;const auto hash=heldHash_;heldQuestion_={};heldHash_.clear();
    auto facts=controller_->state();facts["answer"]=answer;facts["ok"]=true;completeTool(request,hash,{true,facts,{}});
}
void AutomationClient::stop(){if(!busy_||ended_)return;stopping_=true;heldQuestion_={};heldHash_.clear();controller_->stop("Stopped by user; committed pixels retained.");setStatus("Stopping the current Codex turn…");if(!thread_.isEmpty()&&!turn_.isEmpty())request("turn/interrupt",{{"threadId",thread_},{"turnId",turn_}},[](QJsonObject){});killTimer_.start();}
void AutomationClient::retireProcess(){
    if(process_.state()==QProcess::NotRunning)return;
#ifdef Q_OS_UNIX
    const auto pid=process_.processId();if(pid>0)::kill(-pid,SIGKILL);
#endif
    process_.kill();
}
void AutomationClient::finish(bool success,const QString& reason){
    if(ended_)return;ended_=true;negotiation_.stop();pending_.clear();if(!success)controller_->stop(reason);setStatus(reason);
    record({{"finished",success},{"reason",reason}});
    QSaveFile report(QDir(directory_).filePath("automation.json"));if(report.open(QIODevice::WriteOnly)){report.write(QJsonDocument(evidence()).toJson());report.commit();}
    process_.closeWriteChannel();killTimer_.start();emit completed(success,reason);
    if(process_.state()==QProcess::NotRunning){busy_=false;emit changed();}
}
}
