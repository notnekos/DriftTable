#pragma once

#include "Util.h"

#include <string>
#include <vector>

namespace dt {

// Finds the Explorer window that hosts the desktop icons (SHELLDLL_DefView).
// Before Windows 11 24H2 that is a top-level WorkerW; on 24H2+ it is Progman.
// Wallpaper apps (Lively, Wallpaper Engine) draw into another WorkerW that sits
// *below* it, so a window placed above the icon view keeps the wallpaper visible.
struct DesktopHost {
    HWND host = nullptr;     // parent of SHELLDLL_DefView
    HWND defView = nullptr;  // SHELLDLL_DefView
    HWND listView = nullptr; // SysListView32 with the native icons

    static DesktopHost find();
    // Asks Progman to create the WorkerW used for wallpaper transitions. This is
    // exactly what wallpaper engines do, so after it the layout is stable.
    static void spawnWorkerW();
    static void setIconsVisible(const DesktopHost& h, bool visible);
};

// The user's and the public desktop folders, plus change notifications.
class DesktopFolders {
public:
    ~DesktopFolders();
    void init();
    const std::wstring& userDesktop() const { return dirs_.empty() ? empty_ : dirs_[0]; }
    bool isInDesktop(const std::wstring& path) const;
    // Full paths of every visible entry in all desktop folders.
    std::vector<std::wstring> scan() const;

    // Handles signalled when something in a desktop folder changes.
    const std::vector<HANDLE>& handles() const { return handles_; }
    void rearm(HANDLE h);

private:
    std::vector<std::wstring> dirs_;
    std::vector<HANDLE> handles_;
    std::wstring empty_;
};

} // namespace dt
