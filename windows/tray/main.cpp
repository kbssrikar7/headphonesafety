// HeadphoneSafetyTray entry point.
//
// Phase 1 built Volume Cap enforcement (--test-cap allocates a console and logs every poll tick;
// see VolumeCap.cpp - untouched by Phase 3/4). Phase 3 adds SharedStateServer: the tray's normal
// (non-test-cap) background loop also pushes the current limiter enabled/headroom settings into
// the shared-memory mapping every tick, so HeadphoneSafetyApo.dll's real-time thread can read
// them. --enable-limiter/--disable-limiter persist that setting and exit immediately - no tray UI
// exists yet (Phase 5 adds it), so this is how a live end-to-end limiter test is driven: run
// --enable-limiter once, then launch the tray normally so its loop keeps pushing the live setting
// while a separate process plays audio through the registered APO.
//
// Phase 4 hardens the normal-mode loop per two of the macOS build's hard-won lessons
// (docs/windows-port.md): #3, every poll tick's Win32/COM work now runs through a TimeoutRunner
// so a hang (e.g. a device mid-disconnect) can never freeze this loop forever; #4, a DeviceWatcher
// (IMMNotificationClient) reacts to device-list changes unconditionally, without ever querying
// the specific device that changed, by just flagging the next regular poll tick to happen
// immediately rather than waiting out the rest of the 350ms cadence.
//
// --test-cap intentionally stays exactly as it was in Phase 1 (does not push shared state, does
// not use TimeoutRunner/DeviceWatcher) - RunVolumeCapTestLoop is VolumeCap.cpp's self-contained
// loop and this file does not modify VolumeCap.cpp. Use the normal (no-flag) launch mode for a
// combined Volume Cap + limiter test, or to exercise the Phase 4 hardening.
#include <windows.h>
#include <shellapi.h>

#include <cstdio>

#include "DeviceWatcher.h"
#include "Settings.h"
#include "SharedStateServer.h"
#include "TimeoutRunner.h"
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
        // Diagnostic-only: makes DeviceWatcher/TimeoutRunner log lines observable when this is
        // launched from a terminal for live testing, exactly like --test-cap already does. If
        // launched normally (e.g. an eventual autostart entry, no inherited console), this is a
        // silent no-op - EnsureConsoleIfNeeded only allocates one when nothing was inherited, and
        // nothing here depends on a console actually being present.
        EnsureConsoleIfNeeded();

        hps::SharedStateServer sharedState;
        sharedState.SetLimiterEnabled(settings.limiterEnabled);
        sharedState.SetHeadroomDb(settings.headroomDb);

        hps::DeviceWatcher deviceWatcher;
        if (!deviceWatcher.IsRegistered()) {
            fwprintf(stderr,
                      L"[main] DeviceWatcher failed to register - falling back to plain "
                      L"350ms polling only (no fast reaction to device changes)\n");
        }

        hps::TimeoutRunner pollRunner;
        constexpr DWORD kPollTimeoutMs = 2000;

        while (true) {
            // Unconditional per hard-won lesson #4 - a device-list change just means "try again
            // now instead of waiting out the rest of this cycle," not "go query what changed."
            if (deviceWatcher.ConsumeChangeFlag()) {
                fwprintf(stderr, L"[main] device change detected, polling immediately\n");
            }

            if (settings.volumeCapEnabled) {
                // Capture by value, not by reference to `settings` - if this tick times out and
                // the worker thread is abandoned (see TimeoutRunner's class comment), it may still
                // be running when a later iteration mutates `settings` (once Phase 5's tray menu
                // can do that); a stale snapshot from the moment this tick started is safe, a
                // live reference into a struct that might be concurrently written is not.
                double headroomDbSnapshot = settings.headroomDb;
                bool completed = pollRunner.Run(
                    [headroomDbSnapshot]() { hps::EnforceVolumeCap(headroomDbSnapshot); },
                    kPollTimeoutMs);
                if (!completed) {
                    fwprintf(stderr,
                              L"[main] poll tick appears hung (exceeded %lums) - skipping this "
                              L"tick, will retry next cycle\n",
                              kPollTimeoutMs);
                }
            }

            // Re-pushed every tick, not just once at startup, so a future Phase 5 tray menu that
            // mutates `settings` interactively only has to change the in-memory struct - this
            // loop already keeps the shared mapping in sync with it.
            sharedState.SetLimiterEnabled(settings.limiterEnabled);
            sharedState.SetHeadroomDb(settings.headroomDb);
            sharedState.Heartbeat();
            fflush(stderr);
            Sleep(350);
        }
    }

    CoUninitialize();
    if (argv) LocalFree(argv);
    return 0;
}
