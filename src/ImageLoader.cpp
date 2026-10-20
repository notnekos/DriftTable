#include "ImageLoader.h"

#include <commctrl.h>
#include <commoncontrols.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>

#include <cstring>

namespace dt {

namespace {

ComPtr<IWICImagingFactory> wicFactory() {
    ComPtr<IWICImagingFactory> f;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    return f;
}

// Reads the EXIF orientation of a frame (1 = normal) so photos from phones are
// shown the right way up.
int exifOrientation(IWICBitmapFrameDecode* frame) {
    ComPtr<IWICMetadataQueryReader> reader;
    if (FAILED(frame->GetMetadataQueryReader(&reader))) return 1;
    const wchar_t* queries[] = {L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}"};
    for (auto q : queries) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(reader->GetMetadataByName(q, &v))) {
            int o = (v.vt == VT_UI2) ? v.uiVal : 1;
            PropVariantClear(&v);
            return o;
        }
    }
    return 1;
}

WICBitmapTransformOptions orientationTransform(int o) {
    switch (o) {
    case 2: return WICBitmapTransformFlipHorizontal;
    case 3: return WICBitmapTransformRotate180;
    case 4: return WICBitmapTransformFlipVertical;
    case 5: return (WICBitmapTransformOptions)(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal);
    case 6: return WICBitmapTransformRotate90;
    case 7: return (WICBitmapTransformOptions)(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal);
    case 8: return WICBitmapTransformRotate270;
    default: return WICBitmapTransformRotate0;
    }
}

