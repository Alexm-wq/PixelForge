#include "ReferenceStore.hpp"
#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStringList>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <new>
#include <vector>

namespace pixelforge::qt {
namespace {
QString slotName(const QString& value) {
    const auto name = value.toLower();
    return name == "source" || name == "content" || name == "style" ? name : QString{};
}
QString normalizedFormat(const QByteArray& format) {
    auto name = QString::fromLatin1(format).toLower();
    if (name == "jpg") name = "jpeg";
    if (name == "tif") name = "tiff";
    return name;
}
const QStringList rasterFormats{"png", "jpeg", "bmp", "gif", "tiff", "webp", "ico"};
bool fail(QString* error, const QString& message) {
    if (error) *error = message;
    return false;
}
ReferenceReply rejected(const QString& code, const QString& message) {
    return {false, {{"ok", false}, {"error", code}, {"message", message}}, {}};
}
bool integer(const QJsonObject& args, const QString& key, int fallback, int low, int high, int& out) {
    if (!args.contains(key)) { out = fallback; return true; }
    const auto value = args.value(key);
    const double n = value.toDouble(-1);
    if (!value.isDouble() || !std::isfinite(n) || std::floor(n) != n || n < low || n > high) return false;
    out = int(n); return true;
}
// PNG encoder cannot allocate an unbounded QByteArray before checking its size.
class PngSink final : public QIODevice {
public:
    QByteArray bytes;
    PngSink() { open(QIODevice::WriteOnly); }
    bool isSequential() const override { return true; }
protected:
    qint64 readData(char*, qint64) override { return -1; }
    qint64 writeData(const char* data, qint64 length) override {
        if (length < 0 || length > ReferenceStore::MaxPngBytes - bytes.size()) return -1;
        bytes.append(data, length); return length;
    }
};
QString pixelHash(const QImage& image) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArray::number(image.width()) + ':' + QByteArray::number(image.height()) + ':');
    QByteArray row(image.width() * 4, Qt::Uninitialized);
    for (int y = 0; y < image.height(); ++y) {
        const auto* pixels = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) qToBigEndian<quint32>(pixels[x], row.data() + x * 4);
        hash.addData(row);
    }
    return QString::fromLatin1(hash.result().toHex());
}
}

bool ReferenceStore::load(const QString& slot, const QString& path, QString* error) {
    const auto key = slotName(slot);
    if (key.isEmpty()) return fail(error, "Unknown reference slot.");
    // Host-selected allowlisted raster only. Never trust the filename extension.
    // No path is retained in state, replies or errors.
    const QFileInfo info(path);
    if (path.isEmpty() || path.contains(QChar(0)) || !info.isFile() || info.isSymLink())
        return fail(error, "Reference must be a regular supported raster image file.");
    try {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > MaxInputBytes)
            return fail(error, "Reference is unreadable or exceeds the encoded size limit.");
        QByteArray bytes = file.read(MaxInputBytes + 1);
        if (file.error() != QFileDevice::NoError || bytes.size() > MaxInputBytes || !file.atEnd())
            return fail(error, "Reference read failed or exceeded the encoded size limit.");
        QBuffer buffer(&bytes); buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer); reader.setDecideFormatFromContent(true); reader.setAutoTransform(false);
        const QString format = normalizedFormat(reader.format());
        if (!rasterFormats.contains(format) || !supportedFormats().contains(format))
            return fail(error, "Unsupported image format or unavailable Qt raster decoder.");
        if (!reader.canRead()) return fail(error, "Raster header validation failed.");
        const QSize dimensions = reader.size();
        const int width = dimensions.width(), height = dimensions.height();
        if (width < 1 || height < 1 || width > MaxDimension || height > MaxDimension || qint64(width) * height > MaxPixels)
            return fail(error, "Reference dimensions exceed the supported limit.");
        // Preserve the original direct PNG header/dimension guard in addition
        // to the plugin header query. Other formats use their installed reader.
        if (format == "png" && (bytes.size() < 33 || bytes.left(8) != QByteArray::fromHex("89504e470d0a1a0a") ||
            qFromBigEndian<quint32>(bytes.constData() + 8) != 13 || bytes.mid(12, 4) != "IHDR" ||
            qFromBigEndian<quint32>(bytes.constData() + 16) != quint32(width) ||
            qFromBigEndian<quint32>(bytes.constData() + 20) != quint32(height)))
            return fail(error, "PNG header validation failed.");
        qint64 total = qint64(width) * height;
        for (auto it = slots_.cbegin(); it != slots_.cend(); ++it)
            if (it.key() != key) total += qint64(it->pixels.width()) * it->pixels.height();
        if (total > MaxTotalPixels) return fail(error, "Combined reference pixel limit exceeded.");
        // One read at initial frame/page/index. No animation or page enumeration,
        // compositing, timing playback, or embedded icon-size selection.
        QImage decoded = reader.read();
        if (decoded.isNull() || decoded.size() != QSize(int(width), int(height)))
            return fail(error, "Raster decoding failed.");
        decoded = decoded.convertToFormat(QImage::Format_ARGB32);
        if (decoded.isNull()) return fail(error, "Reference pixel allocation failed.");
        Entry next; next.pixels = std::move(decoded); next.hash = pixelHash(next.pixels); next.decodedFormat = format;
        // Reloading identical pixels retains the bounded render/palette caches.
        if (!slots_.contains(key) || slots_[key].hash != next.hash) slots_.insert(key, std::move(next));
        else slots_[key].decodedFormat = format;
        if (error) error->clear();
        return true;
    } catch (const std::bad_alloc&) { return fail(error, "Reference allocation failed."); }
}

