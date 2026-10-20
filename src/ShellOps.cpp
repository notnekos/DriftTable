#include "ShellOps.h"

#include <shellapi.h>
#include <shlobj.h>

namespace dt {

namespace {

ComPtr<IContextMenu2> g_menu2;
ComPtr<IContextMenu3> g_menu3;

struct Pidl {
    PIDLIST_ABSOLUTE p = nullptr;
    Pidl() = default;
    Pidl(const Pidl&) = delete;
    Pidl(Pidl&& o) noexcept : p(o.p) { o.p = nullptr; }
    ~Pidl() {
        if (p) CoTaskMemFree(p);
    }
};

bool parse(const std::wstring& path, Pidl& out) {
    return SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &out.p, 0, nullptr));
}

std::wstring doubleNull(const std::vector<std::wstring>& list) {
    std::wstring s;
    for (auto& p : list) {
        s += p;
        s.push_back(L'\0');
    }
    s.push_back(L'\0');
    return s;
}

ComPtr<IDataObject> dataObjectFor(const std::vector<std::wstring>& paths) {
    std::vector<Pidl> pidls;
    std::vector<PCIDLIST_ABSOLUTE> raw;
    for (auto& path : paths) {
        Pidl p;
        if (parse(path, p)) {
            raw.push_back(p.p);
            pidls.push_back(std::move(p));
        }
    }
    if (raw.empty()) return nullptr;
    ComPtr<IShellItemArray> items;
    if (FAILED(SHCreateShellItemArrayFromIDLists((UINT)raw.size(), raw.data(), &items))) return nullptr;
    ComPtr<IDataObject> data;
    items->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data));
    return data;
}

} // namespace

namespace shell {

void open(HWND owner, const std::wstring& path) {
    Pidl pidl;
    if (!parse(path, pidl)) return;
    AllowSetForegroundWindow(ASFW_ANY);
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_INVOKEIDLIST | SEE_MASK_ASYNCOK;
    sei.hwnd = owner;
    sei.lpIDList = pidl.p;
    sei.nShow = SW_SHOWNORMAL;
    ShellExecuteExW(&sei);
}

void openWith(HWND owner, const std::wstring& path) {
    OPENASINFO info{};
    info.pcszFile = path.c_str();
    info.oaifInFlags = OAIF_ALLOW_REGISTRATION | OAIF_EXEC;
    SHOpenWithDialog(owner, &info);
}

UINT contextMenu(HWND owner, const std::vector<std::wstring>& paths, POINT screenPt,
                 const std::vector<ExtraMenuItem>& extra) {
    if (paths.empty()) return 0;

    // A shell context menu can only span items of one folder.
    std::vector<std::wstring> use;
    std::wstring dir = toLower(dirOf(paths[0]));
    for (auto& p : paths)
        if (!dir.empty() && toLower(dirOf(p)) == dir) use.push_back(p);
    if (use.empty()) use.push_back(paths[0]);

    std::vector<Pidl> pidls;
    std::vector<PCUITEMID_CHILD> children;
    ComPtr<IShellFolder> folder;
    for (auto& path : use) {
        Pidl p;
        if (!parse(path, p)) continue;
        PCUITEMID_CHILD child = nullptr;
        ComPtr<IShellFolder> parent;
        if (FAILED(SHBindToParent(p.p, IID_PPV_ARGS(&parent), &child))) continue;
        if (!folder) folder = parent;
        children.push_back(child);
        pidls.push_back(std::move(p));
    }

    HMENU menu = CreatePopupMenu();
    UINT pos = 0;
    for (auto& e : extra) {
        InsertMenuW(menu, pos++, MF_BYPOSITION | MF_STRING, e.id, e.text.c_str());
        if (e.separatorAfter) InsertMenuW(menu, pos++, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
    }

    ComPtr<IContextMenu> cm;
    if (folder && !children.empty() &&
        SUCCEEDED(folder->GetUIObjectOf(owner, (UINT)children.size(), children.data(), IID_IContextMenu, nullptr,
                                        reinterpret_cast<void**>(cm.GetAddressOf())))) {
        if (!extra.empty()) InsertMenuW(menu, pos++, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
        UINT flags = CMF_NORMAL;
        if (GetKeyState(VK_SHIFT) < 0) flags |= CMF_EXTENDEDVERBS;
        cm->QueryContextMenu(menu, pos, 1, kExtraMenuBase - 1, flags);
        cm.As(&g_menu2);
        cm.As(&g_menu3);
    }

    UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPt.x, screenPt.y, owner, nullptr);
    g_menu2.Reset();
    g_menu3.Reset();

    UINT result = 0;
    if (cmd >= kExtraMenuBase) {
        result = cmd;
    } else if (cmd > 0 && cm) {
        CMINVOKECOMMANDINFOEX info{};
        info.cbSize = sizeof(info);
        info.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE | CMIC_MASK_ASYNCOK;
        if (GetKeyState(VK_CONTROL) < 0) info.fMask |= CMIC_MASK_CONTROL_DOWN;
        if (GetKeyState(VK_SHIFT) < 0) info.fMask |= CMIC_MASK_SHIFT_DOWN;
        info.hwnd = owner;
        info.lpVerb = MAKEINTRESOURCEA(cmd - 1);
        info.lpVerbW = MAKEINTRESOURCEW(cmd - 1);
        info.nShow = SW_SHOWNORMAL;
        info.ptInvoke = screenPt;
        AllowSetForegroundWindow(ASFW_ANY);
        cm->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&info));
    }
    DestroyMenu(menu);
    return result;
}

