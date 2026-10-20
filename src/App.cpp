#include "App.h"

#include <dwmapi.h>
#include <shlobj.h>

namespace dt {

namespace {

constexpr wchar_t kMainClass[] = L"DriftTable.Main";
constexpr wchar_t kCanvasClass[] = L"DriftTable.Canvas";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

enum : UINT {
    WM_APP_IMAGES = WM_APP + 1,
    WM_APP_TRAY,
    WM_APP_DROP,
};

enum : UINT {
    TRAY_RESET = 1,
    TRAY_FIT,
    TRAY_OVERVIEW,
    TRAY_PEN,
    TRAY_HIDE_ICONS,
    TRAY_SYSTEM_ICONS,
    TRAY_AUTOSTART,
    TRAY_OPEN_DATA,
    TRAY_EXIT,
};

App* g_app = nullptr;
HWND g_listView = nullptr; // for restoring desktop icons if we crash

LONG WINAPI crashFilter(EXCEPTION_POINTERS*) {
    if (g_listView) ShowWindow(g_listView, SW_SHOWNA);
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

int App::run(HINSTANCE instance, const Options& options) {
    g_app = this;
    instance_ = instance;

    PWSTR appData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &appData)))
        dataDir_ = std::wstring(appData) + L"\\DriftTable";
    CoTaskMemFree(appData);
    picturesDir_ = dataDir_ + L"\\pictures";
    boardFile_ = dataDir_ + L"\\canvas.txt";
    settingsFile_ = dataDir_ + L"\\settings.ini";
    SHCreateDirectoryExW(nullptr, picturesDir_.c_str(), nullptr);
    logPath() = dataDir_ + L"\\log.txt";
    DeleteFileW(logPath().c_str());
    {
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        OSVERSIONINFOW v{};
        v.dwOSVersionInfoSize = sizeof(v);
        if (auto fn = (RtlGetVersionFn)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")) fn(&v);
        logf(L"DriftTable 0.2, Windows %lu.%lu build %lu", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    }

    loadSettings();
    if (options.modeForced) mode_ = options.mode;

    if (!renderer_.init()) {
        MessageBoxW(nullptr, L"Не удалось инициализировать Direct2D.", L"DriftTable", MB_ICONERROR);
        return 1;
    }
    if (!registerClasses()) return 1;

    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    // The main window also takes keyboard focus for the overlay canvas, which must
    // never be activated itself (it would jump above other windows). It is a 1x1
    // fully transparent tool window, so activating it changes nothing on screen.
    main_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT, kMainClass, L"DriftTable", WS_POPUP,
                            GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 1, 1, nullptr,
                            nullptr, instance_, nullptr);
    if (!main_) return 1;
    SetLayeredWindowAttributes(main_, 0, 0, LWA_ALPHA);
    ShowWindow(main_, SW_SHOWNA);

    HDC screen = GetDC(nullptr);
    dpi_ = GetDeviceCaps(screen, LOGPIXELSX) / 96.0;
    ReleaseDC(nullptr, screen);

    SetUnhandledExceptionFilter(crashFilter);
    folders_.init();
    loader_.start(main_, WM_APP_IMAGES);

    if (mode_ != HostMode::Window) DesktopHost::spawnWorkerW();
    if (!createCanvas(true)) {
        MessageBoxW(nullptr, L"Не удалось создать окно холста.", L"DriftTable", MB_ICONERROR);
        return 1;
    }

    loadBoard();
    syncDesktop();
    cleanupPictures();
    addTrayIcon();
    SetTimer(main_, TIMER_HOST, 2000, nullptr);

    mainLoop();

    saveNow();
    saveSettings();
    loader_.stop();
    if (trayAdded_) Shell_NotifyIconW(NIM_DELETE, &tray_);
    if (host_.listView) DesktopHost::setIconsVisible(host_, true);
    return 0;
}

bool App::registerClasses() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = mainProc;
    wc.hInstance = instance_;
    wc.lpszClassName = kMainClass;
    if (!RegisterClassExW(&wc)) return false;

