#include "ImageIO.hpp"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace pixelforge::win32 {

namespace {

struct ScopedComApartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ~ScopedComApartment() {
        if (SUCCEEDED(result)) CoUninitialize();
    }

    [[nodiscard]] bool usable() const noexcept {
        // RPC_E_CHANGED_MODE means this thread already has a different COM
        // apartment (the GUI uses STA). COM is still initialized and WIC is usable.
        return SUCCEEDED(result) || result == RPC_E_CHANGED_MODE;
    }
};

} // namespace

static std::wstring hresult_message(HRESULT hr) {
    wchar_t* buffer = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, hr, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text = buffer ? buffer : L"Unknown Windows imaging error.";
    if (buffer) LocalFree(buffer);
    return text;
}

static bool create_factory(ComPtr<IWICImagingFactory>& factory, std::wstring& error) {
    const HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        error = hresult_message(hr);
        return false;
    }
    return true;
}

bool load_image_wic(const std::wstring& path, ImageData& out, std::wstring& error) {
    ScopedComApartment com;
    if (!com.usable()) {
        error = hresult_message(com.result);
        return false;
    }

    ComPtr<IWICImagingFactory> factory;
    if (!create_factory(factory, error)) return false;

    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                     WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr)) { error = hresult_message(hr); return false; }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) { error = hresult_message(hr); return false; }

    UINT width = 0, height = 0;
    hr = frame->GetSize(&width, &height);
    if (FAILED(hr) || width == 0 || height == 0) {
        error = L"Image has invalid dimensions.";
        return false;
    }

    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr)) { error = hresult_message(hr); return false; }

    hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                               WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) { error = hresult_message(hr); return false; }

    const UINT stride = width * 4u;
    const UINT bytes = stride * height;
    out.width = static_cast<int>(width);
    out.height = static_cast<int>(height);
    out.bgra.resize(bytes);
    hr = converter->CopyPixels(nullptr, stride, bytes, out.bgra.data());
    if (FAILED(hr)) {
        error = hresult_message(hr);
        out = {};
        return false;
    }
    return true;
}

bool save_png_wic(const std::wstring& path, int width, int height,
                  const std::vector<std::uint32_t>& argb, std::wstring& error) {
    if (width <= 0 || height <= 0 || argb.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) {
        error = L"Canvas is empty or malformed.";
        return false;
    }

    ScopedComApartment com;
    if (!com.usable()) {
        error = hresult_message(com.result);
        return false;
    }

    ComPtr<IWICImagingFactory> factory;
    if (!create_factory(factory, error)) return false;

    ComPtr<IWICStream> stream;
    HRESULT hr = factory->CreateStream(&stream);
    if (FAILED(hr)) { error = hresult_message(hr); return false; }
    hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) { error = hresult_message(hr); return false; }

    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(hr)) { error = hresult_message(hr); return false; }
    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) { error = hresult_message(hr); return false; }

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    hr = encoder->CreateNewFrame(&frame, &properties);
    if (FAILED(hr)) { error = hresult_message(hr); return false; }
    hr = frame->Initialize(properties.Get());
    if (FAILED(hr)) { error = hresult_message(hr); return false; }
    hr = frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height));
    if (FAILED(hr)) { error = hresult_message(hr); return false; }

    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    hr = frame->SetPixelFormat(&format);
    if (FAILED(hr)) { error = hresult_message(hr); return false; }

    std::vector<std::uint8_t> bgra(argb.size() * 4u);
    for (std::size_t i = 0; i < argb.size(); ++i) {
        const std::uint32_t value = argb[i];
        bgra[i * 4 + 0] = static_cast<std::uint8_t>(value & 0xffu);
        bgra[i * 4 + 1] = static_cast<std::uint8_t>((value >> 8) & 0xffu);
        bgra[i * 4 + 2] = static_cast<std::uint8_t>((value >> 16) & 0xffu);
        bgra[i * 4 + 3] = static_cast<std::uint8_t>((value >> 24) & 0xffu);
    }

    const UINT stride = static_cast<UINT>(width * 4);
    hr = frame->WritePixels(static_cast<UINT>(height), stride,
                            static_cast<UINT>(bgra.size()), bgra.data());
    if (FAILED(hr)) { error = hresult_message(hr); return false; }
    hr = frame->Commit();
    if (FAILED(hr)) { error = hresult_message(hr); return false; }
    hr = encoder->Commit();
    if (FAILED(hr)) { error = hresult_message(hr); return false; }
    return true;
}

} // namespace pixelforge::win32
