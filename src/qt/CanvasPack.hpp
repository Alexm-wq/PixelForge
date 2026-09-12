#pragma once
#include "ProjectStore.hpp"
#include <QByteArray>
#include <QJsonObject>
#include <QMap>

namespace pixelforge::qt {
struct PackReply {
    bool success = false;
    QJsonObject facts;
    QByteArray image; // Actual PNG bytes; empty for non-image replies.
};

// Data-only, caller-serialized workspace. Parent owns current-turn/task-state
// admission and synchronizing the selected editor document after successful calls.
class CanvasPack final {
public:
    PackReply call(const QJsonObject& arguments, quint64 currentTaskId,
                   const QString& outputDirectory);
    const ProjectBundle& bundle() const { return bundle_; }
    bool restore(const ProjectBundle& bundle, QString* error = nullptr);
    bool setProjectBrief(const QString& brief, QString* error = nullptr);
    // Trusted host/manual edit seam; no task admission is granted by this call.
    bool replaceCanvasImage(const QString& name, const QImage& image,
                            quint64 expectedRevision, QString* error = nullptr);
    void setExportReplacementAllowed(bool allowed) { exportReplacementAllowed_=allowed; }
    void setSourceImage(const QImage& image) { sourceImage_=image; }
    quint64 revision() const { return revision_; }
    void setPalette(const QList<quint32>& palette) { if (!palette.isEmpty() && palette.size() <= 65536) palette_ = palette; }
private:
    struct History {
        QMap<QString, QImage> before, after;
        quint64 bytes = 0;
    };
    struct Analysis { qint64 imageKey=0; QJsonObject facts; };
    QMap<QString,Analysis> analysis_;
    QMap<QString,QPair<QString,qint64>> deltas_;
    ProjectBundle bundle_;
    QImage sourceImage_;
    quint64 revision_ = 0;
    bool exportReplacementAllowed_ = false;
    QList<History> undo_, redo_;
    quint64 selectionRevision_ = ~quint64(0);
    QMap<QString, QList<int>> selections_;
    QString renderedIdentity_;
    PackReply renderedCache_;
    qint64 selectionCacheHits_ = 0, renderCacheHits_ = 0, renders_ = 0;
    QList<int> select(const QJsonObject&, QString& error);
    QList<quint32> palette_{0xff17202e,0xffffffff,0xff8b95a7,0xffeb546c,0xfff4a64c,0xfff9d85c,0xff58c478,0xff32b8b1,0xff4ba6e8,0xff7569e7,0xffc576d7,0xff805742};
    PackReply respond(bool success, QJsonObject facts = {}, QByteArray image = {}) const;
    PackReply reject(const QString& code, const QString& message) const;
    void trimHistory();
};
} // namespace pixelforge::qt
