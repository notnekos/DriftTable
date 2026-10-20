#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <string>

using Microsoft::WRL::ComPtr;

namespace dt {

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

struct Vec2 {
    double x = 0, y = 0;
    Vec2() = default;
    Vec2(double x_, double y_) : x(x_), y(y_) {}
    Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(double s) const { return {x * s, y * s}; }
    double length() const { return std::sqrt(x * x + y * y); }
};

struct RectD {
    double x = 0, y = 0, w = 0, h = 0;
    double right() const { return x + w; }
    double bottom() const { return y + h; }
    bool contains(Vec2 p) const { return p.x >= x && p.y >= y && p.x <= x + w && p.y <= y + h; }
    bool intersects(const RectD& o) const {
        return x < o.right() && o.x < right() && y < o.bottom() && o.y < bottom();
    }
    bool containsRect(const RectD& o) const {
        return o.x >= x && o.y >= y && o.right() <= right() && o.bottom() <= bottom();
    }
    RectD inflated(double d) const { return {x - d, y - d, w + 2 * d, h + 2 * d}; }
    static RectD fromPoints(Vec2 a, Vec2 b) {
        return {std::min(a.x, b.x), std::min(a.y, b.y), std::fabs(a.x - b.x), std::fabs(a.y - b.y)};
    }
};

inline double distToSegment(Vec2 p, Vec2 a, Vec2 b) {
    Vec2 ab = b - a;
    double len2 = ab.x * ab.x + ab.y * ab.y;
    double t = len2 > 0 ? ((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / len2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    return (p - (a + ab * t)).length();
}

// ---------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------

inline std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

inline std::wstring fromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

inline std::wstring toLower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}

// Escapes a string so that it fits on a single line of the save file.
inline std::wstring escapeLine(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) {
        switch (c) {
        case L'\\': out += L"\\\\"; break;
        case L'\n': out += L"\\n"; break;
        case L'\r': break;
        case L'\t': out += L"\\t"; break;
        default: out += c;
        }
    }
    return out;
}

inline std::wstring unescapeLine(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'\\' && i + 1 < s.size()) {
            wchar_t n = s[++i];
            if (n == L'n') out += L'\n';
            else if (n == L't') out += L'\t';
            else out += n;
        } else {
            out += s[i];
        }
    }
    return out;
}

inline std::wstring fileNameOf(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? path : path.substr(p + 1);
}

inline std::wstring dirOf(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? std::wstring() : path.substr(0, p);
}

inline std::wstring extensionOf(const std::wstring& path) {
    std::wstring name = fileNameOf(path);
    size_t p = name.find_last_of(L'.');
    return p == std::wstring::npos ? std::wstring() : toLower(name.substr(p));
}

inline bool isImageExtension(const std::wstring& ext) {
    static const wchar_t* exts[] = {L".png", L".jpg", L".jpeg", L".bmp", L".gif", L".tif",
                                    L".tiff", L".webp", L".ico", L".jxr", L".heic", L".avif"};
    for (auto e : exts)
        if (ext == e) return true;
    return false;
}

inline bool fileExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Diagnostic log (%APPDATA%\DriftTable\log.txt), rewritten on every start.
inline std::wstring& logPath() {
    static std::wstring path;
    return path;
}

inline void logf(const wchar_t* fmt, ...) {
    if (logPath().empty()) return;
    wchar_t buf[1024];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);
    va_end(args);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t stamp[32];
    swprintf(stamp, 32, L"%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    std::string line = toUtf8(std::wstring(stamp) + buf + L"\r\n");
    HANDLE h = CreateFileW(logPath().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(h, line.data(), (DWORD)line.size(), &written, nullptr);
    CloseHandle(h);
}

// True when the process runs with administrator rights (UAC elevated).
inline bool isElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) &&
                    elevation.TokenIsElevated;
    CloseHandle(token);
    return elevated;
}

// Monotonic time in seconds.
inline double seconds() {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return (double)f.QuadPart;
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / freq;
}

} // namespace dt
