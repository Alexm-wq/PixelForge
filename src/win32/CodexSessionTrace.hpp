#pragma once

#include <windows.h>

namespace pixelforge::win32 {

// Compact, privacy-conscious trace for automatic Codex turns. The trace records
// RPC lifecycle/method metadata only; raw prompts, patches, image/base64 payloads
// and recording bytes are never written.
void codex_trace_tx_bytes(const void* data, DWORD size);
void codex_trace_rx_bytes(const void* data, DWORD size);

} // namespace pixelforge::win32
