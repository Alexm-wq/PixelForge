#pragma once
#include "ProjectFileIO.hpp"
#include <fstream>
namespace pixelforge::win32 {
inline std::string compact_project_brief(std::string text) {
    if (text.size() > 4096) {
        std::size_t end = 4096;
        while (end && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
        text.resize(end);
    }
    return text;
}
inline bool save_project_brief(const std::filesystem::path& project, std::string_view summary, std::wstring& error) {
    return write_project_text_atomic(project / L"project_brief.md", compact_project_brief(std::string(summary)), error);
}
inline std::string read_project_brief(const std::filesystem::path& project) {
    std::ifstream file(project / L"project_brief.md", std::ios::binary);
    std::string text(4097, '\0');
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(file.gcount()));
    return compact_project_brief(std::move(text));
}
}
