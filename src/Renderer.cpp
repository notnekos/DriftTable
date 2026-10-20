#include "Renderer.h"

namespace dt {

Renderer::~Renderer() {
    for (auto& sf : surfaces_) sf.release();
}

bool Renderer::init() {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf()))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()))))
        return false;

    D2D1_STROKE_STYLE_PROPERTIES round = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    factory_->CreateStrokeStyle(round, nullptr, 0, &roundStroke_);
    D2D1_STROKE_STYLE_PROPERTIES dash = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_MITER, 10.f,
        D2D1_DASH_STYLE_DASH);
    factory_->CreateStrokeStyle(dash, nullptr, 0, &dashStroke_);

    // Segoe UI everywhere on Windows; the fallbacks only matter on unusual systems (and Wine).
    const wchar_t* family = L"Segoe UI";
    ComPtr<IDWriteFontCollection> fonts;
    if (SUCCEEDED(dwrite_->GetSystemFontCollection(&fonts))) {
        for (const wchar_t* f : {L"Segoe UI", L"Tahoma", L"Arial", L"DejaVu Sans"}) {
            UINT32 index;
            BOOL exists = FALSE;
            if (SUCCEEDED(fonts->FindFamilyName(f, &index, &exists)) && exists) {
                family = f;
                break;
            }
        }
    }
    auto makeFormat = [&](float size, DWRITE_FONT_WEIGHT weight, ComPtr<IDWriteTextFormat>& out) {
        dwrite_->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                  size, L"", &out);
    };
    makeFormat(12.f, DWRITE_FONT_WEIGHT_NORMAL, labelFormat_);
    makeFormat(16.f, DWRITE_FONT_WEIGHT_NORMAL, noteFormat_);
    makeFormat(13.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, hudFormat_);
    if (!labelFormat_ || !noteFormat_ || !hudFormat_) return false;

    labelFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    // Emergency break so long file names without spaces still wrap.
    if (FAILED(labelFormat_->SetWordWrapping((DWRITE_WORD_WRAPPING)2)))
        labelFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    ComPtr<IDWriteInlineObject> ellipsis;
    dwrite_->CreateEllipsisTrimmingSign(labelFormat_.Get(), &ellipsis);
    DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    labelFormat_->SetTrimming(&trim, ellipsis.Get());

    noteFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    hudFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    hudFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    hudFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return true;
}

void Renderer::Surface::release() {
    if (dc) {
        if (old) SelectObject(dc, old);
        DeleteDC(dc);
    }
    if (dib) DeleteObject(dib);
    dc = nullptr;
    dib = nullptr;
    old = nullptr;
    width = height = 0;
}

void Renderer::setSurfaceCount(size_t count) {
    for (size_t i = count; i < surfaces_.size(); ++i) surfaces_[i].release();
    surfaces_.resize(count);
}

void Renderer::resize(size_t surface, int width, int height) {
    if (surface >= surfaces_.size()) return;
    Surface& sf = surfaces_[surface];
    width = std::max(1, width);
    height = std::max(1, height);
    if (width == sf.width && height == sf.height && sf.dib) return;
    sf.release();
    sf.width = width;
    sf.height = height;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    sf.dc = CreateCompatibleDC(screen);
    sf.dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (sf.dib) sf.old = SelectObject(sf.dc, sf.dib);
}

bool Renderer::createTarget() {
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 0, 0,
        D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
    HRESULT hr = factory_->CreateDCRenderTarget(&props, &target_);
    if (FAILED(hr)) {
        logf(L"CreateDCRenderTarget failed: 0x%08lX", (unsigned long)hr);
        return false;
    }
    target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    target_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush_);
    ++generation_; // invalidates every cached device bitmap
    return true;
}

ID2D1RenderTarget* Renderer::beginDraw(size_t surface) {
    if (surface >= surfaces_.size() || !surfaces_[surface].dib) return nullptr;
    if (!target_ && !createTarget()) return nullptr;
    Surface& sf = surfaces_[surface];
    RECT rc{0, 0, sf.width, sf.height};
    HRESULT hr = target_->BindDC(sf.dc, &rc);
    if (FAILED(hr)) {
        static bool logged = false;
        if (!logged) logf(L"BindDC failed: 0x%08lX", (unsigned long)hr);
        logged = true;
        return nullptr;
    }
    target_->BeginDraw();
    target_->SetTransform(D2D1::Matrix3x2F::Identity());
    return target_.Get();
}

bool Renderer::endDraw(size_t surface, HWND hwnd, bool layered) {
    Surface& sf = surfaces_[surface];
    HRESULT hr = target_->EndDraw();
    if (FAILED(hr)) logf(L"EndDraw: 0x%08lX", (unsigned long)hr);
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) {
        target_.Reset();
        brush_.Reset();
        return false;
    }
    if (layered) {
        SIZE size{sf.width, sf.height};
        POINT src{0, 0};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        static int failures = 0;
        if (!UpdateLayeredWindow(hwnd, nullptr, nullptr, &size, sf.dc, &src, 0, &blend, ULW_ALPHA)) {
            if (failures++ < 3) logf(L"UpdateLayeredWindow failed: error %lu", GetLastError());
        } else if (!sf.shown) {
            sf.shown = true;
            logf(L"surface %u: first frame shown (%dx%d)", (unsigned)surface, sf.width, sf.height);
        }
    } else {
        HDC dc = GetDC(hwnd);
        BitBlt(dc, 0, 0, sf.width, sf.height, sf.dc, 0, 0, SRCCOPY);
        ReleaseDC(hwnd, dc);
    }
    return true;
}

ID2D1SolidColorBrush* Renderer::brush(D2D1_COLOR_F color) {
    brush_->SetColor(color);
    brush_->SetOpacity(1.f);
    return brush_.Get();
}

ID2D1Bitmap* Renderer::bitmapFor(Image& image) {
    if (image.bitmap && image.generation == generation_) return image.bitmap.Get();
    image.bitmap.Reset();
    if (image.pixels.empty()) return nullptr;
    D2D1_BITMAP_PROPERTIES props =
        D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(target_->CreateBitmap(D2D1::SizeU(image.width, image.height), image.pixels.data(), image.width * 4,
                                     props, &image.bitmap)))
        return nullptr;
    image.generation = generation_;
    return image.bitmap.Get();
}

} // namespace dt
