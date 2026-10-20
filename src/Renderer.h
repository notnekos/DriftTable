#pragma once

#include "ImageLoader.h"
#include "Util.h"

#include <d2d1.h>
#include <dwrite.h>

namespace dt {

inline D2D1_COLOR_F argb(uint32_t c, float alphaMul = 1.f) {
    return D2D1::ColorF(((c >> 16) & 0xFF) / 255.f, ((c >> 8) & 0xFF) / 255.f, (c & 0xFF) / 255.f,
                        ((c >> 24) & 0xFF) / 255.f * alphaMul);
}

// Direct2D renderer that draws into 32-bit premultiplied DIBs, one per canvas
// window (surface). In layered mode a DIB is pushed with UpdateLayeredWindow, so
// the wallpaper (static, Lively, Wallpaper Engine, ...) shows through every
// transparent pixel.
class Renderer {
public:
    ~Renderer();
    bool init();
    void setSurfaceCount(size_t count);
    void resize(size_t surface, int width, int height);

    // Returns null if nothing can be drawn right now.
    ID2D1RenderTarget* beginDraw(size_t surface);
    // Ends drawing and shows the frame on the window.
    // Returns false if the frame was lost and has to be drawn again.
    bool endDraw(size_t surface, HWND hwnd, bool layered);

    ID2D1Factory* factory() const { return factory_.Get(); }
    IDWriteFactory* dwrite() const { return dwrite_.Get(); }
    ID2D1SolidColorBrush* brush(D2D1_COLOR_F color);
    ID2D1Bitmap* bitmapFor(Image& image);
    ID2D1StrokeStyle* roundStroke() const { return roundStroke_.Get(); }
    ID2D1StrokeStyle* dashStroke() const { return dashStroke_.Get(); }

    IDWriteTextFormat* labelFormat() const { return labelFormat_.Get(); }
    IDWriteTextFormat* noteFormat() const { return noteFormat_.Get(); }
    IDWriteTextFormat* hudFormat() const { return hudFormat_.Get(); }

private:
    struct Surface {
        HDC dc = nullptr;
        HBITMAP dib = nullptr;
        HGDIOBJ old = nullptr;
        int width = 0, height = 0;
        bool shown = false;
        void release();
    };

    bool createTarget();

    ComPtr<ID2D1Factory> factory_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<ID2D1DCRenderTarget> target_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1StrokeStyle> roundStroke_, dashStroke_;
    ComPtr<IDWriteTextFormat> labelFormat_, noteFormat_, hudFormat_;

    std::vector<Surface> surfaces_;
    uint32_t generation_ = 1;
};

} // namespace dt
