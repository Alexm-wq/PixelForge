#include "SessionRecording.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QThread>
#include <new>
#include <filesystem>

namespace pixelforge::qt {
namespace {
bool refuse(QString* error, const QString& text) { if (error) *error = text; return false; }
bool safeParent(QString directory) {
    // This is a cooperating local output contract, not protection from hostile
    // concurrent ancestor replacement. Reject existing symbolic-link paths.
    for (;;) {
        const QFileInfo info(directory);
        if (!info.isDir() || info.isSymLink()) return false;
        const QString parent = info.dir().absolutePath();
        if (parent == directory) return true;
        directory = parent;
    }
}
std::filesystem::path nativePath(const QString& value) {
#ifdef Q_OS_WIN
    return std::filesystem::path(value.toStdWString());
#else
    return std::filesystem::path(QFile::encodeName(value).toStdString());
#endif
}
}

SessionRecording::SessionRecording(QObject* parent) : QObject(parent) {
    deadline_.setSingleShot(true);
    connect(&process_, &QProcess::started, this, [this] {
        if (terminal_) return;
        if (state_ == "starting") { deadline_.stop(); state_ = "recording"; emit changed(); }
        else if (state_ == "stopping") process_.closeWriteChannel();
        else if (state_ == "failing") process_.terminate();
    });
    connect(&process_, &QProcess::bytesWritten, this, [this](qint64 n) {
        if (!terminal_) pipeBytes_ += n;
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (terminal_) return;
        if (error == QProcess::FailedToStart) {
            error_ = "The installed ffmpeg encoder could not start.";
            complete(false);
        } else if (error == QProcess::WriteError || error == QProcess::ReadError)
            fail("The recording encoder pipe failed.");
        // Crashed is finalized by QProcess::finished; no duplicate completion.
    });
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &SessionRecording::encoderFinished);
    connect(&deadline_, &QTimer::timeout, this, [this] {
        if (terminal_) return;
        if (killStage_ == 0) {
            fail(state_ == "starting" ? "The recording encoder startup timed out." : "The recording encoder finalization timed out.");
        } else {
            // Only the directly owned ffmpeg process; no process-name/group kill.
            process_.kill(); killStage_ = 2;
            // finished is the actual release receipt. Do not report clean/saved
            // merely because the kill request was issued.
            emit changed();
        }
    });
}

SessionRecording::~SessionRecording() {
    // Normal UI stop is asynchronous. Destruction is a bounded emergency abort,
    // never a successful-save path; only this object's child is signaled/reaped.
    deadline_.stop(); process_.disconnect(this);
    if (process_.state() != QProcess::NotRunning) {
        process_.kill(); process_.waitForFinished(2000);
    }
    if (process_.state() == QProcess::NotRunning) removeStage();
}

bool SessionRecording::start(const QString& path, int width, int height, int fps, QString* error) {
    if (QThread::currentThread() != thread()) return refuse(error, "Recorder must be used on its owning thread.");
    if (width < 1 || height < 1 || width > 4096 || height > 4096 || qint64(width) * height > MaxFramePixels || fps < 1 || fps > 60)
        return refuse(error, "Recording dimensions or FPS exceed the supported limits.");
    const QString destination = QDir::cleanPath(path);
    if (!terminal_) {
        if ((state_ == "recording" || state_ == "starting") && destination == output_ && width == width_ && height == height_ && fps == fps_) {
            if (error) error->clear(); return true;
        }
        return refuse(error, "A recording is already active or finalizing.");
    }
    if (cleanupPending_) return refuse(error, "Prior staged-file cleanup is still pending.");
    const QFileInfo info(destination);
    if (path.isEmpty() || path.contains(QChar(0)) || !QDir::isAbsolutePath(path) || info.suffix().compare("mp4", Qt::CaseInsensitive) != 0 ||
        info.exists() || info.isSymLink() || !safeParent(info.absolutePath()))
        return refuse(error, "Choose a new MP4 file in an existing regular directory.");
    if (process_.state() != QProcess::NotRunning) return refuse(error, "The prior recording process has not exited.");
    const QString encoder = QStandardPaths::findExecutable("ffmpeg");
    if (encoder.isEmpty()) return refuse(error, "ffmpeg is unavailable; recording was not started.");
    QTemporaryFile stage(info.absolutePath() + "/.pixelforge-recording-XXXXXX.mp4");
    if (!stage.open()) return refuse(error, "Could not create the staged recording file.");
    output_ = destination; staged_ = stage.fileName(); stage.setAutoRemove(false); stage.close();
    width_ = width; height_ = height; fps_ = fps; frameBytes_ = qint64(width) * height * 3;
    submitted_ = dropped_ = invalid_ = pipeBytes_ = 0; killStage_ = 0;
    saved_ = false; terminal_ = false; cleanupPending_ = false; error_.clear(); state_ = "starting";
    // No shell, desktop device, network input, media-read tool, or user-supplied
    // argument. Caller-supplied actual composited frames are the sole input.
    process_.setProgram(encoder);
    process_.setArguments({"-hide_banner", "-loglevel", "error", "-nostdin", "-y",
        "-f", "rawvideo", "-pixel_format", "rgb24", "-video_size", QString::number(width) + "x" + QString::number(height),
        "-framerate", QString::number(fps), "-i", "pipe:0", "-an",
        "-vf", "pad=ceil(iw/2)*2:ceil(ih/2)*2:0:0:black", "-c:v", "libx264", "-threads", "1",
        "-preset", "ultrafast", "-crf", "18", "-pix_fmt", "yuv420p", "-movflags", "+faststart", "-f", "mp4", staged_});
    // Encoder text can contain local paths. Never retain or forward it to model
    // facts/logs. Exit/pipe/startup failures expose bounded fixed diagnostics.
    process_.setStandardOutputFile(QProcess::nullDevice());
    process_.setStandardErrorFile(QProcess::nullDevice());
    deadline_.start(10000);
    process_.start(QIODevice::WriteOnly);
    if (error) error->clear(); emit changed(); return true;
}

