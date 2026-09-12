#include "SessionRecording.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <iostream>

using namespace pixelforge::qt;
#define CHECK(condition) do { if (!(condition)) { std::cerr << "SessionRecording check failed at line " << __LINE__ << '\n'; return 1; } } while (false)
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto encoder = QStandardPaths::findExecutable("ffmpeg");
    if (encoder.isEmpty()) { std::cerr << "SKIP: installed ffmpeg unavailable\n"; return 77; }
    QTemporaryDir directory;
    CHECK(directory.isValid());
    const auto root = QFileInfo(directory.path()).canonicalFilePath();
    const auto output = root + "/session.mp4";
    SessionRecording recorder;
    QString error;
    CHECK(!recorder.start(output, 8192, 2, 30, &error));
    CHECK(!recorder.start(output, 8, 6, 61, &error));
    CHECK(!recorder.start(root + "/missing/session.mp4", 8, 6, 12, &error));
    bool finished = false, saved = false, sent = false, valid = true;
    QEventLoop loop;
    QObject::connect(&recorder, &SessionRecording::finished, &loop, [&](bool value, const QString& message) {
        finished = true; saved = value; error = message; loop.quit();
    });
    QTimer frames;
    frames.setInterval(10);
    QObject::connect(&frames, &QTimer::timeout, &loop, [&] {
        if (sent || recorder.status()["state"].toString() != "recording") return;
        sent = true;
        valid = valid && !recorder.append(QImage(1, 1, QImage::Format_RGB32));
        QImage image(8, 6, QImage::Format_ARGB32);
        for (auto color : {Qt::red, Qt::green, Qt::blue}) { image.fill(color); valid = recorder.append(image) && valid; }
        valid = valid && !QFile::exists(output); // No published partial file.
        recorder.stop(); recorder.stop();
        valid = valid && !recorder.status()["saved"].toBool();
        valid = valid && !recorder.append(image);
    });
    QTimer watchdog; watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, &loop, &QEventLoop::quit);
    CHECK(recorder.start(output, 8, 6, 12, &error));
    CHECK(recorder.start(output, 8, 6, 12, &error)); // Exact active idempotence.
    CHECK(!recorder.start(root + "/other.mp4", 8, 6, 12, &error));
    frames.start(); watchdog.start(25000); loop.exec(); frames.stop(); watchdog.stop();
    CHECK(finished && saved && sent && valid && error.isEmpty());
    CHECK(recorder.status()["frames"].toInteger() == 3);
    CHECK(recorder.status()["invalid_frames"].toInteger() == 1);
    CHECK(!recorder.status()["process_running"].toBool() && !recorder.status()["cleanup_pending"].toBool());
    CHECK(!QJsonDocument(recorder.status()).toJson().contains(root.toUtf8()));
    CHECK(!recorder.start(output, 8, 6, 12, &error)); // Existing output protected.
    QFile file(output); CHECK(file.open(QIODevice::ReadOnly)); const auto original = file.readAll(); file.close();
    CHECK(original.size() > 16 && original.mid(4, 4) == "ftyp");
    // Actual installed decoder roundtrip verifies three frames and dimensions,
    // not pixel equality: H.264/yuv420p is explicitly lossy.
    QProcess decode;
    decode.setProgram(encoder);
    decode.setArguments({"-v", "error", "-i", output, "-f", "rawvideo", "-pix_fmt", "rgb24", "pipe:1"});
    decode.start();
    if (!decode.waitForFinished(10000)) { decode.kill(); decode.waitForFinished(2000); CHECK(false); }
    CHECK(decode.exitStatus() == QProcess::NormalExit && decode.exitCode() == 0);
    CHECK(decode.readAllStandardOutput().size() == 3 * 8 * 6 * 3);
    CHECK(QDir(root).entryList({".pixelforge-recording-*"}, QDir::Files | QDir::Hidden).isEmpty());
    std::cout << "SessionRecording real ffmpeg asynchronous start/frames/stop/atomic output/decode PASS\n";
    return 0;
}
