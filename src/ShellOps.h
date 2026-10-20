#pragma once

#include "Util.h"

#include <oleidl.h>
#include <shobjidl.h>

#include <functional>
#include <string>
#include <vector>

namespace dt {

namespace shell {

// Opens a file, folder or virtual item ("::{GUID}") like a double click in Explorer.
void open(HWND owner, const std::wstring& path);
void openWith(HWND owner, const std::wstring& path);

struct ExtraMenuItem {
    UINT id = 0; // must be >= kExtraMenuBase
    std::wstring text;
    bool separatorAfter = false;
};
constexpr UINT kExtraMenuBase = 0x8000;

// Shows the native Explorer context menu for the given items, with our own
// entries on top. Returns the chosen extra id, or 0 if the shell handled the
// command (or the menu was dismissed).
UINT contextMenu(HWND owner, const std::vector<std::wstring>& paths, POINT screenPt,
                 const std::vector<ExtraMenuItem>& extra);
// Forwards owner-drawn menu messages to the active context menu (Send to, Open with...).
bool handleMenuMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT* result);

// The desktop background context menu (what Explorer shows on an empty spot of the
// desktop: "New", "Display settings", and items added by apps such as NVIDIA
// Control Panel, VS Code or Git), merged into one of our menus.
class BackgroundMenu {
public:
    // Inserts the shell items at `position`. Returns false if there are none.
    bool insert(HWND owner, HMENU menu, UINT position);
    // Shows the menu. Returns our own command (>= kExtraMenuBase), or 0 when the
    // shell handled the command or the menu was dismissed. *shellCommand tells
    // which of the two happened.
    UINT track(HWND owner, HMENU menu, POINT screenPt, const std::wstring& directory, bool* shellCommand);

private:
    ComPtr<IContextMenu> menu_;
};

enum class FileOp { Move, Copy };
bool transfer(HWND owner, FileOp op, const std::vector<std::wstring>& sources, const std::wstring& destDir);
bool recycle(HWND owner, const std::vector<std::wstring>& paths, bool silent = false);
bool rename(HWND owner, const std::wstring& path, const std::wstring& newName);

// Starts an OLE drag of the given files out of the canvas. Returns the effect.
DWORD dragOut(const std::vector<std::wstring>& paths);

// Starts a program through Explorer, i.e. with Explorer's (normal, not elevated)
// rights. Used to drop administrator rights we were started with by mistake.
bool launchUnelevated(const std::wstring& exe, const std::wstring& args);

// Places files on the clipboard (Ctrl+C) so they can be pasted in Explorer.
bool copyToClipboard(HWND owner, const std::vector<std::wstring>& paths);

} // namespace shell

// OLE drop target for files dragged onto the canvas from Explorer or anything else.
class DropTarget : public IDropTarget {
public:
    // Called while dragging: returns the effect to show for these files at this point.
    std::function<DWORD(const std::vector<std::wstring>&, POINT screen, DWORD keys, DWORD allowed)> onOver;
    std::function<void(const std::vector<std::wstring>&, POINT screen, DWORD effect, DWORD keys)> onDrop;
    std::function<void()> onLeave;

    explicit DropTarget(HWND hwnd);
    virtual ~DropTarget();

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override;
    HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL pt, DWORD* effect) override;
    HRESULT STDMETHODCALLTYPE DragLeave() override;
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override;

private:
    LONG refs_ = 1;
    HWND hwnd_;
    std::vector<std::wstring> files_;
    ComPtr<IDropTargetHelper> helper_;
    DWORD allowed_ = 0;
};

std::vector<std::wstring> filesFromDataObject(IDataObject* data);

} // namespace dt
