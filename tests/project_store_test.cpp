#include "ProjectStore.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <iostream>

using namespace pixelforge::qt;
#define CHECK(condition) do { if (!(condition)) { std::cerr << "ProjectStore check failed at line " << __LINE__ << '\n'; return 1; } } while (false)

static bool write(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
static QByteArray read(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary;
    CHECK(temporary.isValid());
    // Resolve platform aliases such as macOS /var before testing no-link paths.
    const QString root = QFileInfo(temporary.path()).canonicalFilePath();
    ProjectBundle original;
    original.taskId = 9007199254740999ull; // Beyond double's exact integer range.
    original.packRevision = 27;
    original.projectName = QString::fromUtf8("Ocean — revision 2");
    original.projectBrief = "Accepted project: three ocean animation frames.";
    QImage image(3, 2, QImage::Format_ARGB32);
    image.fill(qRgba(0, 0, 0, 0));
    image.setPixel(0, 0, qRgba(12, 34, 56, 255));
    image.setPixel(2, 1, qRgba(9, 7, 5, 128));
    original.canvases.append({"walk_2", "walk", 2, image});
    original.canvases.append({"base", "", -1, image.mirrored()});
    original.canvases.append({"walk_0", "walk", 0, image});
    const QString project = root + "/roundtrip";
    QString error;
    CHECK(ProjectStore::save(project, original, &error));
    ProjectSaveStats repeatStats;
    CHECK(ProjectStore::save(project, original, &error, &repeatStats));
    CHECK(repeatStats.encodedImages == 0 && repeatStats.reusedImages == 3 && repeatStats.hashedImages == 0);
    ProjectBundle loaded;
    CHECK(ProjectStore::load(project, loaded, &error));
    CHECK(loaded.taskId == original.taskId && loaded.packRevision == 27);
    CHECK(loaded.projectName == original.projectName && loaded.canvases.size() == 3);
    CHECK(loaded.projectBrief == original.projectBrief);
    for (int i = 0; i < original.canvases.size(); ++i) {
        CHECK(loaded.canvases[i].name == original.canvases[i].name);
        CHECK(loaded.canvases[i].group == original.canvases[i].group);
        CHECK(loaded.canvases[i].frame == original.canvases[i].frame);
        CHECK(loaded.canvases[i].image == original.canvases[i].image);
    }
    const auto oldManifest = read(project + "/project.json");
    const auto oldObject = QJsonDocument::fromJson(oldManifest).object();
    const auto oldPng = oldObject["canvases"].toArray()[0].toObject()["file"].toString();
    ProjectBundle changed = original;
    changed.canvases[0].image.fill(Qt::red);
    ProjectSaveStats changedStats;
    CHECK(ProjectStore::save(project, changed, &error, &changedStats));
    CHECK(changedStats.encodedImages == 1 && changedStats.reusedImages == 2);
    const auto changedArray = QJsonDocument::fromJson(read(project + "/project.json")).object()["canvases"].toArray();
    CHECK(changedArray[1].toObject()["file"] == oldObject["canvases"].toArray()[1].toObject()["file"]);
    CHECK(QFile::exists(project + "/" + oldPng));
    // The prior manifest can still load its immutable prior artwork after save.
    CHECK(write(project + "/previous.pfg.json", oldManifest));
    CHECK(ProjectStore::load(project + "/previous.pfg.json", loaded, &error));
    CHECK(loaded.canvases[0].image == original.canvases[0].image);
    const auto committed = read(project + "/project.json");
    changed.canvases[1].name = "WALK_2";
    CHECK(!ProjectStore::save(project, changed, &error));
    CHECK(!error.isEmpty() && read(project + "/project.json") == committed);

    // Exact Windows writer shape, fallback path, alpha and array order.
    const QString windows = root + "/windows";
    CHECK(QDir().mkpath(windows + "/canvases/ungrouped"));
    CHECK(image.save(windows + "/canvases/ungrouped/main.png", "PNG"));
    QJsonObject canvas{{"name", "main"}, {"group", ""}, {"frame", -1}, {"width", 3}, {"height", 2}};
    QJsonObject metadata{{"version", 1}, {"task_id", 42}, {"pack_revision", 8}, {"canvases", QJsonArray{canvas}}};
    CHECK(write(windows + "/project.json", QJsonDocument(metadata).toJson()));
    CHECK(ProjectStore::load(windows, loaded, &error));
    CHECK(loaded.projectName.isEmpty() && loaded.taskId == 42 && loaded.canvases[0].image == image);
    CHECK(loaded.projectBrief.isEmpty() && loaded.changeTrackingFloor == 8 && loaded.canvases[0].changedRevision == 8);
    canvas["file"] = "canvases\\ungrouped\\main.png";
    metadata["canvases"] = QJsonArray{canvas};
    CHECK(write(windows + "/project.json", QJsonDocument(metadata).toJson()));
    CHECK(ProjectStore::load(windows, loaded, &error));
    const ProjectBundle sentinel = loaded;
    canvas["width"] = 4;
    metadata["canvases"] = QJsonArray{canvas};
    CHECK(write(windows + "/project.json", QJsonDocument(metadata).toJson()));
    CHECK(!ProjectStore::load(windows, loaded, &error));
    CHECK(loaded.taskId == sentinel.taskId && loaded.canvases[0].image == sentinel.canvases[0].image);
    canvas["width"] = 3;
    for (const QString unsafe : {QString("../outside.png"), QString("C:/outside.png"), QString("//server/share.png")}) {
        canvas["file"] = unsafe;
        metadata["canvases"] = QJsonArray{canvas};
        CHECK(write(windows + "/project.json", QJsonDocument(metadata).toJson()));
        CHECK(!ProjectStore::load(windows, loaded, &error));
    }
#ifdef Q_OS_UNIX
    CHECK(QDir().mkpath(root + "/outside"));
    CHECK(image.save(root + "/outside/main.png", "PNG"));
    CHECK(QFile::link(root + "/outside", windows + "/linked"));
    canvas["file"] = "linked/main.png";
    metadata["canvases"] = QJsonArray{canvas};
    CHECK(write(windows + "/project.json", QJsonDocument(metadata).toJson()));
    CHECK(!ProjectStore::load(windows, loaded, &error));
    CHECK(!ProjectStore::save(windows + "/linked/save", original, &error));
    CHECK(!QFile::exists(root + "/outside/save"));
#endif
    canvas["file"] = "canvases/ungrouped/main.png";
    canvas["width"] = ProjectStore::MaxDimension + 1;
    metadata["canvases"] = QJsonArray{canvas};
    CHECK(write(windows + "/project.json", QJsonDocument(metadata).toJson()));
    CHECK(!ProjectStore::load(windows, loaded, &error));
    CHECK(write(project + "/.autosave-pending", "incomplete"));
    CHECK(!ProjectStore::load(project, loaded, &error));
    CHECK(!ProjectStore::save(project, original, &error));
    CHECK(read(project + "/project.json") == committed);
    std::cout << "ProjectStore format/atomic-generation/validation checks passed\n";
    return 0;
}
