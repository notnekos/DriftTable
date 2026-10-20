#include "App.h"

namespace dt {

namespace {

constexpr double kMinZoom = 0.02;
constexpr double kMaxZoom = 8.0;
constexpr double kEraserRadius = 10.0;

const uint32_t kNoteColors[] = {0xFFFFF3A6, 0xFFFFC7DD, 0xFFC7E3FF, 0xFFCFF5C7, 0xFFFFFFFF, 0xFF2B2B30};
const wchar_t* kNoteColorNames[] = {L"Жёлтый", L"Розовый", L"Голубой", L"Зелёный", L"Белый", L"Тёмный"};

enum : UINT {
    CMD_RENAME = shell::kExtraMenuBase,
    CMD_PIN,
    CMD_REMOVE,
    CMD_OPEN,
    CMD_OPEN_WITH,
    CMD_SAVE_TO_DESKTOP,
    CMD_ORIGINAL_SIZE,
    CMD_EDIT,
    CMD_FRONT,
    CMD_DELETE,
    CMD_PASTE,
    CMD_NEW_NOTE,
    CMD_TOOL_SELECT,
    CMD_TOOL_PEN,
    CMD_TOOL_ERASER,
    CMD_HOME,
    CMD_FIT,
    CMD_UNDO,
    CMD_OPEN_DESKTOP,
    CMD_REFRESH,
    CMD_EXIT,
    CMD_PERSONALIZE,
    CMD_OVERVIEW,
    CMD_ICONS_LARGE,
    CMD_ICONS_MEDIUM,
    CMD_ICONS_SMALL,
    CMD_ALIGN_GRID,
    CMD_SHOW_ICONS,
    CMD_EMPTY_HINT,
    CMD_SORT_NAME,
    CMD_SORT_SIZE,
    CMD_SORT_TYPE,
    CMD_SORT_DATE,
    CMD_DISPLAY,
    CMD_NOTE_COLOR, // + index
};

bool keyDown(int vk) { return GetKeyState(vk) < 0; }

} // namespace

// ---------------------------------------------------------------------------
// Camera
// ---------------------------------------------------------------------------

void App::zoomAt(Vec2 s, double factor) {
    Vec2 w = toWorld(s);
    zoom_ = std::clamp(zoom_ * factor, kMinZoom, kMaxZoom);
    camX_ = w.x - s.x / scale();
    camY_ = w.y - s.y / scale();
    zoomHudAlpha_ = 1.6;
    invalidate();
}

void App::stopMotion() {
    velocity_ = {};
    pendScroll_ = {};
    pendZoom_ = 0;
    flight_.active = false;
}

void App::flyTo(double x, double y, double zoom) {
    stopMotion();
    flight_ = {true, 0, camX_, camY_, zoom_, x, y, std::clamp(zoom, kMinZoom, kMaxZoom)};
    invalidate();
}

void App::homeCamera(double* x, double* y) const {
    RECT pa = primaryWorkArea();
    *x = -(pa.left + 24.0) / dpi_;
    *y = -(pa.top + 24.0) / dpi_;
}

void App::resetView(bool animate) {
    overview_ = false;
    double x, y;
    homeCamera(&x, &y);
    if (animate) {
        flyTo(x, y, 1.0);
    } else {
        camX_ = x;
        camY_ = y;
        zoom_ = 1.0;
        invalidate();
    }
}

void App::fitAll() {
    overview_ = false;
    bool any = false;
    RectD b = board_.contentBounds(&any);
    if (!any) {
        resetView();
        return;
    }
    RECT pa = primaryWorkArea();
    double aw = pa.right - pa.left - 96, ah = pa.bottom - pa.top - 96;
    double z = std::clamp(std::min(aw / std::max(1.0, b.w), ah / std::max(1.0, b.h)) / dpi_, kMinZoom, 1.5);
    double s = z * dpi_;
    double cx = (pa.left + pa.right) / 2.0, cy = (pa.top + pa.bottom) / 2.0;
    flyTo(b.x + b.w / 2 - cx / s, b.y + b.h / 2 - cy / s, z);
}

void App::toggleOverview() {
    commitEdit();
    if (overview_) {
        overview_ = false;
        flyTo(ovX_, ovY_, ovZoom_);
        return;
    }
    RECT pa = primaryWorkArea();
    Vec2 a = toWorld({(double)pa.left, (double)pa.top}), b = toWorld({(double)pa.right, (double)pa.bottom});
    ovView_ = RectD::fromPoints(a, b);
    ovX_ = camX_;
    ovY_ = camY_;
    ovZoom_ = zoom_;
    overview_ = true;

    // Everything on the canvas plus the current view, with some air around it.
    bool any = false;
    RectD all = board_.contentBounds(&any);
    if (any) {
        double x0 = std::min(all.x, ovView_.x), y0 = std::min(all.y, ovView_.y);
        double x1 = std::max(all.right(), ovView_.right()), y1 = std::max(all.bottom(), ovView_.bottom());
        all = {x0, y0, x1 - x0, y1 - y0};
    } else {
        all = ovView_;
    }
    double aw = pa.right - pa.left - 160, ah = pa.bottom - pa.top - 200;
    double z = std::min(aw / std::max(1.0, all.w), ah / std::max(1.0, all.h)) / dpi_;
    // Always step back noticeably, even when everything already fits.
    z = std::clamp(std::min(z, zoom_ * 0.45), kMinZoom, kMaxZoom);
    double s = z * dpi_;
    double cx = (pa.left + pa.right) / 2.0, cy = (pa.top + pa.bottom) / 2.0 + 20;
    flyTo(all.x + all.w / 2 - cx / s, all.y + all.h / 2 - cy / s, z);
}

// Fly from the overview to a point, back at the zoom we had before.
void App::leaveOverviewAt(Vec2 world) {
    overview_ = false;
    RECT pa = primaryWorkArea();
    double s = ovZoom_ * dpi_;
    double cx = (pa.left + pa.right) / 2.0, cy = (pa.top + pa.bottom) / 2.0;
    flyTo(world.x - cx / s, world.y - cy / s, ovZoom_);
}

bool App::tick(double dt) {
    bool moving = false;

    if (flight_.active) {
        flight_.t = std::min(1.0, flight_.t + dt / 0.45);
        double t = flight_.t;
        double e = t < 0.5 ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3) / 2;
        // Interpolate the world point under the screen centre, and zoom in log space.
        double fromS = flight_.fromZoom * dpi_, toS = flight_.toZoom * dpi_;
        double cx = width_ / 2.0, cy = height_ / 2.0;
        double fcx = flight_.fromX + cx / fromS, fcy = flight_.fromY + cy / fromS;
        double tcx = flight_.toX + cx / toS, tcy = flight_.toY + cy / toS;
        zoom_ = std::exp(std::log(flight_.fromZoom) + (std::log(flight_.toZoom) - std::log(flight_.fromZoom)) * e);
        camX_ = fcx + (tcx - fcx) * e - cx / scale();
        camY_ = fcy + (tcy - fcy) * e - cy / scale();
        if (t >= 1.0) {
            flight_.active = false;
            scheduleSave();
        }
        moving = true;
    }

