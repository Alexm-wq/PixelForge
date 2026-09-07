#include <windows.h>
#include <sddl.h>

namespace {

HANDLE WINAPI pixelforge_create_local_record_pipe(
    LPCWSTR name,
    DWORD open_mode,
    DWORD pipe_mode,
    DWORD max_instances,
    DWORD out_buffer_size,
    DWORD in_buffer_size,
    DWORD default_timeout,
    LPSECURITY_ATTRIBUTES) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    SECURITY_ATTRIBUTES attributes{};
    SECURITY_ATTRIBUTES* security = nullptr;

    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;WD)S:(ML;;NW;;;LW)",
            SDDL_REVISION_1,
            &descriptor,
            nullptr)) {
        attributes.nLength = sizeof(attributes);
        attributes.lpSecurityDescriptor = descriptor;
        attributes.bInheritHandle = FALSE;
        security = &attributes;
    }

    const HANDLE pipe = ::CreateNamedPipeW(
        name,
        open_mode,
        pipe_mode | PIPE_REJECT_REMOTE_CLIENTS,
        max_instances,
        out_buffer_size,
        in_buffer_size,
        default_timeout,
        security);

    if (descriptor) LocalFree(descriptor);
    return pipe;
}

} // namespace

#define CreateNamedPipeW pixelforge_create_local_record_pipe
#include "RecordMcpServer.cpp"
#undef CreateNamedPipeW
