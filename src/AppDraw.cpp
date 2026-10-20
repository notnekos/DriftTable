#include "App.h"

namespace dt {

namespace {

constexpr uint32_t kAccent = 0xFF3D8BFF;
constexpr float kNotePad = 12.f;

D2D1_RECT_F rectF(double x, double y, double w, double h) {
    return D2D1::RectF((float)x, (float)y, (float)(x + w), (float)(y + h));
}

D2D1_RECT_F rectF(const RectD& r) { return rectF(r.x, r.y, r.w, r.h); }

bool isDark(uint32_t c) {
    double l = 0.2126 * ((c >> 16) & 0xFF) + 0.7152 * ((c >> 8) & 0xFF) + 0.0722 * (c & 0xFF);
    return l < 128;
}

} // namespace

IDWriteTextLayout* App::textLayout(const Item& it, const std::wstring& text, IDWriteTextFormat* fmt, float w,
                                   float h) {
    if (it.layout && it.layoutText == text && it.layout->GetMaxWidth() == w && it.layout->GetMaxHeight() == h)
        return it.layout.Get();
    it.layout.Reset();
    renderer_.dwrite()->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt, w, h, &it.layout);
    it.layoutText = text;
    return it.layout.Get();
}

// A label limited to maxLines lines, with an ellipsis when the name is longer.
IDWriteTextLayout* App::labelLayout(const Item& it, float width, int maxLines) {
    std::wstring text = labelFor(it);
    std::wstring key = text + L'\x1' + std::to_wstring(maxLines);
    if (it.layout && it.layoutText == key) return it.layout.Get();
    it.layout.Reset();
    auto lineCount = [&](const std::wstring& s, ComPtr<IDWriteTextLayout>& out) {
        out.Reset();
        renderer_.dwrite()->CreateTextLayout(s.c_str(), (UINT32)s.size(), renderer_.labelFormat(), width, 10000.f,
                                             &out);
        DWRITE_TEXT_METRICS m{};
        if (out) out->GetMetrics(&m);
        return (int)m.lineCount;
    };
    ComPtr<IDWriteTextLayout> layout;
    if (lineCount(text, layout) > maxLines && layout) {
        // Keep what fits on the allowed lines, then shorten until the ellipsis fits too.
        std::vector<DWRITE_LINE_METRICS> lines(256);
        UINT32 n = 0;
        layout->GetLineMetrics(lines.data(), (UINT32)lines.size(), &n);
        size_t keep = 0;
        for (int i = 0; i < maxLines && i < (int)n; ++i) keep += lines[i].length;
        std::wstring cut = text.substr(0, std::min(keep, text.size()));
        for (;;) {
            while (!cut.empty() && (cut.back() == L' ' || cut.back() == L'\n')) cut.pop_back();
            if (cut.empty() || lineCount(cut + L"\u2026", layout) <= maxLines) break;
            cut.pop_back();
        }
        lineCount(cut + L"\u2026", layout);
    }
    it.layout = layout;
    it.layoutText = key;
    return it.layout.Get();
}