    // Inertia after a middle-button fling: the desk drifts and slows down.
    if (velocity_.length() > 0) {
        camX_ -= velocity_.x * dt / scale();
        camY_ -= velocity_.y * dt / scale();
        velocity_ = velocity_ * std::exp(-3.0 * dt);
        if (velocity_.length() < 12) {
            velocity_ = {};
            scheduleSave();
        }
        moving = true;
    }

    if (pendScroll_.length() > 0.3) {
        Vec2 d = pendScroll_ * (1 - std::exp(-dt * 20));
        camX_ += d.x / scale();
        camY_ += d.y / scale();
        pendScroll_ = pendScroll_ - d;
        moving = true;
    } else if (pendScroll_.length() > 0) {
        pendScroll_ = {};
        scheduleSave();
    }

    if (std::fabs(pendZoom_) > 1e-4) {
        double dz = pendZoom_ * (1 - std::exp(-dt * 20));
        zoomAt(zoomAnchor_, std::exp(dz));
        pendZoom_ -= dz;
        moving = true;
    } else if (pendZoom_ != 0) {
        pendZoom_ = 0;
        scheduleSave();
    }

    bool fading = false;
    if (moving || drag_ == Drag::Pan) {
        gridAlpha_ = 1.0;
    } else if (gridAlpha_ > 0) {
        gridAlpha_ = std::max(0.0, gridAlpha_ - dt * 1.8);
        fading = true;
    }
    if (zoomHudAlpha_ > 0 && pendZoom_ == 0) {
        zoomHudAlpha_ = std::max(0.0, zoomHudAlpha_ - dt * 1.5);
        fading = true;
    }
    if (moving || fading) invalidate();
    return moving || fading;
}

// ---------------------------------------------------------------------------
// Mouse
// ---------------------------------------------------------------------------

void App::setTool(Tool t) {
    if (t != Tool::Select) commitEdit();
    tool_ = t;
    hoverId_ = 0;
    invalidate();
    POINT p;
    GetCursorPos(&p);
    SetCursor(cursorFor(screenToClient(p)));
}

int App::hitTest(Vec2 w) const {
    double tol = 4 / scale();
    for (int i = (int)board_.items.size() - 1; i >= 0; --i)
        if (itemVisible(board_.items[i]) && board_.hitItem(board_.items[i], w, tol)) return i;
    return -1;
}

bool App::resizeHandleHit(Vec2 p, uint64_t* id) const {
    if (selected_.size() != 1 || tool_ != Tool::Select) return false;
    for (auto& it : board_.items) {
        if (it.id != *selected_.begin()) continue;
        if (it.kind != ItemKind::Picture && it.kind != ItemKind::Note) return false;
        Vec2 corner = toScreen({it.x + it.w, it.y + it.h});
        if (std::fabs(p.x - corner.x) <= 9 && std::fabs(p.y - corner.y) <= 9) {
            *id = it.id;
            return true;
        }
    }
    return false;
}

bool App::isOutsideCanvas(Vec2 p) const {
    HWND h = WindowFromPoint(clientToScreen(p));
    return h && !isOurWindow(h);
}

HCURSOR App::cursorFor(Vec2 p) const {
    auto c = [](LPCWSTR id) { return LoadCursorW(nullptr, id); };
    if (drag_ == Drag::Pan || spaceDown_) return c(IDC_SIZEALL);
    if (drag_ == Drag::Resize) return c(IDC_SIZENWSE);
    if (hudHit(p)) return c(IDC_HAND);
    if (tool_ != Tool::Select) return c(IDC_CROSS);
    uint64_t id;
    if (resizeHandleHit(p, &id)) return c(IDC_SIZENWSE);
    if (editId_ && !editRename_) {
        for (auto& it : board_.items)
            if (it.id == editId_ && it.rect().contains(toWorld(p))) return c(IDC_IBEAM);
    }
    return c(IDC_ARROW);
}

