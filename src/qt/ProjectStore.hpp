#pragma once

#include <QImage>
#include <QList>
#include <QString>
#include <QtGlobal>

namespace pixelforge::qt {

struct ProjectCanvas {
    QString name;
    QString group;
    int frame = -1;
    QImage image;
    quint64 changedRevision = 0;
};

struct ProjectBundle {
    quint64 taskId = 0;
    QString projectName; // Optional project_name extension; absent Windows value stays empty.
    QList<ProjectCanvas> canvases; // Manifest order, including non-monotonic frame numbers.
    quint64 packRevision = 0;
    QString projectBrief;
    quint64 changeTrackingFloor = 0;
};
struct ProjectSaveStats {
    int reusedImages = 0, encodedImages = 0, hashedImages = 0, verifiedImages = 0;
    qint64 referencedBytes = 0;
};

// Version-1 Windows project.json + PNG reader/writer. No editor/agent mutation.
// Caller must stop its workspace writes while taking the input snapshot/applying
// a returned bundle. Saves serialize cooperating ProjectStore writers;
// project directories must not be concurrently replaced by other filesystem actors.
class ProjectStore final {
public:
    // Shared path-component policy for host-owned exports; no filesystem mutation.
    static bool safeAbsolutePath(const QString& path);
    static bool safeExportFileName(const QString& name);
    static constexpr int MaxCanvases = 4096;
    static constexpr int MaxDimension = 16384;
    static constexpr quint64 MaxCanvasPixels = 16ull * 1024 * 1024;
    static constexpr quint64 MaxTotalPixels = 64ull * 1024 * 1024;
    static constexpr qint64 MaxManifestBytes = 4ll * 1024 * 1024;
    static constexpr qint64 MaxPngBytes = 80ll * 1024 * 1024;
    static constexpr qint64 MaxTotalPngBytes = 320ll * 1024 * 1024;

    // Accepts a directory (project.json) or explicit JSON manifest. On any error,
    // output remains unchanged. PNG dimensions are checked before decoding.
    static bool load(const QString& manifestOrDirectory, ProjectBundle& output,
                     QString* error = nullptr);
    // Stable dimensions+ARGB SHA256, with a bounded process-local QImage cache.
    static QString pixelIdentity(const QImage& image, bool* newlyHashed = nullptr);
    // Creates a missing destination, reuses verified immutable content assets or
    // writes only new PNGs, then commits project.json last via QSaveFile.
    // Prior assets stay intact, including after a failed/interrupted save.
    // Does not export derived strips/GIFs, preserve undo stacks, or alter task IDs.
    static bool save(const QString& directory, const ProjectBundle& bundle,
                     QString* error = nullptr, ProjectSaveStats* stats = nullptr);
};

} // namespace pixelforge::qt