    WNDCLASSEXW cc{};
    cc.cbSize = sizeof(cc);
    cc.style = CS_DBLCLKS;
    cc.lpfnWndProc = canvasProc;
    cc.hInstance = instance_;
    cc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(1));
    cc.lpszClassName = kCanvasClass;
    return RegisterClassExW(&cc) != 0;
}

namespace {

// One overlay window per monitor work area: it never overlaps a taskbar and never
// covers a whole monitor, which Windows would treat as a full-screen app (and then
// drop the taskbar behind it).
std::vector<RECT> workAreas() {
    std::vector<RECT> out;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL {
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(m, &mi)) {
                RECT r = mi.rcWork;
                if (EqualRect(&r, &mi.rcMonitor)) r.bottom -= 1; // auto-hidden taskbar
                reinterpret_cast<std::vector<RECT>*>(lp)->push_back(r);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&out));
    return out;
}

} // namespace

bool App::createCanvas(bool startup) {
    if (mode_ == HostMode::Embed || mode_ == HostMode::Overlay) {
        host_ = DesktopHost::find();
        g_listView = host_.listView;
        if (!host_.host) {
            // No Explorer desktop (another shell, or Explorer is not running yet):
            // an unowned overlay at the bottom still works.
            if (!startup) return false;
            mode_ = HostMode::Overlay;
        }
    }

    {
        wchar_t cls[64] = L"-";
        if (host_.host) GetClassNameW(host_.host, cls, 64);
        logf(L"mode=%s host=%p (%s) defView=%p listView=%p",
             mode_ == HostMode::Embed ? L"embed" : mode_ == HostMode::Overlay ? L"overlay" : L"window",
             (void*)host_.host, cls, (void*)host_.defView, (void*)host_.listView);
    }
    if (mode_ == HostMode::Embed) {
        RECT rc;
        GetClientRect(host_.host, &rc);
        // A layered *child* window: needs Windows 8+ (declared in the manifest).
        HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOPARENTNOTIFY, kCanvasClass, L"DriftTable",
                                    WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, rc.right, rc.bottom, host_.host,
                                    nullptr, instance_, nullptr);
        if (hwnd) {
            SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            addSurface(hwnd);
        } else {
            mode_ = HostMode::Overlay; // fall back
        }
    }
    if (mode_ == HostMode::Window) {
        HWND hwnd = CreateWindowExW(0, kCanvasClass, L"DriftTable — окно", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                    CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800, nullptr, nullptr, instance_, nullptr);
        if (!hwnd) return false;
        addSurface(hwnd);
    }
    updateBounds(); // creates the overlay windows, one per monitor

    if (surfaces_.empty()) {
        if (!startup || mode_ != HostMode::Overlay) return false;
        mode_ = HostMode::Window;
        return createCanvas(startup);
    }
    if (hideIcons_) DesktopHost::setIconsVisible(host_, false);
    return true;
}

HWND App::createOverlayWindow(const RECT& r) {
    // Owned by the desktop: survives Win+D and never goes below the wallpaper.
    HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kCanvasClass, L"DriftTable",
                                WS_POPUP, r.left, r.top, r.right - r.left, r.bottom - r.top, host_.host, nullptr,
                                instance_, nullptr);
    if (hwnd) addSurface(hwnd);
    return hwnd;
}

void App::addSurface(HWND hwnd) {
    Surface s{hwnd, {}, new DropTarget(hwnd)};
    s.drop->onOver = [this](const std::vector<std::wstring>& f, POINT pt, DWORD keys, DWORD allowed) {
        return dropEffect(f, pt, keys, allowed);
    };
    s.drop->onDrop = [this](const std::vector<std::wstring>& f, POINT pt, DWORD effect, DWORD keys) {
        // Do the actual work after the OLE call returns, so the drag source is
        // never blocked by our file operation dialogs.
        drops_.push_back({f, pt, effect, keys});
        PostMessageW(main_, WM_APP_DROP, 0, 0);
    };
    RegisterDragDrop(hwnd, s.drop);
    surfaces_.push_back(s);
    canvas_ = surfaces_[0].hwnd;
    logf(L"canvas window %p", (void*)hwnd);
}

const App::Surface* App::surfaceOf(HWND hwnd) const {
    for (auto& s : surfaces_)
        if (s.hwnd == hwnd) return &s;
    return nullptr;
}

