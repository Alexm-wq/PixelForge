#pragma once

#include <windows.h>
#include <string_view>

namespace pixelforge::win32 {

// Automatic-turn diagnostics: RPC metadata, tool outcomes, agent messages and
// token counts. Raw patch/image/recording payloads are not written.
void codex_trace_tx_bytes(const void* data, DWORD size);
void codex_trace_rx_bytes(const void* data, DWORD size);
void codex_trace_detail(std::string_view text);

} // namespace pixelforge::win32