void App::onMouseDown(int button, Vec2 p, bool dbl) {
    cursor_ = p;
    focusCanvas();
    if (drag_ != Drag::None) return;

    auto beginPan = [&](int btn) {
        stopMotion();
        drag_ = Drag::Pan;
        dragButton_ = btn;
        dragLast_ = p;
        panTravel_ = 0;
        panSamples_.clear();
        panSamples_.push_back({seconds(), p});
        SetCapture(inputWnd_ ? inputWnd_ : canvas_);
        SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
    };

    if (button == 1) {
        beginPan(1);
        return;
    }
    if (button != 0) return; // right button acts on release

    if (dbl) {
        onDoubleClick(p);
        return;
    }
    if (int action = hudHit(p)) {
        hudAction(action);
        return;
    }

    Vec2 w = toWorld(p);
    if (editId_) {
        Item* e = board_.find(editId_);
        if (e && !editRename_ && e->rect().contains(w)) {
            // Click inside the note being edited: move the caret there.
            if (e->layout) {
                BOOL trailing = FALSE, inside = FALSE;
                DWRITE_HIT_TEST_METRICS m{};
                e->layout->HitTestPoint((float)(w.x - e->x - 12), (float)(w.y - e->y - 12), &trailing, &inside, &m);
                caret_ = std::min(editText_.size(), (size_t)m.textPosition + (trailing ? m.length : 0));
                caretOn_ = true;
                invalidate();
            }
            return;
        }
        commitEdit();
    }
    if (spaceDown_) {
        beginPan(0);
        return;
    }
    stopMotion();
    dragButton_ = 0;
    dragStart_ = p;
    dragMoved_ = false;

    if (tool_ == Tool::Pen) {
        board_.pushUndo();
        Item s;
        s.kind = ItemKind::Stroke;
        s.x = w.x;
        s.y = w.y;
        s.color = penColor_;
        s.strokeWidth = (float)(kPenWidths[penWidthIndex_] / zoom_);
        s.pts.push_back({0, 0});
        drawingId_ = board_.add(std::move(s));
        drag_ = Drag::Draw;
        SetCapture(inputWnd_ ? inputWnd_ : canvas_);
        invalidate();
        return;
    }
    if (tool_ == Tool::Eraser) {
        board_.pushUndo();
        erasedAny_ = false;
        drag_ = Drag::Erase;
        eraseAt(w);
        SetCapture(inputWnd_ ? inputWnd_ : canvas_);
        return;
    }

    uint64_t rid;
    if (resizeHandleHit(p, &rid)) {
        Item* it = board_.find(rid);
        resizeId_ = rid;
        resizeOrig_ = it->rect();
        drag_ = Drag::Resize;
        SetCapture(inputWnd_ ? inputWnd_ : canvas_);
        return;
    }

    int idx = hitTest(w);
    bool ctrl = keyDown(VK_CONTROL) || keyDown(VK_SHIFT);
    if (idx >= 0) {
        uint64_t id = board_.items[idx].id;
        if (ctrl) {
            if (selected_.count(id)) {
                selected_.erase(id);
                invalidate();
                return;
            }
            selected_.insert(id);
        } else if (!selected_.count(id)) {
            selectOnly(id);
        }
        pressedId_ = id;
        moveOrigins_.clear();
        for (auto& it : board_.items)
            if (selected_.count(it.id)) moveOrigins_.push_back({it.id, {it.x, it.y}});
        drag_ = Drag::Move;
    } else {
        if (!ctrl) selected_.clear();
        rubber_ = {};
        drag_ = Drag::Rubber;
    }
    SetCapture(inputWnd_ ? inputWnd_ : canvas_);
    invalidate();
}

