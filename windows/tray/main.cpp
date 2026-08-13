// HeadphoneSafetyTray entry point.
//
// Phase 1: Volume Cap enforcement only, no tray icon yet (Phase 5 adds the Shell_NotifyIcon menu
// mirroring linux/src/tray.rs). --test-cap allocates a console and logs every poll tick (device,
// kind, current dB, ceiling, whether a clamp was applied) - mirrors linux/src/main.rs's
// test-routing/test-limiter subcommands. Without --test-cap, runs the same enforcement loop
// silently forever, as a plain WIN32-subsystem process with no console.
#include <windows.h>
#include <shellapi.h>

#include <cstdio>

#include "Settings.h"
#include "VolumeCap.h"

namespace {

bool HasFlag(int argc, wchar_t** argv, const wchar_t* flag) {
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], flag) == 0) return true;
    }
    return false;
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool testCap = argv && HasFlag(argc, argv, L"--test-cap");

    if (testCap) {
        // Only force a fresh console window if stdout wasn't already inherited from the parent
        // (e.g. a pipe when launched from a terminal/test harness) - forcing AllocConsole+CONOUT$
        // unconditionally would redirect away from an already-valid inherited pipe handle into a
        // separate, unobservable window instead. This subsystem is WIN32 (no console auto-
        // attached), but an inherited stdout handle from CreateProcess still works via the CRT
        // regardless of subsystem.
        if (GetStdHandle(STD_OUTPUT_HANDLE) == nullptr) {
            AllocConsole();
            FILE* dummy = nullptr;
            freopen_s(&dummy, "CONOUT$", "w", stdout);
        }
    }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    hps::Settings settings = hps::Settings::LoadOrDefault();

    if (testCap) {
        hps::RunVolumeCapTestLoop(settings.headroomDb);
    } else {
        while (true) {
            if (settings.volumeCapEnabled) {
                hps::EnforceVolumeCap(settings.headroomDb);
            }
            Sleep(350);
        }
    }

    CoUninitialize();
    if (argv) LocalFree(argv);
    return 0;
}
