#include "Editor.hpp"
#include "Automation.hpp"
#include "LocalMcpBridge.hpp"
#include <QAction>
#include <QLineEdit>
#include <QInputDialog>
#include <QSignalBlocker>
#include <QBuffer>
#include <QMovie>
#include <QDir>
#include <QActionGroup>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QBoxLayout>
#include <QImageReader>
#include <QImageWriter>
#include <QKeyEvent>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPlainTextEdit>
#include <QStandardPaths>
#include <QUuid>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSpinBox>
#include <QStatusBar>
#include <QToolBar>
#include <QToolButton>
#include <algorithm>
#include <cmath>

namespace pixelforge::qt {
namespace {
CanvasLimits limits() { return {1, 1, 4096, 4096, 4u * 1024u * 1024u}; }
bool fail(QString* error, const QString& message) { if(error) *error = message; return false; }
}
Canvas::Canvas(QWidget* parent) : QWidget(parent), document_(limits()) {
    setObjectName("pixelCanvas"); setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true); setCursor(Qt::CrossCursor);
    newImage(16,16);
}
void Canvas::syncImage() {
    image_ = QImage(document_.width(), document_.height(), QImage::Format_ARGB32);
    for(int y=0; y<document_.height(); ++y)
        std::copy_n(document_.pixels().data()+y*document_.width(), document_.width(),
                    reinterpret_cast<QRgb*>(image_.scanLine(y)));
    setFixedSize(document_.width()*zoom_, document_.height()*zoom_); update();
}
bool Canvas::newImage(int width, int height, QString* error) {
    cancelStroke(); std::string reason;
    if(!document_.resize(width,height,&reason)) return fail(error,QString::fromStdString(reason));
    undoCount_=redoCount_=0; syncImage(); emit documentChanged(); return true;
}
bool Canvas::openPng(const QString& path, QString* error) {
    QImageReader reader(path);reader.setDecideFormatFromContent(true);
    const auto format=reader.format().toLower();
    if(!QList<QByteArray>{"png","jpeg","jpg","bmp","gif","tiff","tif","webp","ico"}.contains(format))return fail(error,"Unsupported or unavailable raster image format.");
    if(QFileInfo(path).size()>80ll*1024*1024)return fail(error,"Encoded image exceeds the 80 MiB import bound.");
    const auto size=reader.size(); std::string reason;
    if(!size.isValid() || !document_.can_resize(size.width(),size.height(),&reason))
        return fail(error,reason.empty() ? "This is not a readable supported raster image." : QString::fromStdString(reason));
    QImage loaded=reader.read().convertToFormat(QImage::Format_ARGB32);
    if(loaded.isNull()) return fail(error,reader.errorString());
    // Decode completely before replacing the current document.
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(loaded.width())*loaded.height());
    for(int y=0;y<loaded.height();++y)
        std::copy_n(reinterpret_cast<const QRgb*>(loaded.constScanLine(y)),loaded.width(),pixels.data()+y*loaded.width());
    cancelStroke();
    if(!document_.replace_pixels(loaded.width(),loaded.height(),std::move(pixels),&reason))
        return fail(error,QString::fromStdString(reason));
    undoCount_=redoCount_=0; syncImage(); emit documentChanged(); return true;
}
bool Canvas::writePng(const QString& path, QString* error) {
    finishStroke();
    QSaveFile file(path); // Failed encode/commit never removes the previous file.
    if(!file.open(QIODevice::WriteOnly)) return fail(error,file.errorString());
    QImageWriter writer(&file,"png");
    if(!writer.write(image_)) { file.cancelWriting(); return fail(error,writer.errorString()); }
    if(!file.commit()) return fail(error,file.errorString());
    return true;
}
void Canvas::setColor(QColor color) { if(color.isValid()) { finishStroke(); color_=color; emit colorChanged(color); } }
void Canvas::setTool(Tool tool) { finishStroke(); tool_=tool; emit viewChanged(); }
void Canvas::setZoom(int zoom) { finishStroke(); zoom_=std::clamp(zoom,1,64); setFixedSize(document_.width()*zoom_,document_.height()*zoom_); update(); emit viewChanged(); }
void Canvas::setGrid(bool grid) { grid_=grid; update(); emit viewChanged(); }
void Canvas::undo() { cancelStroke(); if(document_.undo()) { --undoCount_; ++redoCount_; syncImage(); emit documentChanged(); } }
void Canvas::redo() { cancelStroke(); if(document_.redo()) { ++undoCount_; --redoCount_; syncImage(); emit documentChanged(); } }
void Canvas::cancelStroke() { if(stroke_) { stroke_->cancel(); stroke_.reset(); syncImage(); } }
void Canvas::finishStroke() {
    if(!stroke_) return;
    const auto before=document_.revision(); stroke_->commit(); stroke_.reset();
    if(document_.revision()!=before) { ++undoCount_; redoCount_=0; emit documentChanged(); }
    syncImage();
}
void Canvas::drawTo(QPoint pixel) {
    if(!stroke_) return;
    // Interpolate fast mouse movement; one drag remains one core undo transaction.
    pixel.setX(std::clamp(pixel.x(),0,document_.width()-1));
    pixel.setY(std::clamp(pixel.y(),0,document_.height()-1));
    int x=previous_.x(), y=previous_.y();
    const int dx=std::abs(pixel.x()-x), sx=x<pixel.x()?1:-1;
    const int dy=-std::abs(pixel.y()-y), sy=y<pixel.y()?1:-1;
    int err=dx+dy;
    for(;;) {
        stroke_->set_pixel(x,y,strokeColor_); image_.setPixel(x,y,strokeColor_);
        if(x==pixel.x() && y==pixel.y()) break;
        const int twice=2*err;
        if(twice>=dy) { err+=dy; x+=sx; }
        if(twice<=dx) { err+=dx; y+=sy; }
    }
    previous_=pixel; update();
}
void Canvas::paintEvent(QPaintEvent* event) {
    QPainter p(this); p.setClipRect(event->rect());
    // Only tile the visible area even when a large image is highly zoomed.
    const QRect r=event->rect().intersected(rect()); constexpr int tile=12;
    p.fillRect(r,QColor("#c8ccd2"));
    for(int y=r.top()/tile*tile;y<=r.bottom();y+=tile)
        for(int x=r.left()/tile*tile;x<=r.right();x+=tile)
            if((x/tile+y/tile)%2==0) p.fillRect(x,y,tile,tile,QColor("#e7e9ed"));
    p.setRenderHint(QPainter::SmoothPixmapTransform,false);
    p.drawImage(rect(),image_);
    if(grid_ && zoom_>=6) {
        p.setPen(QColor(18,24,34,65));
        for(int x=r.left()/zoom_*zoom_;x<=r.right();x+=zoom_) p.drawLine(x,r.top(),x,r.bottom());
        for(int y=r.top()/zoom_*zoom_;y<=r.bottom();y+=zoom_) p.drawLine(r.left(),y,r.right(),y);
    }
}
void Canvas::mousePressEvent(QMouseEvent* event) {
    if(event->button()!=Qt::LeftButton || !rect().contains(event->position().toPoint())) return;
    setFocus(); const QPoint point=QPoint(int(std::floor(event->position().x()/zoom_)),int(std::floor(event->position().y()/zoom_)));
    if(tool_==Tool::Picker) { setColor(QColor::fromRgba(document_.pixel(point.x(),point.y()))); return; }
    stroke_=std::make_unique<PixelDocument::Transaction>(document_);
    previous_=point; strokeColor_=tool_==Tool::Eraser ? 0u : color_.rgba(); drawTo(point);
}
void Canvas::mouseMoveEvent(QMouseEvent* event) {
    const QPoint point=QPoint(int(std::floor(event->position().x()/zoom_)),int(std::floor(event->position().y()/zoom_))); emit pixelHovered(point);
    if(stroke_) drawTo(point);
}
void Canvas::mouseReleaseEvent(QMouseEvent* event) {
    if(event->button()==Qt::LeftButton && stroke_) { drawTo(QPoint(int(std::floor(event->position().x()/zoom_)),int(std::floor(event->position().y()/zoom_)))); finishStroke(); }
}
void Canvas::keyPressEvent(QKeyEvent* event) { if(event->key()==Qt::Key_Escape) cancelStroke(); else QWidget::keyPressEvent(event); }
void Canvas::focusOutEvent(QFocusEvent* event) { finishStroke(); QWidget::focusOutEvent(event); }

