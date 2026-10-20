#include "DesktopHost.h"

#include <knownfolders.h>
#include <shlobj.h>

namespace dt {

DesktopHost DesktopHost::find() {
    DesktopHost h;
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman) {
        h.defView = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
        if (h.defView) h.host = progman;
    }
    if (!h.defView) {
        EnumWindows(
            [](HWND wnd, LPARAM lp) -> BOOL {
                auto* out = reinterpret_cast<DesktopHost*>(lp);
                HWND view = FindWindowExW(wnd, nullptr, L"SHELLDLL_DefView", nullptr);
                if (view) {
                    out->host = wnd;
                    out->defView = view;
                    return FALSE;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&h));
    }
    if (h.defView) h.listView = FindWindowExW(h.defView, nullptr, L"SysListView32", nullptr);
    return h;
}

void DesktopHost::spawnWorkerW() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman) return;
    DWORD_PTR result = 0;
    SendMessageTimeoutW(progman, 0x052C, 0xD, 0x1, SMTO_NORMAL, 1000, &result);
    SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, &result);
}

void DesktopHost::setIconsVisible(const DesktopHost& h, bool visible) {
    if (h.listView && (IsWindowVisible(h.listView) != 0) != visible)
        ShowWindow(h.listView, visible ? SW_SHOWNA : SW_HIDE);
}

DesktopFolders::~DesktopFolders() {
    for (HANDLE h : handles_) FindCloseChangeNotification(h);
}

void DesktopFolders::init() {
    const KNOWNFOLDERID* ids[] = {&FOLDERID_Desktop, &FOLDERID_PublicDesktop};
    for (auto id : ids) {
        PWSTR path = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(*id, KF_FLAG_DEFAULT, nullptr, &path))) {
            dirs_.push_back(path);
            HANDLE h = FindFirstChangeNotificationW(path, FALSE,
                                                    FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME);
            if (h != INVALID_HANDLE_VALUE) handles_.push_back(h);
        }
        CoTaskMemFree(path);
    }
}

bool DesktopFolders::isInDesktop(const std::wstring& path) const {
    std::wstring dir = toLower(dirOf(path));
    for (auto& d : dirs_)
        if (toLower(d) == dir) return true;
    return false;
}

std::vector<std::wstring> DesktopFolders::scan() const {
    std::vector<std::wstring> out;
    for (auto& dir : dirs_) {
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (f == INVALID_HANDLE_VALUE) continue;
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
            out.push_back(dir + L"\\" + fd.cFileName);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    return out;
}

void DesktopFolders::rearm(HANDLE h) { FindNextChangeNotification(h); }

} // namespace dt
