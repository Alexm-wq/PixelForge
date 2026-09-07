#pragma once

#include <windows.h>
#include <string_view>

namespace pixelforge::win32 {

// Automatic-turn diagnostics. A compact summary is written to
// pixelforge-codex-session.log, the complete App Server TX/RX exchange to
// .conversation.jsonl, and a human-readable user/agent/tool transcript to
// .transcript.md. Inline image data is materialized into the adjacent .assets
// directory and replaced by trace-asset markers in the raw JSONL.
void codex_trace_tx_bytes(const void* data, DWORD size);
void codex_trace_rx_bytes(const void* data, DWORD size);
void codex_trace_detail(std::string_view text);

} // namespace pixelforge::win32