void App::render() {
    ID2D1RenderTarget* rt = renderer_.beginDraw();
    if (!rt) return;
    dirty_ = false;

    drawBackground(rt);
    drawGrid(rt);

    const double s = scale();
    D2D1::Matrix3x2F view =
        D2D1::Matrix3x2F::Translation((float)-camX_, (float)-camY_) * D2D1::Matrix3x2F::Scale((float)s, (float)s);
    rt->SetTransform(view);

    RectD visible{camX_, camY_, width_ / s, height_ / s};
    visible = visible.inflated(24 / s);
    int shown = 0;
    for (auto& it : board_.items) {
        RectD r = it.rect().inflated(it.kind == ItemKind::Stroke ? it.strokeWidth : 0);
        if (it.kind == ItemKind::Stroke && it.id == drawingId_) r = visible; // bounds not final yet
        if (!r.intersects(visible)) continue;
        ++shown;
        drawItem(rt, it, selected_.count(it.id) != 0, it.id == hoverId_);
        rt->SetTransform(view);
    }

    if (drag_ == Drag::Rubber && rubber_.w > 0) {
        D2D1_RECT_F r = rectF(rubber_);
        rt->FillRectangle(r, renderer_.brush(argb(kAccent, 0.14f)));
        rt->DrawRectangle(r, renderer_.brush(argb(kAccent, 0.75f)), (float)(1 / s));
    }

    // Overview: where the main screen was before zooming out.
    if (overview_) {
        D2D1_RECT_F r = rectF(ovView_);
        D2D1_ROUNDED_RECT rr{r, (float)(10 / s), (float)(10 / s)};
        rt->FillRoundedRectangle(rr, renderer_.brush(D2D1::ColorF(1, 1, 1, 0.06f)));
        rt->DrawRoundedRectangle(rr, renderer_.brush(D2D1::ColorF(1, 1, 1, 0.75f)), (float)(2 / s),
                                 renderer_.dashStroke());
    }

    rt->SetTransform(D2D1::Matrix3x2F::Identity());

    // Eraser cursor.
    if (tool_ == Tool::Eraser && cursor_.x > -1000) {
        D2D1_ELLIPSE e = D2D1::Ellipse(D2D1::Point2F((float)cursor_.x, (float)cursor_.y), 10.f, 10.f);
        rt->DrawEllipse(e, renderer_.brush(D2D1::ColorF(0, 0, 0, 0.6f)), 3.f);
        rt->DrawEllipse(e, renderer_.brush(D2D1::ColorF(1, 1, 1, 0.95f)), 1.5f);
    }

    drawHud(rt);

    // "Nothing here": help finding the way back after drifting away.
    if (shown == 0 && !board_.items.empty() && !flight_.active) {
        RECT pa = primaryWorkArea();
        float d = (float)dpi_;
        float cx = (pa.left + pa.right) / 2.f / d, by = pa.bottom / d;
        rt->SetTransform(D2D1::Matrix3x2F::Scale(d, d));
        D2D1_RECT_F r = D2D1::RectF(cx - 150, by - 96, cx + 150, by - 62);
        drawPill(rt, r, 0.8f);
        const wchar_t* text = L"Здесь пусто · Home — вернуться к началу";
        rt->DrawText(text, (UINT32)wcslen(text), renderer_.hudFormat(), r,
                     renderer_.brush(D2D1::ColorF(1, 1, 1, 0.85f)));
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
    }

    if (!renderer_.endDraw(canvas_, mode_ != HostMode::Window)) {
        board_.clearCaches();
        invalidate();
    }
}

void App::drawBackground(ID2D1RenderTarget* rt) {
    if (mode_ != HostMode::Window) {
        // Almost transparent: the wallpaper shows through, but the canvas still
        // receives mouse input everywhere (fully transparent pixels click through).
        rt->Clear(D2D1::ColorF(0, 0, 0, 1.5f / 255.f));
        return;
    }
    // Window mode: a stand-in for a wallpaper.
    D2D1_GRADIENT_STOP stops[] = {{0.f, D2D1::ColorF(0x1B2A4A)}, {0.55f, D2D1::ColorF(0x3B2A5C)},
                                  {1.f, D2D1::ColorF(0x6A3B5E)}};
    ComPtr<ID2D1GradientStopCollection> collection;
    ComPtr<ID2D1LinearGradientBrush> brush;
    rt->CreateGradientStopCollection(stops, 3, &collection);
    if (collection)
        rt->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F((float)width_, (float)height_)),
            collection.Get(), &brush);
    if (brush) rt->FillRectangle(D2D1::RectF(0, 0, (float)width_, (float)height_), brush.Get());
    else rt->Clear(D2D1::ColorF(0x1B2A4A));
}

// A dot grid that appears while the desk moves, so motion is visible even on
// an empty part of the canvas, then fades away.
void App::drawGrid(ID2D1RenderTarget* rt) {
    if (gridAlpha_ <= 0.01) return;
    double s = scale();
    double spacing = 64;
    while (spacing * s < 36) spacing *= 2;
    while (spacing * s > 110) spacing /= 2;
    double step = spacing * s;
    double startX = std::floor(camX_ / spacing) * spacing, startY = std::floor(camY_ / spacing) * spacing;
    float size = 2.f * (float)dpi_;
    auto* b = renderer_.brush(D2D1::ColorF(1, 1, 1, (float)(0.28 * std::min(1.0, gridAlpha_))));
    for (double sy = (startY - camY_) * s; sy < height_; sy += step) {
        for (double sx = (startX - camX_) * s; sx < width_; sx += step) {
            rt->FillRectangle(D2D1::RectF((float)sx - size / 2, (float)sy - size / 2, (float)sx + size / 2,
                                          (float)sy + size / 2),
                              b);
        }
    }
}

