#include "App.h"

#include <shlwapi.h>

namespace dt {

namespace {

constexpr wchar_t kRecycleBin[] = L"::{645FF040-5081-101B-9F08-00AA002F954E}";
constexpr wchar_t kThisPc[] = L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}";
constexpr double kCellW = kTileW + 2;
constexpr double kCellH = kTileH + 4;
constexpr double kNotePad = 12;
constexpr uint32_t kDefaultNoteColor = 0xFFFFF3A6;

bool isDirectory(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool allImages(const std::vector<std::wstring>& files) {
    for (auto& f : files)
        if (isDirectory(f) || !isImageExtension(extensionOf(f))) return false;
    return !files.empty();
}

std::wstring volumeOf(const std::wstring& path) {
    wchar_t buf[MAX_PATH] = {};
    if (!GetVolumePathNameW(path.c_str(), buf, MAX_PATH)) return {};
    return toLower(buf);
}

} // namespace

// ---------------------------------------------------------------------------
// Load / save
// ---------------------------------------------------------------------------

void App::loadBoard() {
    CameraState cam;
    firstRun_ = !board_.load(boardFile_, cam);
    if (cam.valid) {
        camX_ = cam.x;
        camY_ = cam.y;
        zoom_ = cam.zoom;
    } else {
        resetView(false);
    }
    for (auto& it : board_.items)
        if (it.kind == ItemKind::Note) layoutNote(it, it.text);
    if (firstRun_) addSystemIcons();
}

void App::scheduleSave() {
    SetTimer(main_, TIMER_SAVE, 1500, nullptr);
    savePending_ = true;
}

void App::saveNow() {
    CameraState cam{camX_, camY_, zoom_, true};
    board_.save(boardFile_, cam);
    savePending_ = false;
}

// ---------------------------------------------------------------------------
// Desktop folder mirroring
// ---------------------------------------------------------------------------

Vec2 App::findFreeSlot(Vec2 origin, double columnHeight) const {
    int rows = std::max(1, (int)(columnHeight / kCellH));
    for (int col = 0; col < 1000; ++col) {
        for (int row = 0; row < rows; ++row) {
            RectD cell{origin.x + col * kCellW, origin.y + row * kCellH, kTileW, kTileH};
            bool free = true;
            for (auto& it : board_.items) {
                if (it.kind == ItemKind::Stroke) continue; // drawings may overlap files
                if (it.rect().inflated(-2).intersects(cell)) {
                    free = false;
                    break;
                }
            }
            if (free) return {cell.x, cell.y};
        }
    }
    return origin;
}

void App::syncDesktop() {
    std::unordered_map<std::wstring, std::wstring> present;
    for (auto& path : folders_.scan()) present[toLower(path)] = path;

    std::unordered_set<std::wstring> have;
    std::vector<size_t> missing;
    for (size_t i = 0; i < board_.items.size(); ++i) {
        const Item& it = board_.items[i];
        if (it.kind != ItemKind::File || it.isVirtual()) continue;
        std::wstring key = toLower(it.path);
        if (present.count(key) && !have.count(key)) have.insert(key);
        else missing.push_back(i);
    }

    std::vector<std::wstring> added;
    for (auto& [key, path] : present)
        if (!have.count(key)) added.push_back(path);
    if (missing.empty() && added.empty()) return;

    std::sort(added.begin(), added.end(), [](const std::wstring& a, const std::wstring& b) {
        bool da = isDirectory(a), db = isDirectory(b);
        if (da != db) return da;
        return StrCmpLogicalW(fileNameOf(a).c_str(), fileNameOf(b).c_str()) < 0;
    });

    // A rename shows up as one file gone and one new: keep its place.
    if (missing.size() == 1 && added.size() == 1 && !pendingPos_.count(toLower(added[0]))) {
        const Item& old = board_.items[missing[0]];
        pendingPos_[toLower(added[0])] = {old.x, old.y};
    }

    for (auto i = missing.rbegin(); i != missing.rend(); ++i) {
        const Item& it = board_.items[*i];
        selected_.erase(it.id);
        if (hoverId_ == it.id) hoverId_ = 0;
        images_.erase(L"icon:" + toLower(it.path));
        board_.items.erase(board_.items.begin() + *i);
    }

    RECT pa = primaryWorkArea();
    Vec2 origin = toWorld({pa.left + 24.0, pa.top + 24.0});
    double columnHeight = (pa.bottom - pa.top - 48) / scale();
    for (auto& path : added) {
        Item it;
        it.kind = ItemKind::File;
        it.path = path;
        it.w = kTileW;
        it.h = kTileH;
        auto pending = pendingPos_.find(toLower(path));
        Vec2 pos = pending != pendingPos_.end() ? pending->second : findFreeSlot(origin, columnHeight);
        if (pending != pendingPos_.end()) pendingPos_.erase(pending);
        it.x = pos.x;
        it.y = pos.y;
        board_.add(std::move(it));
    }
    scheduleSave();
    invalidate();
}

void App::addSystemIcons() {
    RECT pa = primaryWorkArea();
    double columnHeight = (pa.bottom - pa.top - 48) / dpi_;
    for (const wchar_t* path : {kRecycleBin, kThisPc}) {
        bool exists = false;
        for (auto& it : board_.items)
            if (it.kind == ItemKind::File && it.path == path) exists = true;
        if (exists) continue;
        Item it;
        it.kind = ItemKind::File;
        it.path = path;
        it.w = kTileW;
        it.h = kTileH;
        Vec2 pos = findFreeSlot({0, 0}, columnHeight);
        it.x = pos.x;
        it.y = pos.y;
        board_.add(std::move(it));
    }
    scheduleSave();
    invalidate();
}

void App::refresh() {
    images_.clear();
    pendingImages_.clear();
    displayNames_.clear();
    syncDesktop();
    invalidate();
}

// ---------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------

Image* App::imageFor(const Item& it) {
    if (it.kind != ItemKind::File && it.kind != ItemKind::Picture) return nullptr;
    std::wstring key = (it.kind == ItemKind::File ? L"icon:" : L"pic:") + toLower(it.path);
    auto found = images_.find(key);
    if (found != images_.end()) return found->second.get();
    if (pendingImages_.insert(key).second) {
        if (it.kind == ItemKind::File)
            loader_.requestIcon(key, it.path, std::clamp((int)(kIconSize * dpi_ * 2), 64, 256));
        else
            loader_.requestPicture(key, it.path, 2048);
    }
    return nullptr;
}

void App::onImagesLoaded() {
    for (auto& r : loader_.takeResults()) {
        pendingImages_.erase(r.key);
        images_[r.key] = r.image;
        if (!r.displayName.empty() && r.key.rfind(L"icon:", 0) == 0) displayNames_[r.key.substr(5)] = r.displayName;
    }
    invalidate();
}

std::wstring App::labelFor(const Item& it) const {
    auto found = displayNames_.find(toLower(it.path));
    if (found != displayNames_.end()) return found->second;
    std::wstring name = fileNameOf(it.path);
    std::wstring ext = extensionOf(name);
    if (ext == L".lnk" || ext == L".url") name.resize(name.size() - ext.size());
    return name;
}

// ---------------------------------------------------------------------------
// Drag & drop into the canvas
// ---------------------------------------------------------------------------

DWORD App::dropEffect(const std::vector<std::wstring>& files, POINT, DWORD keys, DWORD allowed) {
    auto pick = [allowed](DWORD want) {
        if (allowed & want) return want;
        for (DWORD e : {DROPEFFECT_MOVE, DROPEFFECT_COPY, DROPEFFECT_LINK})
            if (allowed & e) return e;
        return (DWORD)DROPEFFECT_NONE;
    };
    // Alt: pin images to the canvas as pictures.
    if ((keys & MK_ALT) && allImages(files)) return pick(DROPEFFECT_LINK);

    bool onDesktop = true;
    for (auto& f : files)
        if (!folders_.isInDesktop(f)) onDesktop = false;
    if (onDesktop) return pick(DROPEFFECT_MOVE); // just a new position

    if (keys & MK_CONTROL) return pick(DROPEFFECT_COPY);
    if (keys & MK_SHIFT) return pick(DROPEFFECT_MOVE);
    // Like Explorer: move within a volume, copy across volumes.
    bool sameVolume = volumeOf(files[0]) == volumeOf(folders_.userDesktop());
    return pick(sameVolume ? DROPEFFECT_MOVE : DROPEFFECT_COPY);
}

void App::processDrops() {
    auto drops = std::move(drops_);
    drops_.clear();
    for (auto& d : drops) {
        Vec2 at = toWorld(screenToClient(d.screen));
        if (d.effect == DROPEFFECT_LINK && allImages(d.files)) {
            board_.pushUndo();
            Vec2 offset;
            for (auto& f : d.files) {
                addPicture(f, at + offset, true);
                offset = offset + Vec2(28, 28);
            }
            scheduleSave();
            continue;
        }

        int cols = std::max(1, (int)std::ceil(std::sqrt((double)d.files.size())));
        std::vector<std::wstring> transfer;
        bool undoPushed = false;
        for (size_t i = 0; i < d.files.size(); ++i) {
            const std::wstring& f = d.files[i];
            Vec2 pos{at.x - kTileW / 2 + (i % cols) * kCellW, at.y - kIconSize / 2 - 6 + (i / cols) * kCellH};
            if (folders_.isInDesktop(f)) {
                int idx = -1;
                for (size_t k = 0; k < board_.items.size(); ++k)
                    if (board_.items[k].kind == ItemKind::File && toLower(board_.items[k].path) == toLower(f))
                        idx = (int)k;
                if (idx >= 0) {
                    if (!undoPushed) board_.pushUndo();
                    undoPushed = true;
                    board_.items[idx].x = pos.x;
                    board_.items[idx].y = pos.y;
                } else {
                    pendingPos_[toLower(f)] = pos;
                }
            } else {
                pendingPos_[toLower(folders_.userDesktop() + L"\\" + fileNameOf(f))] = pos;
                transfer.push_back(f);
            }
        }
        if (!transfer.empty())
            shell::transfer(canvas_, d.effect == DROPEFFECT_MOVE ? shell::FileOp::Move : shell::FileOp::Copy,
                            transfer, folders_.userDesktop());
        syncDesktop();
        scheduleSave();
    }
    invalidate();
}

// ---------------------------------------------------------------------------
// Pictures
// ---------------------------------------------------------------------------

std::wstring App::newPicturePath(const std::wstring& ext) const {
    static int counter = 0;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    wchar_t name[64];
    swprintf(name, 64, L"%08lX%08lX-%d", ft.dwHighDateTime, ft.dwLowDateTime, ++counter);
    return picturesDir_ + L"\\" + name + ext;
}

uint64_t App::addPicture(const std::wstring& source, Vec2 center, bool copyIntoStore) {
    int pw = 0, ph = 0;
    if (!ImageLoader::imageSize(source, &pw, &ph)) return 0;
    std::wstring path = source;
    if (copyIntoStore) {
        // Keep our own copy so the picture survives the original being moved.
        path = newPicturePath(extensionOf(source));
        if (!CopyFileW(source.c_str(), path.c_str(), FALSE)) return 0;
    }
    double w = pw / dpi_, h = ph / dpi_;
    double f = std::min(1.0, 480.0 / std::max(w, h));
    Item it;
    it.kind = ItemKind::Picture;
    it.path = path;
    it.w = std::max(8.0, w * f);
    it.h = std::max(8.0, h * f);
    it.x = center.x - it.w / 2;
    it.y = center.y - it.h / 2;
    uint64_t id = board_.add(std::move(it));
    selectOnly(id);
    invalidate();
    return id;
}

void App::cleanupPictures() {
    // Without a readable canvas we cannot know what is referenced: keep everything.
    if (firstRun_ && fileExists(boardFile_)) return;
    std::unordered_set<std::wstring> used;
    for (auto& it : board_.items)
        if (it.kind == ItemKind::Picture) used.insert(toLower(it.path));
    std::vector<std::wstring> orphans;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((picturesDir_ + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring path = picturesDir_ + L"\\" + fd.cFileName;
        if (!used.count(toLower(path))) orphans.push_back(path);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    // To the Recycle Bin, not gone for good.
    if (!orphans.empty()) shell::recycle(main_, orphans, true);
}

void App::paste(Vec2 world) {
    if (!OpenClipboard(main_)) return;
    UINT pngFormat = RegisterClipboardFormatW(L"PNG");
    std::vector<std::wstring> files;
    std::wstring text, picture;

    if (IsClipboardFormatAvailable(CF_HDROP)) {
        if (auto h = static_cast<HDROP>(GetClipboardData(CF_HDROP))) {
            UINT n = DragQueryFileW(h, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; ++i) {
                UINT len = DragQueryFileW(h, i, nullptr, 0);
                std::wstring s(len, L'\0');
                DragQueryFileW(h, i, s.data(), len + 1);
                files.push_back(s);
            }
        }
    } else if (IsClipboardFormatAvailable(pngFormat)) {
        if (HGLOBAL g = GetClipboardData(pngFormat)) {
            if (void* data = GlobalLock(g)) {
                std::wstring path = newPicturePath(L".png");
                HANDLE out = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
                if (out != INVALID_HANDLE_VALUE) {
                    DWORD written = 0;
                    WriteFile(out, data, (DWORD)GlobalSize(g), &written, nullptr);
                    CloseHandle(out);
                    picture = path;
                }
                GlobalUnlock(g);
            }
        }
    } else if (IsClipboardFormatAvailable(CF_DIB)) {
        if (HGLOBAL g = GetClipboardData(CF_DIB)) {
            if (void* data = GlobalLock(g)) {
                std::wstring path = newPicturePath(L".png");
                if (ImageLoader::saveDibAsPng(data, GlobalSize(g), path)) picture = path;
                GlobalUnlock(g);
            }
        }
    } else if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        if (HGLOBAL g = GetClipboardData(CF_UNICODETEXT)) {
            if (auto s = static_cast<const wchar_t*>(GlobalLock(g))) {
                text = s;
                GlobalUnlock(g);
            }
        }
    }
    CloseClipboard();

    if (!files.empty()) {
        drops_.push_back({files, clientToScreen(toScreen(world)), DROPEFFECT_COPY, 0});
        processDrops();
    } else if (!picture.empty()) {
        board_.pushUndo();
        if (!addPicture(picture, world, false)) board_.discardUndo();
        scheduleSave();
    } else if (!text.empty()) {
        text.erase(std::remove(text.begin(), text.end(), L'\r'), text.end());
        board_.pushUndo();
        selectOnly(addNote(world, text));
        scheduleSave();
    }
}

// ---------------------------------------------------------------------------
// Notes and in-place editing
// ---------------------------------------------------------------------------

uint64_t App::addNote(Vec2 world, const std::wstring& text) {
    Item it;
    it.kind = ItemKind::Note;
    it.x = world.x;
    it.y = world.y;
    it.w = 220;
    it.color = kDefaultNoteColor;
    it.text = text;
    layoutNote(it, text);
    invalidate();
    return board_.add(std::move(it));
}

void App::layoutNote(Item& note, const std::wstring& text) {
    ComPtr<IDWriteTextLayout> layout;
    std::wstring measured = text.empty() ? L"M" : text;
    if (!text.empty() && text.back() == L'\n') measured += L"M"; // count the trailing empty line
    float textH = 22;
    if (SUCCEEDED(renderer_.dwrite()->CreateTextLayout(measured.c_str(), (UINT32)measured.size(),
                                                       renderer_.noteFormat(), (float)(note.w - 2 * kNotePad),
                                                       100000.f, &layout))) {
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        textH = m.height;
    }
    note.h = std::max(48.0, textH + 2 * kNotePad);
}

void App::beginEdit(uint64_t id, bool rename) {
    if (editId_ && editId_ != id) commitEdit();
    Item* it = board_.find(id);
    if (!it) return;
    if (rename && (it->kind != ItemKind::File || it->isVirtual())) return;
    if (!rename && it->kind != ItemKind::Note) return;
    editId_ = id;
    editRename_ = rename;
    editText_ = rename ? fileNameOf(it->path) : it->text;
    caret_ = editText_.size();
    if (rename && !isDirectory(it->path)) {
        size_t dot = editText_.find_last_of(L'.');
        if (dot != std::wstring::npos && dot > 0) caret_ = dot; // like Explorer: before the extension
    }
    caretOn_ = true;
    selectOnly(id);
    SetTimer(main_, TIMER_CARET, 530, nullptr);
    if (mode_ == HostMode::Overlay) SetForegroundWindow(canvas_);
    SetFocus(canvas_);
    invalidate();
}

void App::commitEdit(bool cancel) {
    if (!editId_) return;
    uint64_t id = editId_;
    editId_ = 0;
    KillTimer(main_, TIMER_CARET);
    invalidate();
    Item* it = board_.find(id);
    if (!it) return;

    if (editRename_) {
        std::wstring name = editText_;
        while (!name.empty() && (name.back() == L' ' || name.back() == L'.')) name.pop_back();
        if (cancel || name.empty() || name == fileNameOf(it->path)) return;
        pendingPos_[toLower(dirOf(it->path) + L"\\" + name)] = {it->x, it->y};
        shell::rename(canvas_, it->path, name);
        syncDesktop();
        return;
    }

    if (editText_.empty()) {
        if (!it->text.empty()) board_.pushUndo();
        board_.remove(id);
        selected_.erase(id);
    } else if (!cancel && editText_ != it->text) {
        if (!it->text.empty()) board_.pushUndo(); // a brand-new note already has its undo step
        it = board_.find(id);
        it->text = editText_;
        it->layout.Reset();
        layoutNote(*it, it->text);
    }
    scheduleSave();
}

// ---------------------------------------------------------------------------
// Selection helpers
// ---------------------------------------------------------------------------

void App::selectOnly(uint64_t id) {
    selected_.clear();
    if (id) selected_.insert(id);
    invalidate();
}

std::vector<std::wstring> App::selectedFilePaths() const {
    std::vector<std::wstring> out;
    for (auto& it : board_.items)
        if (it.kind == ItemKind::File && selected_.count(it.id)) out.push_back(it.path);
    return out;
}

void App::deleteSelection() {
    commitEdit();
    std::vector<std::wstring> files;
    std::vector<uint64_t> others;
    for (auto& it : board_.items) {
        if (!selected_.count(it.id)) continue;
        if (it.kind == ItemKind::File && !it.isVirtual()) files.push_back(it.path);
        else others.push_back(it.id);
    }
    if (!others.empty()) {
        board_.pushUndo();
        for (auto id : others) board_.remove(id);
        scheduleSave();
    }
    selected_.clear();
    // Real files go to the Recycle Bin, exactly like Delete on the desktop.
    if (!files.empty() && shell::recycle(canvas_, files)) syncDesktop();
    invalidate();
}

} // namespace dt
