#pragma once

#include "Board.h"
#include "DesktopHost.h"
#include "ImageLoader.h"
#include "Renderer.h"
#include "ShellOps.h"
#include "Util.h"

#include <shellapi.h>

#include <deque>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace dt {

enum class HostMode {
    Overlay, // top-level layered window owned by the desktop, kept just above the wallpaper
    Embed,   // child of the desktop icon host (does not render on some Windows 11 builds)
    Window,  // ordinary window, for trying things out and debugging
};

struct Options {
    HostMode mode = HostMode::Overlay;
    bool modeForced = false;
};

enum class Tool { Select, Pen, Eraser };
enum class SortKey { Name, Size, Type, Date };

enum : UINT_PTR {
    TIMER_SAVE = 1,
    TIMER_HOST,
    TIMER_SYNC,
    TIMER_CARET,
};

// Pen palette, shared by the keyboard shortcuts (1-8) and the toolbar.
inline constexpr uint32_t kPenColors[8] = {0xFFFFFFFF, 0xFF1E1E1E, 0xFFFF4D4D, 0xFFFF9F1C,
                                           0xFFFFE14D, 0xFF3DDC84, 0xFF3D8BFF, 0xFFB05CFF};
inline constexpr float kPenWidths[3] = {2.f, 4.f, 9.f};

class App {
public:
    int run(HINSTANCE instance, const Options& options);

private:
    // ---- App.cpp: windows, lifecycle, tray, settings, main loop ----------
    static LRESULT CALLBACK mainProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK canvasProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT onMainMessage(HWND, UINT, WPARAM, LPARAM);
    LRESULT onCanvasMessage(HWND, UINT, WPARAM, LPARAM);
    bool registerClasses();
    bool createCanvas(bool startup);
    void onCanvasDestroyed(HWND hwnd);
    HWND createOverlayWindow(const RECT& r);
    void addSurface(HWND hwnd);
    struct Surface {
        HWND hwnd;
        RECT rect; // in canvas coordinates
        DropTarget* drop;
    };
    const Surface* surfaceOf(HWND hwnd) const;
    bool isOurWindow(HWND hwnd) const;
    void checkHost();
    void placeAtBottom();
    void focusCanvas();
    HWND menuOwner() const;
    void updateBounds();
    void mainLoop();
    void quit();
    void loadSettings();
    void saveSettings();
    void addTrayIcon();
    void showTrayMenu();
    bool autostartEnabled() const;
    void setAutostart(bool on);
    void setIconsHidden(bool hidden);
    RECT primaryWorkArea() const; // in canvas client coordinates

    // ---- AppBoard.cpp: desktop sync, files, pictures, notes ---------------
    void loadBoard();
    void scheduleSave();
    void saveNow();
    void syncDesktop();
    Vec2 findFreeSlot(Vec2 origin, double columnHeight) const;
    void addSystemIcons();
    void onImagesLoaded();
    Image* imageFor(const Item& it);
    int iconPixels() const;
    std::wstring iconKey(const std::wstring& path) const;
    void setIconSize(int size);
    void snapToGrid(const std::vector<uint64_t>& ids);
    void setAlignToGrid(bool on);
    void arrangeFiles(SortKey key);
    void setShowIcons(bool on);
    int hitTest(Vec2 world) const; // like Board::hitTest, skipping hidden desktop icons
    std::wstring labelFor(const Item& it) const;
    DWORD dropEffect(const std::vector<std::wstring>& files, POINT screen, DWORD keys, DWORD allowed);
    void processDrops();
    uint64_t addPicture(const std::wstring& source, Vec2 center, bool copyIntoStore);
    void cleanupPictures();
    void paste(Vec2 world);
    uint64_t addNote(Vec2 world, const std::wstring& text);
    void layoutNote(Item& note, const std::wstring& text);
    void beginEdit(uint64_t id, bool rename);
    void commitEdit(bool cancel = false);
    void deleteSelection();
    std::vector<std::wstring> selectedFilePaths() const;
    std::wstring newPicturePath(const std::wstring& ext) const;

    // ---- AppInput.cpp: camera and interaction ----------------------------
    double scale() const { return zoom_ * dpi_; }
    Vec2 toWorld(Vec2 s) const { return {s.x / scale() + camX_, s.y / scale() + camY_}; }
    Vec2 toScreen(Vec2 w) const { return {(w.x - camX_) * scale(), (w.y - camY_) * scale()}; }
    void zoomAt(Vec2 screen, double factor);
    bool tick(double dt);
    void flyTo(double x, double y, double zoom);
    void resetView(bool animate = true);
    void homeCamera(double* x, double* y) const;
    void refresh();
    void selectOnly(uint64_t id);
    void fitAll();
    void stopMotion();
    void toggleOverview();
    void leaveOverviewAt(Vec2 world);
    void invalidate() { dirty_ = true; }

    void onMouseDown(int button, Vec2 p, bool dbl);
    void onMouseMove(Vec2 p);
    void onMouseUp(int button, Vec2 p);
    void onWheel(int delta, bool horizontal, Vec2 p);
    void onKeyDown(WPARAM vk);
    void onKeyUp(WPARAM vk);
    void onChar(wchar_t c);
    void onDoubleClick(Vec2 p);
    void endDrag(bool cancelled);
    void eraseAt(Vec2 world);
    void startDragOut();
    bool isOutsideCanvas(Vec2 p) const;
    int hudHit(Vec2 p) const;
    void hudAction(int action);
    void showItemMenu(int index, Vec2 p);
    void showCanvasMenu(Vec2 p);
    void runCommand(UINT cmd, Vec2 p);
    HCURSOR cursorFor(Vec2 p) const;
    bool resizeHandleHit(Vec2 p, uint64_t* id) const;
    void setTool(Tool t);
    POINT clientToScreen(Vec2 p) const;
    Vec2 screenToClient(POINT p) const;

