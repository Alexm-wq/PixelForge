# Shared PixelProgram generation. Original Windows/core sources stay untouched.
include_guard(GLOBAL)

# Exact replacement guard: source drift must fail configure, not silently retain
# a lower capacity or skip a preallocation check. Semicolons remain quoted text.
function(_pixelforge_program_replace variable needle replacement expected)
    set(_rest "${${variable}}")
    set(_count 0)
    string(LENGTH "${needle}" _length)
    while(TRUE)
        string(FIND "${_rest}" "${needle}" _position)
        if(_position EQUAL -1)
            break()
        endif()
        math(EXPR _count "${_count} + 1")
        math(EXPR _after "${_position} + ${_length}")
        string(SUBSTRING "${_rest}" ${_after} -1 _rest)
    endwhile()
    if(NOT _count EQUAL expected)
        message(FATAL_ERROR "PixelProgram source drift: expected ${expected} guarded site(s), found ${_count}")
    endif()
    string(REPLACE "${needle}" "${replacement}" _result "${${variable}}")
    set(${variable} "${_result}" PARENT_SCOPE)
endfunction()

# out_var: generated C++ path returned into caller scope.
# mode: WINDOWS preserves the existing Windows generated transformation exactly;
# QT_HOST adds explicit portable host budgets and preallocation checks.
function(pixelforge_generate_pixel_program out_var mode input output)
    if(NOT mode STREQUAL "WINDOWS" AND NOT mode STREQUAL "QT_HOST")
        message(FATAL_ERROR "Unknown PixelProgram generation mode: ${mode}")
    endif()
    if(NOT IS_ABSOLUTE "${input}" OR NOT IS_ABSOLUTE "${output}" OR input STREQUAL output)
        message(FATAL_ERROR "PixelProgram generation requires separate absolute source/output paths")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${input}")
    file(READ "${input}" PIXELFORGE_PIXEL_PROGRAM_TEXT)
    string(REPLACE
        "constexpr std::size_t kMaxPatchOperations = 20000;\nconstexpr std::size_t kMaxNamedMasks = 64;\nconstexpr std::size_t kMaxMaskBytes = 64u * 1024u * 1024u;\n"
        ""
        PIXELFORGE_PIXEL_PROGRAM_TEXT "${PIXELFORGE_PIXEL_PROGRAM_TEXT}")
    string(REPLACE "return parse_int(token, index) && index >= 0 && index <= 255;"
                   "return parse_int(token, index) && index >= 0;"
        PIXELFORGE_PIXEL_PROGRAM_TEXT "${PIXELFORGE_PIXEL_PROGRAM_TEXT}")
    string(REPLACE "if (name.empty() || name.size() > 64) return false;" "if (name.empty()) return false;"
        PIXELFORGE_PIXEL_PROGRAM_TEXT "${PIXELFORGE_PIXEL_PROGRAM_TEXT}")
    string(REPLACE
[=[                ++operations;
                if (operations > kMaxPatchOperations) return {};]=]
[=[                ++operations;]=]
        PIXELFORGE_PIXEL_PROGRAM_TEXT "${PIXELFORGE_PIXEL_PROGRAM_TEXT}")
    string(REPLACE
[=[    bool store_mask(std::string_view name, Mask mask, std::string& error) {
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
    }]=]
[=[    bool store_mask(std::string_view name, Mask mask, std::string& error) {
        (void)error;
        masks_[mask_key(name)] = std::move(mask);
        return true;
    }]=]
        PIXELFORGE_PIXEL_PROGRAM_TEXT "${PIXELFORGE_PIXEL_PROGRAM_TEXT}")
    string(REPLACE "color must be palette index 0..255 or #AARRGGBB."
                   "color must be a non-negative palette index or #AARRGGBB."
        PIXELFORGE_PIXEL_PROGRAM_TEXT "${PIXELFORGE_PIXEL_PROGRAM_TEXT}")
    string(REPLACE "mask name must be 1..64 letters, digits, '_' or '-'."
                   "mask name must contain letters, digits, '_' or '-'."
        PIXELFORGE_PIXEL_PROGRAM_TEXT "${PIXELFORGE_PIXEL_PROGRAM_TEXT}")
    string(REPLACE
        "if(size_min <= 0 || size_max < size_min || size_max > 256) { result.error = \"Line \" + std::to_string(line_no) + \": cluster sizes must satisfy 1 <= size_min <= size_max <= 256.\"; return result; }"
        "if(size_min <= 0 || size_max < size_min) { result.error = \"Line \" + std::to_string(line_no) + \": cluster sizes must satisfy 1 <= size_min <= size_max.\"; return result; }"
        PIXELFORGE_PIXEL_PROGRAM_TEXT "${PIXELFORGE_PIXEL_PROGRAM_TEXT}")
    string(REPLACE
[=[    if (result.patch.empty()) {
        result.error = "Program expands beyond the 20,000-operation atomic patch limit; split it into two artistic passes.";
        return result;
    }]=]
[=[]=]
        PIXELFORGE_PIXEL_PROGRAM_TEXT "${PIXELFORGE_PIXEL_PROGRAM_TEXT}")

    # Detect a changed raw source rather than quietly generating a mixed policy.
    foreach(_forbidden
        "kMaxPatchOperations" "kMaxNamedMasks" "kMaxMaskBytes"
        "index <= 255" "name.size() > 64" "size_max > 256"
        "20,000-operation atomic patch limit")
        string(FIND "${PIXELFORGE_PIXEL_PROGRAM_TEXT}" "${_forbidden}" _remaining)
        if(NOT _remaining EQUAL -1)
            message(FATAL_ERROR "Incomplete Windows PixelProgram transform: ${_forbidden}")
        endif()
    endforeach()

    if(mode STREQUAL "QT_HOST")
        _pixelforge_program_replace(PIXELFORGE_PIXEL_PROGRAM_TEXT
[=[constexpr std::uint64_t kTokenFlag = 1ull << 63;]=]
[=[// Explicit portable host budgets; not an artistic pass-count restriction.
constexpr std::size_t kHostMaxPatchOperations = 262144;
constexpr std::size_t kHostMaxPatchBytes = 16u * 1024u * 1024u;
constexpr std::size_t kHostMaxNamedMasks = 16384;
constexpr std::size_t kHostMaxLiveMaskBytes = 256u * 1024u * 1024u;
constexpr std::size_t kHostMaxCanvasPixels = 4u * 1024u * 1024u;
constexpr std::uint64_t kTokenFlag = 1ull << 63;]=] 1)
        _pixelforge_program_replace(PIXELFORGE_PIXEL_PROGRAM_TEXT
[=[if (name.empty()) return false;]=]
[=[if (name.empty() || name.size() > 1024) return false;]=] 1)
        _pixelforge_program_replace(PIXELFORGE_PIXEL_PROGRAM_TEXT
[=[mask name must contain letters, digits, '_' or '-'.]=]
[=[mask name must be 1..1024 letters, digits, '_' or '-' (host name budget).]=] 1)
        _pixelforge_program_replace(PIXELFORGE_PIXEL_PROGRAM_TEXT
[=[                if (!out.empty()) out.push_back(';');
                if (x2 - x == 1) out += "P," + std::to_string(x) + "," + std::to_string(y) + "," + token(value);
                else out += "H," + std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(x2 - x) + "," + token(value);
                ++operations;]=]
[=[                if (operations >= kHostMaxPatchOperations) { operations = kHostMaxPatchOperations + 1; return {}; }
                const std::string operation = x2 - x == 1
                    ? "P," + std::to_string(x) + "," + std::to_string(y) + "," + token(value)
                    : "H," + std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(x2 - x) + "," + token(value);
                const std::size_t extra = operation.size() + (out.empty() ? 0u : 1u);
                if (extra > kHostMaxPatchBytes - out.size()) { operations = kHostMaxPatchOperations + 1; return {}; }
                if (!out.empty()) out.push_back(';');
                out += operation;
                ++operations;]=] 1)
        _pixelforge_program_replace(PIXELFORGE_PIXEL_PROGRAM_TEXT
[=[    bool store_mask(std::string_view name, Mask mask, std::string& error) {
        (void)error;
        masks_[mask_key(name)] = std::move(mask);
        return true;
    }]=]
[=[    bool can_allocate_mask(std::string_view name, std::string& error) const {
        const bool new_name = masks_.find(mask_key(name)) == masks_.end();
        if (new_name && masks_.size() >= kHostMaxNamedMasks) {
            error = "Host named-mask count budget exceeded (16384).";
            return false;
        }
        // Every mask has cells_.size() bytes. Include the staged replacement
        // while the prior mask is still alive; do not allocate then refuse.
        if (cells_.size() > kHostMaxLiveMaskBytes / (masks_.size() + 1u)) {
            error = "Host live plus staged named-mask storage budget exceeded (256 MiB).";
            return false;
        }
        return true;
    }
    bool store_mask(std::string_view name, Mask mask, std::string& error) {
        if (!can_allocate_mask(name, error)) return false;
        masks_[mask_key(name)] = std::move(mask);
        return true;
    }]=] 1)
        _pixelforge_program_replace(PIXELFORGE_PIXEL_PROGRAM_TEXT
[=[        Mask mask(cells_.size(), 0);]=]
[=[        if (!can_allocate_mask(name, error)) return false;
        Mask mask(cells_.size(), 0);]=] 3)
        _pixelforge_program_replace(PIXELFORGE_PIXEL_PROGRAM_TEXT
[=[    PixelProgramResult result;
    if (width <= 0 || height <= 0 || current_pixels.size() != static_cast<std::size_t>(width) * height) {]=]
[=[    PixelProgramResult result;
    if (width <= 0 || height <= 0 || static_cast<std::uint64_t>(width) > kHostMaxCanvasPixels / static_cast<std::uint64_t>(height)) {
        result.error = "Host compiler canvas budget exceeded (positive dimensions, at most 4 Mi pixels).";
        return result;
    }
    if (current_pixels.size() != static_cast<std::size_t>(width) * height) {]=] 1)
        _pixelforge_program_replace(PIXELFORGE_PIXEL_PROGRAM_TEXT
[=[    ProgramRaster raster(width, height, current_pixels);]=]
[=[    if (current_pixels.size() > kHostMaxCanvasPixels) {
        result.error = "Host compiler canvas budget exceeded (4 Mi pixels).";
        return result;
    }
    ProgramRaster raster(width, height, current_pixels);]=] 1)
        _pixelforge_program_replace(PIXELFORGE_PIXEL_PROGRAM_TEXT
[=[            if (w <= 0 || h <= 0) { result.error = "Line " + std::to_string(line_no) + ": copy width/height must be positive."; return result; }]=]
[=[            if (w <= 0 || h <= 0) { result.error = "Line " + std::to_string(line_no) + ": copy width/height must be positive."; return result; }
            if (static_cast<std::size_t>(w) > kHostMaxCanvasPixels / static_cast<std::size_t>(h)) {
                result.error = "Line " + std::to_string(line_no) + ": COPY temporary area exceeds host budget (4 Mi pixels).";
                return result;
            }]=] 1)
        _pixelforge_program_replace(PIXELFORGE_PIXEL_PROGRAM_TEXT
[=[    result.clipped_writes = raster.clipped();]=]
[=[    result.clipped_writes = raster.clipped();
    if (result.patch_operations > kHostMaxPatchOperations) {
        result.error = "Host expanded patch budget exceeded (262144 operations or 16 MiB).";
        return result;
    }]=] 1)
    endif()
    get_filename_component(_directory "${output}" DIRECTORY)
    file(MAKE_DIRECTORY "${_directory}")
    # Avoid timestamp churn on unchanged configure/generation.
    set(_write TRUE)
    if(EXISTS "${output}")
        file(READ "${output}" _existing)
        if(_existing STREQUAL PIXELFORGE_PIXEL_PROGRAM_TEXT)
            set(_write FALSE)
        endif()
    endif()
    if(_write)
        file(WRITE "${output}" "${PIXELFORGE_PIXEL_PROGRAM_TEXT}")
    endif()
    set(${out_var} "${output}" PARENT_SCOPE)
endfunction()
