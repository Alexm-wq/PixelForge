#include "PixelProgram.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pixelforge::win32 {
namespace {

constexpr std::size_t kMaxPatchOperations = 20000;
constexpr std::size_t kMaxNamedMasks = 64;
constexpr std::size_t kMaxMaskBytes = 64u * 1024u * 1024u;
constexpr std::uint64_t kTokenFlag = 1ull << 63;
constexpr double kPi = 3.14159265358979323846;

struct Point { int x = 0; int y = 0; };

std::string trim(std::string_view text) {
    std::size_t b = 0, e = text.size();
    while (b < e && std::isspace(static_cast<unsigned char>(text[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(text[e - 1]))) --e;
    return std::string(text.substr(b, e - b));
}

std::string upper(std::string value) {
    for (char& c : value) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return value;
}

std::vector<std::string> fields(std::string_view line) {
    std::vector<std::string> out;
    std::string current;
    for (char c : line) {
        if (std::isspace(static_cast<unsigned char>(c)) || c == ',') {
            if (!current.empty()) { out.push_back(std::move(current)); current.clear(); }
        } else current.push_back(c);
    }
    if (!current.empty()) out.push_back(std::move(current));
    return out;
}

std::vector<std::string_view> split_pipe(std::string_view text) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto pos = text.find('|', start);
        if (pos == std::string_view::npos) {
            out.push_back(text.substr(start));
            break;
        }
        out.push_back(text.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

bool parse_int(std::string_view token, int& value) {
    if (token.empty()) return false;
    const auto* b = token.data();
    const auto* e = b + token.size();
    const auto r = std::from_chars(b, e, value);
    return r.ec == std::errc{} && r.ptr == e;
}

bool parse_double(std::string_view token, double& value) {
    if (token.empty()) return false;
    std::string copy(token);
    char* end = nullptr;
    value = std::strtod(copy.c_str(), &end);
    return end == copy.c_str() + copy.size() && std::isfinite(value);
}

bool valid_color_token(std::string_view token) {
    if (token.empty()) return false;
    if (token.front() == '#') {
        if (token.size() != 9) return false;
        for (std::size_t i = 1; i < token.size(); ++i)
            if (!std::isxdigit(static_cast<unsigned char>(token[i]))) return false;
        return true;
    }
    int index = -1;
    return parse_int(token, index) && index >= 0 && index <= 255;
}

bool valid_mask_name(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    for (const unsigned char c : name) {
        if (!std::isalnum(c) && c != '_' && c != '-') return false;
    }
    return true;
}

std::string hex32(std::uint32_t value) {
    static constexpr char h[] = "0123456789ABCDEF";
    std::string out = "#00000000";
    for (int i = 0; i < 8; ++i) out[8 - i] = h[(value >> (i * 4)) & 0xfu];
    return out;
}

std::uint64_t hash64(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31);
}

std::uint64_t hash_string(std::string_view text) {
    std::uint64_t h = 1469598103934665603ull;
    for (const unsigned char c : text) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

double noise01(int x, int y, std::uint64_t seed) {
    const auto packed = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) |
                        static_cast<std::uint32_t>(y);
    const auto h = hash64(packed ^ hash64(seed));
    return static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0);
}

class DeterministicRng {
public:
    explicit DeterministicRng(std::uint64_t seed) : state_(hash64(seed ? seed : 0x6a09e667f3bcc909ull)) {}
    std::uint64_t next() {
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return state_ * 2685821657736338717ull;
    }
    int range(int lo, int hi) {
        if (hi <= lo) return lo;
        return lo + static_cast<int>(next() % static_cast<std::uint64_t>(hi - lo + 1));
    }
private:
    std::uint64_t state_;
};

class ProgramRaster {
public:
    ProgramRaster(int width, int height, const std::vector<std::uint32_t>& pixels)
        : w_(width), h_(height), original_(pixels), cells_(pixels.size()) {
        for (std::size_t i = 0; i < pixels.size(); ++i) cells_[i] = pixels[i];
    }

    std::uint64_t color(std::string token) {
        // Exact colors can stay as their actual ARGB value. Besides shrinking
        // redundant patches, this lets masked operations compare an exact source
        // color against pixels that already existed before this program call.
        if (!token.empty() && token.front() == '#') {
            std::uint32_t value = 0;
            const auto parsed = std::from_chars(token.data() + 1, token.data() + token.size(), value, 16);
            if (parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size()) return value;
        }
        for (std::size_t i = 0; i < tokens_.size(); ++i)
            if (tokens_[i] == token) return kTokenFlag | static_cast<std::uint64_t>(i);
        tokens_.push_back(std::move(token));
        return kTokenFlag | static_cast<std::uint64_t>(tokens_.size() - 1);
    }

    void set(int x, int y, std::uint64_t c) {
        if (x < 0 || y < 0 || x >= w_ || y >= h_) { ++clipped_; return; }
        cells_[static_cast<std::size_t>(y) * w_ + x] = c;
    }

    void hline(int x, int y, int len, std::uint64_t c) {
        if (len <= 0) return;
        if (y < 0 || y >= h_) { clipped_ += static_cast<std::size_t>(len); return; }
        const long long x1ll = static_cast<long long>(x) + len - 1;
        const int a = std::max(0, x);
        const int b = std::min(w_ - 1, x1ll > std::numeric_limits<int>::max() ? w_ - 1 : static_cast<int>(x1ll));
        if (a > b) { clipped_ += static_cast<std::size_t>(len); return; }
        clipped_ += static_cast<std::size_t>(a - x) + static_cast<std::size_t>(x1ll - b);
        for (int px = a; px <= b; ++px) cells_[static_cast<std::size_t>(y) * w_ + px] = c;
    }

    void vline(int x, int y, int len, std::uint64_t c) {
        if (len <= 0) return;
        if (x < 0 || x >= w_) { clipped_ += static_cast<std::size_t>(len); return; }
        const long long y1ll = static_cast<long long>(y) + len - 1;
        const int a = std::max(0, y);
        const int b = std::min(h_ - 1, y1ll > std::numeric_limits<int>::max() ? h_ - 1 : static_cast<int>(y1ll));
        if (a > b) { clipped_ += static_cast<std::size_t>(len); return; }
        clipped_ += static_cast<std::size_t>(a - y) + static_cast<std::size_t>(y1ll - b);
        for (int py = a; py <= b; ++py) cells_[static_cast<std::size_t>(py) * w_ + x] = c;
    }

    void rect(int x, int y, int width, int height, std::uint64_t c) {
        if (width <= 0 || height <= 0) return;
        for (int dy = 0; dy < height; ++dy) hline(x, y + dy, width, c);
    }

    void box(int x, int y, int width, int height, std::uint64_t c) {
        if (width <= 0 || height <= 0) return;
        hline(x, y, width, c);
        if (height > 1) hline(x, y + height - 1, width, c);
        if (height > 2) {
            vline(x, y + 1, height - 2, c);
            if (width > 1) vline(x + width - 1, y + 1, height - 2, c);
        }
    }

    void line(int x0, int y0, int x1, int y1, std::uint64_t c) {
        long long dx = std::llabs(static_cast<long long>(x1) - x0);
        const int sx = x0 < x1 ? 1 : -1;
        long long dy = -std::llabs(static_cast<long long>(y1) - y0);
        const int sy = y0 < y1 ? 1 : -1;
        long long err = dx + dy;
        int guard = 0;
        const int guard_limit = std::max(w_, h_) * 8 + 100000;
        for (;;) {
            set(x0, y0, c);
            if (x0 == x1 && y0 == y1) break;
            if (++guard > guard_limit) break;
            const long long e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }

    void ellipse(int cx, int cy, int rx, int ry, std::uint64_t c, bool fill) {
        if (rx < 0 || ry < 0) return;
        if (rx == 0 && ry == 0) { set(cx, cy, c); return; }
        if (fill) {
            if (ry == 0) { hline(cx - rx, cy, rx * 2 + 1, c); return; }
            for (int y = -ry; y <= ry; ++y) {
                const double yy = static_cast<double>(y) / ry;
                const int ex = static_cast<int>(std::floor(rx * std::sqrt(std::max(0.0, 1.0 - yy * yy)) + 0.5));
                hline(cx - ex, cy + y, ex * 2 + 1, c);
            }
            return;
        }
        const int steps = std::max(16, static_cast<int>(std::ceil(2.0 * kPi * std::max(rx, ry) * 1.5)));
        Point prev{cx + rx, cy};
        for (int i = 1; i <= steps; ++i) {
            const double a = 2.0 * kPi * i / steps;
            Point p{cx + static_cast<int>(std::lround(rx * std::cos(a))),
                    cy + static_cast<int>(std::lround(ry * std::sin(a)))};
            line(prev.x, prev.y, p.x, p.y, c);
            prev = p;
        }
    }

    void quadratic(Point p0, Point p1, Point p2, std::uint64_t c) {
        const int steps = std::max(4, std::max({std::abs(p2.x - p0.x), std::abs(p2.y - p0.y),
                                                std::abs(p1.x - p0.x), std::abs(p1.y - p0.y)}) * 2);
        Point prev = p0;
        for (int i = 1; i <= steps; ++i) {
            const double t = static_cast<double>(i) / steps, u = 1.0 - t;
            Point p{static_cast<int>(std::lround(u*u*p0.x + 2*u*t*p1.x + t*t*p2.x)),
                    static_cast<int>(std::lround(u*u*p0.y + 2*u*t*p1.y + t*t*p2.y))};
            line(prev.x, prev.y, p.x, p.y, c); prev = p;
        }
    }

    void cubic(Point p0, Point p1, Point p2, Point p3, std::uint64_t c) {
        const int steps = std::max(6, std::max({std::abs(p3.x - p0.x), std::abs(p3.y - p0.y),
                                                std::abs(p1.x - p0.x), std::abs(p1.y - p0.y),
                                                std::abs(p2.x - p3.x), std::abs(p2.y - p3.y)}) * 2);
        Point prev = p0;
        for (int i = 1; i <= steps; ++i) {
            const double t = static_cast<double>(i) / steps, u = 1.0 - t;
            Point p{static_cast<int>(std::lround(u*u*u*p0.x + 3*u*u*t*p1.x + 3*u*t*t*p2.x + t*t*t*p3.x)),
                    static_cast<int>(std::lround(u*u*u*p0.y + 3*u*u*t*p1.y + 3*u*t*t*p2.y + t*t*t*p3.y))};
            line(prev.x, prev.y, p.x, p.y, c); prev = p;
        }
    }

    void polygon(const std::vector<Point>& pts, std::uint64_t c, bool fill) {
        if (pts.size() < 2) return;
        if (!fill) {
            for (std::size_t i = 0; i < pts.size(); ++i) {
                const auto& a = pts[i]; const auto& b = pts[(i + 1) % pts.size()];
                line(a.x, a.y, b.x, b.y, c);
            }
            return;
        }
        int miny = pts[0].y, maxy = pts[0].y;
        for (const auto& p : pts) { miny = std::min(miny, p.y); maxy = std::max(maxy, p.y); }
        miny = std::max(miny, 0); maxy = std::min(maxy, h_ - 1);
        for (int y = miny; y <= maxy; ++y) {
            std::vector<double> xs;
            for (std::size_t i = 0; i < pts.size(); ++i) {
                const auto& a = pts[i]; const auto& b = pts[(i + 1) % pts.size()];
                if (a.y == b.y) continue;
                const int lo = std::min(a.y, b.y), hi = std::max(a.y, b.y);
                if (y < lo || y >= hi) continue;
                xs.push_back(a.x + (static_cast<double>(y - a.y) * (b.x - a.x)) / (b.y - a.y));
            }
            std::sort(xs.begin(), xs.end());
            for (std::size_t i = 0; i + 1 < xs.size(); i += 2) {
                const int x0 = static_cast<int>(std::ceil(xs[i]));
                const int x1 = static_cast<int>(std::floor(xs[i + 1]));
                if (x1 >= x0) hline(x0, y, x1 - x0 + 1, c);
            }
        }
        polygon(pts, c, false);
    }

    void copy(int sx, int sy, int width, int height, int dx, int dy, bool mirror_x, bool mirror_y) {
        if (width <= 0 || height <= 0) return;
        std::vector<std::uint64_t> temp(static_cast<std::size_t>(width) * height, 0);
        std::vector<bool> valid(temp.size(), false);
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
            const int px = sx + x, py = sy + y;
            const auto idx = static_cast<std::size_t>(y) * width + x;
            if (px >= 0 && py >= 0 && px < w_ && py < h_) {
                temp[idx] = cells_[static_cast<std::size_t>(py) * w_ + px]; valid[idx] = true;
            } else ++clipped_;
        }
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
            const int source_x = mirror_x ? (width - 1 - x) : x;
            const int source_y = mirror_y ? (height - 1 - y) : y;
            const auto idx = static_cast<std::size_t>(source_y) * width + source_x;
            if (valid[idx]) set(dx + x, dy + y, temp[idx]);
        }
    }

    bool mask_rect(std::string_view name, int x, int y, int width, int height, std::string& error) {
        Mask mask(cells_.size(), 0);
        if (width > 0 && height > 0) {
            const long long x2ll = static_cast<long long>(x) + width - 1;
            const long long y2ll = static_cast<long long>(y) + height - 1;
            const int x0 = std::max(0, x), y0 = std::max(0, y);
            const int x1 = std::min(w_ - 1, x2ll > std::numeric_limits<int>::max() ? w_ - 1 : static_cast<int>(x2ll));
            const int y1 = std::min(h_ - 1, y2ll > std::numeric_limits<int>::max() ? h_ - 1 : static_cast<int>(y2ll));
            if (x0 <= x1 && y0 <= y1) {
                for (int py = y0; py <= y1; ++py)
                    for (int px = x0; px <= x1; ++px)
                        mask[static_cast<std::size_t>(py) * w_ + px] = 1;
            }
        }
        return store_mask(name, std::move(mask), error);
    }

    bool mask_ellipse(std::string_view name, int cx, int cy, int rx, int ry, std::string& error) {
        Mask mask(cells_.size(), 0);
        if (rx == 0 && ry == 0) {
            mask_set(mask, cx, cy);
        } else if (ry == 0) {
            for (int x = -rx; x <= rx; ++x) mask_set(mask, cx + x, cy);
        } else if (rx == 0) {
            for (int y = -ry; y <= ry; ++y) mask_set(mask, cx, cy + y);
        } else {
            for (int y = -ry; y <= ry; ++y) {
                const double yy = static_cast<double>(y) / ry;
                const int ex = static_cast<int>(std::floor(rx * std::sqrt(std::max(0.0, 1.0 - yy * yy)) + 0.5));
                for (int x = -ex; x <= ex; ++x) mask_set(mask, cx + x, cy + y);
            }
        }
        return store_mask(name, std::move(mask), error);
    }

    bool mask_polygon(std::string_view name, const std::vector<Point>& pts, std::string& error) {
        Mask mask(cells_.size(), 0);
        if (!pts.empty()) {
            int miny = pts[0].y, maxy = pts[0].y;
            for (const auto& p : pts) { miny = std::min(miny, p.y); maxy = std::max(maxy, p.y); }
            miny = std::max(miny, 0); maxy = std::min(maxy, h_ - 1);
            for (int y = miny; y <= maxy; ++y) {
                std::vector<double> xs;
                for (std::size_t i = 0; i < pts.size(); ++i) {
                    const auto& a = pts[i]; const auto& b = pts[(i + 1) % pts.size()];
                    if (a.y == b.y) continue;
                    const int lo = std::min(a.y, b.y), hi = std::max(a.y, b.y);
                    if (y < lo || y >= hi) continue;
                    xs.push_back(a.x + (static_cast<double>(y - a.y) * (b.x - a.x)) / (b.y - a.y));
                }
                std::sort(xs.begin(), xs.end());
                for (std::size_t i = 0; i + 1 < xs.size(); i += 2) {
                    const int x0 = std::max(0, static_cast<int>(std::ceil(xs[i])));
                    const int x1 = std::min(w_ - 1, static_cast<int>(std::floor(xs[i + 1])));
                    if (x1 >= x0) for (int x = x0; x <= x1; ++x) mask[static_cast<std::size_t>(y) * w_ + x] = 1;
                }
            }
            // Include the polygon boundary so masks match FPOLY geometry.
            for (std::size_t i = 0; i < pts.size(); ++i) mask_line(mask, pts[i], pts[(i + 1) % pts.size()]);
        }
        return store_mask(name, std::move(mask), error);
    }

    bool clear_mask(std::string_view name) {
        return masks_.erase(mask_key(name)) != 0;
    }

    bool fill_mask(std::string_view name, std::uint64_t c) {
        const auto* mask = get_mask(name);
        if (!mask) return false;
        for (std::size_t i = 0; i < mask->size(); ++i) if ((*mask)[i]) cells_[i] = c;
        return true;
    }

    bool shade_mask(std::string_view name, std::uint64_t shadow, std::uint64_t mid, std::uint64_t highlight,
                    std::string_view direction, double strength, double irregularity) {
        const auto* mask = get_mask(name);
        if (!mask) return false;
        int dx = 0, dy = 0;
        if (!direction_vector(direction, dx, dy)) return false;

        double min_p = std::numeric_limits<double>::infinity();
        double max_p = -std::numeric_limits<double>::infinity();
        for (int y = 0; y < h_; ++y) for (int x = 0; x < w_; ++x) {
            if (!(*mask)[static_cast<std::size_t>(y) * w_ + x]) continue;
            const double p = static_cast<double>(x * dx + y * dy);
            min_p = std::min(min_p, p); max_p = std::max(max_p, p);
        }
        if (!std::isfinite(min_p)) return true;
        const double span = max_p - min_p;
        const double edge = std::clamp(strength, 0.0, 1.0) * 0.45;
        const double jitter_scale = std::clamp(irregularity, 0.0, 1.0) * 0.30;
        const auto seed = hash_string(mask_key(name)) ^ 0x243f6a8885a308d3ull;
        for (int y = 0; y < h_; ++y) for (int x = 0; x < w_; ++x) {
            const auto idx = static_cast<std::size_t>(y) * w_ + x;
            if (!(*mask)[idx]) continue;
            double t = span > 0.0 ? (static_cast<double>(x * dx + y * dy) - min_p) / span : 0.5;
            if (jitter_scale > 0.0) {
                const double jitter = noise01(x / 2, y / 2, seed) * 2.0 - 1.0;
                t = std::clamp(t + jitter * jitter_scale, 0.0, 1.0);
            }
            cells_[idx] = t < edge ? shadow : (t > 1.0 - edge ? highlight : mid);
        }
        return true;
    }

    bool dither_mask(std::string_view name, std::uint64_t a, std::uint64_t b, double amount,
                     std::string_view pattern, std::uint64_t seed) {
        const auto* mask = get_mask(name);
        if (!mask) return false;
        const auto mode = upper(std::string(pattern));
        if (mode != "IRREGULAR" && mode != "BAYER" && mode != "CHECKER") return false;
        amount = std::clamp(amount, 0.0, 1.0);
        static constexpr std::array<int, 16> bayer4 = {
             0,  8,  2, 10,
            12,  4, 14,  6,
             3, 11,  1,  9,
            15,  7, 13,  5
        };
        for (int y = 0; y < h_; ++y) for (int x = 0; x < w_; ++x) {
            const auto idx = static_cast<std::size_t>(y) * w_ + x;
            if (!(*mask)[idx]) continue;
            double sample = 0.0;
            if (mode == "IRREGULAR") {
                sample = noise01(x, y, seed ^ 0x13198a2e03707344ull);
            } else if (mode == "BAYER") {
                const int ox = static_cast<int>(seed & 3ull), oy = static_cast<int>((seed >> 2) & 3ull);
                const int px = (x + ox) & 3, py = (y + oy) & 3;
                sample = (bayer4[static_cast<std::size_t>(py) * 4 + px] + 0.5) / 16.0;
            } else {
                const int ox = static_cast<int>(seed & 1ull), oy = static_cast<int>((seed >> 1) & 1ull);
                sample = (((x + ox) & 1) ^ ((y + oy) & 1)) ? 0.75 : 0.25;
            }
            // DITHER is a controlled breakup pass: only replace pixels that are
            // currently color_a. This lets it follow SHADE/CLUSTERS without
            // flattening highlights, shadows, or texture colors laid down earlier.
            if (cells_[idx] == a && sample < amount) cells_[idx] = b;
        }
        return true;
    }

    bool clusters_mask(std::string_view name, const std::vector<std::uint64_t>& colors,
                       double density, int size_min, int size_max, std::uint64_t seed) {
        const auto* mask = get_mask(name);
        if (!mask) return false;
        if (colors.empty()) return true;
        density = std::clamp(density, 0.0, 1.0);
        std::vector<int> members;
        members.reserve(std::min<std::size_t>(mask->size(), 65536));
        for (std::size_t i = 0; i < mask->size(); ++i) if ((*mask)[i]) members.push_back(static_cast<int>(i));
        if (members.empty() || density <= 0.0) return true;

        const std::size_t target = std::min<std::size_t>(members.size(),
            static_cast<std::size_t>(std::llround(static_cast<double>(members.size()) * density)));
        if (target == 0) return true;
        std::vector<std::uint8_t> used(mask->size(), 0);
        DeterministicRng rng(seed ^ hash_string(mask_key(name)) ^ 0xa4093822299f31d0ull);
        std::size_t painted = 0;
        std::size_t global_attempts = 0;
        const std::size_t global_limit = target * 24 + 4096;
        static constexpr std::array<Point, 8> dirs = {{{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}}};

        while (painted < target && global_attempts++ < global_limit) {
            const int seed_index = members[static_cast<std::size_t>(rng.next() % members.size())];
            if (used[static_cast<std::size_t>(seed_index)]) continue;
            const int wanted = std::min<int>(rng.range(size_min, size_max), static_cast<int>(target - painted));
            const auto cluster_color = colors[static_cast<std::size_t>(rng.next() % colors.size())];
            std::vector<Point> cluster;
            Point start{seed_index % w_, seed_index / w_};
            cluster.push_back(start);
            used[static_cast<std::size_t>(seed_index)] = 1;
            cells_[static_cast<std::size_t>(seed_index)] = cluster_color;
            ++painted;

            int added = 1;
            int attempts = 0;
            const int attempt_limit = std::max(16, wanted * 16);
            while (added < wanted && attempts++ < attempt_limit && painted < target) {
                const Point base = cluster[static_cast<std::size_t>(rng.next() % cluster.size())];
                const Point d = dirs[static_cast<std::size_t>(rng.next() % dirs.size())];
                const int nx = base.x + d.x, ny = base.y + d.y;
                if (nx < 0 || ny < 0 || nx >= w_ || ny >= h_) continue;
                const auto ni = static_cast<std::size_t>(ny) * w_ + nx;
                if (!(*mask)[ni] || used[ni]) continue;
                used[ni] = 1;
                cells_[ni] = cluster_color;
                cluster.push_back({nx, ny});
                ++added; ++painted;
            }
        }
        return true;
    }

    bool outline_mask(std::string_view name, std::uint64_t c, std::string_view mode) {
        const auto* mask = get_mask(name);
        if (!mask) return false;
        const auto m = upper(std::string(mode));
        if (m != "OUTSIDE" && m != "INSIDE") return false;
        std::vector<std::uint8_t> edge(mask->size(), 0);
        for (int y = 0; y < h_; ++y) for (int x = 0; x < w_; ++x) {
            const auto idx = static_cast<std::size_t>(y) * w_ + x;
            if (m == "INSIDE" && !(*mask)[idx]) continue;
            if (m == "OUTSIDE" && (*mask)[idx]) continue;
            bool adjacent = false;
            for (int dy = -1; dy <= 1 && !adjacent; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) continue;
                const int nx = x + dx, ny = y + dy;
                if (m == "INSIDE") {
                    if (nx < 0 || ny < 0 || nx >= w_ || ny >= h_ ||
                        !(*mask)[static_cast<std::size_t>(ny) * w_ + nx]) { adjacent = true; break; }
                } else if (nx >= 0 && ny >= 0 && nx < w_ && ny < h_ &&
                           (*mask)[static_cast<std::size_t>(ny) * w_ + nx]) {
                    adjacent = true; break;
                }
            }
            if (adjacent) edge[idx] = 1;
        }
        for (std::size_t i = 0; i < edge.size(); ++i) if (edge[i]) cells_[i] = c;
        return true;
    }

