#include "ReferenceStore.hpp"
#include <QBuffer>
#include <QCoreApplication>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtEndian>
#include <iostream>

using namespace pixelforge::qt;
#define CHECK(condition) do { if (!(condition)) { std::cerr << "ReferenceStore check failed at line " << __LINE__ << '\n'; return 1; } } while (false)
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir root;
    CHECK(root.isValid());
    QImage source(3, 2, QImage::Format_ARGB32);
    source.fill(0x00123456); // Hidden RGB must not pollute extracted palette.
    source.setPixel(0, 0, 0xffff0000); source.setPixel(1, 0, 0xffff0000);
    source.setPixel(2, 0, 0x800000ff); source.setPixel(0, 1, 0xff00ff00);
    const auto path = root.path() + "/private-source.png";
    CHECK(source.save(path, "PNG"));
    ReferenceStore refs;
    QString error;
    CHECK(refs.load("source", path, &error) && error.isEmpty());
    CHECK(refs.load("CONTENT", path, &error));
    QImage style(1, 1, QImage::Format_ARGB32); style.fill(Qt::black);
    CHECK(style.save(root.path() + "/style.png", "PNG"));
    CHECK(refs.load("style", root.path() + "/style.png", &error));
    CHECK(refs.image("source") == source && refs.image("style") == style);
    auto detached = refs.image("source"); detached.fill(Qt::white);
    CHECK(refs.image("source") == source);
    const auto meta = QJsonDocument(refs.metadata()).toJson();
    CHECK(!meta.contains(root.path().toUtf8()) && !meta.contains("private-source") && !meta.contains("path"));
    auto palette = refs.call({{"action", "palette"}, {"reference", "source"}, {"max_colors", 2}});
    CHECK(palette.success && palette.facts["unique_colors"].toInt() == 3);
    CHECK(palette.facts["colors"].toString() == "FFFF0000,800000FF");
    CHECK(palette.facts["visible_pixels"].toInteger() == 4 && palette.facts["truncated"].toBool());
    CHECK(palette.facts["counts"].toArray()[0].toInt() == 2);
    CHECK(!refs.call({{"action", "palette"}, {"reference", "source"}, {"max_colors", 257}}).success);
    auto view = refs.call({{"action", "view"}, {"reference", "source"}});
    CHECK(view.success && QImage::fromData(view.image, "PNG").convertToFormat(QImage::Format_ARGB32) == source);
    const auto observation = view.facts["observation"].toString();
    auto duplicate = refs.call({{"action", "view"}, {"reference", "source"}, {"known_observation", observation}});
    CHECK(duplicate.success && duplicate.image.isEmpty() && duplicate.facts["unchanged"].toBool());
    CHECK(refs.call({{"action", "view"}, {"reference", "source"}, {"known_observation", observation}, {"resend_image", true}}).image == view.image);
    auto scaled = refs.call({{"action", "view"}, {"reference", "source"}, {"scale", 2}});
    const auto scaledImage = QImage::fromData(scaled.image, "PNG");
    CHECK(scaled.success && scaledImage.size() == QSize(6, 4) && scaledImage.pixel(4, 0) == source.pixel(2, 0));
    auto small = refs.call({{"action", "view"}, {"reference", "source"}, {"max_edge", 2}});
    CHECK(small.success && small.facts["render_width"].toInt() == 2);
    CHECK(!refs.call({{"action", "view"}, {"reference", "source"}, {"scale", 1.5}}).success);
    CHECK(!refs.call({{"action", "load"}, {"reference", "source"}, {"path", path}}).success);
    auto seed = refs.call({{"action", "seed"}, {"reference", "source"}});
    CHECK(!seed.success && seed.facts["error"].toString() == "host_seed_required" && refs.image("source") == source);
    QFile raw(path); CHECK(raw.open(QIODevice::ReadOnly)); auto bytes = raw.readAll(); raw.close();
    qToBigEndian<quint32>(ReferenceStore::MaxDimension + 1, bytes.data() + 16);
    QFile bad(root.path() + "/bad.png"); CHECK(bad.open(QIODevice::WriteOnly)); CHECK(bad.write(bytes) == bytes.size()); bad.close();
    CHECK(!refs.load("source", bad.fileName(), &error) && refs.image("source") == source);
    CHECK(!error.contains(root.path()));
    CHECK(!refs.load("other", path, &error) && !refs.clear("other"));
    CHECK(refs.clear("source") && refs.image("source").isNull());
    CHECK(refs.image("content") == source && refs.image("style") == style);
    CHECK(!refs.call({{"action", "view"}, {"reference", "source"}}).success);
    style.fill(Qt::transparent); CHECK(style.save(root.path() + "/transparent.png", "PNG"));
    CHECK(refs.load("style", root.path() + "/transparent.png"));
    palette = refs.call({{"action", "palette"}, {"reference", "style"}});
    CHECK(palette.success && palette.facts["unique_colors"].toInt() == 0 && palette.facts["colors"].toString().isEmpty());
    CHECK(refs.load("source", path));
    CHECK(refs.call({{"action", "view"}, {"reference", "source"}}).facts["observation"].toString() == observation);
    CHECK(refs.metadata()["source"].toObject()["decoded_format"].toString() == "png");
    CHECK(refs.metadata()["source"].toObject()["first_frame_only"].toBool());
    const auto formats = ReferenceStore::supportedFormats();
    CHECK(formats.contains("png") && !formats.contains("svg"));
    QImage ordinary(8, 4, QImage::Format_RGB32); ordinary.fill(QColor(40, 80, 120));
    // The fixture has a misleading extension: content, not filename, decides.
    const auto jpeg = root.path() + "/photo.dat", bmp = root.path() + "/bitmap.dat";
    CHECK(formats.contains("jpeg") && formats.contains("bmp"));
    CHECK(ordinary.save(jpeg, "JPEG") && ordinary.save(bmp, "BMP"));
    CHECK(refs.load("content", jpeg, &error));
    CHECK(refs.image("content").size() == ordinary.size()); // JPEG is lossy.
    CHECK(refs.metadata()["content"].toObject()["decoded_format"].toString() == "jpeg");
    CHECK(refs.load("style", bmp, &error) && refs.image("style") == ordinary.convertToFormat(QImage::Format_ARGB32));
    CHECK(refs.metadata()["style"].toObject()["decoded_format"].toString() == "bmp");
    CHECK(refs.image("source") == source);
    const auto beforeUnsupported = refs.image("content");
    QFile svg(root.path() + "/unsupported.png"); CHECK(svg.open(QIODevice::WriteOnly));
    svg.write("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"8\" height=\"4\"></svg>"); svg.close();
    CHECK(!refs.load("content", svg.fileName(), &error) && refs.image("content") == beforeUnsupported);
    QFile executable(root.path() + "/executable.png"); CHECK(executable.open(QIODevice::WriteOnly));
    executable.write("MZ-this-is-not-raster-data"); executable.close();
    CHECK(!refs.load("content", executable.fileName(), &error) && refs.image("content") == beforeUnsupported);
    std::cout << "ReferenceStore PNG/JPEG/BMP/slots/palette/observation/seed-boundary PASS\n";
    return 0;
}