void App::onMouseMove(Vec2 p) {
    cursor_ = p;
    switch (drag_) {
    case Drag::Pan: {
        Vec2 d = p - dragLast_;
        panTravel_ += d.length();
        camX_ -= d.x / scale();
        camY_ -= d.y / scale();
        dragLast_ = p;
        double now = seconds();
        panSamples_.push_back({now, p});
        while (panSamples_.size() > 2 && now - panSamples_.front().first > 0.1) panSamples_.pop_front();
        gridAlpha_ = 1;
        invalidate();
        break;
    }
    case Drag::Move: {
        if (!dragMoved_) {
            if ((p - dragStart_).length() < 4) break;
            board_.pushUndo();
            dragMoved_ = true;
        }
        Vec2 d = (p - dragStart_) * (1 / scale());
        bool files = false;
        for (auto& [id, origin] : moveOrigins_) {
            if (Item* it = board_.find(id)) {
                it->x = origin.x + d.x;
                it->y = origin.y + d.y;
                files |= it->kind == ItemKind::File;
            }
        }
        invalidate();
        // Dragged over another window (or the taskbar): hand the files to OLE.
        if (files && isOutsideCanvas(p)) startDragOut();
        break;
    }
    case Drag::Rubber: {
        rubber_ = RectD::fromPoints(toWorld(dragStart_), toWorld(p));
        if (!keyDown(VK_CONTROL) && !keyDown(VK_SHIFT)) selected_.clear();
        for (auto& it : board_.items)
            if (itemVisible(it) && rubber_.intersects(it.rect())) selected_.insert(it.id);
        invalidate();
        break;
    }
    case Drag::Draw: {
        Item* s = board_.find(drawingId_);
        if (!s) break;
        Vec2 w = toWorld(p);
        PointF local{(float)(w.x - s->x), (float)(w.y - s->y)};
        PointF last = s->pts.back();
        double dist = std::hypot(local.x - last.x, local.y - last.y) * scale();
        if (dist >= 1.5) {
            s->pts.push_back(local);
            s->geometry.Reset();
            invalidate();
        }
        break;
    }
    case Drag::Erase:
        eraseAt(toWorld(p));
        invalidate();
        break;
    case Drag::Resize: {
        Item* it = board_.find(resizeId_);
        if (!it) break;
        if (!dragMoved_) {
            board_.pushUndo();
            dragMoved_ = true;
            it = board_.find(resizeId_);
        }
        double dx = (p.x - dragStart_.x) / scale();
        if (it->kind == ItemKind::Picture) {
            it->w = std::max(24.0, resizeOrig_.w + dx);
            it->h = it->w * resizeOrig_.h / resizeOrig_.w;
        } else {
            it->w = std::max(120.0, resizeOrig_.w + dx);
            it->layout.Reset();
            layoutNote(*it, editId_ == it->id ? editText_ : it->text);
        }
        invalidate();
        break;
    }
    case Drag::None: {
        uint64_t hover = 0;
        if (tool_ == Tool::Select) {
            int idx = hitTest(toWorld(p));
            if (idx >= 0 && board_.items[idx].kind != ItemKind::Stroke) hover = board_.items[idx].id;
        }
        if (hover != hoverId_) {
            hoverId_ = hover;
            invalidate();
        }
        if (tool_ == Tool::Eraser) invalidate();
        break;
    }
    }
}

void App::onMouseUp(int button, Vec2 p) {
    if (button == 2) {
        if (drag_ != Drag::None) return;
        if (tool_ != Tool::Select) {
            setTool(Tool::Select);
            return;
        }
        commitEdit();
        int idx = hitTest(toWorld(p));
        if (idx >= 0) showItemMenu(idx, p);
        else showCanvasMenu(p);
        return;
    }
    if (drag_ == Drag::None || button != dragButton_) return;
    endDrag(false);
}

void App::endDrag(bool cancelled) {
    Drag d = drag_;
    drag_ = Drag::None; // before ReleaseCapture: it sends WM_CAPTURECHANGED
    if (isOurWindow(GetCapture())) ReleaseCapture();

    switch (d) {
    case Drag::Pan: {
        velocity_ = {};
        double now = seconds();
        if (!cancelled && panSamples_.size() >= 2 && now - panSamples_.back().first < 0.05) {
            auto& a = panSamples_.front();
            auto& b = panSamples_.back();
            double dt = b.first - a.first;
            if (dt > 0.005) {
                velocity_ = (b.second - a.second) * (1 / dt);
                double speed = velocity_.length();
                if (speed < 120) velocity_ = {};
                else if (speed > 8000) velocity_ = velocity_ * (8000 / speed);
            }
        }
        if (velocity_.length() == 0) scheduleSave();
        break;
    }
    case Drag::Move:
        if (dragMoved_ && alignToGrid_) {
            std::vector<uint64_t> ids;
            for (auto& [id, origin] : moveOrigins_) ids.push_back(id);
            snapToGrid(ids);
        }
        if (!dragMoved_) {
            if (!cancelled && !keyDown(VK_CONTROL) && !keyDown(VK_SHIFT)) selectOnly(pressedId_);
            if (!cancelled && overview_) {
                if (Item* it = board_.find(pressedId_)) leaveOverviewAt({it->x + it->w / 2, it->y + it->h / 2});
            }
        } else {
            scheduleSave();
        }
        break;
    case Drag::Draw:
        if (Item* s = board_.find(drawingId_)) s->recomputeStrokeBounds();
        drawingId_ = 0;
        scheduleSave();
        break;
    case Drag::Erase:
        if (!erasedAny_) board_.discardUndo();
        else scheduleSave();
        break;
    case Drag::Resize:
        if (dragMoved_) scheduleSave();
        break;
    case Drag::Rubber:
        // A plain click on an empty spot in the overview: go there.
        if (!cancelled && overview_ && (cursor_ - dragStart_).length() < 5) leaveOverviewAt(toWorld(dragStart_));
        rubber_ = {};
        break;
    case Drag::None: break;
    }
    invalidate();
}

void App::eraseAt(Vec2 w) {
    double r = kEraserRadius / scale();
    for (int i = (int)board_.items.size() - 1; i >= 0; --i) {
        const Item& it = board_.items[i];
        if (it.kind == ItemKind::Stroke && board_.hitItem(it, w, r)) {
            selected_.erase(it.id);
            board_.items.erase(board_.items.begin() + i);
            erasedAny_ = true;
        }
    }
    invalidate();
}