std::shared_ptr<Image> fromHBitmap(HBITMAP hbm) {
    BITMAP bm{};
    if (!GetObjectW(hbm, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight == 0) return nullptr;
    auto img = std::make_shared<Image>();
    img->width = bm.bmWidth;
    img->height = std::abs(bm.bmHeight);
    img->pixels.resize((size_t)img->width * img->height * 4);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = img->width;
    bi.bmiHeader.biHeight = -img->height; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(nullptr);
    int lines = GetDIBits(dc, hbm, 0, img->height, img->pixels.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    if (lines != img->height) return nullptr;

    // Thumbnails of photos come without alpha (all zero); icons come with it.
    // Some providers hand out straight alpha, so premultiply when needed.
    bool anyAlpha = false, straight = false;
    uint8_t* p = img->pixels.data();
    size_t n = (size_t)img->width * img->height;
    for (size_t i = 0; i < n; ++i) {
        uint8_t a = p[i * 4 + 3];
        if (a) anyAlpha = true;
        if (p[i * 4] > a || p[i * 4 + 1] > a || p[i * 4 + 2] > a) straight = true;
    }
    for (size_t i = 0; i < n; ++i) {
        uint8_t* px = p + i * 4;
        if (!anyAlpha) {
            px[3] = 255;
        } else if (straight) {
            px[0] = (uint8_t)(px[0] * px[3] / 255);
            px[1] = (uint8_t)(px[1] * px[3] / 255);
            px[2] = (uint8_t)(px[2] * px[3] / 255);
        }
    }
    return img;
}

// Shortcuts to apps that ship only small icons come back as a 16-32 px picture in
// the middle of a big transparent square. Crop that padding so they fill the tile.
void trimPadding(Image& img) {
    int w = img.width, h = img.height;
    int minX = w, minY = h, maxX = -1, maxY = -1;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (img.pixels[((size_t)y * w + x) * 4 + 3] > 8) {
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
            }
    if (maxX < 0) return;
    int side = std::max(maxX - minX + 1, maxY - minY + 1);
    if (side * 100 >= std::max(w, h) * 55) return; // normal icon
    side += side / 5;                               // keep a little air around it
    if (side > w || side > h) return;
    int x0 = std::clamp((minX + maxX + 1) / 2 - side / 2, 0, w - side);
    int y0 = std::clamp((minY + maxY + 1) / 2 - side / 2, 0, h - side);
    std::vector<uint8_t> out((size_t)side * side * 4);
    for (int y = 0; y < side; ++y)
        memcpy(&out[(size_t)y * side * 4], &img.pixels[((size_t)(y0 + y) * w + x0) * 4], (size_t)side * 4);
    img.pixels.swap(out);
    img.width = img.height = side;
}

bool isBlank(const Image& img) {
    for (size_t i = 0; i < img.pixels.size(); i += 4)
        if (img.pixels[i] | img.pixels[i + 1] | img.pixels[i + 2]) return false;
    return true;
}

// Draws an icon into a 32-bit DIB of the given size.
std::shared_ptr<Image> iconToImage(HICON icon, int size) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    std::shared_ptr<Image> img;
    if (dib) {
        HGDIOBJ old = SelectObject(dc, dib);
        DrawIconEx(dc, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL);
        GdiFlush();
        img = std::make_shared<Image>();
        img->width = img->height = size;
        img->pixels.assign(static_cast<uint8_t*>(bits), static_cast<uint8_t*>(bits) + (size_t)size * size * 4);
        SelectObject(dc, old);
        DeleteObject(dib);
        // Icons without an alpha channel: opaque wherever something was drawn.
        bool anyAlpha = false;
        for (size_t i = 3; i < img->pixels.size(); i += 4) anyAlpha |= img->pixels[i] != 0;
        if (!anyAlpha)
            for (size_t i = 0; i < img->pixels.size(); i += 4)
                if (img->pixels[i] | img->pixels[i + 1] | img->pixels[i + 2]) img->pixels[i + 3] = 255;
    }
    DeleteDC(dc);
    return img;
}

// System image list index and overlay number (0 = none) of a shell item.
bool systemIconIndex(const std::wstring& path, int* index, int* overlay, bool* isLink = nullptr) {
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr))) return false;
    SHFILEINFOW sfi{};
    // SHGFI_OVERLAYINDEX needs SHGFI_ICON; the overlay comes in the top byte of iIcon.
    sfi.dwAttributes = SFGAO_LINK; // with SHGFI_ATTR_SPECIFIED: only ask for this one
    DWORD_PTR ok = SHGetFileInfoW(reinterpret_cast<LPCWSTR>(pidl), 0, &sfi, sizeof(sfi),
                                  SHGFI_PIDL | SHGFI_SYSICONINDEX | SHGFI_ICON | SHGFI_OVERLAYINDEX |
                                      SHGFI_ATTRIBUTES | SHGFI_ATTR_SPECIFIED);
    CoTaskMemFree(pidl);
    if (sfi.hIcon) DestroyIcon(sfi.hIcon);
    if (!ok) return false;
    *index = sfi.iIcon & 0x00FFFFFF;
    *overlay = (sfi.iIcon >> 24) & 0xFF;
    if (isLink) *isLink = (sfi.dwAttributes & SFGAO_LINK) != 0;
    return true;
}

ComPtr<IImageList> systemImageList(int size) {
    ComPtr<IImageList> list;
    SHGetImageList(size > 48 ? SHIL_JUMBO : SHIL_EXTRALARGE, IID_PPV_ARGS(&list));
    return list;
}

// Fallback: the icon from the system image list.
std::shared_ptr<Image> systemIcon(const std::wstring& path, int size) {
    int index = 0, overlay = 0;
    if (!systemIconIndex(path, &index, &overlay)) return nullptr;
    auto list = systemImageList(size);
    HICON icon = nullptr;
    if (!list || FAILED(list->GetIcon(index, ILD_TRANSPARENT, &icon)) || !icon) return nullptr;
    auto img = iconToImage(icon, size);
    DestroyIcon(icon);
    return img;
}

