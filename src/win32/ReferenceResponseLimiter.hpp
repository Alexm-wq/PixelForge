#pragma once

#include <string>
#include <string_view>

namespace pixelforge::win32 {

// Caps only content/style reference images sent over MCP. The loaded GUI
// reference remains full resolution. Canvas renders are never modified here.
std::string limit_reference_response(std::string_view json_line);

} // namespace pixelforge::win32
