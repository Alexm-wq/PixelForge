#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pixelforge::win32 {

struct PixelProgramResult {
    bool ok = false;
    std::string patch;
    std::string error;
    std::size_t commands = 0;
    std::size_t patch_operations = 0;
    std::size_t clipped_writes = 0;
};

// Compiles a compact, stateful raster program against a snapshot of the current
// canvas into one atomic exact-pixel patch understood by pixelforge_edit.
// Geometry is clipped to the canvas; malformed syntax is rejected before edit.
PixelProgramResult compile_pixel_program(std::string_view program,
                                         int width,
                                         int height,
                                         const std::vector<std::uint32_t>& current_pixels);

} // namespace pixelforge::win32
