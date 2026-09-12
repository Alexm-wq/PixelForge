#include "ProjectStore.hpp"

#include <QDir>
#include <QBuffer>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>
#include <QCryptographicHash>
#include <QMap>
#include <QMutex>
#include <QMutexLocker>
#include <QtEndian>

#include <cmath>
#include <limits>
#include <utility>

#ifdef Q_OS_WIN
#  define NOMINMAX
#  include <windows.h>
#endif

namespace pixelforge::qt {
namespace {

bool fail(QString* error, const QString& message) {
    if (error) *error = message;
    return false;
}

bool linkLike(const QFileInfo& info) {
    if (info.isSymLink()) return true;
#ifdef Q_OS_WIN
    const auto attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(info.absoluteFilePath().utf16()));
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return true;
#endif
    return false;
}

// Reject links in every existing component, not just the last PNG. In
// particular a symlinked group directory cannot escape the selected root.
bool safeAbsolute(const QString& path) {
    QFileInfo info(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
    for (;;) {
        if (linkLike(info)) return false;
        const auto parent = info.dir().absolutePath();
        if (parent == info.absoluteFilePath()) return true;
        info.setFile(parent);
    }
}

bool component(const QString& value) {
    static const QRegularExpression valid(QStringLiteral("^[A-Za-z0-9_.-]+$"));
    static const QRegularExpression device(QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\.|$)"), QRegularExpression::CaseInsensitiveOption);
    return !value.isEmpty() && value.size() <= 240 && value != "." && value != ".." &&
           !value.endsWith('.') && valid.match(value).hasMatch() && !device.match(value).hasMatch();
}

bool relativeFile(QString value, QString& normalized) {
    // Windows-generated manifests may use either separator. Reject drive, UNC,
    // traversal and alternate-stream syntax on every OS before normalization.
    value.replace('\\', '/');
    if (value.isEmpty() || value.size() > 2048 || value.startsWith('/') || value.contains(':') || value.contains(QChar(0))) return false;
    const auto parts = value.split('/');
    for (const auto& part : parts)
        if (part.isEmpty() || part == "." || part == ".." || part.endsWith('.') || part.endsWith(' ')) return false;
    normalized = value;
    return QFileInfo(value).suffix().compare("png", Qt::CaseInsensitive) == 0;
}

bool integer(const QJsonValue& value, qint64 minimum, qint64 maximum, qint64& out) {
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number) return false;
    // Qt 6 retains signed JSON integer precision; do not route task IDs through a
    // double cast. IDs beyond the signed core/tool range are rejected explicitly.
    const qint64 parsed = value.toInteger(std::numeric_limits<qint64>::min());
    if (parsed < minimum || parsed > maximum) return false;
    out = parsed;
    return true;
}

bool text(const QJsonObject& object, const QString& key, QString& out, int limit, bool optional = false) {
    if (!object.contains(key) && optional) { out.clear(); return true; }
    if (!object.value(key).isString()) return false;
    out = object.value(key).toString();
    return out.size() <= limit && !out.contains(QChar(0));
}

bool dimensions(int width, int height, quint64& total) {
    if (width <= 0 || height <= 0 || width > ProjectStore::MaxDimension || height > ProjectStore::MaxDimension) return false;
    const auto pixels = quint64(width) * quint64(height);
    if (pixels > ProjectStore::MaxCanvasPixels || total > ProjectStore::MaxTotalPixels - pixels) return false;
    total += pixels;
    return true;
}

bool boundedRead(const QString& path, qint64 maximum, QByteArray& bytes) {
    if (!safeAbsolute(path)) return false;
    const QFileInfo before(path);
    if (!before.isFile() || before.size() < 1 || before.size() > maximum) return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    bytes = file.read(maximum + 1);
    const QFileInfo after(path);
    return bytes.size() == before.size() && bytes.size() <= maximum && file.atEnd() &&
           file.error() == QFileDevice::NoError && safeAbsolute(path) &&
           before.canonicalFilePath() == after.canonicalFilePath() &&
           before.size() == after.size() && before.lastModified() == after.lastModified();
}

bool validBundle(const ProjectBundle& bundle, QString* error) {
    const auto signedMax = quint64(std::numeric_limits<qint64>::max());
    if (bundle.taskId == 0 || bundle.taskId > signedMax || bundle.packRevision > signedMax)
        return fail(error, "Task ID or revision is outside the supported positive signed-64-bit format.");
    if (bundle.projectName.size() > 1024 || bundle.projectName.contains(QChar(0)))
        return fail(error, "Invalid project name.");
    if (bundle.projectBrief.size() > 8192 || bundle.projectBrief.contains(QChar(0)) || bundle.changeTrackingFloor > bundle.packRevision)
        return fail(error, "Invalid project brief or change tracking floor.");
    if (bundle.canvases.isEmpty() || bundle.canvases.size() > ProjectStore::MaxCanvases)
        return fail(error, "Project canvas count is outside the supported range (1–4096).");
    QSet<QString> names;
    quint64 total = 0;
    for (const auto& canvas : bundle.canvases) {
        if (!component(canvas.name) || (!canvas.group.isEmpty() && !component(canvas.group)) ||
            canvas.frame < -1 || canvas.changedRevision > bundle.packRevision || names.contains(canvas.name.toUpper()))
            return fail(error, "Invalid or duplicate canvas name, group or frame.");
        names.insert(canvas.name.toUpper());
        if (canvas.image.isNull() || !dimensions(canvas.image.width(), canvas.image.height(), total))
            return fail(error, "Canvas dimensions or total decoded pixel budget exceeded.");
    }
    return true;
}

bool pending(const QString& root) {
    const QFileInfo info(QDir(root).filePath(".autosave-pending"));
    return info.exists() || info.isSymLink();
}

} // namespace

