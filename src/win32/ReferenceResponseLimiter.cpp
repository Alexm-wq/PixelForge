#include "ReferenceResponseLimiter.hpp"

#include <windows.h>
#include <objidl.h>
#include <ole2.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace pixelforge::win32 {
namespace {

constexpr std::uint64_t kMaxReferencePixels = 256ull * 256ull;

int base64_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool base64_decode(std::string_view text, std::vector<std::uint8_t>& out) {
    out.clear();
    out.reserve((text.size() / 4) * 3);
    std::uint32_t value = 0;
    int bits = -8;
    for (unsigned char c : text) {
        if (c == '=') break;
        const int v = base64_value(c);
        if (v < 0) return false;
        value = (value << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<std::uint8_t>((value >> bits) & 0xffu));
            bits -= 8;
        }
    }
    return true;
}

std::string base64_encode(const std::vector<std::uint8_t>& data) {
    static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < data.size(); i += 3) {
        const std::uint32_t a = data[i];
        const std::uint32_t b = i + 1 < data.size() ? data[i + 1] : 0;
        const std::uint32_t c = i + 2 < data.size() ? data[i + 2] : 0;
        const std::uint32_t n = (a << 16) | (b << 8) | c;
        out.push_back(table[(n >> 18) & 63]);
        out.push_back(table[(n >> 12) & 63]);
        out.push_back(i + 1 < data.size() ? table[(n >> 6) & 63] : '=');
        out.push_back(i + 2 < data.size() ? table[n & 63] : '=');
    }
    return out;
}

void fit_area(UINT source_width, UINT source_height, UINT& width, UINT& height) {
    width = source_width;
    height = source_height;
    const std::uint64_t area = static_cast<std::uint64_t>(source_width) * source_height;
    if (area <= kMaxReferencePixels || source_width == 0 || source_height == 0) return;

    const double scale = std::sqrt(static_cast<double>(kMaxReferencePixels) / static_cast<double>(area));
    width = std::max<UINT>(1, static_cast<UINT>(std::floor(source_width * scale)));
    height = std::max<UINT>(1, static_cast<UINT>(std::floor(source_height * scale)));

    while (static_cast<std::uint64_t>(width) * height > kMaxReferencePixels) {
        if (width >= height && width > 1) --width;
        else if (height > 1) --height;
        else break;
    }
}

