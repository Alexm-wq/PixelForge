#include "PixelProgram.hpp"

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using namespace pixelforge::win32;

#define CHECK(x) do { if (!(x)) { std::cerr << "Failed at line " << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)

namespace {

std::vector<std::string_view> split(std::string_view text, char delimiter) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    for (;;) {
        const auto pos = text.find(delimiter, start);
        if (pos == std::string_view::npos) {
            out.push_back(text.substr(start));
            return out;
        }
        out.push_back(text.substr(start, pos - start));
        start = pos + 1;
    }
}

int parse_int(std::string_view text) {
    int value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    CHECK(result.ec == std::errc{} && result.ptr == text.data() + text.size());
    return value;
}

std::uint32_t parse_hex(std::string_view text) {
    CHECK(text.size() == 9 && text.front() == '#');
    std::uint32_t value = 0;
    const auto result = std::from_chars(text.data() + 1, text.data() + text.size(), value, 16);
    CHECK(result.ec == std::errc{} && result.ptr == text.data() + text.size());
    return value;
}

std::vector<std::uint32_t> apply_patch(std::vector<std::uint32_t> pixels, int width, const std::string& patch) {
    for (const auto op : split(patch, ';')) {
        const auto fields = split(op, ',');
        CHECK(!fields.empty());
        if (fields[0] == "P") {
            CHECK(fields.size() == 4);
            const int x = parse_int(fields[1]), y = parse_int(fields[2]);
            pixels[static_cast<std::size_t>(y) * width + x] = parse_hex(fields[3]);
        } else if (fields[0] == "H") {
            CHECK(fields.size() == 5);
            const int x = parse_int(fields[1]), y = parse_int(fields[2]), count = parse_int(fields[3]);
            const auto color = parse_hex(fields[4]);
            for (int i = 0; i < count; ++i)
                pixels[static_cast<std::size_t>(y) * width + x + i] = color;
        } else {
            CHECK(false);
        }
    }
    return pixels;
}

std::size_t count_color(const std::vector<std::uint32_t>& pixels, std::uint32_t color) {
    std::size_t count = 0;
    for (const auto pixel : pixels) if (pixel == color) ++count;
    return count;
}

} // namespace

int main() {
    constexpr int width = 8, height = 8;
    const std::vector<std::uint32_t> blank(width * height, 0);

    // Masks are reusable and case-insensitive. SHADE uses hard directional bands,
    // and OUTLINE derives the edge mechanically from the same region.
    auto result = compile_pixel_program(
        "MASKRECT Body 1 1 6 6\n"
        "FILLMASK body #FF102030\n"
        "SHADE BODY #FF203040 #FF405060 #FF8090A0 NW 0.6 0\n"
        "OUTLINE body #FFFFFFFF OUTSIDE\n",
        width, height, blank);
    CHECK(result.ok && result.commands == 4 && result.patch_operations > 0);
    auto pixels = apply_patch(blank, width, result.patch);
    CHECK(pixels[0] == 0xFFFFFFFFu);                         // diagonal outside outline
    CHECK(pixels[1 * width + 1] == 0xFF8090A0u);            // lit NW edge
    CHECK(pixels[6 * width + 6] == 0xFF203040u);            // SE shadow
    CHECK(pixels[3 * width + 3] == 0xFF405060u);            // middle band

    // Texture passes are deterministic. DITHER only replaces its source color,
    // so cluster colors survive when dithering follows CLUSTERS.
    const std::string textured =
        "MASKPOLY tissue 1 1 6 1 6 6 1 6\n"
        "FILLMASK tissue #FF223344\n"
        "CLUSTERS tissue #FF556677|#FF778899 0.35 1 3 492\n"
        "DITHER tissue #FF223344 #FF334455 0.2 IRREGULAR 91\n";
    const auto texture_a = compile_pixel_program(textured, width, height, blank);
    const auto texture_b = compile_pixel_program(textured, width, height, blank);
    CHECK(texture_a.ok && texture_b.ok && texture_a.patch == texture_b.patch);
    pixels = apply_patch(blank, width, texture_a.patch);
    CHECK(count_color(pixels, 0xFF556677u) + count_color(pixels, 0xFF778899u) > 0);
    CHECK(count_color(pixels, 0xFF334455u) > 0);

    // Exact source colors can be matched against pixels that existed before the
    // program call, not only colors written earlier in the same program.
    const std::vector<std::uint32_t> black(width * height, 0xFF000000u);
    result = compile_pixel_program(
        "MASKRECT all 0 0 8 8;DITHER all #FF000000 #FFFFFFFF 0.5 BAYER 0",
        width, height, black);
    CHECK(result.ok);
    pixels = apply_patch(black, width, result.patch);
    CHECK(count_color(pixels, 0xFFFFFFFFu) == 32);
    CHECK(count_color(pixels, 0xFF000000u) == 32);

    result = compile_pixel_program(
        "MASKELLIPSE eye 4 4 2 1;FILLMASK eye #FFFFFFFF;MASKCLEAR eye",
        width, height, blank);
    CHECK(result.ok);

    result = compile_pixel_program(
        "MASKRECT all 0 0 8 8;OUTLINE all #FFFFFFFF INSIDE",
        width, height, blank);
    CHECK(result.ok);
    pixels = apply_patch(blank, width, result.patch);
    CHECK(pixels[0] == 0xFFFFFFFFu);
    CHECK(pixels[3 * width + 3] == 0u);
    CHECK(pixels[7 * width + 7] == 0xFFFFFFFFu);

    result = compile_pixel_program("FILLMASK missing #FFFFFFFF", width, height, blank);
    CHECK(!result.ok && result.error.find("unknown mask") != std::string::npos);

    result = compile_pixel_program("MASKRECT m 0 0 8 8;CLUSTERS m #FFFFFFFF 1.5 1 3 1", width, height, blank);
    CHECK(!result.ok && result.error.find("density") != std::string::npos);

    std::cout << "PixelProgram high-level raster operations passed.\n";
    return 0;
}