void App::drawItem(ID2D1RenderTarget* rt, const Item& it, bool selected, bool hovered) {
    switch (it.kind) {
    case ItemKind::File: drawFile(rt, it, selected, hovered); break;
    case ItemKind::Picture: drawPicture(rt, it, selected); break;
    case ItemKind::Note: drawNote(rt, it, selected); break;
    case ItemKind::Stroke: drawStroke(rt, it, selected); break;
    }
    // Resize handle for a single selected picture or note.
    if (selected && selected_.size() == 1 && tool_ == Tool::Select &&
        (it.kind == ItemKind::Picture || it.kind == ItemKind::Note)) {
        float h = (float)(5 / scale());
        float x = (float)(it.x + it.w), y = (float)(it.y + it.h);
        D2D1_RECT_F r = D2D1::RectF(x - h, y - h, x + h, y + h);
        rt->FillRectangle(r, renderer_.brush(D2D1::ColorF(1, 1, 1)));
        rt->DrawRectangle(r, renderer_.brush(argb(kAccent)), (float)(1.5 / scale()));
    }
}

void App::drawFile(ID2D1RenderTarget* rt, const Item& it, bool selected, bool hovered) {
    const double s = scale();
    const bool renaming = editId_ == it.id && editRename_;
    const bool showLabel = s * 12 >= 5;

    // Label first: a selected tile grows to show the full name, like Explorer.
    IDWriteTextLayout* label = nullptr;
    float labelH = 0;
    float labelX = (float)it.x + 4, labelY = (float)(it.y + 6 + kIconSize + 4);
    float labelW = (float)kTileW - 8;
    if (showLabel && !renaming) {
        label = labelLayout(it, labelW, selected ? 12 : 2);
        if (label) {
            DWRITE_TEXT_METRICS m{};
            label->GetMetrics(&m);
            labelH = m.height;
        }
    }

    D2D1_RECT_F tile = rectF(it.rect());
    if (selected && labelH > 0) tile.bottom = std::max(tile.bottom, labelY + labelH + 4);
    D2D1_ROUNDED_RECT rr{tile, 6, 6};
    if (selected) {
        rt->FillRoundedRectangle(rr, renderer_.brush(argb(kAccent, 0.32f)));
        rt->DrawRoundedRectangle(rr, renderer_.brush(argb(kAccent, 0.8f)), (float)(1 / s));
    } else if (hovered) {
        rt->FillRoundedRectangle(rr, renderer_.brush(D2D1::ColorF(1, 1, 1, 0.14f)));
    }

    // Icon or thumbnail, aspect preserved inside the icon box.
    D2D1_RECT_F box = rectF(it.x + (kTileW - kIconSize) / 2, it.y + 6, kIconSize, kIconSize);
    Image* img = imageFor(it);
    ID2D1Bitmap* bmp = img ? renderer_.bitmapFor(*img) : nullptr;
    if (bmp) {
        double iw = img->width, ih = img->height;
        double f = std::min(kIconSize / iw, kIconSize / ih);
        double dw = iw * f, dh = ih * f;
        D2D1_RECT_F dst = rectF(box.left + (kIconSize - dw) / 2, box.top + (kIconSize - dh), dw, dh);
        rt->DrawBitmap(bmp, dst, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
        D2D1_ROUNDED_RECT ph{D2D1::RectF(box.left + 8, box.top + 8, box.right - 8, box.bottom - 8), 6, 6};
        rt->FillRoundedRectangle(ph, renderer_.brush(D2D1::ColorF(1, 1, 1, 0.18f)));
    }

    if (renaming) {
        ComPtr<IDWriteTextLayout> edit;
        float w = (float)kTileW + 40;
        renderer_.dwrite()->CreateTextLayout(editText_.c_str(), (UINT32)editText_.size(), renderer_.labelFormat(), w,
                                             400.f, &edit);
        if (!edit) return;
        DWRITE_TEXT_METRICS m{};
        edit->GetMetrics(&m);
        float x = (float)(it.x + kTileW / 2 - w / 2);
        D2D1_ROUNDED_RECT bg{D2D1::RectF(x - 2, labelY - 2, x + w + 2, labelY + std::max(m.height, 16.f) + 2), 3, 3};
        rt->FillRoundedRectangle(bg, renderer_.brush(D2D1::ColorF(1, 1, 1, 0.97f)));
        rt->DrawRoundedRectangle(bg, renderer_.brush(argb(kAccent)), (float)(1.5 / s));
        rt->DrawTextLayout(D2D1::Point2F(x, labelY), edit.Get(), renderer_.brush(D2D1::ColorF(0x111111)));
        if (caretOn_) {
            FLOAT cx, cy;
            DWRITE_HIT_TEST_METRICS hm{};
            edit->HitTestTextPosition((UINT32)caret_, FALSE, &cx, &cy, &hm);
            rt->FillRectangle(D2D1::RectF(x + cx, labelY + cy, x + cx + (float)(1.5 / s), labelY + cy + hm.height),
                              renderer_.brush(D2D1::ColorF(0x111111)));
        }
        return;
    }

    if (label) {
        // A soft shadow keeps white text readable on any wallpaper.
        float o = (float)(1 / s);
        rt->DrawTextLayout(D2D1::Point2F(labelX + o, labelY + o), label, renderer_.brush(D2D1::ColorF(0, 0, 0, 0.85f)));
        rt->DrawTextLayout(D2D1::Point2F(labelX, labelY + o), label, renderer_.brush(D2D1::ColorF(0, 0, 0, 0.35f)));
        rt->DrawTextLayout(D2D1::Point2F(labelX, labelY), label, renderer_.brush(D2D1::ColorF(1, 1, 1)));
    }
}

void App::drawPicture(ID2D1RenderTarget* rt, const Item& it, bool selected) {
    const double s = scale();
    D2D1_RECT_F r = rectF(it.rect());
    float o = (float)(3 / s);
    rt->FillRectangle(D2D1::RectF(r.left + o, r.top + o, r.right + o, r.bottom + o),
                      renderer_.brush(D2D1::ColorF(0, 0, 0, 0.3f)));
    Image* img = imageFor(it);
    ID2D1Bitmap* bmp = img ? renderer_.bitmapFor(*img) : nullptr;
    if (bmp) {
        rt->DrawBitmap(bmp, r, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
        rt->FillRectangle(r, renderer_.brush(D2D1::ColorF(1, 1, 1, 0.2f)));
    }
    if (selected) rt->DrawRectangle(r, renderer_.brush(argb(kAccent)), (float)(2 / s));
}

void App::drawNote(ID2D1RenderTarget* rt, const Item& it, bool selected) {
    const double s = scale();
    const bool editing = editId_ == it.id && !editRename_;
    D2D1_RECT_F r = rectF(it.rect());
    float o = (float)(2 / s);
    D2D1_ROUNDED_RECT shadow{D2D1::RectF(r.left, r.top + o, r.right, r.bottom + o * 2), 8, 8};
    rt->FillRoundedRectangle(shadow, renderer_.brush(D2D1::ColorF(0, 0, 0, 0.25f)));
    D2D1_ROUNDED_RECT rr{r, 8, 8};
    rt->FillRoundedRectangle(rr, renderer_.brush(argb(it.color)));
    if (selected || editing) rt->DrawRoundedRectangle(rr, renderer_.brush(argb(kAccent)), (float)(2 / s));

    const std::wstring& text = editing ? editText_ : it.text;
    if (s * 16 < 3) return; // too small to read anyway
    IDWriteTextLayout* layout = textLayout(it, text, renderer_.noteFormat(), (float)it.w - 2 * kNotePad, 100000.f);
    if (!layout) return;
    D2D1_COLOR_F ink = isDark(it.color) ? D2D1::ColorF(0xF2F2F2) : D2D1::ColorF(0x1E1E1E);
    D2D1_POINT_2F at = D2D1::Point2F(r.left + kNotePad, r.top + kNotePad);
    rt->DrawTextLayout(at, layout, renderer_.brush(ink));
    if (editing && caretOn_) {
        FLOAT cx, cy;
        DWRITE_HIT_TEST_METRICS hm{};
        layout->HitTestTextPosition((UINT32)caret_, FALSE, &cx, &cy, &hm);
        float h = hm.height > 0 ? hm.height : 21.f;
        rt->FillRectangle(D2D1::RectF(at.x + cx, at.y + cy, at.x + cx + (float)(1.5 / s), at.y + cy + h),
                          renderer_.brush(ink));
    }
}

void App::drawStroke(ID2D1RenderTarget* rt, const Item& it, bool selected) {
    if (it.pts.empty()) return;
    if (!it.geometry) {
        ComPtr<ID2D1PathGeometry> g;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(renderer_.factory()->CreatePathGeometry(&g)) || FAILED(g->Open(&sink))) return;
        auto pt = [&](size_t i) { return D2D1::Point2F(it.pts[i].x, it.pts[i].y); };
        size_t n = it.pts.size();
        sink->BeginFigure(pt(0), D2D1_FIGURE_BEGIN_HOLLOW);
        if (n == 1) {
            sink->AddLine(D2D1::Point2F(it.pts[0].x + 0.01f, it.pts[0].y)); // a dot
        } else if (n == 2) {
            sink->AddLine(pt(1));
        } else {
            // Quadratic curves through the midpoints: smooth lines from raw mouse input.
            for (size_t i = 1; i + 1 < n; ++i) {
                D2D1_POINT_2F mid = D2D1::Point2F((it.pts[i].x + it.pts[i + 1].x) / 2, (it.pts[i].y + it.pts[i + 1].y) / 2);
                sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(pt(i), mid));
            }
            sink->AddLine(pt(n - 1));
        }
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        it.geometry = g;
    }
    D2D1::Matrix3x2F view;
    rt->GetTransform(&view);
    rt->SetTransform(D2D1::Matrix3x2F::Translation((float)it.x, (float)it.y) * view);
    rt->DrawGeometry(it.geometry.Get(), renderer_.brush(argb(it.color)), it.strokeWidth, renderer_.roundStroke());
    rt->SetTransform(view);
    if (selected) {
        RectD b = it.rect().inflated(it.strokeWidth / 2 + 4 / scale());
        rt->DrawRectangle(rectF(b), renderer_.brush(argb(kAccent, 0.9f)), (float)(1.2 / scale()),
                          renderer_.dashStroke());
    }
}