bool App::isOurWindow(HWND hwnd) const {
    if (!hwnd) return false;
    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (root == main_) return true;
    for (auto& s : surfaces_)
        if (s.hwnd == hwnd || GetAncestor(s.hwnd, GA_ROOT) == root) return true;
    return false;
}

// Overlay: directly above the desktop window, i.e. below every normal window.
void App::placeAtBottom() {
    if (mode_ != HostMode::Overlay) return;
    for (auto& s : surfaces_) {
        HWND after = HWND_BOTTOM;
        if (host_.host) {
            HWND above = GetWindow(host_.host, GW_HWNDPREV);
            while (above && surfaceOf(above)) above = GetWindow(above, GW_HWNDPREV);
            if (above == nullptr) after = HWND_TOP;
            // Inserting under a topmost window would make us topmost (above the taskbar).
            else after = (GetWindowLongW(above, GWL_EXSTYLE) & WS_EX_TOPMOST) ? HWND_NOTOPMOST : above;
        }
        allowZOrder_ = true;
        SetWindowPos(s.hwnd, after, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (GetWindowLongW(s.hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST)
            SetWindowPos(s.hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        allowZOrder_ = false;
    }
}

void App::focusCanvas() {
    if (mode_ == HostMode::Overlay) {
        if (GetForegroundWindow() != main_) SetForegroundWindow(main_);
        if (GetFocus() != main_) SetFocus(main_);
    } else if (canvas_ && GetFocus() != canvas_) {
        SetFocus(canvas_);
    }
}

HWND App::menuOwner() const { return mode_ == HostMode::Overlay ? main_ : canvas_; }

void App::onCanvasDestroyed(HWND hwnd) {
    for (size_t i = 0; i < surfaces_.size(); ++i) {
        if (surfaces_[i].hwnd != hwnd) continue;
        surfaces_[i].drop->Release();
        surfaces_.erase(surfaces_.begin() + i);
        break;
    }
    canvas_ = surfaces_.empty() ? nullptr : surfaces_[0].hwnd;
    if (drag_ != Drag::None) drag_ = Drag::None;
    renderer_.setSurfaceCount(surfaces_.size());
    for (size_t i = 0; i < surfaces_.size(); ++i)
        renderer_.resize(i, surfaces_[i].rect.right - surfaces_[i].rect.left,
                         surfaces_[i].rect.bottom - surfaces_[i].rect.top);
    invalidate();
}

// Explorer can restart, and wallpaper engines can move the icon view into a new
// WorkerW at any time. Re-attach whenever that happens.
void App::checkHost() {
    if (mode_ == HostMode::Window || quitting_) return;
    DesktopHost h = DesktopHost::find();
    if (!h.host) return;
    bool dead = surfaces_.empty();
    for (auto& s : surfaces_) dead |= !IsWindow(s.hwnd);
    if (dead) {
        // The windows died together with the old desktop (owned / child windows).
        while (!surfaces_.empty()) {
            HWND hwnd = surfaces_.back().hwnd;
            if (IsWindow(hwnd)) DestroyWindow(hwnd);
            else onCanvasDestroyed(hwnd);
        }
        createCanvas(false);
        return;
    }
    bool changed = h.host != host_.host;
    host_ = h;
    g_listView = h.listView;
    if (mode_ == HostMode::Embed) {
        if (GetParent(canvas_) != h.host) SetParent(canvas_, h.host);
        if (GetWindow(h.host, GW_CHILD) != canvas_)
            SetWindowPos(canvas_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else if (changed) {
        for (auto& s : surfaces_) SetWindowLongPtrW(s.hwnd, GWLP_HWNDPARENT, (LONG_PTR)h.host);
        placeAtBottom();
    }
    if (hideIcons_) DesktopHost::setIconsVisible(host_, false);
    updateBounds(); // cheap when nothing moved
}

void App::updateBounds() {
    int oldX = origin_.x, oldY = origin_.y;
    bool changed = false;
    auto setRect = [&](Surface& s, RECT r) {
        if (!EqualRect(&s.rect, &r)) changed = true;
        s.rect = r;
    };
    if (mode_ == HostMode::Embed) {
        if (surfaces_.empty()) return;
        RECT rc;
        GetClientRect(host_.host, &rc);
        width_ = rc.right;
        height_ = rc.bottom;
        origin_ = {0, 0};
        ClientToScreen(host_.host, &origin_);
        if (!EqualRect(&surfaces_[0].rect, &rc))
            SetWindowPos(canvas_, nullptr, 0, 0, width_, height_, SWP_NOZORDER | SWP_NOACTIVATE);
        setRect(surfaces_[0], rc);
    } else if (mode_ == HostMode::Overlay) {
        origin_ = {GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN)};
        width_ = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        height_ = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        std::vector<RECT> areas = workAreas();
        while (surfaces_.size() > areas.size()) {
            HWND hwnd = surfaces_.back().hwnd;
            onCanvasDestroyed(hwnd);
            DestroyWindow(hwnd);
            changed = true;
        }
        bool added = false;
        while (surfaces_.size() < areas.size()) {
            if (!createOverlayWindow(areas[surfaces_.size()])) break;
            added = changed = true;
        }
        for (size_t i = 0; i < surfaces_.size(); ++i) {
            RECT a = areas[i];
            RECT local = a;
            OffsetRect(&local, -origin_.x, -origin_.y);
            if (!EqualRect(&surfaces_[i].rect, &local))
                SetWindowPos(surfaces_[i].hwnd, nullptr, a.left, a.top, a.right - a.left, a.bottom - a.top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            setRect(surfaces_[i], local);
        }
        if (added) placeAtBottom();
    } else {
        if (surfaces_.empty()) return;
        RECT rc;
        GetClientRect(canvas_, &rc);
        width_ = rc.right;
        height_ = rc.bottom;
        origin_ = {0, 0};
        ClientToScreen(canvas_, &origin_);
        setRect(surfaces_[0], rc);
    }
    if (!changed && origin_.x == oldX && origin_.y == oldY) return;

    // Keep the content where it was on screen when the canvas origin moves.
    if (mode_ != HostMode::Window) {
        camX_ += (origin_.x - oldX) / scale();
        camY_ += (origin_.y - oldY) / scale();
    }
    renderer_.setSurfaceCount(surfaces_.size());
    for (size_t i = 0; i < surfaces_.size(); ++i) {
        const RECT& r = surfaces_[i].rect;
        renderer_.resize(i, r.right - r.left, r.bottom - r.top);
        logf(L"surface %u: %d,%d %dx%d", (unsigned)i, r.left + origin_.x, r.top + origin_.y, r.right - r.left,
             r.bottom - r.top);
    }
    invalidate();
}

RECT App::primaryWorkArea() const {
    if (mode_ == HostMode::Window) return {0, 0, width_, height_};
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    RECT r = mi.rcWork;
    OffsetRect(&r, -origin_.x, -origin_.y);
    return r;
}

void App::mainLoop() {
    double last = seconds();
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        double now = seconds();
        bool animating = tick(std::min(now - last, 0.1));
        last = now;
        if (dirty_ && canvas_) render();

        const auto& handles = folders_.handles();
        DWORD n = (DWORD)handles.size();
        DWORD r;
        if (animating) {
            // Pace animation to the compositor; fall back to a short sleep.
            double before = seconds();
            if (FAILED(DwmFlush()) || seconds() - before < 0.002) Sleep(8);
            r = n ? WaitForMultipleObjects(n, handles.data(), FALSE, 0) : WAIT_TIMEOUT;
        } else {
            r = MsgWaitForMultipleObjectsEx(n, handles.data(), INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            last = seconds();
        }
        if (r >= WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + n) {
            folders_.rearm(handles[r - WAIT_OBJECT_0]);
            // Debounce: a copy of many files fires lots of notifications.
            SetTimer(main_, TIMER_SYNC, 250, nullptr);
        }
    }
}

void App::quit() {
    quitting_ = true;
    commitEdit();
    while (!surfaces_.empty()) {
        HWND hwnd = surfaces_.back().hwnd;
        if (!IsWindow(hwnd) || !DestroyWindow(hwnd)) onCanvasDestroyed(hwnd);
    }
    PostQuitMessage(0);
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

void App::loadSettings() {
    wchar_t buf[64];
    GetPrivateProfileStringW(L"DriftTable", L"HostMode", L"overlay", buf, 64, settingsFile_.c_str());
    std::wstring m = toLower(buf);
    savedMode_ = m == L"embed" ? HostMode::Embed : m == L"window" ? HostMode::Window : HostMode::Overlay;
    mode_ = savedMode_;
    hideIcons_ = GetPrivateProfileIntW(L"DriftTable", L"HideDesktopIcons", 1, settingsFile_.c_str()) != 0;
    GetPrivateProfileStringW(L"DriftTable", L"PenColor", L"FFFF4D4D", buf, 64, settingsFile_.c_str());
    penColor_ = (uint32_t)wcstoul(buf, nullptr, 16) | 0xFF000000;
    penWidthIndex_ = std::clamp((int)GetPrivateProfileIntW(L"DriftTable", L"PenWidth", 1, settingsFile_.c_str()), 0, 2);
    iconSize_ = std::clamp((int)GetPrivateProfileIntW(L"DriftTable", L"IconSize", 1, settingsFile_.c_str()), 0, 2);
    setTileSize(iconSize_);
    alignToGrid_ = GetPrivateProfileIntW(L"DriftTable", L"AlignToGrid", 1, settingsFile_.c_str()) != 0;
    showIcons_ = GetPrivateProfileIntW(L"DriftTable", L"ShowIcons", 1, settingsFile_.c_str()) != 0;
}

void App::saveSettings() {
    const wchar_t* m =
        savedMode_ == HostMode::Overlay ? L"overlay" : savedMode_ == HostMode::Window ? L"window" : L"embed";
    WritePrivateProfileStringW(L"DriftTable", L"HostMode", m, settingsFile_.c_str());
    WritePrivateProfileStringW(L"DriftTable", L"Mode", nullptr, settingsFile_.c_str()); // 0.1 key, now unused
    WritePrivateProfileStringW(L"DriftTable", L"HideDesktopIcons", hideIcons_ ? L"1" : L"0", settingsFile_.c_str());
    wchar_t buf[16];
    swprintf(buf, 16, L"%08X", penColor_);
    WritePrivateProfileStringW(L"DriftTable", L"PenColor", buf, settingsFile_.c_str());
    WritePrivateProfileStringW(L"DriftTable", L"PenWidth", std::to_wstring(penWidthIndex_).c_str(),
                               settingsFile_.c_str());
    WritePrivateProfileStringW(L"DriftTable", L"IconSize", std::to_wstring(iconSize_).c_str(), settingsFile_.c_str());
    WritePrivateProfileStringW(L"DriftTable", L"AlignToGrid", alignToGrid_ ? L"1" : L"0", settingsFile_.c_str());
    WritePrivateProfileStringW(L"DriftTable", L"ShowIcons", showIcons_ ? L"1" : L"0", settingsFile_.c_str());
}

bool App::autostartEnabled() const {
    wchar_t buf[MAX_PATH * 2];
    DWORD size = sizeof(buf);
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"DriftTable", RRF_RT_REG_SZ, nullptr, buf, &size) ==
           ERROR_SUCCESS;
}

void App::setAutostart(bool on) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
        RegSetValueExW(key, L"DriftTable", 0, REG_SZ, (const BYTE*)cmd.c_str(),
                       (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, L"DriftTable");
    }
    RegCloseKey(key);
}

void App::setIconsHidden(bool hidden) {
    hideIcons_ = hidden;
    DesktopHost::setIconsVisible(host_, !hidden);
    saveSettings();
}

// ---------------------------------------------------------------------------
// Tray
// ---------------------------------------------------------------------------

void App::addTrayIcon() {
    tray_ = {};
    tray_.cbSize = sizeof(tray_);
    tray_.hWnd = main_;
    tray_.uID = 1;
    tray_.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    tray_.uCallbackMessage = WM_APP_TRAY;
    tray_.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(1));
    if (!tray_.hIcon) tray_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(tray_.szTip, L"DriftTable — бесконечный рабочий стол");
    trayAdded_ = Shell_NotifyIconW(NIM_ADD, &tray_) != 0;
    tray_.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &tray_);
}

void App::showTrayMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, TRAY_RESET, L"Вернуться к началу\tHome");
    AppendMenuW(m, MF_STRING | (overview_ ? MF_CHECKED : 0), TRAY_OVERVIEW, L"Обзор (отдалить)\tTab");
    AppendMenuW(m, MF_STRING, TRAY_FIT, L"Показать всё\tF");
    AppendMenuW(m, MF_STRING | (tool_ == Tool::Pen ? MF_CHECKED : 0), TRAY_PEN, L"Рисовать\tB");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    if (mode_ != HostMode::Window)
        AppendMenuW(m, MF_STRING | (hideIcons_ ? MF_CHECKED : 0), TRAY_HIDE_ICONS, L"Скрывать значки Windows");
    AppendMenuW(m, MF_STRING, TRAY_SYSTEM_ICONS, L"Вернуть «Корзину» и «Этот компьютер»");
    AppendMenuW(m, MF_STRING | (autostartEnabled() ? MF_CHECKED : 0), TRAY_AUTOSTART,
                L"Запускать вместе с Windows");
    AppendMenuW(m, MF_STRING, TRAY_OPEN_DATA, L"Открыть папку данных");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, TRAY_EXIT, L"Выход");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(main_);
    UINT cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, main_, nullptr);
    PostMessageW(main_, WM_NULL, 0, 0);
    DestroyMenu(m);

    switch (cmd) {
    case TRAY_RESET: resetView(); break;
    case TRAY_FIT: fitAll(); break;
    case TRAY_OVERVIEW: toggleOverview(); break;
    case TRAY_PEN: setTool(tool_ == Tool::Pen ? Tool::Select : Tool::Pen); break;
    case TRAY_HIDE_ICONS: setIconsHidden(!hideIcons_); break;
    case TRAY_SYSTEM_ICONS: addSystemIcons(); break;
    case TRAY_AUTOSTART: setAutostart(!autostartEnabled()); break;
    case TRAY_OPEN_DATA: shell::open(main_, dataDir_); break;
    case TRAY_EXIT: quit(); break;
    }
}