void App::startDragOut() {
    auto paths = selectedFilePaths();
    if (paths.empty()) return;
    // Put everything back: from here on it is an ordinary Explorer drag.
    for (auto& [id, origin] : moveOrigins_) {
        if (Item* it = board_.find(id)) {
            it->x = origin.x;
            it->y = origin.y;
        }
    }
    if (dragMoved_) board_.discardUndo();
    dragMoved_ = false;
    drag_ = Drag::None;
    if (isOurWindow(GetCapture())) ReleaseCapture();
    invalidate();
    render();
    shell::dragOut(paths);
    SetTimer(main_, TIMER_SYNC, 250, nullptr);
}

void App::onWheel(int delta, bool horizontal, Vec2 p) {
    flight_.active = false;
    velocity_ = {};
    if (keyDown(VK_CONTROL) && !horizontal) {
        pendZoom_ += (delta / 120.0) * std::log(1.2);
        zoomAnchor_ = p;
        zoomHudAlpha_ = 1.6;
    } else if (horizontal) {
        pendScroll_.x += delta * 1.4;
    } else if (keyDown(VK_SHIFT)) {
        pendScroll_.x -= delta * 1.4;
    } else {
        pendScroll_.y -= delta * 1.4;
    }
    invalidate();
}

void App::onDoubleClick(Vec2 p) {
    if (int action = hudHit(p)) {
        hudAction(action);
        return;
    }
    if (tool_ != Tool::Select) return;
    Vec2 w = toWorld(p);
    int idx = hitTest(w);
    if (idx < 0) {
        board_.pushUndo();
        uint64_t id = addNote({w.x - 20, w.y - 20}, L"");
        beginEdit(id, false);
        return;
    }
    const Item& it = board_.items[idx];
    switch (it.kind) {
    case ItemKind::File:
    case ItemKind::Picture: shell::open(canvas_, it.path); break;
    case ItemKind::Note: beginEdit(it.id, false); break;
    case ItemKind::Stroke: break;
    }
}

// ---------------------------------------------------------------------------
// Keyboard
// ---------------------------------------------------------------------------

void App::onKeyDown(WPARAM vk) {
    bool ctrl = keyDown(VK_CONTROL), shift = keyDown(VK_SHIFT);

    if (editId_) {
        switch (vk) {
        case VK_ESCAPE: commitEdit(editRename_); break;
        case VK_RETURN:
            if (editRename_ || ctrl) commitEdit();
            break;
        case VK_LEFT:
            if (caret_ > 0) --caret_;
            break;
        case VK_RIGHT:
            if (caret_ < editText_.size()) ++caret_;
            break;
        case VK_HOME: caret_ = 0; break;
        case VK_END: caret_ = editText_.size(); break;
        case VK_DELETE:
            if (caret_ < editText_.size()) editText_.erase(caret_, 1);
            break;
        case 'V':
            if (ctrl && OpenClipboard(main_)) {
                if (HGLOBAL g = GetClipboardData(CF_UNICODETEXT)) {
                    if (auto s = static_cast<const wchar_t*>(GlobalLock(g))) {
                        std::wstring t = s;
                        GlobalUnlock(g);
                        t.erase(std::remove(t.begin(), t.end(), L'\r'), t.end());
                        if (editRename_) std::replace(t.begin(), t.end(), L'\n', L' ');
                        editText_.insert(caret_, t);
                        caret_ += t.size();
                    }
                }
                CloseClipboard();
            }
            break;
        }
        if (editId_ && !editRename_) {
            if (Item* it = board_.find(editId_)) layoutNote(*it, editText_);
        }
        caretOn_ = true;
        invalidate();
        return;
    }

    RECT pa = primaryWorkArea();
    Vec2 center{(pa.left + pa.right) / 2.0, (pa.top + pa.bottom) / 2.0};
    Vec2 at = (cursor_.x > -1000) ? cursor_ : center;
    double step = shift ? 480 : 120;

    switch (vk) {
    case VK_SPACE:
        if (!spaceDown_) {
            spaceDown_ = true;
            SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
        }
        break;
    case VK_DELETE: deleteSelection(); break;
    case VK_ESCAPE:
        if (overview_) toggleOverview();
        else if (tool_ != Tool::Select) setTool(Tool::Select);
        else selectOnly(0);
        break;
    case VK_TAB: toggleOverview(); break;
    case VK_HOME: resetView(); break;
    case VK_F2:
        if (selected_.size() == 1) {
            uint64_t id = *selected_.begin();
            Item* it = board_.find(id);
            if (it) beginEdit(id, it->kind == ItemKind::File);
        }
        break;
    case VK_F5: refresh(); break;
    case VK_RETURN:
        for (auto& it : board_.items) {
            if (!selected_.count(it.id)) continue;
            if (it.kind == ItemKind::File || it.kind == ItemKind::Picture) shell::open(canvas_, it.path);
        }
        break;
    case VK_LEFT: pendScroll_.x -= step; break;
    case VK_RIGHT: pendScroll_.x += step; break;
    case VK_UP: pendScroll_.y -= step; break;
    case VK_DOWN: pendScroll_.y += step; break;
    case VK_PRIOR: pendScroll_.y -= height_ * 0.8; break;
    case VK_NEXT: pendScroll_.y += height_ * 0.8; break;
    case VK_ADD:
    case VK_OEM_PLUS:
        pendZoom_ += std::log(1.25);
        zoomAnchor_ = at;
        break;
    case VK_SUBTRACT:
    case VK_OEM_MINUS:
        pendZoom_ -= std::log(1.25);
        zoomAnchor_ = at;
        break;
    case '0':
        if (ctrl) {
            Vec2 w = toWorld(at);
            flyTo(w.x - at.x / dpi_, w.y - at.y / dpi_, 1.0);
        }
        break;
    case 'Z':
        if (ctrl) {
            if (shift ? board_.redo() : board_.undo()) {
                syncDesktop();
                scheduleSave();
            }
        }
        break;
    case 'Y':
        if (ctrl && board_.redo()) {
            syncDesktop();
            scheduleSave();
        }
        break;
    case 'A':
        if (ctrl)
            for (auto& it : board_.items)
                if (itemVisible(it)) selected_.insert(it.id);
        break;
    case 'C':
        if (ctrl) shell::copyToClipboard(canvas_, selectedFilePaths());
        break;
    case 'V':
        if (ctrl) paste(toWorld(at));
        else setTool(Tool::Select);
        break;
    case 'B':
    case 'P': setTool(Tool::Pen); break;
    case 'E': setTool(Tool::Eraser); break;
    case 'N': {
        board_.pushUndo();
        Vec2 w = toWorld(at);
        beginEdit(addNote({w.x - 20, w.y - 20}, L""), false);
        break;
    }
    case 'F':
        if (!ctrl) fitAll();
        break;
    case VK_OEM_4: penWidthIndex_ = std::max(0, penWidthIndex_ - 1); break;
    case VK_OEM_6: penWidthIndex_ = std::min(2, penWidthIndex_ + 1); break;
    default:
        if (vk >= '1' && vk <= '8' && !ctrl) {
            penColor_ = kPenColors[vk - '1'];
            if (tool_ != Tool::Pen) setTool(Tool::Pen);
        }
        break;
    }
    // Forget selected items that no longer exist (after undo, delete...).
    for (auto i = selected_.begin(); i != selected_.end();)
        i = board_.indexOf(*i) < 0 ? selected_.erase(i) : std::next(i);
    invalidate();
}