// The overlay Explorer draws over this item (the shortcut arrow for .lnk/.url,
// sync status icons...). Respects a user who removed the shortcut arrow.
std::shared_ptr<Image> overlayImage(const std::wstring& path, int size) {
    int index = 0, overlay = 0;
    bool isLink = false;
    if (!systemIconIndex(path, &index, &overlay, &isLink)) return nullptr;
    auto list = systemImageList(size);
    if (!list) return nullptr;
    int image = -1;
    if (overlay > 0) list->GetOverlayImage(overlay, &image);
    // A shortcut whose overlay was not reported: the standard shortcut arrow.
    if (image < 0 && isLink) image = SHGetIconOverlayIndexW(nullptr, IDO_SHGIOI_LINK);
    if (image < 0) return nullptr;
    HICON icon = nullptr;
    if (FAILED(list->GetIcon(image, ILD_TRANSPARENT, &icon)) || !icon) return nullptr;
    auto img = iconToImage(icon, size);
    DestroyIcon(icon);
    if (img && isBlank(*img)) img.reset();
    return img;
}

} // namespace

ImageLoader::~ImageLoader() { stop(); }

void ImageLoader::start(HWND notifyWindow, UINT notifyMessage) {
    notify_ = notifyWindow;
    message_ = notifyMessage;
    stopping_ = false;
    thread_ = std::thread([this] { run(); });
}

void ImageLoader::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        jobs_.clear();
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void ImageLoader::requestIcon(const std::wstring& key, const std::wstring& parsingName, int sizePx) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.push_back({true, key, parsingName, sizePx});
    }
    cv_.notify_one();
}

void ImageLoader::requestPicture(const std::wstring& key, const std::wstring& path, int maxSide) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.push_back({false, key, path, maxSide});
    }
    cv_.notify_one();
}

std::vector<LoadResult> ImageLoader::takeResults() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<LoadResult> out;
    out.swap(results_);
    return out;
}

void ImageLoader::run() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (stopping_) break;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        LoadResult r;
        if (job.icon) {
            r = loadIcon(job);
        } else {
            r.key = job.key;
            r.image = decodeFile(job.path, job.size);
        }
        bool notify;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            notify = results_.empty();
            results_.push_back(std::move(r));
        }
        if (notify) PostMessageW(notify_, message_, 0, 0);
    }
    CoUninitialize();
}

LoadResult ImageLoader::loadIcon(const Job& job) {
    LoadResult r;
    r.key = job.key;
    ComPtr<IShellItem> item;
    if (FAILED(SHCreateItemFromParsingName(job.path.c_str(), nullptr, IID_PPV_ARGS(&item)))) return r;

    PWSTR name = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name))) {
        r.displayName = name;
        CoTaskMemFree(name);
    }

    ComPtr<IShellItemImageFactory> factory;
    if (SUCCEEDED(item.As(&factory))) {
        HBITMAP hbm = nullptr;
        SIZE size{job.size, job.size};
        // Thumbnail when the file has one (photos, videos, documents), icon otherwise.
        if (SUCCEEDED(factory->GetImage(size, SIIGBF_RESIZETOFIT, &hbm)) && hbm) {
            r.image = fromHBitmap(hbm);
            DeleteObject(hbm);
        }
    }
    if (!r.image || isBlank(*r.image)) r.image = systemIcon(job.path, job.size);
    if (r.image) {
        trimPadding(*r.image);
        r.image->overlay = overlayImage(job.path, job.size);
    }
    return r;
}

bool ImageLoader::imageSize(const std::wstring& path, int* w, int* h) {
    auto wic = wicFactory();
    if (!wic) return false;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                              &decoder)))
        return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return false;
    UINT fw = 0, fh = 0;
    frame->GetSize(&fw, &fh);
    int o = exifOrientation(frame.Get());
    bool swap = o >= 5 && o <= 8;
    *w = (int)(swap ? fh : fw);
    *h = (int)(swap ? fw : fh);
    return fw > 0 && fh > 0;
}