QStringList ReferenceStore::supportedFormats() {
    QStringList result;
    for (const auto& raw : QImageReader::supportedImageFormats()) {
        const auto name = normalizedFormat(raw);
        if (rasterFormats.contains(name) && !result.contains(name)) result.append(name);
    }
    result.sort(); return result;
}

bool ReferenceStore::clear(const QString& slot) {
    const auto key = slotName(slot);
    if (key.isEmpty()) return false;
    slots_.remove(key); return true;
}
QImage ReferenceStore::image(const QString& slot) const {
    const auto it = slots_.constFind(slotName(slot));
    return it == slots_.cend() ? QImage{} : it->pixels;
}
QJsonObject ReferenceStore::metadata() const {
    QJsonObject result{{"supported_formats", QJsonArray::fromStringList(supportedFormats())}};
    for (const auto& name : {QString("source"), QString("content"), QString("style")}) {
        const auto it = slots_.constFind(name);
        QJsonObject item{{"present", it != slots_.cend()}};
        if (it != slots_.cend()) {
            item.insert("width", it->pixels.width()); item.insert("height", it->pixels.height());
            item.insert("pixel_hash", it->hash);
            item.insert("decoded_format", it->decodedFormat); item.insert("first_frame_only", true);
        }
        result.insert(name, item);
    }
    return result;
}

