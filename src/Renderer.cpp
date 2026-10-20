#include "Renderer.h"

namespace dt {

Renderer::~Renderer() { releaseSurface(); }

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

void Renderer::releaseSurface() {
    if (memDC_) {
        if (oldBitmap_) SelectObject(memDC_, oldBitmap_);
        DeleteDC(memDC_);
        memDC_ = nullptr;
    }
    if (dib_) {
        DeleteObject(dib_);
        dib_ = nullptr;
    }
    oldBitmap_ = nullptr;
}

void Renderer::resize(int width, int height) {
    width = std::max(1, width);
    height = std::max(1, height);
    if (width == width_ && height == height_ && dib_) return;
    releaseSurface();
    width_ = width;
    height_ = height;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width_;
    bi.bmiHeader.biHeight = -height_;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    memDC_ = CreateCompatibleDC(screen);
    dib_ = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (dib_) oldBitmap_ = SelectObject(memDC_, dib_);
}

bool Renderer::createTarget() {
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 0, 0,
        D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
    if (FAILED(factory_->CreateDCRenderTarget(&props, &target_))) return false;
    target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    target_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush_);
    ++generation_; // invalidates every cached device bitmap
    return true;
}

ID2D1RenderTarget* Renderer::beginDraw() {
    if (!dib_) return nullptr;
    if (!target_ && !createTarget()) return nullptr;
    RECT rc{0, 0, width_, height_};
    if (FAILED(target_->BindDC(memDC_, &rc))) return nullptr;
    target_->BeginDraw();
    target_->SetTransform(D2D1::Matrix3x2F::Identity());
    return target_.Get();
}

bool Renderer::endDraw(HWND hwnd, bool layered) {
    HRESULT hr = target_->EndDraw();
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) {
        target_.Reset();
        brush_.Reset();
        return false;
    }
    if (layered) {
        SIZE size{width_, height_};
        POINT src{0, 0};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(hwnd, nullptr, nullptr, &size, memDC_, &src, 0, &blend, ULW_ALPHA);
    } else {
        HDC dc = GetDC(hwnd);
        BitBlt(dc, 0, 0, width_, height_, memDC_, 0, 0, SRCCOPY);
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