    std::string patch(std::size_t& operations) const {
        std::string out;
        operations = 0;
        for (int y = 0; y < h_; ++y) {
            int x = 0;
            while (x < w_) {
                const auto idx = static_cast<std::size_t>(y) * w_ + x;
                if (same_as_original(idx)) { ++x; continue; }
                const auto value = cells_[idx];
                int x2 = x + 1;
                while (x2 < w_) {
                    const auto i2 = static_cast<std::size_t>(y) * w_ + x2;
                    if (same_as_original(i2) || cells_[i2] != value) break;
                    ++x2;
                }
                if (!out.empty()) out.push_back(';');
                if (x2 - x == 1) out += "P," + std::to_string(x) + "," + std::to_string(y) + "," + token(value);
                else out += "H," + std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(x2 - x) + "," + token(value);
                ++operations;
                if (operations > kMaxPatchOperations) return {};
                x = x2;
            }
        }
        if (operations == 0 && w_ > 0 && h_ > 0) {
            out = "P,0,0," + hex32(original_[0]);
            operations = 1;
        }
        return out;
    }

    std::size_t clipped() const { return clipped_; }

private:
    using Mask = std::vector<std::uint8_t>;

    static std::string mask_key(std::string_view name) {
        return upper(std::string(name));
    }

    void mask_set(Mask& mask, int x, int y) const {
        if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
        mask[static_cast<std::size_t>(y) * w_ + x] = 1;
    }