QString ProjectStore::pixelIdentity(const QImage& source, bool* newlyHashed) {
    if (newlyHashed) *newlyHashed = false;
    if (source.isNull() || quint64(source.width()) * source.height() > MaxCanvasPixels) return {};
    static QMutex mutex;
    static QMap<qint64, QString> cache;
    QMutexLocker guard(&mutex);
    const qint64 key = source.cacheKey();
    const auto found = cache.constFind(key);
    if (found != cache.cend()) return found.value();
    const auto image = source.convertToFormat(QImage::Format_ARGB32);
    if (image.isNull()) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArray::number(image.width()) + ':' + QByteArray::number(image.height()) + ':');
    QByteArray row(image.width() * 4, Qt::Uninitialized);
    for (int y = 0; y < image.height(); ++y) {
        const auto* pixels = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) qToBigEndian<quint32>(pixels[x], row.data() + x * 4);
        hash.addData(row);
    }
    const QString value = QString::fromLatin1(hash.result().toHex());
    if (cache.size() >= 8192) cache.clear();
    cache.insert(key, value);
    if (newlyHashed) *newlyHashed = true;
    return value;
}

bool ProjectStore::safeExportFileName(const QString& name) { return component(name); }

bool ProjectStore::safeAbsolutePath(const QString& path) { return QDir::isAbsolutePath(path) && safeAbsolute(path); }

