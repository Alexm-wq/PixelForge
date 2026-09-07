#pragma once

#include <windows.h>
#include <string_view>

namespace pixelforge::win32 {

// Automatic-turn diagnostics. A compact summary is written to
// pixelforge-codex-session.log and the complete App Server TX/RX conversation is
// written beside it as .conversation.jsonl. Inline image data is materialized
// into the adjacent .assets directory and replaced by trace-asset markers.
void codex_trace_tx_bytes(const void* data, DWORD size);
void codex_trace_rx_bytes(const void* data, DWORD size);
void codex_trace_detail(std::string_view text);

} // namespace pixelforge::win32
