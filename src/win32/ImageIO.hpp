#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pixelforge::win32 {

struct ImageData {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> bgra;

    [[nodiscard]] bool valid() const noexcept {
        return width > 0 && height > 0 && bgra.size() == static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    }
};

bool load_image_wic(const std::wstring& path, ImageData& out, std::wstring& error);
bool save_png_wic(const std::wstring& path, int width, int height,
                  const std::vector<std::uint32_t>& argb, std::wstring& error);

} // namespace pixelforge::win32