bool SessionRecording::append(const QImage& frame) {
    if (QThread::currentThread() != thread()) return false;
    if (state_ != "recording" || process_.state() != QProcess::Running) return false;
    if (frame.isNull() || frame.size() != QSize(width_, height_)) { ++invalid_; emit changed(); return false; }
    if (process_.bytesToWrite() > MaxQueuedBytes - frameBytes_) { ++dropped_; return false; }
    try {
        // Actual transparency is composited onto black; hidden RGB does not
        // become visible merely because MP4 lacks an alpha channel.
        QImage composite(width_, height_, QImage::Format_RGB32);
        if (composite.isNull()) { fail("Recording frame allocation failed."); return false; }
        composite.fill(Qt::black);
        { QPainter painter(&composite); painter.drawImage(0, 0, frame); }
        QImage rgb = composite.convertToFormat(QImage::Format_RGB888);
        if (rgb.isNull()) { fail("Recording frame conversion failed."); return false; }
        QByteArray bytes; bytes.reserve(frameBytes_);
        for (int y = 0; y < height_; ++y) bytes.append(reinterpret_cast<const char*>(rgb.constScanLine(y)), width_ * 3);
        const qint64 written = process_.write(bytes);
        if (written != frameBytes_) { fail("Recording frame enqueue failed; the partial stream will not be published."); return false; }
        ++submitted_; return true;
    } catch (const std::bad_alloc&) { fail("Recording frame allocation failed."); return false; }
}

void SessionRecording::stop() {
    if (QThread::currentThread() != thread() || terminal_ || state_ == "stopping" || state_ == "failing") return;
    state_ = "stopping";
    // Qt flushes its existing bounded write buffer before closing the channel.
    process_.closeWriteChannel(); deadline_.start(15000); emit changed();
}
void SessionRecording::fail(const QString& message) {
    if (terminal_ || state_ == "failing") return;
    error_ = message; state_ = "failing"; killStage_ = 1;
    if (process_.state() == QProcess::NotRunning) { complete(false); return; }
    process_.terminate(); deadline_.start(2000); emit changed();
}
void SessionRecording::encoderFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    if (terminal_) return;
    const bool clean = state_ == "stopping" && error_.isEmpty() && exitStatus == QProcess::NormalExit && exitCode == 0 && submitted_ > 0;
    if (!clean && error_.isEmpty()) error_ = submitted_ == 0 ? "No recording frames were submitted." : "The recording encoder did not finalize successfully.";
    if (clean) {
        const QFileInfo stage(staged_), destination(output_);
        // Atomically publish the complete same-directory inode without replacing
        // any destination. Hard-link + stage unlink avoids QFile::rename's copy
        // fallback and platform overwrite races. Unsupported filesystems fail.
        std::error_code commitError;
        if (!stage.isFile() || stage.isSymLink() || stage.size() <= 0 || destination.exists() || destination.isSymLink() ||
            !safeParent(destination.absolutePath())) {
            error_ = "Could not commit the completed recording without replacing an existing file.";
            complete(false); return;
        }
        std::filesystem::create_hard_link(nativePath(staged_), nativePath(output_), commitError);
        if (commitError) {
            error_ = "Atomic recording publication is unavailable or the destination already exists.";
            complete(false); return;
        }
        removeStage();
        if (cleanupPending_) error_ = "Recording saved, but its private staging link could not be removed.";
        complete(true); return;
    }
    complete(false);
}
void SessionRecording::removeStage() {
    if (!staged_.isEmpty()) {
        cleanupPending_ = !QFile::remove(staged_) && QFileInfo::exists(staged_);
        if (!cleanupPending_) staged_.clear();
    }
}
void SessionRecording::complete(bool saved) {
    if (terminal_) return;
    deadline_.stop(); terminal_ = true; saved_ = saved; state_ = saved ? "saved" : "failed";
    if (!saved) removeStage();
    if (cleanupPending_ && error_.isEmpty()) error_ = "Staged recording cleanup is pending.";
    const QString resultError = error_;
    emit changed(); emit finished(saved, resultError);
}
QJsonObject SessionRecording::status() const {
    return {{"recording", state_ == "recording" || state_ == "starting"}, {"state", state_},
        {"saved", saved_}, {"fps", fps_}, {"width", width_}, {"height", height_},
        {"encoded_width", width_ + (width_ % 2)}, {"encoded_height", height_ + (height_ % 2)},
        {"frames", saved_ ? submitted_ : 0}, {"frames_submitted", submitted_},
        {"frames_pipe_written", frameBytes_ ? pipeBytes_ / frameBytes_ : 0},
        {"dropped_frames", dropped_}, {"invalid_frames", invalid_}, {"queued_bytes", process_.bytesToWrite()},
        {"queue_limit_bytes", MaxQueuedBytes}, {"process_running", process_.state() != QProcess::NotRunning},
        {"capture_scope", "caller-supplied-frames"}, {"cleanup_pending", cleanupPending_},
        {"timing", "constant-fps; dropped input frames shorten playback"},
        {"alpha", "composited-on-black"}, {"codec", "H.264/yuv420p lossy"}, {"error", error_}};
}
} // namespace pixelforge::qt
