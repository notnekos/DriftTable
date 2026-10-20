#include "Board.h"

#include <cwchar>

namespace dt {

void Item::recomputeStrokeBounds() {
    if (pts.empty()) {
        w = h = 0;
        return;
    }
    float minX = pts[0].x, minY = pts[0].y, maxX = minX, maxY = minY;
    for (auto& p : pts) {
        minX = std::min(minX, p.x);
        minY = std::min(minY, p.y);
        maxX = std::max(maxX, p.x);
        maxY = std::max(maxY, p.y);
    }
    // Re-base the points so that (x, y) is the top-left corner of the bounds.
    for (auto& p : pts) {
        p.x -= minX;
        p.y -= minY;
    }
    x += minX;
    y += minY;
    w = maxX - minX;
    h = maxY - minY;
    geometry.Reset();
}

Item* Board::find(uint64_t id) {
    for (auto& it : items)
        if (it.id == id) return &it;
    return nullptr;
}

int Board::indexOf(uint64_t id) const {
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].id == id) return (int)i;
    return -1;
}

uint64_t Board::add(Item item) {
    item.id = nextId_++;
    items.push_back(std::move(item));
    return items.back().id;
}

void Board::remove(uint64_t id) {
    int i = indexOf(id);
    if (i >= 0) items.erase(items.begin() + i);
}

void Board::bringToFront(uint64_t id) {
    int i = indexOf(id);
    if (i < 0 || i == (int)items.size() - 1) return;
    Item it = std::move(items[i]);
    items.erase(items.begin() + i);
    items.push_back(std::move(it));
}

void Board::clearCaches() {
    for (auto& it : items) {
        it.geometry.Reset();
        it.layout.Reset();
    }
}

bool Board::hitItem(const Item& it, Vec2 p, double tolerance) const {
    if (it.kind != ItemKind::Stroke) return it.rect().contains(p);
    if (!it.rect().inflated(it.strokeWidth / 2 + tolerance).contains(p)) return false;
    Vec2 local{p.x - it.x, p.y - it.y};
    double r = it.strokeWidth / 2 + tolerance;
    if (it.pts.size() == 1) return (local - Vec2(it.pts[0].x, it.pts[0].y)).length() <= r;
    for (size_t i = 1; i < it.pts.size(); ++i) {
        Vec2 a{it.pts[i - 1].x, it.pts[i - 1].y}, b{it.pts[i].x, it.pts[i].y};
        if (distToSegment(local, a, b) <= r) return true;
    }
    return false;
}

int Board::hitTest(Vec2 p, double tolerance) const {
    for (int i = (int)items.size() - 1; i >= 0; --i)
        if (hitItem(items[i], p, tolerance)) return i;
    return -1;
}

RectD Board::contentBounds(bool* any) const {
    *any = false;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    for (auto& it : items) {
        if (!*any) {
            x0 = it.x, y0 = it.y, x1 = it.x + it.w, y1 = it.y + it.h;
            *any = true;
        }
        x0 = std::min(x0, it.x);
        y0 = std::min(y0, it.y);
        x1 = std::max(x1, it.x + it.w);
        y1 = std::max(y1, it.y + it.h);
    }
    return {x0, y0, x1 - x0, y1 - y0};
}

// ---------------------------------------------------------------------------
// Persistence: a line based UTF-8 text file. Every line is
//   <kind> <numbers...> <string to the end of the line>
// ---------------------------------------------------------------------------

namespace {

struct LineReader {
    const std::wstring& s;
    size_t pos = 0;

    std::wstring token() {
        while (pos < s.size() && s[pos] == L' ') ++pos;
        size_t start = pos;
        while (pos < s.size() && s[pos] != L' ') ++pos;
        return s.substr(start, pos - start);
    }
    double num() { return std::wcstod(token().c_str(), nullptr); }
    uint32_t hex() { return (uint32_t)std::wcstoul(token().c_str(), nullptr, 16); }
    std::wstring rest() {
        if (pos < s.size() && s[pos] == L' ') ++pos;
        return unescapeLine(s.substr(std::min(pos, s.size())));
    }
};

std::wstring fmt(double v) {
    wchar_t buf[64];
    swprintf(buf, 64, L"%.2f", v);
    return buf;
}

std::wstring hexColor(uint32_t c) {
    wchar_t buf[16];
    swprintf(buf, 16, L"%08X", c);
    return buf;
}

} // namespace