    void mask_line(Mask& mask, Point a, Point b) const {
        long long dx = std::llabs(static_cast<long long>(b.x) - a.x);
        const int sx = a.x < b.x ? 1 : -1;
        long long dy = -std::llabs(static_cast<long long>(b.y) - a.y);
        const int sy = a.y < b.y ? 1 : -1;
        long long err = dx + dy;
        int guard = 0;
        const int guard_limit = std::max(w_, h_) * 8 + 100000;
        for (;;) {
            mask_set(mask, a.x, a.y);
            if (a.x == b.x && a.y == b.y) break;
            if (++guard > guard_limit) break;
            const long long e2 = 2 * err;
            if (e2 >= dy) { err += dy; a.x += sx; }
            if (e2 <= dx) { err += dx; a.y += sy; }
        }
    }

    bool store_mask(std::string_view name, Mask mask, std::string& error) {
        const auto key = mask_key(name);
        const auto found = masks_.find(key);
        if (found == masks_.end()) {
            if (masks_.size() >= kMaxNamedMasks) {
                error = "Program exceeds the 64 named-mask limit.";
                return false;
            }
            if ((masks_.size() + 1) * mask.size() > kMaxMaskBytes) {
                error = "Named masks would exceed the 64 MiB mask-memory limit; clear/reuse masks or split the pass.";
                return false;
            }
        }
        masks_[key] = std::move(mask);
        return true;
    }

