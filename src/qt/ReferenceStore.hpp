#pragma once
#include <QByteArray>
#include <QImage>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QPair>
#include <QString>
#include <QStringList>

namespace pixelforge::qt {
struct ReferenceReply {
    bool success = false;
    QJsonObject facts;
    QByteArray image; // Encoded PNG, never a path.
};

// Caller-serialized data store. Only trusted host file-picker code calls load.
// The controller owns task/turn/revision admission and exact same-size seed
// transactions. This object never replaces or mutates an editor document.
class ReferenceStore final {
public:
    static constexpr int MaxDimension = 16384;
    static constexpr qint64 MaxPixels = 16ll * 1024 * 1024;
    static constexpr qint64 MaxTotalPixels = 32ll * 1024 * 1024;
    static constexpr qint64 MaxInputBytes = 80ll * 1024 * 1024;
    static constexpr qint64 MaxPngBytes = 32ll * 1024 * 1024;
    bool load(const QString& slot, const QString& path, QString* error = nullptr);
    bool clear(const QString& slot);
    static QStringList supportedFormats(); // Allowed formats with installed Qt reader plugins.
    QJsonObject metadata() const;
    QImage image(const QString& slot) const; // Value copy; writes detach from store.
    ReferenceReply call(const QJsonObject& arguments);
private:
    struct Entry {
        QImage pixels;
        QString hash;
        QString decodedFormat;
        QString pngKey;
        QByteArray png;
        bool paletteReady = false;
        int unique = 0;
        qint64 visible = 0;
        QList<QPair<quint32, qint64>> colors;
    };
    QMap<QString, Entry> slots_;
};
} // namespace pixelforge::qt