bool ProjectStore::load(const QString& input, ProjectBundle& output, QString* error) {
    if (error) error->clear();
    if (input.isEmpty() || !safeAbsolute(input)) return fail(error, "Project path contains a symbolic link or reparse point.");
    const QFileInfo selected(input);
    const QString manifest = selected.isDir() ? QDir(selected.absoluteFilePath()).filePath("project.json") : selected.absoluteFilePath();
    const QString root = QFileInfo(manifest).absolutePath();
    if (!safeAbsolute(root) || !QFileInfo(root).isDir()) return fail(error, "Project directory is unavailable.");
    // Loading needs no write permission and creates no files. A manifest reread
    // below fences a concurrent publication; immutable Qt PNG generations remain.
    const auto lockPath = QDir(root).filePath(".qt-project-write.lock");
    if (!safeAbsolute(lockPath)) return fail(error, "Unsafe project lock path.");
    if (QFileInfo::exists(lockPath)) return fail(error, "Project is busy; wait for its current save to finish.");
    if (pending(root)) return fail(error, "An interrupted Windows autosave is present; complete or recover it before loading.");
    QByteArray bytes;
    if (!boundedRead(manifest, MaxManifestBytes, bytes)) return fail(error, "Cannot read a bounded regular project manifest.");
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(bytes, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) return fail(error, "Malformed project JSON.");
    const auto object = document.object();
    qint64 number = 0;
    // Old loader tolerated missing version; current writer emits exactly 1.
    if (object.contains("version") && (!integer(object.value("version"), 1, 1, number)))
        return fail(error, "Unsupported project version.");
    ProjectBundle candidate;
    if (!integer(object.value("task_id"), 1, std::numeric_limits<qint64>::max(), number))
        return fail(error, "Project has no supported task_id.");
    candidate.taskId = quint64(number);
    if (object.contains("pack_revision")) {
        if (!integer(object.value("pack_revision"), 0, std::numeric_limits<qint64>::max(), number)) return fail(error, "Invalid pack_revision.");
        candidate.packRevision = quint64(number);
    }
    if (!text(object, "project_name", candidate.projectName, 1024, true)) return fail(error, "Invalid project_name.");
    if (!text(object, "project_brief", candidate.projectBrief, 8192, true)) return fail(error, "Invalid project_brief.");
    candidate.changeTrackingFloor = candidate.packRevision;
    if (object.contains("change_tracking_floor")) {
        if (!integer(object.value("change_tracking_floor"), 0, qint64(candidate.packRevision), number)) return fail(error, "Invalid change_tracking_floor.");
        candidate.changeTrackingFloor = quint64(number);
    }
    if (!object.value("canvases").isArray()) return fail(error, "Project has no canvases array.");
    const auto array = object.value("canvases").toArray();
    if (array.isEmpty() || array.size() > MaxCanvases) return fail(error, "Project canvas count is outside the supported range.");
    quint64 pixels = 0;
    qint64 encodedBytes = 0;
    QSet<QString> names;
    for (const auto& value : array) {
        if (!value.isObject()) return fail(error, "Malformed canvas entry.");
        const auto entry = value.toObject();
        ProjectCanvas canvas;
        canvas.changedRevision = candidate.packRevision;
        if (entry.contains("changed_revision")) {
            if (!integer(entry.value("changed_revision"), 0, qint64(candidate.packRevision), number)) return fail(error, "Invalid canvas changed_revision.");
            canvas.changedRevision = quint64(number);
        } else candidate.changeTrackingFloor = candidate.packRevision;
        QString relative;
        if (!text(entry, "name", canvas.name, 240) || !component(canvas.name) ||
            !text(entry, "group", canvas.group, 240, true) || (!canvas.group.isEmpty() && !component(canvas.group)) || names.contains(canvas.name.toUpper()))
            return fail(error, "Invalid or duplicate canvas name/group.");
        names.insert(canvas.name.toUpper());
        if (entry.contains("frame")) {
            if (!integer(entry.value("frame"), -1, std::numeric_limits<int>::max(), number)) return fail(error, "Invalid canvas frame.");
            canvas.frame = int(number);
        }
        if (!integer(entry.value("width"), 1, MaxDimension, number)) return fail(error, "Invalid canvas width.");
        const int width = int(number);
        if (!integer(entry.value("height"), 1, MaxDimension, number)) return fail(error, "Invalid canvas height.");
        const int height = int(number);
        if (!dimensions(width, height, pixels)) return fail(error, "Decoded image resource budget exceeded.");
        if (!text(entry, "file", relative, 2048, true)) return fail(error, "Invalid canvas file path.");
        if (relative.isEmpty()) relative = "canvases/" + (canvas.group.isEmpty() ? QString("ungrouped") : canvas.group) + "/" + canvas.name + ".png";
        QString normalized;
        if (!relativeFile(relative, normalized)) return fail(error, "Canvas path must be a contained relative PNG path.");
        const auto file = QDir(root).filePath(normalized);
        QByteArray png;
        if (!boundedRead(file, MaxPngBytes, png) || encodedBytes > MaxTotalPngBytes - png.size()) return fail(error, "Canvas PNG missing, unsafe or above resource limits.");
        encodedBytes += png.size();
        // Decode from the bounded bytes already read, never reopen the path.
        QBuffer buffer(&png);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer, "PNG");
        reader.setAutoDetectImageFormat(false);
        reader.setAutoTransform(false);
        if (!reader.canRead() || reader.size() != QSize(width, height)) return fail(error, "PNG dimensions do not match the manifest.");
        canvas.image = reader.read();
        if (canvas.image.isNull() || canvas.image.size() != QSize(width, height)) return fail(error, "PNG decode failed.");
        canvas.image = canvas.image.convertToFormat(QImage::Format_ARGB32);
        if (canvas.image.isNull()) return fail(error, "Cannot allocate decoded canvas.");
        candidate.canvases.append(std::move(canvas));
    }
    if (!validBundle(candidate, error)) return false;
    if (pending(root)) return fail(error, "Windows autosave started during load; retry after it finishes.");
    QByteArray currentManifest;
    if (QFileInfo::exists(lockPath) || !boundedRead(manifest, MaxManifestBytes, currentManifest) || currentManifest != bytes)
        return fail(error, "Project changed during load; no workspace state was replaced.");
    output = std::move(candidate); // Sole publication; failure never clears active work.
    return true;
}

