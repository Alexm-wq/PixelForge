#include "PixelProgram.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pixelforge::win32 {
namespace {

constexpr std::size_t kMaxPatchOperations = 20000;
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

bool parse_int(std::string_view token, int& value) {
    if (token.empty()) return false;
    const auto* b = token.data();
    const auto* e = b + token.size();
    const auto r = std::from_chars(b, e, value);
    return r.ec == std::errc{} && r.ptr == e;
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

std::string hex32(std::uint32_t value) {
    static constexpr char h[] = "0123456789ABCDEF";
    std::string out = "#00000000";
    for (int i = 0; i < 8; ++i) out[8 - i] = h[(value >> (i * 4)) & 0xfu];
    return out;
}

class ProgramRaster {
public:
    ProgramRaster(int width, int height, const std::vector<std::uint32_t>& pixels)
        : w_(width), h_(height), original_(pixels), cells_(pixels.size()) {
        for (std::size_t i = 0; i < pixels.size(); ++i) cells_[i] = pixels[i];
    }

    std::uint64_t color(std::string token) {
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
        int a = std::max(0, x);
        int b = std::min(w_ - 1, x1ll > std::numeric_limits<int>::max() ? w_ - 1 : static_cast<int>(x1ll));
        if (a > b) { clipped_ += static_cast<std::size_t>(len); return; }
        clipped_ += static_cast<std::size_t>(a - x) + static_cast<std::size_t>(x1ll - b);
        for (int px = a; px <= b; ++px) cells_[static_cast<std::size_t>(y) * w_ + px] = c;
    }

    void vline(int x, int y, int len, std::uint64_t c) {
        if (len <= 0) return;
        if (x < 0 || x >= w_) { clipped_ += static_cast<std::size_t>(len); return; }
        const long long y1ll = static_cast<long long>(y) + len - 1;
        int a = std::max(0, y);
        int b = std::min(h_ - 1, y1ll > std::numeric_limits<int>::max() ? h_ - 1 : static_cast<int>(y1ll));
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
        int sx = x0 < x1 ? 1 : -1;
        long long dy = -std::llabs(static_cast<long long>(y1) - y0);
        int sy = y0 < y1 ? 1 : -1;
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
        auto color = [&](std::size_t i, std::uint64_t& out) {
            if (i >= f.size() || !valid_color_token(f[i])) {
                result.error = "Line " + std::to_string(line_no) + ": color must be palette index 0..255 or #AARRGGBB.";
                return false;
            }
            out = raster.color(f[i]); return true;
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