std::shared_ptr<Image> ImageLoader::decodeFile(const std::wstring& path, int maxSide, int* fullW, int* fullH) {
    auto wic = wicFactory();
    if (!wic) return nullptr;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                              &decoder)))
        return nullptr;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return nullptr;

    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (!w || !h) return nullptr;

    ComPtr<IWICBitmapSource> source = frame;
    double scale = std::min(1.0, (double)maxSide / std::max(w, h));
    if (scale < 1.0) {
        ComPtr<IWICBitmapScaler> scaler;
        UINT sw = std::max(1u, (UINT)(w * scale)), sh = std::max(1u, (UINT)(h * scale));
        if (SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(source.Get(), sw, sh, WICBitmapInterpolationModeFant))) {
            source = scaler;
        }
    }

    int o = exifOrientation(frame.Get());
    if (o != 1) {
        ComPtr<IWICBitmapFlipRotator> rot;
        if (SUCCEEDED(wic->CreateBitmapFlipRotator(&rot)) &&
            SUCCEEDED(rot->Initialize(source.Get(), orientationTransform(o)))) {
            source = rot;
        }
    }
    if (fullW && fullH) {
        bool swap = o >= 5 && o <= 8;
        *fullW = (int)(swap ? h : w);
        *fullH = (int)(swap ? w : h);
    }

    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv))) return nullptr;
    if (FAILED(conv->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeMedianCut)))
        return nullptr;

    UINT cw = 0, ch = 0;
    conv->GetSize(&cw, &ch);
    auto img = std::make_shared<Image>();
    img->width = (int)cw;
    img->height = (int)ch;
    img->pixels.resize((size_t)cw * ch * 4);
    if (FAILED(conv->CopyPixels(nullptr, cw * 4, (UINT)img->pixels.size(), img->pixels.data()))) return nullptr;
    return img;
}

bool ImageLoader::saveDibAsPng(const void* dib, size_t size, const std::wstring& path) {
    if (size < sizeof(BITMAPINFOHEADER)) return false;
    auto* hdr = static_cast<const BITMAPINFOHEADER*>(dib);
    size_t colors = hdr->biClrUsed ? hdr->biClrUsed : (hdr->biBitCount <= 8 ? (1u << hdr->biBitCount) : 0);
    size_t offset = hdr->biSize + colors * sizeof(RGBQUAD);
    if (hdr->biCompression == BI_BITFIELDS && hdr->biSize == sizeof(BITMAPINFOHEADER)) offset += 3 * sizeof(DWORD);
    if (offset >= size) return false;

    HDC dc = GetDC(nullptr);
    HBITMAP hbm = CreateDIBitmap(dc, hdr, CBM_INIT, static_cast<const uint8_t*>(dib) + offset,
                                 reinterpret_cast<const BITMAPINFO*>(hdr), DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    if (!hbm) return false;

    bool ok = false;
    auto wic = wicFactory();
    ComPtr<IWICBitmap> bitmap;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    // Clipboard bitmaps (screenshots) usually carry no meaningful alpha.
    if (wic && SUCCEEDED(wic->CreateBitmapFromHBITMAP(hbm, nullptr, WICBitmapIgnoreAlpha, &bitmap)) &&
        SUCCEEDED(wic->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->CreateNewFrame(&frame, &props)) && SUCCEEDED(frame->Initialize(props.Get()))) {
        UINT w = 0, h = 0;
        bitmap->GetSize(&w, &h);
        WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
        ok = SUCCEEDED(frame->SetSize(w, h)) && SUCCEEDED(frame->SetPixelFormat(&fmt)) &&
             SUCCEEDED(frame->WriteSource(bitmap.Get(), nullptr)) && SUCCEEDED(frame->Commit()) &&
             SUCCEEDED(encoder->Commit());
    }
    DeleteObject(hbm);
    frame.Reset();
    encoder.Reset();
    stream.Reset(); // closes the file
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

} // namespace dt
