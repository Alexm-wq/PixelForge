#pragma once
#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QTimer>

namespace pixelforge::qt {
// Optional, explicit-user-intent only. The host owns that intent, task admission,
// frame cadence and any user-review finalization gate. All calls must occur on
// this QObject's thread. No desktop/window capture and no model media read API.
class SessionRecording final : public QObject {
    Q_OBJECT
public:
    static constexpr qint64 MaxFramePixels = 4ll * 1024 * 1024;
    static constexpr qint64 MaxQueuedBytes = 32ll * 1024 * 1024;
    explicit SessionRecording(QObject* parent = nullptr);
    ~SessionRecording() override;
    // Asynchronous admission, not encoder readiness. Existing destination is
    // never overwritten. Parent directory must already exist. Same active start
    // is idempotent; a different active destination/configuration is refused.
    bool start(const QString& path, int width, int height, int fps, QString* error = nullptr);
    // Nonblocking pipe enqueue. False means this frame was not accepted.
    // Backpressure drops the new frame; old queued frames retain order.
    bool append(const QImage& frame);
    void stop(); // Asynchronous drain/finalize; observe finished, not saved=true yet.
    QJsonObject status() const; // No paths, stderr, image/video bytes or account data.
signals:
    void changed();
    void finished(bool saved, const QString& error);
private:
    QProcess process_;
    QTimer deadline_;
    QString state_ = "idle", output_, staged_, error_;
    int width_ = 0, height_ = 0, fps_ = 0, killStage_ = 0;
    qint64 frameBytes_ = 0, submitted_ = 0, dropped_ = 0, invalid_ = 0, pipeBytes_ = 0;
    bool saved_ = false, terminal_ = true;
    bool cleanupPending_ = false;
    void encoderFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void fail(const QString& message);
    void complete(bool saved);
    void removeStage();
};
} // namespace pixelforge::qt