    const Mask* get_mask(std::string_view name) const {
        const auto it = masks_.find(mask_key(name));
        return it == masks_.end() ? nullptr : &it->second;
    }

    static bool direction_vector(std::string_view direction, int& dx, int& dy) {
        const auto d = upper(std::string(direction));
        if (d == "N")  { dx = 0;  dy = -1; return true; }
        if (d == "NE") { dx = 1;  dy = -1; return true; }
        if (d == "E")  { dx = 1;  dy = 0;  return true; }
        if (d == "SE") { dx = 1;  dy = 1;  return true; }
        if (d == "S")  { dx = 0;  dy = 1;  return true; }
        if (d == "SW") { dx = -1; dy = 1;  return true; }
        if (d == "W")  { dx = -1; dy = 0;  return true; }
        if (d == "NW") { dx = -1; dy = -1; return true; }
        return false;
    }

    bool same_as_original(std::size_t i) const {
        return (cells_[i] & kTokenFlag) == 0 && static_cast<std::uint32_t>(cells_[i]) == original_[i];
    }
    std::string token(std::uint64_t value) const {
        if (value & kTokenFlag) return tokens_[static_cast<std::size_t>(value & ~kTokenFlag)];
        return hex32(static_cast<std::uint32_t>(value));
    }

    int w_ = 0, h_ = 0;
    const std::vector<std::uint32_t>& original_;
    std::vector<std::uint64_t> cells_;
    std::vector<std::string> tokens_;
    std::unordered_map<std::string, Mask> masks_;
    std::size_t clipped_ = 0;
};

