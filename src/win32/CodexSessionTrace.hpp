#pragma once

#include <windows.h>

#include <string_view>

namespace pixelforge::win32 {

// Compact, privacy-conscious trace for automatic Codex turns. The trace records
// RPC lifecycle/method metadata only; raw prompts, patches, image/base64 payloads
// and recording bytes are never written.
void codex_trace_reset();
void codex_trace_tx(std::string_view json_line);
void codex_trace_rx_bytes(HANDLE source, const void* data, DWORD size);

} // namespace pixelforge::win32
