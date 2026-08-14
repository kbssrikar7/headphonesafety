// HeadphoneSafetyTray entry point.
//
// Phase 1 built Volume Cap enforcement (--test-cap allocates a console and logs every poll tick;
// see VolumeCap.cpp - untouched by Phase 3). Phase 3 adds SharedStateServer: the tray's normal
// (non-test-cap) background loop now also pushes the current limiter enabled/headroom settings
// into the shared-memory mapping every tick, so HeadphoneSafetyApo.dll's real-time thread can
// read them. --enable-limiter/--disable-limiter persist that setting and exit immediately - no
// tray UI exists yet (Phase 5 adds it), so this is how Phase 3's live end-to-end test is driven:
// run --enable-limiter once, then launch the tray normally so its loop keeps pushing the live
// setting while a separate process plays audio through the registered APO.
//
// --test-cap intentionally stays exactly as it was in Phase 1 (does not push shared state) -
// RunVolumeCapTestLoop is VolumeCap.cpp's self-contained loop and Phase 3 does not modify
// VolumeCap.cpp. Use the normal (no-flag) launch mode for a combined Volume Cap + limiter test.
#include <windows.h>
#include <shellapi.h>

#include <cstdio>

#include "Settings.h"
#include "SharedStateServer.h"
#include "VolumeCap.h"

namespace {

bool HasFlag(int argc, wchar_t** argv, const wchar_t* flag) {
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], flag) == 0) return true;
    }
    return false;
}

void EnsureConsoleIfNeeded() {
    // Only force a fresh console window if stdout wasn't already inherited from the parent (e.g.
    // a pipe when launched from a terminal/test harness) - forcing AllocConsole+CONOUT$
    // unconditionally would redirect away from an already-valid inherited pipe handle into a
    // separate, unobservable window instead. This subsystem is WIN32 (no console auto-attached),
    // but an inherited stdout handle from CreateProcess still works via the CRT regardless of
    // subsystem.
    if (GetStdHandle(STD_OUTPUT_HANDLE) == nullptr) {
        AllocConsole();
        FILE* dummy = nullptr;
        freopen_s(&dummy, "CONOUT$", "w", stdout);
    }
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool testCap = argv && HasFlag(argc, argv, L"--test-cap");
    bool enableLimiter = argv && HasFlag(argc, argv, L"--enable-limiter");
    bool disableLimiter = argv && HasFlag(argc, argv, L"--disable-limiter");

    if (enableLimiter || disableLimiter) {
        EnsureConsoleIfNeeded();
        hps::Settings settings = hps::Settings::LoadOrDefault();
        settings.limiterEnabled = enableLimiter;
        settings.Save();
        printf("Limiter setting saved: %s (headroom %.1f dB)\n",
               settings.limiterEnabled ? "enabled" : "disabled", settings.headroomDb);
        if (argv) LocalFree(argv);
        return 0;
    }

    if (testCap) {
        EnsureConsoleIfNeeded();
    }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    hps::Settings settings = hps::Settings::LoadOrDefault();

    if (testCap) {
        hps::RunVolumeCapTestLoop(settings.headroomDb);
    } else {
        hps::SharedStateServer sharedState;
        sharedState.SetLimiterEnabled(settings.limiterEnabled);
        sharedState.SetHeadroomDb(settings.headroomDb);

        while (true) {
            if (settings.volumeCapEnabled) {
                hps::EnforceVolumeCap(settings.headroomDb);
            }
            // Re-pushed every tick, not just once at startup, so a future Phase 5 tray menu that
            // mutates `settings` interactively only has to change the in-memory struct - this
            // loop already keeps the shared mapping in sync with it.
            sharedState.SetLimiterEnabled(settings.limiterEnabled);
            sharedState.SetHeadroomDb(settings.headroomDb);
            sharedState.Heartbeat();
            Sleep(350);
        }
    }

    CoUninitialize();
    if (argv) LocalFree(argv);
    return 0;
}