ReferenceReply ReferenceStore::call(const QJsonObject& args) {
    if (QJsonDocument(args).toJson(QJsonDocument::Compact).size() > 8192)
        return rejected("arguments_too_large", "Reference arguments exceed the bounded limit.");
    const QStringList allowed{"action", "reference", "task_id", "expected_revision", "max_colors", "scale", "max_edge", "known_observation", "resend_image"};
    for (auto it = args.begin(); it != args.end(); ++it)
        if (!allowed.contains(it.key())) return rejected("unknown_argument", "Unsupported reference argument; file loading is host-only.");
    if (!args.value("action").isString() || !args.value("reference").isString())
        return rejected("invalid_arguments", "Provide a reference action and slot.");
    const auto key = slotName(args.value("reference").toString());
    auto it = slots_.find(key);
    if (key.isEmpty() || it == slots_.end()) return rejected("reference_unavailable", "That reference slot is not loaded. Choose an available slot or continue without it.");
    auto& entry = it.value();
    const auto action = args.value("action").toString();
    if (action == "seed") return rejected("host_seed_required", "The host must validate the accepted task and copy exact reference pixels into a same-size canvas.");
    QJsonObject facts{{"ok", true}, {"reference", key}, {"width", entry.pixels.width()}, {"height", entry.pixels.height()}, {"pixel_hash", entry.hash},
        {"decoded_format", entry.decodedFormat}, {"first_frame_only", true}};
    try {
        if (action == "palette") {
            int count;
            if (!integer(args, "max_colors", 20, 1, 256, count)) return rejected("invalid_max_colors", "max_colors must be an integer from 1 through 256.");
            if (!entry.paletteReady) {
                std::vector<quint32> pixels;
                pixels.reserve(size_t(entry.pixels.width()) * entry.pixels.height());
                for (int y = 0; y < entry.pixels.height(); ++y) {
                    const auto* row = reinterpret_cast<const QRgb*>(entry.pixels.constScanLine(y));
                    for (int x = 0; x < entry.pixels.width(); ++x) if (qAlpha(row[x])) pixels.push_back(row[x]);
                }
                std::sort(pixels.begin(), pixels.end());
                QList<QPair<quint32, qint64>> top;
                int unique = 0;
                const auto better = [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; };
                for (size_t p = 0; p < pixels.size();) {
                    size_t end = p + 1; while (end < pixels.size() && pixels[end] == pixels[p]) ++end;
                    ++unique;
                    const QPair<quint32, qint64> value{pixels[p], qint64(end - p)};
                    auto pos = std::lower_bound(top.begin(), top.end(), value, better);
                    if (pos != top.end() || top.size() < 256) { top.insert(pos, value); if (top.size() > 256) top.removeLast(); }
                    p = end;
                }
                entry.unique = unique; entry.visible = qint64(pixels.size()); entry.colors = std::move(top); entry.paletteReady = true;
            }
            QStringList colors; QJsonArray counts;
            for (int i = 0; i < std::min(count, int(entry.colors.size())); ++i) {
                colors.append(QString::number(entry.colors[i].first, 16).rightJustified(8, '0').toUpper());
                counts.append(entry.colors[i].second);
            }
            facts.insert("colors", colors.join(',')); facts.insert("counts", counts);
            facts.insert("unique_colors", entry.unique); facts.insert("visible_pixels", entry.visible);
            facts.insert("transparent_pixels_ignored", true); facts.insert("truncated", entry.unique > count);
            return {true, facts, {}};
        }
        if (action != "view") return rejected("unknown_reference_action", "Supported reference actions are view, palette, and host-mediated seed.");
        int scale, maxEdge;
        if (!integer(args, "scale", 1, 1, 16, scale) || !integer(args, "max_edge", 0, 1, MaxDimension, maxEdge))
            return rejected("invalid_view_options", "scale must be 1..16 and optional max_edge 1..16384.");
        if ((args.contains("known_observation") && (!args.value("known_observation").isString() || args.value("known_observation").toString().size() > 256)) ||
            (args.contains("resend_image") && !args.value("resend_image").isBool()))
            return rejected("invalid_observation", "Invalid observation or resend option.");
        QSize size(entry.pixels.width() * scale, entry.pixels.height() * scale);
        if (maxEdge && (size.width() > maxEdge || size.height() > maxEdge)) size.scale(maxEdge, maxEdge, Qt::KeepAspectRatio);
        size.setWidth(std::max(1, size.width())); size.setHeight(std::max(1, size.height()));
        if (size.width() > MaxDimension || size.height() > MaxDimension || qint64(size.width()) * size.height() > MaxPixels)
            return rejected("view_too_large", "Rendered reference exceeds the bounded pixel limit; request a smaller max_edge.");
        const QString observation = key + ':' + entry.hash + ':' + QString::number(size.width()) + 'x' + QString::number(size.height()) + ":nearest-png-v1";
        facts.insert("observation", observation); facts.insert("render_width", size.width()); facts.insert("render_height", size.height());
        facts.insert("scaled", size != entry.pixels.size()); facts.insert("image_mime", "image/png");
        if (!args.value("resend_image").toBool() && args.value("known_observation").toString() == observation) {
            facts.insert("unchanged", true); return {true, facts, {}};
        }
        if (entry.pngKey != observation || entry.png.isEmpty()) {
            const QImage rendered = size == entry.pixels.size() ? entry.pixels : entry.pixels.scaled(size, Qt::IgnoreAspectRatio, Qt::FastTransformation);
            PngSink sink;
            if (rendered.isNull() || !rendered.save(&sink, "PNG") || sink.bytes.isEmpty())
                return rejected("image_encode_failed", "PNG encoding failed or exceeded the byte limit; request a smaller max_edge.");
            // At most one cached rendered PNG across all slots (32 MiB total).
            for (auto other = slots_.begin(); other != slots_.end(); ++other) { other->png.clear(); other->pngKey.clear(); }
            entry.png = std::move(sink.bytes); entry.pngKey = observation;
        }
        facts.insert("image_bytes", entry.png.size());
        return {true, facts, entry.png};
    } catch (const std::bad_alloc&) { return rejected("allocation_failed", "Reference processing allocation failed."); }
}
} // namespace pixelforge::qt