Editor::Editor(QWidget* parent) : QMainWindow(parent) {
    setObjectName("pixelForgeEditor"); setMinimumSize(760,640); resize(1100,740);
    canvas_=new Canvas;
    automation_=new AutomationController(canvas_,this);client_=new AutomationClient(automation_,this);
    auto* file=menuBar()->addMenu("&File");
    auto action=[this](const QString& text,const QString& id,const QKeySequence& key) {
        auto* a=new QAction(text,this); a->setObjectName(id); a->setShortcut(key); return a;
    };
    auto* create=action("New…","newCanvas",QKeySequence::New);
    auto* open=action("Open image…","openPng",QKeySequence::Open);
    auto* save=action("Save PNG","savePng",QKeySequence::Save);
    auto* saveAs=action("Save PNG as…","savePngAs",QKeySequence::SaveAs);
    auto* exportPng=action("Export PNG copy…","exportPng",QKeySequence("Ctrl+Shift+E"));
    file->addActions({create,open,save,saveAs,exportPng});
    file->addSeparator();
    auto* openProject=action("Open project…","openProject",QKeySequence("Ctrl+Alt+O"));
    auto* saveProject=action("Save project…","saveProject",QKeySequence("Ctrl+Alt+S"));
    auto* exportProject=action("Export project…","exportProject",QKeySequence("Ctrl+Alt+E"));
    file->addActions({openProject,saveProject,exportProject});
    connect(openProject,&QAction::triggered,this,&Editor::openProjectDialog);
    connect(saveProject,&QAction::triggered,this,[this]{saveProjectDialog(true);});
    connect(exportProject,&QAction::triggered,this,&Editor::exportProjectDialog);
    documentActions_={create,open,openProject};
    file->addSeparator();
    auto* quit=action("Quit","quit",QKeySequence::Quit); file->addAction(quit);
    connect(quit,&QAction::triggered,this,&QWidget::close);
    connect(create,&QAction::triggered,this,&Editor::newDialog);
    connect(open,&QAction::triggered,this,&Editor::openDialog);
    connect(save,&QAction::triggered,this,[this]{saveDialog(false);});
    connect(saveAs,&QAction::triggered,this,[this]{saveDialog(true);});
    connect(exportPng,&QAction::triggered,this,[this]{
        QString path=QFileDialog::getSaveFileName(this,"Export PNG copy",path_.isEmpty()?"sprite.png":path_,"PNG image (*.png)");
        if(path.isEmpty()) return;
        if(!path.endsWith(".png",Qt::CaseInsensitive)) path+=".png";
        QString error; if(!canvas_->writePng(path,&error)) QMessageBox::warning(this,"Export failed",error);
        else statusBar()->showMessage("PNG copy exported",4000);
    });
    auto* edit=menuBar()->addMenu("&Edit");
    undo_=action("Undo","undo",QKeySequence::Undo); redo_=action("Redo","redo",QKeySequence::Redo);
    edit->addActions({undo_,redo_});
    connect(undo_,&QAction::triggered,this,[this]{if(automation_->project().canvases.isEmpty())canvas_->undo();else{QString why;if(!automation_->history("undo",&why))statusBar()->showMessage(why,4000);}}); connect(redo_,&QAction::triggered,this,[this]{if(automation_->project().canvases.isEmpty())canvas_->redo();else{QString why;if(!automation_->history("redo",&why))statusBar()->showMessage(why,4000);}});
    auto* toolbar=addToolBar("File and history"); toolbar->setObjectName("mainToolbar"); toolbar->setMovable(false);
    toolbar->addActions({create,open,save}); toolbar->addSeparator(); toolbar->addActions({undo_,redo_});

    auto* root=new QWidget; auto* layout=new QHBoxLayout(root); layout->setSizeConstraint(QLayout::SetMinimumSize); layout->setContentsMargins(12,12,12,12); layout->setSpacing(12);
    auto* panel=new QWidget; panel->setObjectName("toolsPanel"); panel->setFixedWidth(160);
    auto* side=new QVBoxLayout(panel); side->setContentsMargins(0,0,0,0); side->setSpacing(8);
    auto heading=[side](const QString& text) { auto* label=new QLabel(text); label->setStyleSheet("font-weight:600; color:#aab8ca; margin-top:6px"); side->addWidget(label); };
    heading("TOOLS"); auto* tools=new QActionGroup(this); tools->setExclusive(true);
    const QStringList toolNames={"Pencil","Eraser","Pick colour"};
    const QStringList keys={"P","E","I"};
    for(int i=0;i<3;++i) {
        auto* a=action(toolNames[i],QString("tool%1").arg(i),QKeySequence(keys[i])); a->setCheckable(true); a->setChecked(i==0); tools->addAction(a); addAction(a);
        a->setToolTip(toolNames[i]+" ("+keys[i]+")");
        auto* button=new QToolButton; button->setObjectName(QString("toolButton%1").arg(i)); button->setDefaultAction(a); button->setToolButtonStyle(Qt::ToolButtonTextOnly); button->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed); button->setMinimumHeight(34); side->addWidget(button);
        connect(a,&QAction::triggered,this,[this,i]{canvas_->setTool(static_cast<Canvas::Tool>(i));});
    }
    heading("COLOUR"); color_=new QPushButton; color_->setObjectName("currentColor"); color_->setMinimumHeight(38); side->addWidget(color_);
    connect(color_,&QPushButton::clicked,this,[this]{const auto c=QColorDialog::getColor(canvas_->color(),this,"Drawing colour",QColorDialog::ShowAlphaChannel); if(c.isValid()) canvas_->setColor(c);});
    auto* palette=new QGridLayout; palette->setSpacing(5);
    const QStringList colors={"#17202e","#ffffff","#8b95a7","#eb546c","#f4a64c","#f9d85c","#58c478","#32b8b1","#4ba6e8","#7569e7","#c576d7","#805742"};
    for(int i=0;i<colors.size();++i) {
        auto* b=new QPushButton; b->setObjectName(QString("swatch%1").arg(i)); b->setFixedSize(34,28); b->setToolTip(colors[i]); b->setAccessibleName("Colour "+colors[i]);
        b->setStyleSheet("background:"+colors[i]+"; border:1px solid #8e9bad; border-radius:4px;");
        b->setProperty("argb",QColor(colors[i]).rgba());palette->addWidget(b,i/4,i%4); connect(b,&QPushButton::clicked,this,[this,b]{canvas_->setColor(QColor::fromRgba(b->property("argb").toUInt()));});
    }
    side->addLayout(palette);
    auto* fullPalette=new QPushButton("All palette colours…");fullPalette->setObjectName("fullPalette");side->addWidget(fullPalette);
    connect(fullPalette,&QPushButton::clicked,this,[this]{QStringList entries;int index=0;for(auto c:automation_->palette())entries<<QString("%1 · #%2").arg(index++).arg(c,8,16,QChar('0'));bool ok=false;const auto selected=QInputDialog::getItem(this,"Current drawing palette","Colour",entries,0,false,&ok);if(ok){const auto i=entries.indexOf(selected);if(i>=0)canvas_->setColor(QColor::fromRgba(automation_->palette()[i]));}});
    side->addStretch();
    auto* hint=new QLabel("P  Pencil   E  Eraser\nI  Pick colour\nEsc  Cancel stroke"); hint->setObjectName("shortcutHint"); hint->setStyleSheet("color:#aab8ca;font-size:11px;"); side->addWidget(hint);
    layout->addWidget(panel);
    auto* center=new QVBoxLayout; center->setSpacing(8);
    auto* view=new QHBoxLayout; view->addWidget(new QLabel("CANVAS")); view->addStretch();
    auto* grid=action("Grid","grid",QKeySequence("G")); grid->setCheckable(true); grid->setChecked(true); addAction(grid);
    auto* gridButton=new QToolButton; gridButton->setObjectName("gridButton"); gridButton->setDefaultAction(grid); view->addWidget(gridButton);
    connect(grid,&QAction::toggled,canvas_,&Canvas::setGrid);
    zoom_=new QComboBox; zoom_->setObjectName("zoomCombo");
    for(int scale:{1,2,4,8,16,24,32,64}) zoom_->addItem(QString::number(scale*100)+"%",scale);
    zoom_->setCurrentIndex(5); zoom_->setAccessibleName("Canvas zoom"); view->addWidget(zoom_);
    connect(zoom_,&QComboBox::activated,this,[this](int i){canvas_->setZoom(zoom_->itemData(i).toInt());});
    auto* fit=new QPushButton("Fit"); fit->setObjectName("fitCanvas"); view->addWidget(fit); connect(fit,&QPushButton::clicked,this,&Editor::fitCanvas);
    center->addLayout(view);
    auto* workspace=new QHBoxLayout;
    groups_=new QComboBox;groups_->setObjectName("canvasGroup");groups_->setMinimumContentsLength(8);workspace->addWidget(groups_);
    canvases_=new QComboBox;canvases_->setObjectName("canvasSelector");canvases_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);canvases_->setMinimumContentsLength(12);workspace->addWidget(canvases_,1);
    auto* addCanvas=new QPushButton("+ Canvas");addCanvas->setObjectName("addPackCanvas");workspace->addWidget(addCanvas);connect(addCanvas,&QPushButton::clicked,this,&Editor::addCanvasDialog);
    auto* preview=new QPushButton("Play frames");preview->setObjectName("previewFrames");workspace->addWidget(preview);connect(preview,&QPushButton::clicked,this,&Editor::previewAnimation);
    connect(groups_,&QComboBox::activated,this,[this]{updateWorkspace();});
    connect(canvases_,&QComboBox::activated,this,[this](int i){QString why;if(!automation_->selectCanvas(canvases_->itemData(i).toString(),&why))statusBar()->showMessage(why,4000);else fitCanvas();});
    center->addLayout(workspace);
    scroll_=new QScrollArea; scroll_->setObjectName("canvasScroll"); scroll_->setMinimumHeight(224); scroll_->setWidget(canvas_); scroll_->setAlignment(Qt::AlignCenter); scroll_->setWidgetResizable(false); center->addWidget(scroll_,1);
    auto* refs=new QHBoxLayout;auto* refSlot=new QComboBox;refSlot->setObjectName("referenceSlot");refSlot->addItems({"source","content","style"});refs->addWidget(new QLabel("Reference"));refs->addWidget(refSlot);
    auto* loadRef=new QPushButton("Choose image…");loadRef->setObjectName("loadReference");refs->addWidget(loadRef);
    auto* clearRef=new QPushButton("Clear");clearRef->setObjectName("clearReference");refs->addWidget(clearRef);refs->addStretch();center->addLayout(refs);
    connect(loadRef,&QPushButton::clicked,this,[this,refSlot]{if(client_->busy())return;const auto path=QFileDialog::getOpenFileName(this,"Select "+refSlot->currentText()+" reference",QString(),"Raster images (*.png *.jpg *.jpeg *.bmp *.gif *.tif *.tiff *.webp *.ico)");if(path.isEmpty())return;QString why;if(!automation_->loadReference(refSlot->currentText(),path,&why))QMessageBox::warning(this,"Reference failed",why);else statusBar()->showMessage(refSlot->currentText()+" reference ready",4000);});
    connect(clearRef,&QPushButton::clicked,this,[this,refSlot]{if(!client_->busy())automation_->clearReference(refSlot->currentText());});
    questionPanel_=new QWidget;questionPanel_->setObjectName("agentQuestionPanel");
    auto* questionLayout=new QVBoxLayout(questionPanel_);questionLayout->setContentsMargins(0,0,0,0);
    question_=new QLabel;question_->setWordWrap(true);question_->setTextFormat(Qt::PlainText);question_->setObjectName("agentQuestion");questionLayout->addWidget(question_);
    auto* answerRow=new QHBoxLayout;answer_=new QLineEdit;answer_->setObjectName("agentAnswer");answer_->setMaxLength(8000);answerRow->addWidget(answer_,1);
    auto* answerButton=new QPushButton("Answer");answerButton->setObjectName("answerAgent");answerRow->addWidget(answerButton);questionLayout->addLayout(answerRow);center->addWidget(questionPanel_);questionPanel_->hide();
    auto answerAction=[this]{if(!answer_->text().trimmed().isEmpty()){if(client_->busy())client_->answerQuestion(answer_->text());else if(bridge_)bridge_->answerQuestion(answer_->text());if(!automation_->state()["awaiting_user_input"].toBool())answer_->clear();}};
    connect(answerButton,&QPushButton::clicked,this,answerAction);connect(answer_,&QLineEdit::returnPressed,this,answerAction);
    auto* generationRow=new QHBoxLayout;
    prompt_=new QPlainTextEdit;prompt_->setObjectName("generationPrompt");prompt_->setPlaceholderText("Describe what you want the pixel artist to create…");prompt_->setMaximumHeight(70);prompt_->setTabChangesFocus(true);
    generationRow->addWidget(prompt_,1);
    auto* generationButtons=new QVBoxLayout;
    generate_=new QPushButton("Generate");generate_->setObjectName("generate");
    stop_=new QPushButton("Stop");stop_->setObjectName("stopGeneration");stop_->setEnabled(false);
    generationButtons->addWidget(generate_);generationButtons->addWidget(stop_);generationRow->addLayout(generationButtons);center->addLayout(generationRow);
    automation_->setRecordingSource([this]{return grab().toImage();});
    bridge_=new LocalMcpBridge(automation_,[this]{return client_->busy();},this);
    auto* bridgeAction=new QAction("Enable local MCP bridge",this);bridgeAction->setObjectName("enableMcp");bridgeAction->setCheckable(true);auto* automationMenu=menuBar()->addMenu("Automation");automationMenu->addAction(bridgeAction);
    auto* recordAllowed=new QAction("Allow this session to be recorded",this);recordAllowed->setObjectName("allowRecording");recordAllowed->setCheckable(true);automationMenu->addAction(recordAllowed);
    connect(recordAllowed,&QAction::toggled,this,[this](bool enabled){automation_->setRecordingAllowed(enabled);if(!enabled)automation_->recordingControl("stop");});
    auto* recordNow=new QAction("Start recording editor",this);recordNow->setObjectName("startRecording");automationMenu->addAction(recordNow);
    connect(recordNow,&QAction::triggered,this,[this,recordAllowed]{if(!recordAllowed->isChecked()){QMessageBox::information(this,"Recording disabled","Enable session recording first. It captures the editor, including its prompt, but no other windows.");return;}if(automation_->state()["task_id"].toInteger()==0){const auto dir=QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)+"/recordings/"+QUuid::createUuid().toString(QUuid::WithoutBraces);QDir().mkpath(dir);automation_->begin("Manual recording",dir);}const auto r=automation_->recordingControl("start");if(!r.success)QMessageBox::warning(this,"Recording failed",r.facts["error"].toString());});
    auto* stopRecord=new QAction("Stop and save recording",this);stopRecord->setObjectName("stopRecording");automationMenu->addAction(stopRecord);connect(stopRecord,&QAction::triggered,this,[this]{automation_->recordingControl("stop");});
    connect(bridgeAction,&QAction::toggled,this,[this,bridgeAction](bool enabled){
        if(!enabled){bridge_->stop();return;}QString why;
        if(!bridge_->start(&why)){bridgeAction->setChecked(false);QMessageBox::warning(this,"MCP bridge failed",why);return;}
        auto* details=new QMessageBox(QMessageBox::Information,"Local MCP bridge","This user-only bridge controls this editor without starting a provider. Turn it off to disconnect external clients.",QMessageBox::Ok,this);
        details->setAttribute(Qt::WA_DeleteOnClose);details->setDetailedText("\""+QCoreApplication::applicationFilePath()+"\" --mcp --socket "+bridge_->name());details->show();
    });
    auto* settings=new QHBoxLayout;
    model_=new QComboBox;model_->setObjectName("generationModel");model_->setEditable(true);model_->addItem("gpt-6-astra");settings->addWidget(model_);
    effort_=new QComboBox;effort_->setObjectName("generationEffort");effort_->addItems({"low","medium","high","xhigh","max","ultra"});effort_->setCurrentText("medium");settings->addWidget(effort_);
    settings->addStretch();accept_=new QPushButton("Accept artwork");accept_->setObjectName("acceptArtwork");accept_->setVisible(false);settings->addWidget(accept_);center->addLayout(settings);
    progress_=new QLabel("Generate uses your installed Codex login. Nothing runs until you click Generate.");progress_->setObjectName("generationProgress");progress_->setWordWrap(true);progress_->setMaximumHeight(56);progress_->setTextFormat(Qt::PlainText);center->addWidget(progress_);
    connect(generate_,&QPushButton::clicked,this,&Editor::generate);
    connect(stop_,&QPushButton::clicked,this,[this]{if(client_->busy())client_->stop();else if(bridge_)bridge_->stop();});
    connect(accept_,&QPushButton::clicked,this,[this]{if(automation_->acceptReview()){modified_=false;updateStatus();updateAutomation();}else QMessageBox::warning(this,"Project acceptance failed","The project could not be saved. Artwork is retained; save the project to a writable directory before accepting.");});
    connect(client_,&AutomationClient::changed,this,&Editor::updateAutomation);
    connect(client_,&AutomationClient::changed,this,&Editor::finishCloseWhenSafe);
    connect(automation_,&AutomationController::recordingFinished,this,[this]{finishCloseWhenSafe();});
    connect(automation_,&AutomationController::changed,this,&Editor::updateAutomation);
    connect(automation_,&AutomationController::persistenceFailed,this,[this](const QString& why){statusBar()->showMessage("Artwork retained; autosave failed: "+why);});
    connect(automation_,&AutomationController::changed,this,&Editor::updateWorkspace);
    layout->addLayout(center,1); setCentralWidget(root);
    status_=new QLabel; status_->setObjectName("documentStatus"); statusBar()->addWidget(status_,1);
    connect(canvas_,&Canvas::documentChanged,this,[this]{modified_=true; if(!contextChange_)automation_->manualChanged();updateStatus();});
    connect(canvas_,&Canvas::viewChanged,this,&Editor::updateStatus);
    connect(canvas_,&Canvas::colorChanged,this,&Editor::updateColor);
    connect(canvas_,&Canvas::pixelHovered,this,[this](QPoint p){ if(p.x()>=0 && p.y()>=0 && p.x()<canvas_->document().width() && p.y()<canvas_->document().height()) statusBar()->showMessage(QString("Pixel %1, %2").arg(p.x()).arg(p.y()),1500); });
    setStyleSheet("QMainWindow,QWidget{background:#19212e;color:#e5eaf2;font-size:13px;} QMenuBar,QMenu,QToolBar{background:#222d3d;} QToolBar{spacing:6px;padding:5px;border-bottom:1px solid #344258;} QToolButton,QPushButton,QComboBox,QSpinBox{background:#2a374a;border:1px solid #42526b;border-radius:5px;padding:6px;} QToolButton:hover,QPushButton:hover{background:#384b63;} QToolButton:checked{background:#195c68;border:1px solid #62ded7;} QToolButton:disabled{color:#8190a4;} QScrollArea{background:#101722;border:1px solid #344258;border-radius:6px;} QStatusBar{background:#222d3d;} QMenu::item:selected{background:#195c68;} ");
    updateColor(canvas_->color()); updateStatus();updateWorkspace();
}
Editor::~Editor(){delete bridge_;bridge_=nullptr; delete client_;client_=nullptr;delete automation_;automation_=nullptr; }
void Editor::generate(){
    const auto text=prompt_->toPlainText().trimmed();if(text.isEmpty()||client_->busy())return;
    if(automation_->state()["awaiting_user_review"].toBool()) {
        if(!automation_->requestChanges(text))return;
    } else if(modified_ && automation_->project().canvases.isEmpty() && !askToSave())return;
    const auto directory=QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)+"/generated/"+QUuid::createUuid().toString(QUuid::WithoutBraces);
    client_->start(text,model_->currentText().trimmed(),effort_->currentText(),directory);
    updateAutomation();
}
void Editor::updateAutomation(){
    if(!client_)return;const bool busy=client_->busy();
    const auto& colors=automation_->palette();for(int i=0;i<12;++i)if(auto* b=findChild<QPushButton*>(QString("swatch%1").arg(i))){b->setVisible(i<colors.size());if(i<colors.size()){const QColor c=QColor::fromRgba(colors[i]);b->setProperty("argb",colors[i]);b->setToolTip(c.name(QColor::HexArgb));b->setStyleSheet("background:"+c.name()+";border:1px solid #8e9bad;border-radius:4px;");}}
    stop_->setEnabled(busy||(bridge_&&bridge_->awaitingAnswer()));generate_->setEnabled(!busy&&!automation_->state()["awaiting_user_input"].toBool());model_->setEnabled(!busy);effort_->setEnabled(!busy);
    for(auto* a:documentActions_)a->setEnabled(!busy);
    canvas_->setEnabled(!busy);undo_->setEnabled(!busy&&(canvas_->canUndo()||!automation_->project().canvases.isEmpty()));redo_->setEnabled(!busy&&(canvas_->canRedo()||!automation_->project().canvases.isEmpty()));
    findChild<QPushButton*>("loadReference")->setEnabled(!busy);findChild<QPushButton*>("clearReference")->setEnabled(!busy);
    canvases_->setEnabled(!busy);groups_->setEnabled(!busy);findChild<QPushButton*>("addPackCanvas")->setEnabled(!busy);
    const auto state=automation_->state();questionPanel_->setVisible(state["awaiting_user_input"].toBool());
    question_->setText(state["question"].toString()+(state["suggestions"].toString().isEmpty()?QString():"\n"+state["suggestions"].toString().replace('|'," · ")));
    const bool review=automation_->state()["awaiting_user_review"].toBool();accept_->setVisible(review);accept_->setEnabled(!busy&&!automation_->recordingState()["process_running"].toBool());
    generate_->setText(review?"Request changes":"Generate");
    if(!client_->status().isEmpty())progress_->setText(client_->status().left(500));
    const auto recording=automation_->recordingState();
    if(recording["process_running"].toBool()||recording["saved"].toBool()||recording["state"]=="failed")progress_->setText(progress_->text().section("\nRecording:",0,0)+"\nRecording: "+recording["state"].toString()+" · "+QString::number(recording["frames_submitted"].toInteger())+" frames · "+QString::number(recording["dropped_frames"].toInteger())+" dropped"+(recording["error"].toString().isEmpty()?QString():" · "+recording["error"].toString()));
}
void Editor::updateColor(QColor c) {
    color_->setText(c.name(c.alpha()==255?QColor::HexRgb:QColor::HexArgb));
    const QString text=c.lightness()>140?"#17202e":"#ffffff";
    color_->setStyleSheet("background:"+c.name()+";color:"+text+";border:2px solid #d4e2ec;");
}
void Editor::updateStatus() {
    setWindowTitle(QString("%1%2 — PixelForge Qt").arg(path_.isEmpty()?"Untitled":QFileInfo(path_).fileName(),modified_?" *":""));
    const QStringList tools={"Pencil","Eraser","Pick colour"};
    status_->setText(QString("%1 × %2 px  ·  %3  ·  %4%  ·  %5").arg(canvas_->document().width()).arg(canvas_->document().height()).arg(tools[static_cast<int>(canvas_->tool())]).arg(canvas_->zoom()*100).arg(modified_?"Unsaved":"PNG canvas"));
    undo_->setEnabled(!client_->busy()&&(canvas_->canUndo()||!automation_->project().canvases.isEmpty())); redo_->setEnabled(!client_->busy()&&(canvas_->canRedo()||!automation_->project().canvases.isEmpty()));
}
void Editor::fitCanvas() {
    const auto size=scroll_->viewport()->size()-QSize(24,24);
    const int scale=std::clamp(std::min(size.width()/canvas_->document().width(),size.height()/canvas_->document().height()),1,64);
    int index=zoom_->findData(scale); if(index<0) {zoom_->addItem(QString::number(scale*100)+"%",scale); index=zoom_->count()-1;}
    zoom_->setCurrentIndex(index); canvas_->setZoom(scale);
}
bool Editor::openFile(const QString& path, QString* error) {
    if(client_->busy())return fail(error,"Stop automation before opening another document.");
    contextChange_=true;const bool opened=canvas_->openPng(path,error);contextChange_=false;if(!opened)return false;
    automation_->resetWorkspace();projectPath_.clear();
    path_=QFileInfo(path).suffix().compare("png",Qt::CaseInsensitive)==0?path:QString(); modified_=false; fitCanvas(); updateStatus(); return true;
}
bool Editor::saveFile(const QString& path, QString* error) {
    if(!canvas_->writePng(path,error)) return false;
    path_=path; modified_=!automation_->project().canvases.isEmpty(); updateStatus(); return true;
}
bool Editor::saveDialog(bool choosePath) {
    QString path=path_;
    if(choosePath || path.isEmpty()) path=QFileDialog::getSaveFileName(this,"Save PNG",path.isEmpty()?"sprite.png":path,"PNG image (*.png)");
    if(path.isEmpty()) return false;
    if(!path.endsWith(".png",Qt::CaseInsensitive)) path+=".png";
    QString error; if(!saveFile(path,&error)) {QMessageBox::warning(this,"Save failed",error);return false;}
    return true;
}
bool Editor::askToSave() {
    if(!modified_) return true;
    const auto answer=QMessageBox::warning(this,"Unsaved pixels","Save your changes before continuing?",QMessageBox::Save|QMessageBox::Discard|QMessageBox::Cancel,QMessageBox::Save);
    return answer==QMessageBox::Discard || (answer==QMessageBox::Save && (automation_->project().canvases.isEmpty()?saveDialog(false):saveProjectDialog(false)));
}
void Editor::newDialog() {
    QDialog dialog(this); dialog.setWindowTitle("New canvas"); auto* form=new QFormLayout(&dialog);
    QSpinBox width,height; width.setObjectName("newWidth"); height.setObjectName("newHeight"); width.setRange(1,4096);height.setRange(1,4096);width.setValue(16);height.setValue(16);
    form->addRow("Width (pixels)",&width);form->addRow("Height (pixels)",&height);
    QLabel limit("Transparent canvas · up to 4,194,304 pixels");form->addRow(&limit);
    QDialogButtonBox buttons(QDialogButtonBox::Ok|QDialogButtonBox::Cancel); form->addRow(&buttons);
    connect(&buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(&buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    if(dialog.exec()!=QDialog::Accepted || !askToSave()) return;
    contextChange_=true;QString error;const bool created=canvas_->newImage(width.value(),height.value(),&error);contextChange_=false;
    if(!created){QMessageBox::warning(this,"Cannot create canvas",error);return;}
    automation_->resetWorkspace();projectPath_.clear();
    path_.clear();modified_=false;fitCanvas();updateStatus();
}
void Editor::openDialog() {
    const auto path=QFileDialog::getOpenFileName(this,"Open image",QString(),"Raster images (*.png *.jpg *.jpeg *.bmp *.gif *.tif *.tiff *.webp *.ico)");
    if(path.isEmpty() || !askToSave()) return;
    QString error; if(!openFile(path,&error)) QMessageBox::warning(this,"Open failed",error);
}
void Editor::updateWorkspace(){
    if(!canvases_||!groups_)return;
    QSignalBlocker bg(groups_),bc(canvases_);const auto group=groups_->currentData().toString();
    groups_->clear();groups_->addItem("All groups","");QStringList seen;
    for(const auto& c:automation_->project().canvases)if(!c.group.isEmpty()&&!seen.contains(c.group)){seen<<c.group;groups_->addItem(c.group,c.group);}
    groups_->setCurrentIndex(std::max(0,groups_->findData(group)));canvases_->clear();
    for(const auto& c:automation_->project().canvases)if(group.isEmpty()||c.group==group)canvases_->addItem(c.name+(c.frame>=0?QString(" · frame %1").arg(c.frame):""),c.name);
    const auto selected=canvases_->findData(automation_->selectedCanvas());if(selected>=0)canvases_->setCurrentIndex(selected);
    if(canvases_->count()==0)canvases_->addItem("Single canvas","");
}
bool Editor::saveProjectDialog(bool choosePath){
    QString directory=projectPath_;if(choosePath||directory.isEmpty())directory=QFileDialog::getExistingDirectory(this,"Save project folder",directory);
    if(directory.isEmpty())return false;QString why;
    if(!automation_->saveProject(directory,&why)){QMessageBox::warning(this,"Save project failed",why);return false;}
    projectPath_=directory;modified_=false;updateStatus();return true;
}
void Editor::openProjectDialog(){
    const auto path=QFileDialog::getOpenFileName(this,"Open PixelForge project",QString(),"PixelForge project (project.json)");
    if(path.isEmpty()||!askToSave())return;QString why;
    contextChange_=true;const bool ok=automation_->loadProject(path,&why);contextChange_=false;
    if(!ok){QMessageBox::warning(this,"Open project failed",why);return;}
    projectPath_=QFileInfo(path).absolutePath();path_.clear();modified_=false;updateWorkspace();fitCanvas();updateStatus();
}
void Editor::addCanvasDialog(){
    if(client_->busy())return;QDialog dialog(this);dialog.setWindowTitle("Add canvas / animation frame");QFormLayout form(&dialog);
    QLineEdit name,group;name.setText("frame"+QString::number(automation_->project().canvases.size()+1));QSpinBox width,height,frame;
    width.setRange(1,4096);height.setRange(1,4096);frame.setRange(-1,1000000);width.setValue(canvas_->image().width());height.setValue(canvas_->image().height());frame.setValue(-1);
    form.addRow("Name",&name);form.addRow("Group (optional)",&group);form.addRow("Frame (-1 for still)",&frame);form.addRow("Width",&width);form.addRow("Height",&height);
    QDialogButtonBox buttons(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);form.addRow(&buttons);connect(&buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(&buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    if(dialog.exec()!=QDialog::Accepted)return;
    const auto r=automation_->hostPack({{"action","add"},{"canvases",QString("%1,%2,%3,%4,%5").arg(name.text(),group.text()).arg(width.value()).arg(height.value()).arg(frame.value())}});
    if(!r.success)QMessageBox::warning(this,"Add canvas refused",r.facts["message"].toString(r.facts["error"].toString()));
    else{automation_->selectCanvas(name.text());fitCanvas();modified_=true;}
}
void Editor::exportProjectDialog(){
    if(automation_->project().canvases.isEmpty()){QMessageBox::information(this,"Project export","Save a project or add canvases first; single PNG export is available from File.");return;}
    bool ok=false;const auto mode=QInputDialog::getItem(this,"Export project","Format",{"canvas","sheet","strip","group","all","animation","timeline"},0,false,&ok);if(!ok)return;
    const auto directory=QFileDialog::getExistingDirectory(this,"Export destination");if(directory.isEmpty())return;
    QJsonObject args{{"action","export"},{"scope",mode},{"scale",1},{"fps",12}};
    if(mode=="canvas")args["canvas"]=automation_->selectedCanvas();
    if(!groups_->currentData().toString().isEmpty())args["group"]=groups_->currentData().toString();
    const auto r=automation_->hostPack(args,directory);
    if(!r.success)QMessageBox::warning(this,"Export failed",r.facts["message"].toString(r.facts["error"].toString()));
    else statusBar()->showMessage("Export saved in "+directory,5000);
}
void Editor::previewAnimation(){
    if(automation_->project().canvases.isEmpty())return;
    QJsonObject args{{"action","view"},{"mode","animation"},{"fps",12},{"scale",std::min(8,canvas_->zoom())}};
    if(!groups_->currentData().toString().isEmpty())args["group"]=groups_->currentData().toString();
    const auto r=automation_->hostPack(args);if(!r.success){QMessageBox::warning(this,"Animation preview",r.facts["message"].toString(r.facts["error"].toString()));return;}
    auto* dialog=new QDialog(this);dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setWindowTitle("Actual project frames · 12 fps");auto* layout=new QVBoxLayout(dialog);auto* label=new QLabel;label->setAlignment(Qt::AlignCenter);layout->addWidget(label);
    auto* buffer=new QBuffer(dialog);buffer->setData(r.image);buffer->open(QIODevice::ReadOnly);auto* movie=new QMovie(buffer,"gif",dialog);label->setMovie(movie);movie->start();dialog->show();
}
void Editor::finishCloseWhenSafe(){
    if(!closingPending_||client_->busy()||automation_->recordingState()["process_running"].toBool())return;
    closingPending_=false;QTimer::singleShot(0,this,[this]{close();});
}
void Editor::closeEvent(QCloseEvent* event) {
    if(client_->busy()||automation_->recordingState()["process_running"].toBool()){
        closingPending_=true;client_->stop();automation_->recordingControl("stop");event->ignore();return;
    }
    if(askToSave())event->accept();else event->ignore();
}
}