    // ---- AppDraw.cpp ---------------------------------------------------
    void render();
    void renderSurface(size_t index);
    bool itemVisible(const Item& it) const { return showIcons_ || it.kind != ItemKind::File; }
    void drawBackground(ID2D1RenderTarget* rt);
    void drawGrid(ID2D1RenderTarget* rt);
    void drawItem(ID2D1RenderTarget* rt, const Item& it, bool selected, bool hovered);
    void drawFile(ID2D1RenderTarget* rt, const Item& it, bool selected, bool hovered);
    void drawPicture(ID2D1RenderTarget* rt, const Item& it, bool selected);
    void drawNote(ID2D1RenderTarget* rt, const Item& it, bool selected);
    void drawStroke(ID2D1RenderTarget* rt, const Item& it, bool selected);
    void drawHud(ID2D1RenderTarget* rt);
    void drawPill(ID2D1RenderTarget* rt, D2D1_RECT_F r, float alpha);
    IDWriteTextLayout* labelLayout(const Item& it, float width, int maxLines);
    IDWriteTextLayout* textLayout(const Item& it, const std::wstring& text, IDWriteTextFormat* fmt, float w,
                                  float h);

    // ---- state ---------------------------------------------------------
    HINSTANCE instance_ = nullptr;
    HostMode mode_ = HostMode::Overlay;
    HostMode savedMode_ = HostMode::Overlay; // from settings.ini; command line overrides don't stick
    bool allowZOrder_ = false;               // overlay: only we may change our z-order
    HWND main_ = nullptr;
    HWND canvas_ = nullptr;           // first surface: owner for dialogs, focus in embed/window mode
    std::vector<Surface> surfaces_;   // one per monitor work area in overlay mode
    HWND inputWnd_ = nullptr;         // surface that got the last mouse message (capture)
    D2D1::Matrix3x2F surfaceBase_ = D2D1::Matrix3x2F::Identity(); // canvas -> current surface
    RECT surfaceRect_{};
    bool anyVisible_ = true;
    DesktopHost host_;
    bool hideIcons_ = true;
    bool quitting_ = false;
    bool elevated_ = false; // running as administrator: no OLE drops from Explorer
    POINT origin_{0, 0}; // canvas client (0,0) in screen coordinates
    int width_ = 0, height_ = 0;
    UINT taskbarCreated_ = 0;
    NOTIFYICONDATAW tray_{};
    bool trayAdded_ = false;

    std::wstring dataDir_, boardFile_, settingsFile_, picturesDir_;
    Renderer renderer_;
    ImageLoader loader_;
    DesktopFolders folders_;
    Board board_;
    bool firstRun_ = false;
    int iconSize_ = 1; // 0 small, 1 medium, 2 large
    bool alignToGrid_ = true;
    bool showIcons_ = true;
    Vec2 newFileAnchor_;            // where files created from the desktop menu appear
    double newFileAnchorUntil_ = 0; // ... until this time (seconds())
    bool savePending_ = false;
    bool syncPending_ = false;

    std::unordered_map<std::wstring, std::shared_ptr<Image>> images_; // key -> image (null = failed)
    std::unordered_set<std::wstring> pendingImages_;
    std::unordered_map<std::wstring, std::wstring> displayNames_; // lower path -> name
    std::unordered_map<std::wstring, Vec2> pendingPos_;           // lower path -> where it should appear

    struct PendingDrop {
        std::vector<std::wstring> files;
        POINT screen;
        DWORD effect, keys;
    };
    std::vector<PendingDrop> drops_;

    // Camera: world point shown at client (0,0) and zoom (1 = 100%).
    double camX_ = 0, camY_ = 0, zoom_ = 1, dpi_ = 1;
    Vec2 velocity_;   // inertia, screen px/s
    Vec2 pendScroll_; // smooth wheel scrolling still to apply, screen px
    double pendZoom_ = 0;
    Vec2 zoomAnchor_;
    double gridAlpha_ = 0, zoomHudAlpha_ = 0;
    struct Flight {
        bool active = false;
        double t = 0;
        double fromX, fromY, fromZoom, toX, toY, toZoom;
    } flight_;
    bool dirty_ = true;

    // Overview: zoomed far out to see the whole canvas; a click flies back in.
    bool overview_ = false;
    double ovX_ = 0, ovY_ = 0, ovZoom_ = 1;
    RectD ovView_; // what was on the main screen before zooming out
    double panTravel_ = 0;

    // Interaction
    enum class Drag { None, Pan, Move, Rubber, Draw, Erase, Resize } drag_ = Drag::None;
    int dragButton_ = 0;
    Vec2 dragStart_, dragLast_, cursor_{-1000, -1000};
    bool dragMoved_ = false, erasedAny_ = false, spaceDown_ = false, mouseTracked_ = false;
    uint64_t pressedId_ = 0, drawingId_ = 0, resizeId_ = 0, hoverId_ = 0;
    RectD resizeOrig_, rubber_;
    std::vector<std::pair<uint64_t, Vec2>> moveOrigins_;
    std::deque<std::pair<double, Vec2>> panSamples_;
    std::set<uint64_t> selected_;

    Tool tool_ = Tool::Select;
    uint32_t penColor_ = 0xFFFF4D4D;
    int penWidthIndex_ = 1;

    uint64_t editId_ = 0;
    bool editRename_ = false;
    std::wstring editText_;
    size_t caret_ = 0;
    bool caretOn_ = true;

    struct HudButton {
        D2D1_RECT_F r;
        int action;
    };
    std::vector<HudButton> hud_;
};

} // namespace dt