void App::onKeyUp(WPARAM vk) {
    if (vk == VK_SPACE) {
        spaceDown_ = false;
        POINT p;
        GetCursorPos(&p);
        SetCursor(cursorFor(screenToClient(p)));
    }
}

void App::onChar(wchar_t c) {
    if (!editId_) return;
    if (c == L'\b') {
        if (caret_ > 0) {
            size_t n = (caret_ >= 2 && IS_LOW_SURROGATE(editText_[caret_ - 1]) &&
                        IS_HIGH_SURROGATE(editText_[caret_ - 2]))
                           ? 2
                           : 1;
            editText_.erase(caret_ - n, n);
            caret_ -= n;
        }
    } else if (c == L'\r') {
        if (editRename_ || keyDown(VK_CONTROL)) return;
        editText_.insert(caret_++, 1, L'\n');
    } else if (c >= 32 && c != 127) {
        if (editRename_ && wcschr(L"\\/:*?\"<>|", c)) {
            MessageBeep(MB_ICONWARNING);
            return;
        }
        editText_.insert(caret_++, 1, c);
    } else {
        return;
    }
    if (!editRename_) {
        if (Item* it = board_.find(editId_)) layoutNote(*it, editText_);
    }
    caretOn_ = true;
    invalidate();
}

// ---------------------------------------------------------------------------
// HUD and menus
// ---------------------------------------------------------------------------

int App::hudHit(Vec2 p) const {
    for (auto& b : hud_)
        if (p.x >= b.r.left && p.x <= b.r.right && p.y >= b.r.top && p.y <= b.r.bottom) return b.action;
    return 0;
}

void App::hudAction(int action) {
    if (action >= 1 && action <= 8) {
        penColor_ = kPenColors[action - 1];
        setTool(Tool::Pen);
    } else if (action >= 11 && action <= 13) {
        penWidthIndex_ = action - 11;
        if (tool_ != Tool::Pen) setTool(Tool::Pen);
    } else if (action == 20) {
        setTool(Tool::Select);
    } else if (action == 21) {
        setTool(Tool::Pen);
    } else if (action == 22) {
        setTool(Tool::Eraser);
    }
    saveSettings();
    invalidate();
}

void App::showItemMenu(int index, Vec2 p) {
    Item it = board_.items[index];
    if (!selected_.count(it.id)) selectOnly(it.id);
    if (dirty_) render();
    POINT sp = clientToScreen(p);

    if (it.kind == ItemKind::File) {
        std::vector<shell::ExtraMenuItem> extra;
        if (!it.isVirtual()) extra.push_back({CMD_RENAME, L"Переименовать\tF2"});
        if (isImageExtension(extensionOf(it.path))) extra.push_back({CMD_PIN, L"Прикрепить как картинку"});
        if (it.isVirtual()) extra.push_back({CMD_REMOVE, L"Убрать с холста"});
        auto paths = selectedFilePaths();
        UINT cmd = shell::contextMenu(menuOwner(), paths, sp, extra);
        if (cmd) runCommand(cmd, p);
        SetTimer(main_, TIMER_SYNC, 400, nullptr);
        return;
    }

    HMENU m = CreatePopupMenu();
    if (it.kind == ItemKind::Note) {
        AppendMenuW(m, MF_STRING, CMD_EDIT, L"Редактировать\tF2");
        HMENU colors = CreatePopupMenu();
        for (int i = 0; i < 6; ++i)
            AppendMenuW(colors, MF_STRING | (it.color == kNoteColors[i] ? MF_CHECKED : 0), CMD_NOTE_COLOR + i,
                        kNoteColorNames[i]);
        AppendMenuW(m, MF_POPUP, (UINT_PTR)colors, L"Цвет");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    } else if (it.kind == ItemKind::Picture) {
        AppendMenuW(m, MF_STRING, CMD_OPEN, L"Открыть");
        AppendMenuW(m, MF_STRING, CMD_OPEN_WITH, L"Открыть с помощью…");
        AppendMenuW(m, MF_STRING, CMD_SAVE_TO_DESKTOP, L"Сохранить файлом на рабочий стол");
        AppendMenuW(m, MF_STRING, CMD_ORIGINAL_SIZE, L"Исходный размер");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(m, MF_STRING, CMD_FRONT, L"На передний план");
    AppendMenuW(m, MF_STRING, CMD_DELETE, L"Удалить\tDel");
    UINT cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, sp.x, sp.y, menuOwner(), nullptr);
    DestroyMenu(m);
    if (cmd) runCommand(cmd, p);
}

