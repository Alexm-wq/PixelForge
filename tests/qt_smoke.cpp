#include "Editor.hpp"
#include "Automation.hpp"
#include <QMovie>
#include <QLabel>
#include <QPlainTextEdit>
#include <QtTest>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QPushButton>
#include <QScrollArea>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QMessageBox>
#include <QLineEdit>
#include <QSettings>
using namespace pixelforge::qt;

class EditorSmoke final : public QObject {
    Q_OBJECT
    QString evidence_;
    QTemporaryDir settings_;
    void capture(Editor& editor, const QString& name) {
        if(!evidence_.isEmpty()) QVERIFY(editor.grab().save(evidence_+"/"+name+".png"));
    }
    void stroke(Canvas* c,QPoint from,QPoint to) {
        const auto pos=[c](QPoint p){return p*c->zoom()+QPoint(c->zoom()/2,c->zoom()/2);};
        QTest::mousePress(c,Qt::LeftButton,Qt::NoModifier,pos(from));
        QTest::mouseMove(c,pos(to),15);
        QTest::mouseRelease(c,Qt::LeftButton,Qt::NoModifier,pos(to));
    }
    void tool(Editor& editor,int index) {
        auto* b=editor.findChild<QToolButton*>(QString("toolButton%1").arg(index)); QVERIFY(b); QTest::mouseClick(b,Qt::LeftButton);
    }
    void fileAction(Editor& editor,const QString& action,const QString& path) {
        qInfo() << "File control" << action << QFileInfo(path).fileName();
        bool observed=false;
        QTimer watchdog;
        watchdog.setSingleShot(true);
        connect(&watchdog,&QTimer::timeout,&editor,[&]{
            if(auto* d=qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
                qWarning()<<"Unexpected pending dialog"<<d->metaObject()->className()<<d->windowTitle();
                if(!evidence_.isEmpty()) d->grab().save(evidence_+"/pending-dialog.png");
                d->reject();
            }
        });
        watchdog.start(3000);
        QTimer::singleShot(50,&editor,[&]{
            auto* dialog=qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
            if(!dialog) return;
            auto* filename=dialog->findChild<QLineEdit*>("fileNameEdit");
            if(!filename) { dialog->reject(); return; }
            // Set the visible filename control after the dialog has restored its
            // remembered selection; selectFile() alone can retain an old name.
            observed=true; filename->setText(path);
            qInfo()<<"Selected file"<<dialog->selectedFiles();
            QMetaObject::invokeMethod(dialog,"accept",Qt::DirectConnection);
        });
        auto* a=editor.findChild<QAction*>(action); QVERIFY(a); a->trigger(); watchdog.stop(); QVERIFY(observed);
    }
private slots:
    void initTestCase() {
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        QVERIFY(settings_.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings_.path());
        evidence_=qEnvironmentVariable("PIXELFORGE_EVIDENCE_DIR");
    }
    void compactLayoutRetainsReadableControls() {
        Editor editor; editor.show(); QVERIFY(QTest::qWaitForWindowExposed(&editor));
        auto* scroll=editor.findChild<QScrollArea*>("canvasScroll"); QVERIFY(scroll);
        auto* prompt=editor.findChild<QPlainTextEdit*>("generationPrompt"); QVERIFY(prompt);
        prompt->setPlainText("Retain this unsent draft while resizing.");
        const auto image=editor.canvas()->image();
        const auto rectInEditor=[&](QWidget* widget){return QRect(widget->mapTo(&editor,QPoint()),widget->size());};
        const QList<QSize> sizes={QSize(640,440),QSize(760,640),QSize(1100,740)};
        for(const auto requested:sizes) {
            editor.resize(requested); QTest::qWait(60);
            QVERIFY(editor.width()>=760); QVERIFY(editor.height()>=640);
            QVERIFY(scroll->viewport()->width()>=400); QVERIFY(scroll->viewport()->height()>=220);
            QList<QWidget*> controls;
            for(int i=0;i<12;++i) {
                auto* swatch=editor.findChild<QPushButton*>(QString("swatch%1").arg(i)); QVERIFY(swatch);
                QCOMPARE(swatch->size(),QSize(34,28)); controls<<swatch;
            }
            for(const auto name:{"fullPalette","currentColor","shortcutHint","documentStatus"}) {
                auto* widget=editor.findChild<QWidget*>(name); QVERIFY(widget); controls<<widget;
                QVERIFY(widget->height()>=widget->sizeHint().height());
            }
            for(int i=0;i<controls.size();++i) {
                const auto rect=rectInEditor(controls[i]); QVERIFY(editor.rect().contains(rect));
                for(int j=0;j<i;++j) QVERIFY(!rect.intersects(rectInEditor(controls[j])));
            }
            auto* fit=editor.findChild<QPushButton*>("fitCanvas"); QTest::mouseClick(fit,Qt::LeftButton);
            QVERIFY(editor.canvas()->width()<=scroll->viewport()->width());
            QVERIFY(editor.canvas()->height()<=scroll->viewport()->height());
            QCOMPARE(editor.canvas()->image(),image);
            QCOMPARE(prompt->toPlainText(),QString("Retain this unsent draft while resizing."));
            capture(editor,QString("layout-request-%1x%2-actual-%3x%4").arg(requested.width()).arg(requested.height()).arg(editor.width()).arg(editor.height()));
            qInfo()<<"Layout"<<requested<<"accepted"<<editor.size()<<"viewport"<<scroll->viewport()->size();
        }
        editor.close(); QVERIFY(!editor.isVisible());
    }
    void real16PixelWorkflow() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        Editor editor; editor.show(); QVERIFY(QTest::qWaitForWindowExposed(&editor));
        auto* c=editor.canvas(); QCOMPARE(c->document().width(),16); QCOMPARE(c->document().height(),16);
        capture(editor,"first-qt-normal"); editor.resize(760,560); QTest::qWait(60); capture(editor,"first-qt-compact");
        auto* scroll=editor.findChild<QScrollArea*>("canvasScroll"); QVERIFY(scroll); QVERIFY(scroll->viewport()->width()>400);
        auto* fit=editor.findChild<QPushButton*>("fitCanvas"); QVERIFY(fit); QTest::mouseClick(fit,Qt::LeftButton);
        QVERIFY(c->width()<=scroll->viewport()->width()); QVERIFY(c->height()<=scroll->viewport()->height());
        auto* red=editor.findChild<QPushButton*>("swatch3"); QVERIFY(red); QTest::mouseClick(red,Qt::LeftButton);
        const auto ink=QColor("#eb546c").rgba(); QCOMPARE(c->color().rgba(),ink);
        // A small flag made through real canvas gestures, with one undo per drag.
        stroke(c,{3,3},{3,13}); stroke(c,{4,3},{11,3}); stroke(c,{4,4},{10,4}); stroke(c,{4,5},{11,5});
        QCOMPARE(c->document().pixel(3,13),ink); QCOMPARE(c->document().pixel(11,3),ink);
        QCOMPARE(c->document().pixel(0,0),0u); QVERIFY(editor.isModified());
        tool(editor,1); stroke(c,{3,13},{3,13}); QCOMPARE(c->document().pixel(3,13),0u);
        editor.findChild<QAction*>("undo")->trigger(); QCOMPARE(c->document().pixel(3,13),ink);
        editor.findChild<QAction*>("redo")->trigger(); QCOMPARE(c->document().pixel(3,13),0u);
        editor.findChild<QAction*>("undo")->trigger(); QCOMPARE(c->document().pixel(3,13),ink);
        tool(editor,2); stroke(c,{4,4},{4,4}); QCOMPARE(c->color().rgba(),ink);
        tool(editor,0);
        const auto beforeCancel=c->image(); const auto revision=c->document().revision();
        QTest::mousePress(c,Qt::LeftButton,Qt::NoModifier,QPoint(c->zoom()/2,c->zoom()/2));
        QTest::keyClick(c,Qt::Key_Escape); QTest::mouseRelease(c,Qt::LeftButton);
        QCOMPARE(c->document().revision(),revision); QCOMPARE(c->image(),beforeCancel);
        auto* grid=editor.findChild<QToolButton*>("gridButton"); QTest::mouseClick(grid,Qt::LeftButton); QVERIFY(!c->grid());
        QTest::mouseClick(grid,Qt::LeftButton); QVERIFY(c->grid());
        auto* zoom=editor.findChild<QComboBox*>("zoomCombo"); zoom->setFocus(); zoom->setCurrentIndex(2); QMetaObject::invokeMethod(zoom,"activated",Q_ARG(int,2)); QCOMPARE(c->zoom(),4);
        QTest::mouseClick(fit,Qt::LeftButton);
        const QString saved=directory.path()+"/flag-ü.png";
        fileAction(editor,"savePng",saved); QVERIFY(QFile::exists(saved)); QVERIFY(!editor.isModified());
        QCOMPARE(QImage(saved).convertToFormat(QImage::Format_ARGB32),c->image());
        const auto original=c->image();
        capture(editor,"edited-compact"); editor.resize(1100,740); QTest::qWait(60); QTest::mouseClick(fit,Qt::LeftButton); capture(editor,"edited-normal");
        if(!evidence_.isEmpty()) QVERIFY(QFile::copy(saved,evidence_+"/flag-16x16.png"));
        bool newObserved=false;
        qInfo()<<"New canvas dialog";
        QTimer::singleShot(40,&editor,[&]{auto* d=qobject_cast<QDialog*>(QApplication::activeModalWidget()); if(d){newObserved=true;d->accept();}});
        editor.findChild<QAction*>("newCanvas")->trigger(); QVERIFY(newObserved); QCOMPARE(c->document().pixel(3,3),0u);
        QTest::mouseClick(editor.findChild<QPushButton*>("swatch7"),Qt::LeftButton);
        stroke(c,{3,8},{12,8}); stroke(c,{8,4},{12,8}); stroke(c,{8,12},{12,8});
        const QString arrow=directory.path()+"/arrow.png"; fileAction(editor,"savePngAs",arrow); QVERIFY(!editor.isModified());
        const auto arrowImage=c->image();
        const QString exported=directory.path()+"/arrow-copy.png";fileAction(editor,"exportPng",exported);
        QCOMPARE(QImage(exported).convertToFormat(QImage::Format_ARGB32),arrowImage);
        fileAction(editor,"openPng",saved); QCOMPARE(c->image(),original); QVERIFY(!c->canUndo());
        fileAction(editor,"openPng",arrow); QCOMPARE(c->image(),arrowImage); QCOMPARE(c->document().width(),16);
        if(!evidence_.isEmpty()) QVERIFY(QFile::copy(exported,evidence_+"/arrow-16x16.png"));
        editor.close(); QVERIFY(!editor.isVisible());
    }
    void actualNamedFramesAndMoviePreview() {
        QTemporaryDir directory;Editor editor;editor.show();QVERIFY(QTest::qWaitForWindowExposed(&editor));
        auto* control=editor.findChild<AutomationController*>();QVERIFY(control);const auto id=control->begin("Supplied two-frame UI fixture",directory.path());
        QVERIFY(control->call("pixelforge_task",{{"action","accept"},{"task_id",qint64(id)},{"width",16},{"height",16}}).success);
        auto r=control->call("pixelforge_pack",{{"action","create"},{"task_id",qint64(id)},{"canvases","pose_a,walk,16,16,0|pose_b,walk,16,16,1"}});QVERIFY(r.success);
        r=control->call("pixelforge_pass",{{"task_id",qint64(id)},{"pack_revision",control->state()["pack_revision"]},{"program","CANVAS pose_a;R 3 3 4 8 #FFEB546C;CANVAS pose_b;R 8 3 4 8 #FF32B8B1"},{"render_mode","strip"}});QVERIFY(r.success);
        auto* selector=editor.findChild<QComboBox*>("canvasSelector");QVERIFY(selector);QCOMPARE(selector->count(),2);selector->setCurrentIndex(1);QMetaObject::invokeMethod(selector,"activated",Q_ARG(int,1));QCOMPARE(editor.canvas()->image().pixel(9,4),0xff32b8b1u);
        QTest::mouseClick(editor.findChild<QPushButton*>("fitCanvas"),Qt::LeftButton);tool(editor,0);stroke(editor.canvas(),{2,2},{2,2});QVERIFY(control->project().canvases[1].image.pixel(2,2)!=0);QCOMPARE(control->project().canvases[0].image.pixel(2,2),0u);QCOMPARE(control->project().canvases[1].image.pixel(3,3),0u);
        auto* play=editor.findChild<QPushButton*>("previewFrames");QVERIFY(play);QTest::mouseClick(play,Qt::LeftButton);auto* movie=editor.findChild<QMovie*>();QVERIFY(movie);QTRY_VERIFY_WITH_TIMEOUT(movie->currentFrameNumber()>=1,2000);QCOMPARE(movie->frameCount(),2);
        capture(editor,"project-frames-normal");editor.resize(760,560);QTest::qWait(80);capture(editor,"project-frames-compact");
        for(auto* dialog:editor.findChildren<QDialog*>())dialog->close();QCoreApplication::processEvents();
        QString why;QVERIFY(control->saveProject(directory.path(),&why));
        const auto before=control->project().canvases[1].image;QVERIFY(control->loadProject(directory.filePath("project.json"),&why));QVERIFY(control->selectCanvas("pose_b"));QCOMPARE(editor.canvas()->image(),before);
        // PNG export must not mark a whole unsaved multi-canvas project clean.
        QVERIFY(editor.saveFile(directory.filePath("selected.png"),&why));QVERIFY(editor.isModified());
        bool discarded=false;QTimer::singleShot(20,&editor,[&]{if(auto* box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget())){if(auto* button=box->button(QMessageBox::Discard)){discarded=true;button->click();}}});editor.close();QVERIFY(discarded);QVERIFY(!editor.isVisible());
    }
    void failurePreservesDocumentAndCloseCancellation() {
        QTemporaryDir directory; Editor editor; editor.show(); QVERIFY(QTest::qWaitForWindowExposed(&editor));
        auto* c=editor.canvas(); stroke(c,{1,1},{8,1});
        const auto image=c->image(); const auto revision=c->document().revision();
        const QString invalid=directory.path()+"/invalid.png"; QFile f(invalid); QVERIFY(f.open(QIODevice::WriteOnly));f.write("not a PNG");f.close();
        QString error; QVERIFY(!editor.openFile(invalid,&error)); QVERIFY(!error.isEmpty()); QCOMPARE(c->image(),image); QCOMPARE(c->document().revision(),revision);
        QVERIFY(!editor.saveFile(directory.path()+"/missing/canvas.png",&error)); QVERIFY(editor.isModified()); QCOMPARE(c->image(),image);
        QVERIFY(!c->newImage(4096,4096,&error)); QCOMPARE(c->image(),image);
        bool dialogSeen=false;
        QTimer::singleShot(40,&editor,[&]{auto* d=qobject_cast<QMessageBox*>(QApplication::activeModalWidget());if(d){dialogSeen=true;d->done(QMessageBox::Cancel);}});
        editor.close(); QVERIFY(dialogSeen); QVERIFY(editor.isVisible()); QCOMPARE(c->image(),image);
        QVERIFY(editor.saveFile(directory.path()+"/retained.png",&error)); editor.close(); QVERIFY(!editor.isVisible());
    }
};
QTEST_MAIN(EditorSmoke)
#include "qt_smoke.moc"