void App::drawPill(ID2D1RenderTarget* rt, D2D1_RECT_F r, float alpha) {
    float radius = (r.bottom - r.top) / 2;
    D2D1_ROUNDED_RECT rr{r, radius, radius};
    rt->FillRoundedRectangle(rr, renderer_.brush(D2D1::ColorF(0.10f, 0.10f, 0.13f, 0.88f * alpha)));
    rt->DrawRoundedRectangle(rr, renderer_.brush(D2D1::ColorF(1, 1, 1, 0.10f * alpha)), 1.f);
}

// Screen-space overlay: the drawing toolbar and the zoom indicator. Laid out in
// DIPs under a DPI scale; hit rectangles are stored in pixels.
void App::drawHud(ID2D1RenderTarget* rt) {
    hud_.clear();
    RECT pa = primaryWorkArea();
    float d = (float)dpi_;
    rt->SetTransform(D2D1::Matrix3x2F::Scale(d, d));
    auto addButton = [&](D2D1_RECT_F r, int action) {
        hud_.push_back({D2D1::RectF(r.left * d, r.top * d, r.right * d, r.bottom * d), action});
    };
    auto white = [&](float a) { return renderer_.brush(D2D1::ColorF(1, 1, 1, a)); };

    if (tool_ != Tool::Select) {
        const float h = 44, btn = 28, toolW = 64;
        float total = 10 + toolW * 2 + 10 + 8 * btn + 10 + 3 * btn + 10 + btn + 10;
        float x = (pa.left + pa.right) / 2.f / d - total / 2, y = pa.top / d + 14;
        drawPill(rt, D2D1::RectF(x, y, x + total, y + h), 1.f);
        float cy = y + h / 2;
        float cx = x + 10;

        const wchar_t* names[] = {L"Перо", L"Ластик"};
        for (int i = 0; i < 2; ++i) {
            D2D1_RECT_F r = D2D1::RectF(cx, y + 7, cx + toolW - 4, y + h - 7);
            bool active = (i == 0) == (tool_ == Tool::Pen);
            if (active) {
                D2D1_ROUNDED_RECT rr{r, 8, 8};
                rt->FillRoundedRectangle(rr, white(0.16f));
            }
            rt->DrawText(names[i], (UINT32)wcslen(names[i]), renderer_.hudFormat(), r, white(active ? 1.f : 0.6f));
            addButton(r, 21 + i);
            cx += toolW;
        }
        cx += 10;
        for (int i = 0; i < 8; ++i) {
            D2D1_POINT_2F c = D2D1::Point2F(cx + btn / 2, cy);
            bool active = tool_ == Tool::Pen && penColor_ == kPenColors[i];
            if (active) rt->DrawEllipse(D2D1::Ellipse(c, 11.5f, 11.5f), white(0.95f), 2.f);
            rt->FillEllipse(D2D1::Ellipse(c, 8.f, 8.f), renderer_.brush(argb(kPenColors[i])));
            rt->DrawEllipse(D2D1::Ellipse(c, 8.f, 8.f), white(0.25f), 1.f);
            addButton(D2D1::RectF(cx, y, cx + btn, y + h), i + 1);
            cx += btn;
        }
        cx += 10;
        for (int i = 0; i < 3; ++i) {
            D2D1_POINT_2F c = D2D1::Point2F(cx + btn / 2, cy);
            float radius = kPenWidths[i] / 2 + 1.5f;
            if (tool_ == Tool::Pen && penWidthIndex_ == i)
                rt->DrawEllipse(D2D1::Ellipse(c, 11.5f, 11.5f), white(0.95f), 2.f);
            rt->FillEllipse(D2D1::Ellipse(c, radius, radius), white(0.9f));
            addButton(D2D1::RectF(cx, y, cx + btn, y + h), 11 + i);
            cx += btn;
        }
        cx += 10;
        // Close (back to selection).
        D2D1_POINT_2F c = D2D1::Point2F(cx + btn / 2, cy);
        rt->DrawLine(D2D1::Point2F(c.x - 5, c.y - 5), D2D1::Point2F(c.x + 5, c.y + 5), white(0.8f), 2.f,
                     renderer_.roundStroke());
        rt->DrawLine(D2D1::Point2F(c.x - 5, c.y + 5), D2D1::Point2F(c.x + 5, c.y - 5), white(0.8f), 2.f,
                     renderer_.roundStroke());
        addButton(D2D1::RectF(cx, y, cx + btn, y + h), 20);
    }

    if (overview_ && tool_ == Tool::Select) {
        const wchar_t* text = L"Обзор · щёлкните, куда перейти · Esc или Tab — назад";
        float cx = (pa.left + pa.right) / 2.f / d, y = pa.top / d + 14;
        D2D1_RECT_F r = D2D1::RectF(cx - 210, y, cx + 210, y + 36);
        drawPill(rt, r, 1.f);
        rt->DrawText(text, (UINT32)wcslen(text), renderer_.hudFormat(), r, white(0.92f));
    }

    if (zoomHudAlpha_ > 0) {
        float a = (float)std::min(1.0, zoomHudAlpha_);
        wchar_t text[16];
        swprintf(text, 16, L"%d%%", (int)std::lround(zoom_ * 100));
        float cx = (pa.left + pa.right) / 2.f / d, by = pa.bottom / d - 24;
        D2D1_RECT_F r = D2D1::RectF(cx - 36, by - 30, cx + 36, by);
        drawPill(rt, r, a);
        rt->DrawText(text, (UINT32)wcslen(text), renderer_.hudFormat(), r, white(a));
    }
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
}

} // namespace dt
