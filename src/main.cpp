#include "App.h"

#include <objbase.h>
#include <ole2.h>
#include <shellapi.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    dt::Options options;
    bool relaunched = false, keepAdmin = false;
    std::wstring passOn; // our arguments, for a relaunch
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        std::wstring a = dt::toLower(argv[i]);
        if (a == L"--window") options.mode = dt::HostMode::Window, options.modeForced = true;
        else if (a == L"--overlay") options.mode = dt::HostMode::Overlay, options.modeForced = true;
        else if (a == L"--embed") options.mode = dt::HostMode::Embed, options.modeForced = true;
        else if (a == L"--relaunched") relaunched = true;
        else if (a == L"--admin") keepAdmin = true;
        if (a != L"--relaunched") passOn += a + L" ";
    }
    LocalFree(argv);

    // OLE (not just COM) for drag & drop and the clipboard.
    if (FAILED(OleInitialize(nullptr))) return 1;

    // Started as administrator (e.g. "Run as administrator"): Windows blocks drag
    // and drop from Explorer into elevated windows, and everything opened from the
    // canvas would run elevated too. Restart with Explorer's normal rights instead.
    // When that is impossible (UAC off) or --admin is given, we carry on elevated
    // and accept drops via WM_DROPFILES.
    if (dt::isElevated() && !relaunched && !keepAdmin) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        if (dt::shell::launchUnelevated(exe, passOn + L"--relaunched")) {
            OleUninitialize();
            return 0;
        }
    }

    // One canvas per desktop is enough.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\DriftTable.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"DriftTable уже запущен — ищите значок в трее.", L"DriftTable", MB_ICONINFORMATION);
        OleUninitialize();
        return 0;
    }

    int code;
    {
        dt::App app;
        code = app.run(instance, options);
    }
    OleUninitialize();
    if (mutex) CloseHandle(mutex);
    return code;
}