bool ProjectStore::save(const QString& directory, const ProjectBundle& bundle, QString* error, ProjectSaveStats* stats) {
    if (error) error->clear();
    if (stats) *stats = {};
    if (!validBundle(bundle, error)) return false;
    if (directory.isEmpty() || !safeAbsolute(directory)) return fail(error, "Destination contains a symbolic link or reparse point.");
    const QString root = QFileInfo(directory).absoluteFilePath();
    if (!QDir().mkpath(root) || !safeAbsolute(root) || !QFileInfo(root).isDir()) return fail(error, "Cannot create a safe project directory.");
    const auto lockPath = QDir(root).filePath(".qt-project-write.lock");
    if (!safeAbsolute(lockPath)) return fail(error, "Unsafe project lock path.");
    QLockFile lock(lockPath);
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) return fail(error, "Project is busy; no save was applied.");
    if (pending(root)) return fail(error, "Interrupted Windows autosave must be resolved before saving.");
    const QString manifest = QDir(root).filePath("project.json");
    if (!safeAbsolute(manifest) || (QFileInfo(manifest).exists() && !QFileInfo(manifest).isFile())) return fail(error, "Unsafe project manifest destination.");
    const QString assets = QDir(root).filePath(".qt-assets");
    if (!safeAbsolute(assets) || !QDir().mkpath(assets) || !safeAbsolute(assets)) return fail(error, "Cannot create immutable image directory.");
    struct Verified { qint64 size; QDateTime modified; QString pixelHash; };
    static QMutex assetMutex;
    static QMap<QString, Verified> verified;
    QMutexLocker assetGuard(&assetMutex);
    ProjectSaveStats measured;
    QJsonArray entries;
    qint64 encoded = 0;
    for (const auto& canvas : bundle.canvases) {
        bool hashed = false;
        const QString identity = pixelIdentity(canvas.image, &hashed);
        measured.hashedImages += hashed;
        if (identity.isEmpty()) return fail(error, "Cannot identify canvas pixels.");
        const QString imagePath = QDir(assets).filePath(identity + ".png");
        if (!safeAbsolute(imagePath)) return fail(error, "Unsafe immutable image path.");
        QFileInfo info(imagePath);
        if (info.exists()) {
            if (!info.isFile() || info.size() < 1 || info.size() > MaxPngBytes) return fail(error, "Invalid existing immutable image.");
            const auto known = verified.constFind(imagePath);
            if (known == verified.cend() || known->size != info.size() || known->modified != info.lastModified() || known->pixelHash != identity) {
                QByteArray bytes;
                if (!boundedRead(imagePath, MaxPngBytes, bytes)) return fail(error, "Cannot verify immutable image.");
                QBuffer input(&bytes); input.open(QIODevice::ReadOnly);
                QImageReader reader(&input, "PNG"); reader.setAutoDetectImageFormat(false); reader.setAutoTransform(false);
                if (!reader.canRead() || reader.size() != canvas.image.size()) return fail(error, "Immutable image header mismatch.");
                const QImage restored = reader.read(); ++measured.verifiedImages;
                if (restored.isNull() || pixelIdentity(restored) != identity) return fail(error, "Immutable image content mismatch; existing asset preserved.");
            }
            ++measured.reusedImages;
        } else {
            QSaveFile file(imagePath); file.setDirectWriteFallback(false);
            if (!file.open(QIODevice::WriteOnly)) return fail(error, "Cannot stage immutable PNG.");
            QImageWriter writer(&file, "PNG");
            if (!writer.write(canvas.image) || file.pos() > MaxPngBytes || encoded > MaxTotalPngBytes - file.pos()) {
                file.cancelWriting(); return fail(error, "PNG encoding failed or exceeded the saved-image budget.");
            }
            if (!file.commit()) return fail(error, "Cannot commit immutable PNG.");
            ++measured.encodedImages; info.refresh();
        }
        if (encoded > MaxTotalPngBytes - info.size()) return fail(error, "Saved image resource budget exceeded.");
        encoded += info.size();
        if (verified.size() >= 8192) verified.clear();
        verified.insert(imagePath, {info.size(), info.lastModified(), identity});
        entries.append(QJsonObject{{"name", canvas.name}, {"group", canvas.group}, {"frame", canvas.frame},
                                  {"width", canvas.image.width()}, {"height", canvas.image.height()},
                                  {"changed_revision", QJsonValue(qint64(canvas.changedRevision))}, {"pixel_sha256", identity},
                                  {"file", QDir(root).relativeFilePath(imagePath)}});
    }
    const QJsonObject metadata{{"version", 1}, {"task_id", QJsonValue(qint64(bundle.taskId))},
                               {"pack_revision", QJsonValue(qint64(bundle.packRevision))},
                               {"project_brief", bundle.projectBrief}, {"change_tracking_floor", QJsonValue(qint64(bundle.changeTrackingFloor))},
                               {"project_name", bundle.projectName}, {"canvases", entries}};
    const auto json = QJsonDocument(metadata).toJson(QJsonDocument::Indented);
    if (json.size() > MaxManifestBytes || !safeAbsolute(manifest) || !safeAbsolute(assets) || pending(root))
        return fail(error, "Project changed or manifest exceeds its limit; old save retained.");
    QSaveFile file(manifest);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(json) != json.size()) return fail(error, "Cannot stage project manifest; old save retained.");
    // Retain the complete generation before publication, including ambiguous
    // commit failures. Never remove PNGs potentially referenced by a committed JSON.
    if (!file.commit()) return fail(error, "Manifest commit failed; previous project retained and staged PNGs left for recovery.");
    measured.referencedBytes = encoded;
    if (stats) *stats = measured;
    return true;
}

} // namespace pixelforge::qt