// ---------------------------------------------------------------------------
// Window procedures
// ---------------------------------------------------------------------------

LRESULT CALLBACK App::mainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return g_app ? g_app->onMainMessage(hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK App::canvasProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return g_app ? g_app->onCanvasMessage(hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT App::onMainMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT menuResult;
    if (shell::handleMenuMessage(msg, wp, lp, &menuResult)) return menuResult;

    if (msg == taskbarCreated_ && taskbarCreated_) {
        // Explorer restarted: our canvas died together with the old desktop.
        addTrayIcon();
        checkHost();
        return 0;
    }
    switch (msg) {
    case WM_APP_IMAGES: onImagesLoaded(); return 0;
    case WM_APP_DROP: processDrops(); return 0;
    case WM_APP_TRAY:
        switch (LOWORD(lp)) {
        case WM_CONTEXTMENU:
        case WM_RBUTTONUP: showTrayMenu(); break;
        case WM_LBUTTONDBLCLK:
        case NIN_SELECT: resetView(); break;
        }
        return 0;
    case WM_TIMER:
        switch (wp) {
        case TIMER_SAVE:
            KillTimer(hwnd, TIMER_SAVE);
            saveNow();
            break;
        case TIMER_HOST: checkHost(); break;
        case TIMER_SYNC:
            KillTimer(hwnd, TIMER_SYNC);
            syncDesktop();
            break;
        case TIMER_CARET:
            caretOn_ = !caretOn_;
            invalidate();
            break;
        }
        return 0;
    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
        updateBounds();
        return 0;
    // Keyboard focus of the overlay canvas lives here (see focusCanvas).
    case WM_KEYDOWN: onKeyDown(wp); return 0;
    case WM_KEYUP: onKeyUp(wp); return 0;
    case WM_CHAR: onChar((wchar_t)wp); return 0;
    case WM_KILLFOCUS: spaceDown_ = false; return 0;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        POINT sp{(short)LOWORD(lp), (short)HIWORD(lp)};
        onWheel(GET_WHEEL_DELTA_WPARAM(wp), msg == WM_MOUSEHWHEEL, screenToClient(sp));
        return 0;
    }
    case WM_QUERYENDSESSION: return TRUE;
    case WM_ENDSESSION:
        if (wp) {
            saveNow();
            if (host_.listView) DesktopHost::setIconsVisible(host_, true);
        }
        return 0;
    case WM_CLOSE: return 0; // Alt+F4 while the canvas has focus: not a reason to quit
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT App::onCanvasMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT menuResult;
    if (shell::handleMenuMessage(msg, wp, lp, &menuResult)) return menuResult;

    // Mouse coordinates are per window; the app works in canvas coordinates,
    // which span every surface (the virtual screen in overlay mode).
    const Surface* surface = surfaceOf(hwnd);
    double ox = surface ? surface->rect.left : 0, oy = surface ? surface->rect.top : 0;
    auto pt = [&] { return Vec2((short)LOWORD(lp) + ox, (short)HIWORD(lp) + oy); };
    if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) inputWnd_ = hwnd;
    switch (msg) {
    case WM_LBUTTONDOWN: onMouseDown(0, pt(), false); return 0;
    case WM_LBUTTONDBLCLK: onMouseDown(0, pt(), true); return 0;
    case WM_MBUTTONDOWN: onMouseDown(1, pt(), false); return 0;
    case WM_MBUTTONDBLCLK:
        // Double click with the middle button (without dragging): overview.
        if (panTravel_ < 6 && drag_ == Drag::None) toggleOverview();
        else onMouseDown(1, pt(), false);
        return 0;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK: onMouseDown(2, pt(), false); return 0;
    case WM_LBUTTONUP: onMouseUp(0, pt()); return 0;
    case WM_MBUTTONUP: onMouseUp(1, pt()); return 0;
    case WM_RBUTTONUP: onMouseUp(2, pt()); return 0;
    case WM_MOUSEMOVE:
        if (!mouseTracked_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            mouseTracked_ = TrackMouseEvent(&tme) != 0;
        }
        onMouseMove(pt());
        return 0;
    case WM_MOUSELEAVE:
        mouseTracked_ = false;
        if (hoverId_) {
            hoverId_ = 0;
            invalidate();
        }
        cursor_ = {-1000, -1000};
        if (tool_ != Tool::Select) invalidate();
        return 0;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        POINT sp{(short)LOWORD(lp), (short)HIWORD(lp)};
        onWheel(GET_WHEEL_DELTA_WPARAM(wp), msg == WM_MOUSEHWHEEL, screenToClient(sp));
        return 0;
    }
    case WM_CAPTURECHANGED:
        if ((HWND)lp != hwnd && drag_ != Drag::None) endDrag(true);
        return 0;
    case WM_KEYDOWN: onKeyDown(wp); return 0;
    case WM_KEYUP: onKeyUp(wp); return 0;
    case WM_CHAR: onChar((wchar_t)wp); return 0;
    case WM_KILLFOCUS:
        spaceDown_ = false;
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p;
            GetCursorPos(&p);
            SetCursor(cursorFor(screenToClient(p)));
            return TRUE;
        }
        break;
    case WM_MOUSEACTIVATE:
        // The overlay window must not jump above other windows when clicked.
        if (mode_ == HostMode::Overlay) return MA_NOACTIVATE;
        break;
    case WM_WINDOWPOSCHANGING:
        if (mode_ == HostMode::Overlay && !allowZOrder_) reinterpret_cast<WINDOWPOS*>(lp)->flags |= SWP_NOZORDER;
        break;
    case WM_SIZE:
        if (mode_ == HostMode::Window && canvas_) updateBounds();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        invalidate();
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_CLOSE:
        if (mode_ == HostMode::Window) quit();
        return 0;
    case WM_DESTROY:
        RevokeDragDrop(hwnd);
        onCanvasDestroyed(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

POINT App::clientToScreen(Vec2 p) const { return {(LONG)p.x + origin_.x, (LONG)p.y + origin_.y}; }

Vec2 App::screenToClient(POINT p) const { return {double(p.x - origin_.x), double(p.y - origin_.y)}; }

} // namespace dt