bool Board::load(const std::wstring& file, CameraState& camera) {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::string data;
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    data.resize((size_t)size.QuadPart);
    DWORD read = 0;
    BOOL ok = ReadFile(h, data.data(), (DWORD)data.size(), &read, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    data.resize(read);

    std::wstring text = fromUtf8(data);
    items.clear();
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty()) continue;

        LineReader r{line};
        std::wstring kind = r.token();
        Item it;
        if (kind == L"camera") {
            camera.x = r.num();
            camera.y = r.num();
            camera.zoom = std::clamp(r.num(), 0.05, 8.0);
            camera.valid = true;
            continue;
        } else if (kind == L"file") {
            it.kind = ItemKind::File;
            it.x = r.num();
            it.y = r.num();
            it.w = kTileW;
            it.h = kTileH;
            it.path = r.rest();
        } else if (kind == L"picture") {
            it.kind = ItemKind::Picture;
            it.x = r.num();
            it.y = r.num();
            it.w = std::max(8.0, r.num());
            it.h = std::max(8.0, r.num());
            it.path = r.rest();
        } else if (kind == L"note") {
            it.kind = ItemKind::Note;
            it.x = r.num();
            it.y = r.num();
            it.w = std::max(40.0, r.num());
            it.h = std::max(30.0, r.num());
            it.color = r.hex();
            it.text = r.rest();
        } else if (kind == L"stroke") {
            it.kind = ItemKind::Stroke;
            it.x = r.num();
            it.y = r.num();
            it.color = r.hex();
            it.strokeWidth = (float)std::max(0.5, r.num());
            size_t n = (size_t)r.num();
            it.pts.reserve(n);
            for (size_t i = 0; i < n; ++i) {
                float px = (float)r.num();
                float py = (float)r.num();
                it.pts.push_back({px, py});
            }
            if (it.pts.empty()) continue;
            it.recomputeStrokeBounds();
        } else {
            continue;
        }
        add(std::move(it));
    }
    return true;
}

bool Board::save(const std::wstring& file, const CameraState& camera) const {
    std::wstring out = L"# DriftTable canvas v1\n";
    out += L"camera " + fmt(camera.x) + L" " + fmt(camera.y) + L" " + fmt(camera.zoom) + L"\n";
    for (auto& it : items) {
        switch (it.kind) {
        case ItemKind::File:
            out += L"file " + fmt(it.x) + L" " + fmt(it.y) + L" " + escapeLine(it.path) + L"\n";
            break;
        case ItemKind::Picture:
            out += L"picture " + fmt(it.x) + L" " + fmt(it.y) + L" " + fmt(it.w) + L" " + fmt(it.h) + L" " +
                   escapeLine(it.path) + L"\n";
            break;
        case ItemKind::Note:
            out += L"note " + fmt(it.x) + L" " + fmt(it.y) + L" " + fmt(it.w) + L" " + fmt(it.h) + L" " +
                   hexColor(it.color) + L" " + escapeLine(it.text) + L"\n";
            break;
        case ItemKind::Stroke: {
            out += L"stroke " + fmt(it.x) + L" " + fmt(it.y) + L" " + hexColor(it.color) + L" " +
                   fmt(it.strokeWidth) + L" " + std::to_wstring(it.pts.size());
            for (auto& p : it.pts) out += L" " + fmt(p.x) + L" " + fmt(p.y);
            out += L"\n";
            break;
        }
        }
    }

    // Write to a temporary file first so a crash never leaves a truncated canvas.
    std::string bytes = toUtf8(out);
    std::wstring tmp = file + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, bytes.data(), (DWORD)bytes.size(), &written, nullptr);
    CloseHandle(h);
    if (!ok || written != bytes.size()) return false;
    return MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

// ---------------------------------------------------------------------------
// Undo / redo: whole-board snapshots. Boards are small, so this is cheap and
// covers every kind of edit uniformly.
// ---------------------------------------------------------------------------

void Board::pushUndo() {
    undo_.push_back(items);
    if (undo_.size() > 64) undo_.erase(undo_.begin());
    redo_.clear();
}

bool Board::undo() {
    if (undo_.empty()) return false;
    redo_.push_back(std::move(items));
    items = std::move(undo_.back());
    undo_.pop_back();
    return true;
}

bool Board::redo() {
    if (redo_.empty()) return false;
    undo_.push_back(std::move(items));
    items = std::move(redo_.back());
    redo_.pop_back();
    return true;
}

} // namespace dt