// Laid out like the Windows desktop menu: View, Sort by, Refresh, Paste, Undo,
// then everything the shell and installed apps put there, then our own tools.
void App::showCanvasMenu(Vec2 p) {
    UINT png = RegisterClipboardFormatW(L"PNG");
    bool canPaste = IsClipboardFormatAvailable(CF_HDROP) || IsClipboardFormatAvailable(CF_DIB) ||
                    IsClipboardFormatAvailable(png) || IsClipboardFormatAvailable(CF_UNICODETEXT);
    auto check = [](bool on) { return MF_STRING | (on ? MF_CHECKED : 0); };

    HMENU view = CreatePopupMenu();
    AppendMenuW(view, check(iconSize_ == 2), CMD_ICONS_LARGE, L"Крупные значки");
    AppendMenuW(view, check(iconSize_ == 1), CMD_ICONS_MEDIUM, L"Обычные значки");
    AppendMenuW(view, check(iconSize_ == 0), CMD_ICONS_SMALL, L"Мелкие значки");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, check(alignToGrid_), CMD_ALIGN_GRID, L"Выровнять значки по сетке");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, check(showIcons_), CMD_SHOW_ICONS, L"Отображать значки рабочего стола");
    AppendMenuW(view, check(emptyHint_), CMD_EMPTY_HINT, L"Подсказка «Здесь пусто»");
    for (UINT id : {CMD_ICONS_LARGE, CMD_ICONS_MEDIUM, CMD_ICONS_SMALL}) {
        MENUITEMINFOW mi{};
        mi.cbSize = sizeof(mi);
        mi.fMask = MIIM_FTYPE;
        mi.fType = MFT_RADIOCHECK;
        SetMenuItemInfoW(view, id, FALSE, &mi);
    }

    HMENU sort = CreatePopupMenu();
    AppendMenuW(sort, MF_STRING, CMD_SORT_NAME, L"Имя");
    AppendMenuW(sort, MF_STRING, CMD_SORT_SIZE, L"Размер");
    AppendMenuW(sort, MF_STRING, CMD_SORT_TYPE, L"Тип элемента");
    AppendMenuW(sort, MF_STRING, CMD_SORT_DATE, L"Дата изменения");

    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_POPUP, (UINT_PTR)view, L"Вид");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sort, L"Сортировка");
    AppendMenuW(m, MF_STRING, CMD_REFRESH, L"Обновить\tF5");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (canPaste ? 0 : MF_GRAYED), CMD_PASTE, L"Вставить\tCtrl+V");
    AppendMenuW(m, MF_STRING | (board_.canUndo() ? 0 : MF_GRAYED), CMD_UNDO, L"Отменить\tCtrl+Z");
    AppendMenuW(m, MF_STRING, CMD_NEW_NOTE, L"Новая заметка\tN");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

    // Shell and third-party items (New, Display settings, NVIDIA, VS Code, Git...).
    shell::BackgroundMenu shellMenu;
    HWND owner = menuOwner();
    if (!shellMenu.insert(owner, m, GetMenuItemCount(m))) {
        AppendMenuW(m, MF_STRING, CMD_DISPLAY, L"Параметры экрана");
        AppendMenuW(m, MF_STRING, CMD_PERSONALIZE, L"Персонализация");
    }

    HMENU drift = CreatePopupMenu();
    AppendMenuW(drift, MF_STRING, CMD_HOME, L"Вернуться к началу\tHome");
    AppendMenuW(drift, MF_STRING, CMD_FIT, L"Показать всё\tF");
    AppendMenuW(drift, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(drift, check(tool_ == Tool::Select), CMD_TOOL_SELECT, L"Выделение\tV");
    AppendMenuW(drift, check(tool_ == Tool::Eraser), CMD_TOOL_ERASER, L"Ластик\tE");
    AppendMenuW(drift, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(drift, MF_STRING, CMD_OPEN_DESKTOP, L"Открыть папку «Рабочий стол»");
    AppendMenuW(drift, MF_STRING, CMD_EXIT, L"Выйти из DriftTable");

    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, check(overview_), CMD_OVERVIEW, L"Обзор (отдалить)\tTab");
    AppendMenuW(m, check(tool_ == Tool::Pen), CMD_TOOL_PEN, L"Рисовать\tB");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)drift, L"DriftTable");

    POINT sp = clientToScreen(p);
    bool shellCommand = false;
    UINT cmd = shellMenu.track(owner, m, sp, folders_.userDesktop(), &shellCommand);
    DestroyMenu(m);
    if (shellCommand) {
        // e.g. New > Text Document: the file should appear where the menu was opened.
        newFileAnchor_ = toWorld(p);
        newFileAnchorUntil_ = seconds() + 20;
        SetTimer(main_, TIMER_SYNC, 300, nullptr);
    }
    if (cmd) runCommand(cmd, p);
}

