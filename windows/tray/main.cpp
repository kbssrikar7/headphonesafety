// HeadphoneSafetyTray entry point.
//
// Phase 1 built Volume Cap enforcement (--test-cap allocates a console and logs every poll tick;
// see VolumeCap.cpp - untouched since). Phase 3 added SharedStateServer (pushes limiter
// enabled/headroom into shared memory for HeadphoneSafetyApo.dll's real-time thread to read).
// Phase 4 hardened the background loop: TimeoutRunner (hard-won lesson #3 - a hung Win32/COM call
// against a mid-disconnect device can never freeze this loop forever) and DeviceWatcher (hard-won
// lesson #4 - react to device-list changes unconditionally, without querying the specific device
// that changed).
//
// Phase 5 (this revision): adds the real Shell_NotifyIcon tray UI (TrayIcon.h/.cpp). This requires
// a real Win32 message loop on the main thread (Shell_NotifyIcon/TrackPopupMenu both need one),
// which the old single-threaded "while (true) { ...; Sleep(350); }" design didn't have - that
// background work now runs on a dedicated worker thread instead, leaving the main thread free to
// pump messages for TrayIcon's hidden window. See BackgroundThreadProc below.
//
// --test-cap/--enable-limiter/--disable-limiter all continue to exit before any of this UI/thread
// machinery starts, exactly as in every prior phase.
#include <windows.h>
#include <shellapi.h>

#include <cstdio>

#include "DeviceWatcher.h"
#include "Settings.h"
#include "SharedStateServer.h"
#include "TimeoutRunner.h"
#include "TrayIcon.h"
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

struct BackgroundContext {
    hps::Settings* settings;
    hps::SharedStateServer* sharedState;
    volatile LONG* stopFlag;
};

// Runs on its own thread so a hung Win32/COM call (TimeoutRunner's whole reason to exist) can
// never freeze the main thread's message pump, which TrayIcon's Shell_NotifyIcon/context menu
// depend on staying responsive.
//
// Reads settings->headroomDb/volumeCapEnabled/limiterEnabled without a lock, concurrently with
// TrayIcon mutating them on the main thread in response to menu clicks. This is an accepted,
// benign race, not an oversight: each field is a native-width (bool or 8-byte-aligned double)
// value that doesn't tear on x86/x64, and using a one-tick-stale value here has the same harmless
// consequence hps_shared_state.h's own IPC fields already accept ("eventually consistent... a
// one-buffer-stale value is inaudible"). Adding a lock would add real complexity for a benefit
// that doesn't matter for this data.
DWORD WINAPI BackgroundThreadProc(LPVOID param) {
    auto* ctx = reinterpret_cast<BackgroundContext*>(param);

    // COM apartments are per-thread - the main thread's CoInitializeEx does not cover this
    // thread. Both threads use COINIT_MULTITHREADED, so they join the same process-wide MTA;
    // that's what makes it safe for CoCreateInstance-produced interface pointers to work
    // correctly per-thread without needing cross-apartment marshaling.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    hps::DeviceWatcher deviceWatcher;
    if (!deviceWatcher.IsRegistered()) {
        fwprintf(stderr,
                  L"[background] DeviceWatcher failed to register - falling back to plain "
                  L"350ms polling only (no fast reaction to device changes)\n");
    }

    hps::TimeoutRunner pollRunner;
    constexpr DWORD kPollTimeoutMs = 2000;

    while (!InterlockedCompareExchange(ctx->stopFlag, 0, 0)) {
        // Unconditional per hard-won lesson #4 - a device-list change just means "try again now
        // instead of waiting out the rest of this cycle," not "go query what changed."
        if (deviceWatcher.ConsumeChangeFlag()) {
            fwprintf(stderr, L"[background] device change detected, polling immediately\n");
        }

        if (ctx->settings->volumeCapEnabled) {
            double headroomDbSnapshot = ctx->settings->headroomDb;
            bool completed = pollRunner.Run(
                [headroomDbSnapshot]() { hps::EnforceVolumeCap(headroomDbSnapshot); },
                kPollTimeoutMs);
            if (!completed) {
                fwprintf(stderr,
                          L"[background] poll tick appears hung (exceeded %lums) - skipping "
                          L"this tick, will retry next cycle\n",
                          kPollTimeoutMs);
            }
        }

        ctx->sharedState->SetLimiterEnabled(ctx->settings->limiterEnabled);
        ctx->sharedState->SetHeadroomDb(ctx->settings->headroomDb);
        ctx->sharedState->Heartbeat();
        fflush(stderr);

        // Slept in small increments rather than one Sleep(350) so the stop flag (checked at the
        // top of this loop) is noticed within ~50ms of being set, not up to 350ms late - process
        // shutdown doesn't need to be instant, but shouldn't be needlessly sluggish either.
        for (int i = 0; i < 7 && !InterlockedCompareExchange(ctx->stopFlag, 0, 0); ++i) {
            Sleep(50);
        }
    }

    CoUninitialize();
    return 0;
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
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
        // Diagnostic-only: makes background-thread/DeviceWatcher log lines observable when this
        // is launched from a terminal for live testing. If launched normally (e.g. an eventual
        // autostart entry, no inherited console), this is a silent no-op.
        EnsureConsoleIfNeeded();

        hps::SharedStateServer sharedState;
        sharedState.SetLimiterEnabled(settings.limiterEnabled);
        sharedState.SetHeadroomDb(settings.headroomDb);

        volatile LONG stopFlag = 0;
        BackgroundContext ctx{&settings, &sharedState, &stopFlag};
        HANDLE backgroundThread = CreateThread(nullptr, 0, &BackgroundThreadProc, &ctx, 0, nullptr);

        hps::TrayIcon trayIcon(hInstance, &settings, &sharedState);
        if (!trayIcon.IsValid()) {
            fwprintf(stderr, L"[main] TrayIcon failed to create its window - no tray icon will "
                              L"be shown, but Volume Cap keeps running in the background\n");
        }

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        // WM_QUIT received (from TrayIcon's Quit menu item) - stop the background thread before
        // exiting. TimeoutRunner inside it always returns within kPollTimeoutMs per call, so the
        // loop notices the stop flag within roughly that bound even in the worst case (a
        // genuinely hung poll tick) - 3000ms comfortably covers that; if it somehow still hasn't
        // joined by then, proceed anyway rather than hang process exit on it (process teardown
        // reclaims the thread regardless).
        if (backgroundThread) {
            InterlockedExchange(&stopFlag, 1);
            WaitForSingleObject(backgroundThread, 3000);
            CloseHandle(backgroundThread);
        }
    }

    CoUninitialize();
    if (argv) LocalFree(argv);
    return 0;
}
