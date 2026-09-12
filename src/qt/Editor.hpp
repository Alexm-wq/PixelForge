#pragma once
#include "PixelDocument.hpp"
#include <QMainWindow>
#include <QImage>
#include <QWidget>
#include <memory>

class QScrollArea;
class QComboBox;
class QLabel;
class QPushButton;
class QPlainTextEdit;
class QLineEdit;

namespace pixelforge::qt {
class AutomationController;
class AutomationClient;
class LocalMcpBridge;
class Canvas final : public QWidget {
    Q_OBJECT
public:
    enum class Tool { Pencil, Eraser, Picker };
    explicit Canvas(QWidget* parent = nullptr);
    const PixelDocument& document() const { return document_; }
    const QImage& image() const { return image_; }
    QColor color() const { return color_; }
    Tool tool() const { return tool_; }
    int zoom() const { return zoom_; }
    bool grid() const { return grid_; }
    bool canUndo() const { return undoCount_ > 0; }
    bool canRedo() const { return redoCount_ > 0; }
    bool newImage(int width, int height, QString* error = nullptr);
    bool openPng(const QString& path, QString* error = nullptr);
    bool writePng(const QString& path, QString* error = nullptr);
    void setColor(QColor color);
    void setTool(Tool tool);
    void setZoom(int zoom);
    void setGrid(bool grid);
    void undo();
    void redo();
    void cancelStroke();
signals:
    void documentChanged();
    void viewChanged();
    void colorChanged(QColor color);
    void pixelHovered(QPoint pixel);
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void focusOutEvent(QFocusEvent*) override;
private:
    friend class AutomationController;
    PixelDocument document_;
    QImage image_;
    QColor color_{"#32b8b1"};
    Tool tool_ = Tool::Pencil;
    int zoom_ = 24;
    bool grid_ = true;
    int undoCount_ = 0, redoCount_ = 0;
    std::unique_ptr<PixelDocument::Transaction> stroke_;
    QPoint previous_;
    QRgb strokeColor_ = 0;
    void syncImage();
    void drawTo(QPoint pixel);
    void finishStroke();
};

class Editor final : public QMainWindow {
    Q_OBJECT
public:
    explicit Editor(QWidget* parent = nullptr);
    ~Editor() override;
    Canvas* canvas() const { return canvas_; }
    bool openFile(const QString& path, QString* error = nullptr);
    bool saveFile(const QString& path, QString* error = nullptr);
    bool isModified() const { return modified_; }
protected:
    void closeEvent(QCloseEvent*) override;
private:
    Canvas* canvas_ = nullptr;
    QScrollArea* scroll_ = nullptr;
    QComboBox* zoom_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* color_ = nullptr;
    QAction *undo_ = nullptr, *redo_ = nullptr;
    AutomationController* automation_ = nullptr;
    AutomationClient* client_ = nullptr;
    LocalMcpBridge* bridge_ = nullptr;
    QPlainTextEdit* prompt_ = nullptr;
    QLabel* progress_ = nullptr;
    QPushButton *generate_ = nullptr, *stop_ = nullptr, *accept_ = nullptr;
    QComboBox *model_ = nullptr, *effort_ = nullptr;
    QList<QAction*> documentActions_;
    QString path_,projectPath_;
    QComboBox *canvases_=nullptr,*groups_=nullptr;
    QWidget* questionPanel_=nullptr;
    QLabel* question_=nullptr;
    QLineEdit* answer_=nullptr;
    bool contextChange_=false,closingPending_=false;
    void finishCloseWhenSafe();
    void updateWorkspace();
    bool saveProjectDialog(bool choosePath=false);
    void openProjectDialog();
    void addCanvasDialog();
    void exportProjectDialog();
    void previewAnimation();
    bool modified_ = false;
    bool askToSave();
    bool saveDialog(bool choosePath);
    void newDialog();
    void openDialog();
    void updateStatus();
    void updateColor(QColor color);
    void fitCanvas();
    void generate();
    void updateAutomation();
};
}
