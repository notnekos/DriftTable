#pragma once

#include "Util.h"

#include <d2d1.h>
#include <dwrite.h>

#include <memory>
#include <vector>

namespace dt {

enum class ItemKind { File, Picture, Note, Stroke };

// Size of a file tile in world units (1 unit == 1 DIP at 100% zoom), set by
// View > Large / Medium / Small icons like on the Windows desktop.
struct TileMetrics {
    double w, h, icon;
    double cellW() const { return w + 2; } // grid used for arranging and snapping
    double cellH() const { return h + 4; }
};
TileMetrics& tile();
void setTileSize(int size); // 0 small, 1 medium, 2 large

struct PointF {
    float x, y;
};

struct Item {
    uint64_t id = 0;
    ItemKind kind = ItemKind::File;
    double x = 0, y = 0, w = 0, h = 0;

    // File: full path or shell parsing name ("::{GUID}").
    // Picture: absolute path of the image stored in the pictures folder.
    std::wstring path;
    // Note: its text.
    std::wstring text;
    // Note background / stroke color, 0xAARRGGBB.
    uint32_t color = 0xFFFFF3A6;
    // Stroke width in world units.
    float strokeWidth = 4.f;
    // Stroke points, relative to (x, y).
    std::vector<PointF> pts;

    // Render caches. They are device independent, shared between undo snapshots
    // and dropped whenever the content they were built from changes.
    mutable ComPtr<ID2D1PathGeometry> geometry;
    mutable ComPtr<IDWriteTextLayout> layout;
    mutable std::wstring layoutText;

    RectD rect() const { return {x, y, w, h}; }
    bool isShellItem() const { return kind == ItemKind::File; }
    bool isVirtual() const { return kind == ItemKind::File && path.rfind(L"::", 0) == 0; }
    void recomputeStrokeBounds();
};

struct CameraState {
    double x = 0, y = 0, zoom = 1.0;
    bool valid = false;
};

class Board {
public:
    std::vector<Item> items;

    Item* find(uint64_t id);
    int indexOf(uint64_t id) const;
    uint64_t add(Item item);
    void remove(uint64_t id);
    void bringToFront(uint64_t id);
    void clearCaches();

    // Topmost item under a world point, or -1.
    int hitTest(Vec2 p, double tolerance) const;
    bool hitItem(const Item& it, Vec2 p, double tolerance) const;
    RectD contentBounds(bool* any) const;

    bool load(const std::wstring& file, CameraState& camera);
    bool save(const std::wstring& file, const CameraState& camera) const;

    void pushUndo();
    bool undo();
    bool redo();
    void discardUndo() {
        if (!undo_.empty()) undo_.pop_back();
    }
    bool canUndo() const { return !undo_.empty(); }

private:
    uint64_t nextId_ = 1;
    std::vector<std::vector<Item>> undo_;
    std::vector<std::vector<Item>> redo_;
};

} // namespace dt