bool resize_png(const std::vector<std::uint8_t>& input,
                std::vector<std::uint8_t>& output,
                UINT& delivered_width,
                UINT& delivered_height) {
    output.clear();
    delivered_width = 0;
    delivered_height = 0;
    if (input.empty() || input.size() > static_cast<std::size_t>(MAXDWORD)) return false;

    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // Destroy WIC interfaces before leaving the apartment, on every return path.
    struct ApartmentGuard {
        bool active;
        ~ApartmentGuard() { if (active) CoUninitialize(); }
    } apartment{SUCCEEDED(com)};
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) return false;

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        return false;
    }

    ComPtr<IWICStream> input_stream;
    hr = factory->CreateStream(&input_stream);
    if (SUCCEEDED(hr)) {
        hr = input_stream->InitializeFromMemory(const_cast<BYTE*>(input.data()), static_cast<DWORD>(input.size()));
    }

    ComPtr<IWICBitmapDecoder> decoder;
    if (SUCCEEDED(hr)) {
        hr = factory->CreateDecoderFromStream(input_stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);

    UINT source_width = 0;
    UINT source_height = 0;
    if (SUCCEEDED(hr)) hr = frame->GetSize(&source_width, &source_height);
    if (FAILED(hr) || source_width == 0 || source_height == 0) {
        return false;
    }

    fit_area(source_width, source_height, delivered_width, delivered_height);
    if (delivered_width == source_width && delivered_height == source_height) {
        return false;
    }

    ComPtr<IWICBitmapScaler> scaler;
    hr = factory->CreateBitmapScaler(&scaler);
    if (SUCCEEDED(hr)) {
        hr = scaler->Initialize(frame.Get(), delivered_width, delivered_height, WICBitmapInterpolationModeFant);
    }

    ComPtr<IWICFormatConverter> converter;
    if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    }

    ComPtr<IStream> output_stream;
    if (SUCCEEDED(hr)) hr = CreateStreamOnHGlobal(nullptr, TRUE, &output_stream);

    ComPtr<IWICBitmapEncoder> encoder;
    if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(output_stream.Get(), WICBitmapEncoderNoCache);

    ComPtr<IWICBitmapFrameEncode> out_frame;
    ComPtr<IPropertyBag2> props;
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&out_frame, &props);
    if (SUCCEEDED(hr)) hr = out_frame->Initialize(props.Get());
    if (SUCCEEDED(hr)) hr = out_frame->SetSize(delivered_width, delivered_height);
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr)) hr = out_frame->SetPixelFormat(&format);
    if (SUCCEEDED(hr)) hr = out_frame->WriteSource(converter.Get(), nullptr);
    if (SUCCEEDED(hr)) hr = out_frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();

    if (SUCCEEDED(hr)) {
        STATSTG stat{};
        hr = output_stream->Stat(&stat, STATFLAG_NONAME);
        if (SUCCEEDED(hr) && stat.cbSize.QuadPart > 0 &&
            stat.cbSize.QuadPart <= static_cast<ULONGLONG>(SIZE_MAX)) {
            HGLOBAL memory = nullptr;
            hr = GetHGlobalFromStream(output_stream.Get(), &memory);
            if (SUCCEEDED(hr) && memory) {
                const auto size = static_cast<std::size_t>(stat.cbSize.QuadPart);
                const void* locked = GlobalLock(memory);
                if (locked) {
                    const auto* bytes = static_cast<const std::uint8_t*>(locked);
                    output.assign(bytes, bytes + size);
                    GlobalUnlock(memory);
                } else {
                    hr = E_FAIL;
                }
            }
        }
    }

    return SUCCEEDED(hr) && !output.empty();
}

bool replace_escaped_number(std::string& line, std::string_view key, UINT value) {
    const std::string needle = "\\\"" + std::string(key) + "\\\":";
    const auto p = line.find(needle);
    if (p == std::string::npos) return false;
    std::size_t start = p + needle.size();
    std::size_t end = start;
    while (end < line.size() && line[end] >= '0' && line[end] <= '9') ++end;
    if (end == start) return false;
    line.replace(start, end - start, std::to_string(value));
    return true;
}

} // namespace

std::string limit_reference_response(std::string_view json_line) {
    // Reference tool results carry content:/style: observation IDs. Canvas
    // renders use canvas: and must remain untouched for pixel-level inspection.
    if (json_line.find("content:") == std::string_view::npos &&
        json_line.find("style:") == std::string_view::npos) {
        return std::string(json_line);
    }

    const std::string data_marker = "\"data\":\"";
    const auto marker = json_line.find(data_marker);
    if (marker == std::string_view::npos) return std::string(json_line);
    const std::size_t data_start = marker + data_marker.size();
    const std::size_t data_end = json_line.find('"', data_start);
    if (data_end == std::string_view::npos || data_end <= data_start) return std::string(json_line);

    std::vector<std::uint8_t> source;
    if (!base64_decode(json_line.substr(data_start, data_end - data_start), source)) {
        return std::string(json_line);
    }

    std::vector<std::uint8_t> resized;
    UINT width = 0;
    UINT height = 0;
    if (!resize_png(source, resized, width, height)) return std::string(json_line);

    std::string result(json_line);
    const std::string encoded = base64_encode(resized);
    result.replace(data_start, data_end - data_start, encoded);
    replace_escaped_number(result, "width", width);
    replace_escaped_number(result, "height", height);
    return result;
}

} // namespace pixelforge::win32
