#include "App.h"

#include <objbase.h>
#include <ole2.h>
#include <shellapi.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    dt::Options options;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        std::wstring a = dt::toLower(argv[i]);
        if (a == L"--window") options.mode = dt::HostMode::Window, options.modeForced = true;
        else if (a == L"--overlay") options.mode = dt::HostMode::Overlay, options.modeForced = true;
        else if (a == L"--embed") options.mode = dt::HostMode::Embed, options.modeForced = true;
    }
    LocalFree(argv);

    // One canvas per desktop is enough.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\DriftTable.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"DriftTable уже запущен — ищите значок в трее.", L"DriftTable", MB_ICONINFORMATION);
        return 0;
    }

    // OLE (not just COM) for drag & drop and the clipboard.
    if (FAILED(OleInitialize(nullptr))) return 1;
    int code;
    {
        dt::App app;
        code = app.run(instance, options);
    }
    OleUninitialize();
    if (mutex) CloseHandle(mutex);
    return code;
}