bool need_count(const std::vector<std::string>& f, std::size_t n, std::string& error, std::size_t line) {
    if (f.size() == n) return true;
    error = "Line " + std::to_string(line) + ": wrong field count for " + (f.empty() ? std::string("command") : f[0]) + ".";
    return false;
}

} // namespace

PixelProgramResult compile_pixel_program(std::string_view program,
                                         int width,
                                         int height,
                                         const std::vector<std::uint32_t>& current_pixels) {
    PixelProgramResult result;
    if (width <= 0 || height <= 0 || current_pixels.size() != static_cast<std::size_t>(width) * height) {
        result.error = "Canvas snapshot is invalid.";
        return result;
    }
    ProgramRaster raster(width, height, current_pixels);
    std::string normalized(program);
    for (char& c : normalized) if (c == ';') c = '\n';
    std::istringstream input(normalized);
    std::string raw;
    std::size_t line_no = 0;
    while (std::getline(input, raw)) {
        ++line_no;
        const auto comment = raw.find("//");
        if (comment != std::string::npos) raw.resize(comment);
        raw = trim(raw);
        if (raw.empty()) continue;
        auto f = fields(raw);
        if (f.empty()) continue;
        const auto command = upper(f[0]);
        auto integer = [&](std::size_t i, int& out) {
            if (i < f.size() && parse_int(f[i], out)) return true;
            result.error = "Line " + std::to_string(line_no) + ": invalid integer '" + (i < f.size() ? f[i] : std::string("<missing>")) + "'.";
            return false;
        };
        auto number = [&](std::size_t i, double& out) {
            if (i < f.size() && parse_double(f[i], out)) return true;
            result.error = "Line " + std::to_string(line_no) + ": invalid number '" + (i < f.size() ? f[i] : std::string("<missing>")) + "'.";
            return false;
        };
        auto color = [&](std::size_t i, std::uint64_t& out) {
            if (i >= f.size() || !valid_color_token(f[i])) {
                result.error = "Line " + std::to_string(line_no) + ": color must be palette index 0..255 or #AARRGGBB.";
                return false;
            }
            out = raster.color(f[i]); return true;
        };
        auto mask_name = [&](std::size_t i) {
            if (i < f.size() && valid_mask_name(f[i])) return true;
            result.error = "Line " + std::to_string(line_no) + ": mask name must be 1..64 letters, digits, '_' or '-'.";
            return false;
        };
        auto range01 = [&](double value, std::string_view field) {
            if (value >= 0.0 && value <= 1.0) return true;
            result.error = "Line " + std::to_string(line_no) + ": " + std::string(field) + " must be between 0 and 1.";
            return false;
        };
        auto known_mask_error = [&]() {
            result.error = "Line " + std::to_string(line_no) + ": unknown mask '" + (f.size() > 1 ? f[1] : std::string("<missing>")) + "'.";
        };

        if (command == "CLEAR") {
            if (!need_count(f, 2, result.error, line_no)) return result;
            std::uint64_t c; if (!color(1, c)) return result;
            raster.rect(0, 0, width, height, c);
        } else if (command == "P" || command == "PIXEL") {
            if (!need_count(f, 4, result.error, line_no)) return result;
            int x,y; std::uint64_t c; if (!integer(1,x)||!integer(2,y)||!color(3,c)) return result; raster.set(x,y,c);
        } else if (command == "H" || command == "HLINE") {
            if (!need_count(f, 5, result.error, line_no)) return result;
            int x,y,n; std::uint64_t c; if (!integer(1,x)||!integer(2,y)||!integer(3,n)||!color(4,c)) return result; raster.hline(x,y,n,c);
        } else if (command == "V" || command == "VLINE") {
            if (!need_count(f, 5, result.error, line_no)) return result;
            int x,y,n; std::uint64_t c; if (!integer(1,x)||!integer(2,y)||!integer(3,n)||!color(4,c)) return result; raster.vline(x,y,n,c);
        } else if (command == "R" || command == "RECT") {
            if (!need_count(f, 6, result.error, line_no)) return result;
            int x,y,w,h; std::uint64_t c; if (!integer(1,x)||!integer(2,y)||!integer(3,w)||!integer(4,h)||!color(5,c)) return result; raster.rect(x,y,w,h,c);
        } else if (command == "BOX") {
            if (!need_count(f, 6, result.error, line_no)) return result;
            int x,y,w,h; std::uint64_t c; if (!integer(1,x)||!integer(2,y)||!integer(3,w)||!integer(4,h)||!color(5,c)) return result; raster.box(x,y,w,h,c);
        } else if (command == "L" || command == "LINE") {
            if (!need_count(f, 6, result.error, line_no)) return result;
            int x0,y0,x1,y1; std::uint64_t c; if (!integer(1,x0)||!integer(2,y0)||!integer(3,x1)||!integer(4,y1)||!color(5,c)) return result; raster.line(x0,y0,x1,y1,c);
        } else if (command == "ELLIPSE" || command == "FELLIPSE") {
            if (!need_count(f, 6, result.error, line_no)) return result;
            int cx,cy,rx,ry; std::uint64_t c; if (!integer(1,cx)||!integer(2,cy)||!integer(3,rx)||!integer(4,ry)||!color(5,c)) return result;
            if (rx < 0 || ry < 0) { result.error = "Line " + std::to_string(line_no) + ": ellipse radii must be non-negative."; return result; }
            raster.ellipse(cx,cy,rx,ry,c,command=="FELLIPSE");
        } else if (command == "CIRCLE" || command == "FCIRCLE") {
            if (!need_count(f, 5, result.error, line_no)) return result;
            int cx,cy,r; std::uint64_t c; if (!integer(1,cx)||!integer(2,cy)||!integer(3,r)||!color(4,c)) return result;
            if (r < 0) { result.error = "Line " + std::to_string(line_no) + ": circle radius must be non-negative."; return result; }
            raster.ellipse(cx,cy,r,r,c,command=="FCIRCLE");
        } else if (command == "Q" || command == "QUAD") {
            if (!need_count(f, 8, result.error, line_no)) return result;
            Point a,b,cpt; std::uint64_t c; if(!integer(1,a.x)||!integer(2,a.y)||!integer(3,b.x)||!integer(4,b.y)||!integer(5,cpt.x)||!integer(6,cpt.y)||!color(7,c)) return result; raster.quadratic(a,b,cpt,c);
        } else if (command == "C" || command == "CUBIC") {
            if (!need_count(f, 10, result.error, line_no)) return result;
            Point a,b,cpt,d; std::uint64_t c; if(!integer(1,a.x)||!integer(2,a.y)||!integer(3,b.x)||!integer(4,b.y)||!integer(5,cpt.x)||!integer(6,cpt.y)||!integer(7,d.x)||!integer(8,d.y)||!color(9,c)) return result; raster.cubic(a,b,cpt,d,c);
        } else if (command == "POLY" || command == "FPOLY") {
            if (f.size() < 8 || ((f.size() - 2) % 2) != 0) { result.error = "Line " + std::to_string(line_no) + ": POLY/FPOLY syntax is command color x0 y0 x1 y1 x2 y2 [...]."; return result; }
            std::uint64_t c; if (!color(1,c)) return result;
            std::vector<Point> pts; pts.reserve((f.size()-2)/2);
            for (std::size_t i=2;i<f.size();i+=2) { Point p; if(!integer(i,p.x)||!integer(i+1,p.y)) return result; pts.push_back(p); }
            raster.polygon(pts,c,command=="FPOLY");
        } else if (command == "COPY" || command == "FLIPX" || command == "FLIPY" || command == "FLIPXY") {
            if (!need_count(f, 7, result.error, line_no)) return result;
            int sx,sy,w,h,dx,dy; if(!integer(1,sx)||!integer(2,sy)||!integer(3,w)||!integer(4,h)||!integer(5,dx)||!integer(6,dy)) return result;
            if (w <= 0 || h <= 0) { result.error = "Line " + std::to_string(line_no) + ": copy width/height must be positive."; return result; }
            raster.copy(sx,sy,w,h,dx,dy,command=="FLIPX"||command=="FLIPXY",command=="FLIPY"||command=="FLIPXY");
        } else if (command == "MASKRECT") {
            if (!need_count(f, 6, result.error, line_no) || !mask_name(1)) return result;
            int x,y,w,h; if(!integer(2,x)||!integer(3,y)||!integer(4,w)||!integer(5,h)) return result;
            if (w <= 0 || h <= 0) { result.error = "Line " + std::to_string(line_no) + ": mask rectangle width/height must be positive."; return result; }
            std::string error; if (!raster.mask_rect(f[1],x,y,w,h,error)) { result.error = "Line " + std::to_string(line_no) + ": " + error; return result; }
        } else if (command == "MASKELLIPSE") {
            if (!need_count(f, 6, result.error, line_no) || !mask_name(1)) return result;
            int cx,cy,rx,ry; if(!integer(2,cx)||!integer(3,cy)||!integer(4,rx)||!integer(5,ry)) return result;
            if (rx < 0 || ry < 0) { result.error = "Line " + std::to_string(line_no) + ": mask ellipse radii must be non-negative."; return result; }
            std::string error; if (!raster.mask_ellipse(f[1],cx,cy,rx,ry,error)) { result.error = "Line " + std::to_string(line_no) + ": " + error; return result; }
        } else if (command == "MASKPOLY") {
            if (f.size() < 8 || ((f.size() - 2) % 2) != 0 || !mask_name(1)) {
                if (result.error.empty()) result.error = "Line " + std::to_string(line_no) + ": MASKPOLY syntax is MASKPOLY name x0 y0 x1 y1 x2 y2 [...].";
                return result;
            }
            std::vector<Point> pts; pts.reserve((f.size()-2)/2);
            for (std::size_t i=2;i<f.size();i+=2) { Point p; if(!integer(i,p.x)||!integer(i+1,p.y)) return result; pts.push_back(p); }
            std::string error; if (!raster.mask_polygon(f[1],pts,error)) { result.error = "Line " + std::to_string(line_no) + ": " + error; return result; }
        } else if (command == "MASKCLEAR") {
            if (!need_count(f, 2, result.error, line_no) || !mask_name(1)) return result;
            if (!raster.clear_mask(f[1])) { known_mask_error(); return result; }
        } else if (command == "FILLMASK") {
            if (!need_count(f, 3, result.error, line_no) || !mask_name(1)) return result;
            std::uint64_t c; if(!color(2,c)) return result;
            if (!raster.fill_mask(f[1],c)) { known_mask_error(); return result; }
        } else if (command == "CLUSTERS") {
            if (!need_count(f, 7, result.error, line_no) || !mask_name(1)) return result;
            std::vector<std::uint64_t> colors;
            for (const auto token : split_pipe(f[2])) {
                if (!valid_color_token(token)) { result.error = "Line " + std::to_string(line_no) + ": CLUSTERS colors must be palette indices or #AARRGGBB joined with '|'."; return result; }
                colors.push_back(raster.color(std::string(token)));
            }
            if (colors.empty()) { result.error = "Line " + std::to_string(line_no) + ": CLUSTERS needs at least one color."; return result; }
            double density; int size_min,size_max,seed;
            if(!number(3,density)||!integer(4,size_min)||!integer(5,size_max)||!integer(6,seed)) return result;
            if(!range01(density,"density")) return result;
            if(size_min <= 0 || size_max < size_min || size_max > 256) { result.error = "Line " + std::to_string(line_no) + ": cluster sizes must satisfy 1 <= size_min <= size_max <= 256."; return result; }
            if (!raster.clusters_mask(f[1],colors,density,size_min,size_max,static_cast<std::uint64_t>(static_cast<std::uint32_t>(seed)))) { known_mask_error(); return result; }
        } else if (command == "SHADE") {
            if (!need_count(f, 8, result.error, line_no) || !mask_name(1)) return result;
            std::uint64_t shadow,mid,highlight; double strength,irregularity;
            if(!color(2,shadow)||!color(3,mid)||!color(4,highlight)||!number(6,strength)||!number(7,irregularity)) return result;
            if(!range01(strength,"strength")||!range01(irregularity,"irregularity")) return result;
            const auto direction = upper(f[5]);
            if(direction!="N"&&direction!="NE"&&direction!="E"&&direction!="SE"&&direction!="S"&&direction!="SW"&&direction!="W"&&direction!="NW") {
                result.error = "Line " + std::to_string(line_no) + ": SHADE direction must be N, NE, E, SE, S, SW, W, or NW."; return result;
            }
            if (!raster.shade_mask(f[1],shadow,mid,highlight,direction,strength,irregularity)) { known_mask_error(); return result; }
        } else if (command == "DITHER") {
            if (!need_count(f, 7, result.error, line_no) || !mask_name(1)) return result;
            std::uint64_t a,b; double amount; int seed;
            if(!color(2,a)||!color(3,b)||!number(4,amount)||!integer(6,seed)) return result;
            if(!range01(amount,"amount")) return result;
            const auto pattern = upper(f[5]);
            if(pattern!="IRREGULAR"&&pattern!="BAYER"&&pattern!="CHECKER") {
                result.error = "Line " + std::to_string(line_no) + ": DITHER pattern must be IRREGULAR, BAYER, or CHECKER."; return result;
            }
            if (!raster.dither_mask(f[1],a,b,amount,pattern,static_cast<std::uint64_t>(static_cast<std::uint32_t>(seed)))) { known_mask_error(); return result; }
        } else if (command == "OUTLINE") {
            if (!need_count(f, 4, result.error, line_no) || !mask_name(1)) return result;
            std::uint64_t c; if(!color(2,c)) return result;
            const auto mode = upper(f[3]);
            if(mode!="OUTSIDE"&&mode!="INSIDE") { result.error = "Line " + std::to_string(line_no) + ": OUTLINE mode must be OUTSIDE or INSIDE."; return result; }
            if (!raster.outline_mask(f[1],c,mode)) { known_mask_error(); return result; }
        } else {
            result.error = "Line " + std::to_string(line_no) + ": unknown command '" + f[0] + "'.";
            return result;
        }
        ++result.commands;
    }
    if (result.commands == 0) { result.error = "Program contains no drawing commands."; return result; }
    result.patch = raster.patch(result.patch_operations);
    result.clipped_writes = raster.clipped();
    if (result.patch.empty()) {
        result.error = "Program expands beyond the 20,000-operation atomic patch limit; split it into two artistic passes.";
        return result;
    }
    result.ok = true;
    return result;
}

} // namespace pixelforge::win32
