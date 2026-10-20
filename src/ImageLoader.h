#pragma once

#include "Util.h"

#include <d2d1.h>

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace dt {

// CPU-side image: premultiplied BGRA, top-down. The GPU copy is created lazily
// by the renderer and re-created whenever the render target is lost.
struct Image {
    int width = 0, height = 0;
    std::vector<uint8_t> pixels;
    ComPtr<ID2D1Bitmap> bitmap;
    uint32_t generation = 0;
};

struct LoadResult {
    std::wstring key;
    std::wstring displayName; // only for shell icons
    std::shared_ptr<Image> image; // null when loading failed
};

// Loads shell icons / thumbnails and picture files on a background thread so the
// desktop never stutters (our window may share an input queue with Explorer).
class ImageLoader {
public:
    ~ImageLoader();
    void start(HWND notifyWindow, UINT notifyMessage);
    void stop();

    void requestIcon(const std::wstring& key, const std::wstring& parsingName, int sizePx);
    void requestPicture(const std::wstring& key, const std::wstring& path, int maxSide);
    std::vector<LoadResult> takeResults();

    // Synchronous helpers, usable from any thread with COM initialized.
    static std::shared_ptr<Image> decodeFile(const std::wstring& path, int maxSide, int* fullW = nullptr,
                                             int* fullH = nullptr);
    static bool imageSize(const std::wstring& path, int* w, int* h);
    // Saves a packed DIB (CF_DIB clipboard data) as a PNG file.
    static bool saveDibAsPng(const void* dib, size_t size, const std::wstring& path);

private:
    struct Job {
        bool icon;
        std::wstring key, path;
        int size;
    };
    void run();
    LoadResult loadIcon(const Job& job);

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    std::vector<LoadResult> results_;
    bool stopping_ = false;
    HWND notify_ = nullptr;
    UINT message_ = 0;
};

} // namespace dt