void App::runCommand(UINT cmd, Vec2 p) {
    Vec2 w = toWorld(p);
    Item* first = selected_.empty() ? nullptr : board_.find(*selected_.begin());

    if (cmd >= CMD_NOTE_COLOR && cmd < CMD_NOTE_COLOR + 6) {
        board_.pushUndo();
        for (auto& it : board_.items)
            if (selected_.count(it.id) && it.kind == ItemKind::Note) it.color = kNoteColors[cmd - CMD_NOTE_COLOR];
        scheduleSave();
        invalidate();
        return;
    }

    switch (cmd) {
    case CMD_RENAME:
        if (first) beginEdit(first->id, true);
        break;
    case CMD_PIN:
        if (first) {
            board_.pushUndo();
            Vec2 at{first->x + first->w + 260, first->y + first->h / 2};
            if (!addPicture(first->path, at, true)) board_.discardUndo();
            scheduleSave();
        }
        break;
    case CMD_REMOVE:
        board_.pushUndo();
        for (auto i = selected_.begin(); i != selected_.end(); ++i) {
            Item* it = board_.find(*i);
            if (it && it->isVirtual()) board_.remove(*i);
        }
        selected_.clear();
        scheduleSave();
        break;
    case CMD_OPEN:
        if (first) shell::open(canvas_, first->path);
        break;
    case CMD_OPEN_WITH:
        if (first) shell::openWith(canvas_, first->path);
        break;
    case CMD_SAVE_TO_DESKTOP:
        if (first && first->kind == ItemKind::Picture) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            wchar_t name[96];
            swprintf(name, 96, L"Картинка %04d-%02d-%02d %02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour,
                     st.wMinute, st.wSecond);
            std::wstring dest = folders_.userDesktop() + L"\\" + name + extensionOf(first->path);
            pendingPos_[toLower(dest)] = {first->x + first->w + 16, first->y};
            if (CopyFileW(first->path.c_str(), dest.c_str(), TRUE)) syncDesktop();
        }
        break;
    case CMD_ORIGINAL_SIZE:
        if (first && first->kind == ItemKind::Picture) {
            int pw, ph;
            if (ImageLoader::imageSize(first->path, &pw, &ph)) {
                board_.pushUndo();
                first = board_.find(*selected_.begin());
                first->w = pw / dpi_;
                first->h = ph / dpi_;
                scheduleSave();
            }
        }
        break;
    case CMD_EDIT:
        if (first) beginEdit(first->id, false);
        break;
    case CMD_FRONT:
        board_.pushUndo();
        for (auto id : std::vector<uint64_t>(selected_.begin(), selected_.end())) board_.bringToFront(id);
        scheduleSave();
        break;
    case CMD_DELETE: deleteSelection(); break;
    case CMD_PASTE: paste(w); break;
    case CMD_NEW_NOTE:
        board_.pushUndo();
        beginEdit(addNote(w, L""), false);
        break;
    case CMD_TOOL_SELECT: setTool(Tool::Select); break;
    case CMD_TOOL_PEN: setTool(Tool::Pen); break;
    case CMD_TOOL_ERASER: setTool(Tool::Eraser); break;
    case CMD_HOME: resetView(); break;
    case CMD_FIT: fitAll(); break;
    case CMD_OVERVIEW: toggleOverview(); break;
    case CMD_ICONS_LARGE: setIconSize(2); break;
    case CMD_ICONS_MEDIUM: setIconSize(1); break;
    case CMD_ICONS_SMALL: setIconSize(0); break;
    case CMD_ALIGN_GRID: setAlignToGrid(!alignToGrid_); break;
    case CMD_SHOW_ICONS: setShowIcons(!showIcons_); break;
    case CMD_EMPTY_HINT: setEmptyHint(!emptyHint_); break;
    case CMD_SORT_NAME: arrangeFiles(SortKey::Name); break;
    case CMD_SORT_SIZE: arrangeFiles(SortKey::Size); break;
    case CMD_SORT_TYPE: arrangeFiles(SortKey::Type); break;
    case CMD_SORT_DATE: arrangeFiles(SortKey::Date); break;
    case CMD_UNDO:
        if (board_.undo()) {
            syncDesktop();
            scheduleSave();
        }
        break;
    case CMD_OPEN_DESKTOP: shell::open(canvas_, folders_.userDesktop()); break;
    case CMD_REFRESH: refresh(); break;
    case CMD_EXIT: quit(); break;
    case CMD_PERSONALIZE:
    case CMD_DISPLAY:
        AllowSetForegroundWindow(ASFW_ANY);
        ShellExecuteW(canvas_, nullptr, cmd == CMD_DISPLAY ? L"ms-settings:display" : L"ms-settings:personalization",
                      nullptr, nullptr, SW_SHOWNORMAL);
        break;
    }
    invalidate();
}

} // namespace dt