bool BackgroundMenu::insert(HWND owner, HMENU menu, UINT position) {
    ComPtr<IShellFolder> desktop;
    if (FAILED(SHGetDesktopFolder(&desktop))) return false;
    if (FAILED(desktop->CreateViewObject(owner, IID_IContextMenu, reinterpret_cast<void**>(menu_.GetAddressOf()))))
        return false;
    int before = GetMenuItemCount(menu);
    UINT flags = CMF_NORMAL | CMF_NODEFAULT;
    if (GetKeyState(VK_SHIFT) < 0) flags |= CMF_EXTENDEDVERBS;
    if (FAILED(menu_->QueryContextMenu(menu, position, 1, kExtraMenuBase - 1, flags))) {
        menu_.Reset();
        return false;
    }
    // We have our own Paste (it also pastes pictures and text) and View/Sort/Refresh.
    for (int i = GetMenuItemCount(menu) - 1; i >= 0; --i) {
        UINT id = GetMenuItemID(menu, i);
        if (id == 0 || id == (UINT)-1 || id >= kExtraMenuBase) continue;
        wchar_t verb[64] = {};
        if (FAILED(menu_->GetCommandString(id - 1, GCS_VERBW, nullptr, reinterpret_cast<LPSTR>(verb), 64))) continue;
        if (!_wcsicmp(verb, L"paste") || !_wcsicmp(verb, L"pastelink") || !_wcsicmp(verb, L"refresh"))
            DeleteMenu(menu, i, MF_BYPOSITION);
    }
    // No double separators where items were removed.
    for (int i = GetMenuItemCount(menu) - 1; i > 0; --i) {
        MENUITEMINFOW a{}, b{};
        a.cbSize = b.cbSize = sizeof(a);
        a.fMask = b.fMask = MIIM_FTYPE;
        GetMenuItemInfoW(menu, i, TRUE, &a);
        GetMenuItemInfoW(menu, i - 1, TRUE, &b);
        if ((a.fType & MFT_SEPARATOR) && (b.fType & MFT_SEPARATOR)) DeleteMenu(menu, i, MF_BYPOSITION);
    }
    menu_.As(&g_menu2);
    menu_.As(&g_menu3);
    return GetMenuItemCount(menu) > before;
}

UINT BackgroundMenu::track(HWND owner, HMENU menu, POINT screenPt, const std::wstring& directory, bool* shellCommand) {
    *shellCommand = false;
    if (menu_) {
        menu_.As(&g_menu2);
        menu_.As(&g_menu3);
    }
    UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPt.x, screenPt.y, owner, nullptr);
    g_menu2.Reset();
    g_menu3.Reset();
    if (cmd >= kExtraMenuBase || cmd == 0 || !menu_) return cmd >= kExtraMenuBase ? cmd : 0;

    CMINVOKECOMMANDINFOEX info{};
    info.cbSize = sizeof(info);
    info.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE | CMIC_MASK_ASYNCOK;
    info.hwnd = owner;
    info.lpVerb = MAKEINTRESOURCEA(cmd - 1);
    info.lpVerbW = MAKEINTRESOURCEW(cmd - 1);
    info.lpDirectoryW = directory.c_str();
    info.nShow = SW_SHOWNORMAL;
    info.ptInvoke = screenPt;
    AllowSetForegroundWindow(ASFW_ANY);
    menu_->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&info));
    *shellCommand = true;
    return 0;
}

bool handleMenuMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT* result) {
    if (g_menu3) {
        LRESULT r = 0;
        if (SUCCEEDED(g_menu3->HandleMenuMsg2(msg, wp, lp, &r))) {
            *result = r;
            return true;
        }
    } else if (g_menu2 && msg != WM_MENUCHAR) {
        if (SUCCEEDED(g_menu2->HandleMenuMsg(msg, wp, lp))) {
            *result = 0;
            return true;
        }
    }
    return false;
}

bool transfer(HWND owner, FileOp op, const std::vector<std::wstring>& sources, const std::wstring& destDir) {
    if (sources.empty()) return false;
    std::wstring from = doubleNull(sources);
    std::wstring to = doubleNull({destDir});
    SHFILEOPSTRUCTW fo{};
    fo.hwnd = owner;
    fo.wFunc = op == FileOp::Move ? FO_MOVE : FO_COPY;
    fo.pFrom = from.c_str();
    fo.pTo = to.c_str();
    fo.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR;
    return SHFileOperationW(&fo) == 0 && !fo.fAnyOperationsAborted;
}

bool recycle(HWND owner, const std::vector<std::wstring>& paths, bool silent) {
    if (paths.empty()) return false;
    std::wstring from = doubleNull(paths);
    SHFILEOPSTRUCTW fo{};
    fo.hwnd = owner;
    fo.wFunc = FO_DELETE;
    fo.pFrom = from.c_str();
    fo.fFlags = FOF_ALLOWUNDO;
    if (silent) fo.fFlags |= FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    return SHFileOperationW(&fo) == 0 && !fo.fAnyOperationsAborted;
}

bool rename(HWND owner, const std::wstring& path, const std::wstring& newName) {
    std::wstring from = doubleNull({path});
    std::wstring to = doubleNull({dirOf(path) + L"\\" + newName});
    SHFILEOPSTRUCTW fo{};
    fo.hwnd = owner;
    fo.wFunc = FO_RENAME;
    fo.pFrom = from.c_str();
    fo.pTo = to.c_str();
    fo.fFlags = FOF_ALLOWUNDO;
    return SHFileOperationW(&fo) == 0 && !fo.fAnyOperationsAborted;
}

DWORD dragOut(const std::vector<std::wstring>& paths) {
    auto data = dataObjectFor(paths);
    if (!data) return DROPEFFECT_NONE;
    DWORD effect = DROPEFFECT_NONE;
    SHDoDragDrop(nullptr, data.Get(), nullptr, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
    return effect;
}

bool copyToClipboard(HWND, const std::vector<std::wstring>& paths) {
    auto data = dataObjectFor(paths);
    return data && SUCCEEDED(OleSetClipboard(data.Get())) && SUCCEEDED(OleFlushClipboard());
}

} // namespace shell

// ---------------------------------------------------------------------------
// DropTarget
// ---------------------------------------------------------------------------

std::vector<std::wstring> filesFromDataObject(IDataObject* data) {
    std::vector<std::wstring> files;
    FORMATETC fmt{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM med{};
    if (FAILED(data->GetData(&fmt, &med))) return files;
    if (auto drop = static_cast<HDROP>(GlobalLock(med.hGlobal))) {
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            UINT len = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring s(len, L'\0');
            DragQueryFileW(drop, i, s.data(), len + 1);
            files.push_back(s);
        }
        GlobalUnlock(med.hGlobal);
    }
    ReleaseStgMedium(&med);
    return files;
}

DropTarget::DropTarget(HWND hwnd) : hwnd_(hwnd) {
    CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper_));
}

DropTarget::~DropTarget() = default;

HRESULT DropTarget::QueryInterface(REFIID riid, void** ppv) {
    if (riid == IID_IUnknown || riid == IID_IDropTarget) {
        *ppv = static_cast<IDropTarget*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG DropTarget::AddRef() { return InterlockedIncrement(&refs_); }

ULONG DropTarget::Release() {
    ULONG r = InterlockedDecrement(&refs_);
    if (!r) delete this;
    return r;
}

HRESULT DropTarget::DragEnter(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) {
    files_ = filesFromDataObject(data);
    allowed_ = *effect;
    POINT p{pt.x, pt.y};
    *effect = (files_.empty() || !onOver) ? DROPEFFECT_NONE : onOver(files_, p, keys, allowed_);
    if (helper_) helper_->DragEnter(hwnd_, data, &p, *effect);
    return S_OK;
}

HRESULT DropTarget::DragOver(DWORD keys, POINTL pt, DWORD* effect) {
    allowed_ = *effect;
    POINT p{pt.x, pt.y};
    *effect = (files_.empty() || !onOver) ? DROPEFFECT_NONE : onOver(files_, p, keys, allowed_);
    if (helper_) helper_->DragOver(&p, *effect);
    return S_OK;
}

HRESULT DropTarget::DragLeave() {
    files_.clear();
    if (helper_) helper_->DragLeave();
    if (onLeave) onLeave();
    return S_OK;
}

HRESULT DropTarget::Drop(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) {
    POINT p{pt.x, pt.y};
    auto files = filesFromDataObject(data);
    allowed_ = *effect;
    *effect = (files.empty() || !onOver) ? DROPEFFECT_NONE : onOver(files, p, keys, allowed_);
    if (helper_) helper_->Drop(data, &p, *effect);
    if (*effect != DROPEFFECT_NONE && onDrop) onDrop(files, p, *effect, keys);
    files_.clear();
    return S_OK;
}

} // namespace dt
