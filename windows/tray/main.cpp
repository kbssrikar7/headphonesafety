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
#include <mmdeviceapi.h>
#include <endpointvolume.h>

#include <cstdio>
#include <string>

#include "DefaultDeviceSwitcher.h"
#include "DeviceWatcher.h"
#include "LimiterEngine.h"
#include "LimiterStatus.h"
#include "Settings.h"
#include "SharedStateServer.h"
#include "TimeoutRunner.h"
#include "TrayIcon.h"
#include "VBCableDetector.h"
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
    hps::LimiterStatus* limiterStatus;
    volatile LONG* stopFlag;
};

// Mirrors macOS's AudioMonitor.swift's revertStrayBlackHoleDefaultOutput(), called unconditionally
// at every launch, before anything else starts - confirmed live during this project's own build
// that VB-Cable's installer itself already leaves VB-Cable as the system default output (not
// hypothetical: this happened on this exact dev machine), so this check is not just for recovering
// from an unclean shutdown of this app, it also covers "something else set VB-Cable as default."
// Falls back to a device classified as speakers (via the existing VolumeCap.cpp classification,
// reused rather than reimplemented) if the cached-nothing case leaves no better option.
void RevertStrayVBCableDefaultOutput() {
    hps::VBCableInfo cable = hps::FindVBCable();
    if (!cable.found) return;

    std::wstring current = hps::GetCurrentDefaultRenderDeviceId();
    if (current.empty() || current != cable.deviceId) return;

    fwprintf(stderr,
              L"[startup] default output is stray VB-Cable at launch - reverting "
              L"unconditionally\n");

    // No prior-device memory is available at this point (this is a fresh launch, possibly the
    // very first one) - fall back to enumerating active render endpoints the same way
    // VBCableDetector does, and pick one that isn't VB-Cable's own endpoint. Two-pass: prefer a
    // candidate that isn't a Bluetooth voice/HFP endpoint (see
    // IsLikelyBluetoothVoiceEndpoint's header comment - reverting to a device's low-quality
    // call-mode endpoint instead of its music endpoint would be a silently wrong recovery choice),
    // falling back to the first non-cable candidate at all if every candidate looks like a voice
    // endpoint (better than refusing to revert at all).
    IMMDeviceEnumerator* enumerator = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator),
                                    reinterpret_cast<void**>(&enumerator)))) {
        IMMDeviceCollection* collection = nullptr;
        if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection)) &&
            collection) {
            UINT count = 0;
            collection->GetCount(&count);
            std::wstring firstNonCableCandidate;
            std::wstring preferredCandidate;
            for (UINT i = 0; i < count; ++i) {
                IMMDevice* candidate = nullptr;
                if (FAILED(collection->Item(i, &candidate)) || !candidate) continue;
                LPWSTR idStr = nullptr;
                if (SUCCEEDED(candidate->GetId(&idStr)) && idStr) {
                    std::wstring candidateId = idStr;
                    CoTaskMemFree(idStr);
                    if (candidateId != cable.deviceId) {
                        if (firstNonCableCandidate.empty()) firstNonCableCandidate = candidateId;
                        if (preferredCandidate.empty() &&
                            !hps::IsLikelyBluetoothVoiceEndpoint(candidateId)) {
                            preferredCandidate = candidateId;
                        }
                    }
                }
                candidate->Release();
            }
            collection->Release();
            enumerator->Release();

            std::wstring chosen =
                !preferredCandidate.empty() ? preferredCandidate : firstNonCableCandidate;
            if (!chosen.empty()) {
                hps::SetDefaultRenderDevice(chosen);
                Sleep(300);
                std::wstring after = hps::GetCurrentDefaultRenderDeviceId();
                fwprintf(stderr, L"[startup] reverted to %ls, verified: %ls\n", chosen.c_str(),
                          (after == chosen) ? L"YES" : L"NO");
                return;
            }
        } else {
            enumerator->Release();
        }
    }
    fwprintf(stderr, L"[startup] could not find any non-VB-Cable device to revert to\n");
}

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

    // Limiter lifecycle state - mirrors macOS AudioMonitor.swift's LimiterState enum. realDeviceId
    // is cached once, at the moment limiting starts, and never re-resolved while active (hard-won
    // lesson #3: don't re-query what you're actively routing through).
    bool limiting = false;
    std::wstring cachedRealDeviceId;
    hps::LimiterEngine limiterEngine;
    volatile LONG engineFailed = 0;
    limiterEngine.SetFailureCallback([&engineFailed]() {
        // Runs on LimiterEngine's own thread - must not call Stop() here (would deadlock joining
        // its own thread). Just flag it; the poll loop below reacts on its own thread.
        InterlockedExchange(&engineFailed, 1);
    });

    // Central revert function every failure/toggle-off path funnels through - mirrors macOS
    // revertLimiter()'s single-function design rather than divergent ad-hoc reverts.
    auto revertLimiter = [&]() {
        if (!limiting) {
            limiterEngine.Stop();
            return;
        }
        // Switch the default device AWAY from VB-Cable FIRST, verified, THEN stop the engine -
        // same order-matters reasoning as attemptStartLimiter below, mirrored from macOS's
        // revertLimiter(). Try the cached real device first; if that fails (e.g. it was
        // unplugged while limiting), fall back to any non-VB-Cable device the same way the
        // startup stray-default check does.
        hps::VBCableInfo cable = hps::FindVBCable();
        hps::SetDefaultRenderDevice(cachedRealDeviceId);
        Sleep(100);
        std::wstring after = hps::GetCurrentDefaultRenderDeviceId();
        if (after != cachedRealDeviceId) {
            fwprintf(stderr,
                      L"[background] revert to cached real device failed, falling back to "
                      L"stray-VB-Cable recovery logic\n");
            RevertStrayVBCableDefaultOutput();
        }
        // Discard the change flag OUR OWN switch(es) above just set - otherwise the next tick
        // sees deviceChanged==true from an event we caused ourselves, not an external change,
        // and would immediately re-trigger the limiter lifecycle again. Without this, a switch
        // and its own OnDefaultDeviceChanged notification form a self-sustaining feedback loop
        // (confirmed live: start/revert oscillating every ~350ms with no external cause).
        deviceWatcher.ConsumeChangeFlag();
        limiting = false;
        limiterEngine.Stop();
        ctx->limiterStatus->Set(false, std::wstring());
        fwprintf(stderr, L"[background] limiter reverted\n");
    };

    // Mirrors macOS attemptStartLimiter(): order matters. VB-Cable must already be the confirmed
    // default output BEFORE capture attaches, or its render cycle can lock onto a "nothing routed
    // yet" timeline and never pick up audio that starts flowing afterward - a real, non-obvious
    // bug already found once on macOS's equivalent code, preserved here rather than re-discovered.
    auto attemptStartLimiter = [&]() -> bool {
        hps::VBCableInfo cable = hps::FindVBCable();
        if (!cable.found) {
            fwprintf(stderr, L"[background] VB-Cable not installed - cannot start limiter\n");
            return false;
        }

        std::wstring realDevice = hps::GetCurrentDefaultRenderDeviceId();
        if (realDevice.empty() || realDevice == cable.deviceId) {
            fwprintf(stderr, L"[background] no valid real device to protect - cannot start "
                              L"limiter\n");
            return false;
        }

        // Refuse rather than silently "protect" a Bluetooth Hands-Free/voice-call endpoint - see
        // IsLikelyBluetoothVoiceEndpoint's header comment. Checked BEFORE ever switching the OS
        // default to VB-Cable, so a blocked attempt leaves the user's actual audio routing
        // completely untouched.
        if (hps::IsLikelyBluetoothVoiceEndpoint(realDevice)) {
            fwprintf(stderr,
                      L"[background] default output %ls looks like a Bluetooth call/voice "
                      L"endpoint (low sample rate) - refusing to start the limiter rather than "
                      L"silently protecting a voice channel instead of music\n",
                      realDevice.c_str());
            ctx->limiterStatus->SetBlockedReason(
                L"Bluetooth device is in call/voice mode - switch it back to music playback");
            return false;
        }

        if (!hps::SetDefaultRenderDevice(cable.deviceId)) {
            fwprintf(stderr, L"[background] SetDefaultRenderDevice(VB-Cable) reported failure\n");
            return false;
        }
        Sleep(200);
        std::wstring afterSwitch = hps::GetCurrentDefaultRenderDeviceId();
        // Discard the change flag this switch itself just set - see the matching comment in
        // revertLimiter for why (self-caused notification, not an external change to react to).
        deviceWatcher.ConsumeChangeFlag();
        if (afterSwitch != cable.deviceId) {
            fwprintf(stderr, L"[background] switch to VB-Cable did not verify via readback\n");
            return false;
        }

        if (!limiterEngine.Start(realDevice, cable.deviceId, ctx->settings->headroomDb)) {
            fwprintf(stderr, L"[background] LimiterEngine::Start failed - reverting default "
                              L"device\n");
            hps::SetDefaultRenderDevice(realDevice);
            Sleep(200);
            if (hps::GetCurrentDefaultRenderDeviceId() != realDevice) {
                RevertStrayVBCableDefaultOutput();
            }
            deviceWatcher.ConsumeChangeFlag();
            return false;
        }

        cachedRealDeviceId = realDevice;
        limiting = true;
        std::wstring realDeviceName = hps::GetFriendlyName(realDevice);
        if (realDeviceName.empty()) realDeviceName = realDevice;
        ctx->limiterStatus->Set(true, realDeviceName);
        fwprintf(stderr, L"[background] limiter started, protecting %ls\n", realDevice.c_str());
        return true;
    };

    while (!InterlockedCompareExchange(ctx->stopFlag, 0, 0)) {
        // Unconditional per hard-won lesson #4 - a device-list change just means "try again now
        // instead of waiting out the rest of this cycle," not "go query what changed."
        bool deviceChanged = deviceWatcher.ConsumeChangeFlag();
        if (deviceChanged) {
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

        // Limiter lifecycle, mirroring macOS's tick()/tickIdle()/tickLimiting() split.
        bool wantLimiting = ctx->settings->limiterEnabled;
        bool failedThisTick = InterlockedExchange(&engineFailed, 0) != 0;

        if (limiting) {
            // Single cheap system-level check, not a per-device query - a device-change
            // notification or engine failure both mean "assume something's wrong, revert
            // unconditionally," not "go figure out what changed" (hard-won lesson #4).
            std::wstring currentDefault = hps::GetCurrentDefaultRenderDeviceId();
            hps::VBCableInfo cable = hps::FindVBCable();
            bool stillOnCable = cable.found && currentDefault == cable.deviceId;

            if (!wantLimiting || deviceChanged || failedThisTick || !stillOnCable ||
                !limiterEngine.IsRunning()) {
                revertLimiter();
                if (wantLimiting) {
                    attemptStartLimiter();
                }
            } else {
                limiterEngine.UpdateHeadroomDb(ctx->settings->headroomDb);
            }
        } else if (wantLimiting) {
            attemptStartLimiter();
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

    // Process is exiting - if still limiting, revert cleanly rather than leaving default output
    // stuck on VB-Cable (the startup check would catch this next launch regardless, but reverting
    // now means the user is not left silently listening to nothing in the meantime).
    if (limiting) {
        revertLimiter();
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
    bool testSwitch = argv && HasFlag(argc, argv, L"--test-switch");
    bool printDefault = argv && HasFlag(argc, argv, L"--print-default");
    bool printVolume = argv && HasFlag(argc, argv, L"--print-volume");

    if (printVolume) {
        // Diagnostic only, not part of the shipped app: prints each device's own master
        // (endpoint) volume in dB. Used to rule out "the two devices just have different Windows
        // volume slider positions" as a confound when comparing WASAPI loopback captures taken
        // from two different endpoints (real device vs VB-Cable) - that comparison is only valid
        // if both devices' own master volume is equal (or known), since loopback capture reflects
        // the post-master-volume mix.
        EnsureConsoleIfNeeded();
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        auto printDeviceVolume = [](const wchar_t* label, const std::wstring& deviceId) {
            IMMDeviceEnumerator* enumerator = nullptr;
            if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                         __uuidof(IMMDeviceEnumerator),
                                         reinterpret_cast<void**>(&enumerator)))) {
                wprintf(L"%ls: enumerator failed\n", label);
                return;
            }
            IMMDevice* device = nullptr;
            HRESULT hr = enumerator->GetDevice(deviceId.c_str(), &device);
            enumerator->Release();
            if (FAILED(hr) || !device) {
                wprintf(L"%ls: GetDevice failed (0x%08x)\n", label, hr);
                return;
            }
            IAudioEndpointVolume* volume = nullptr;
            hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                   reinterpret_cast<void**>(&volume));
            device->Release();
            if (FAILED(hr) || !volume) {
                wprintf(L"%ls: Activate(IAudioEndpointVolume) failed (0x%08x)\n", label, hr);
                return;
            }
            float currentDb = 0.0f, minDb = 0.0f, maxDb = 0.0f, stepDb = 0.0f;
            volume->GetMasterVolumeLevel(&currentDb);
            volume->GetVolumeRange(&minDb, &maxDb, &stepDb);
            float scalar = 0.0f;
            volume->GetMasterVolumeLevelScalar(&scalar);
            BOOL muted = FALSE;
            volume->GetMute(&muted);
            wprintf(L"%ls: %.2f dB (range %.2f..%.2f), scalar=%.3f, muted=%ls\n", label, currentDb,
                    minDb, maxDb, scalar, muted ? L"YES" : L"NO");
            volume->Release();
        };

        hps::VBCableInfo cable = hps::FindVBCable();
        if (cable.found) {
            printDeviceVolume(L"VB-Cable", cable.deviceId);
        } else {
            wprintf(L"VB-Cable: not found\n");
        }
        int devIndex = -1;
        for (int i = 1; i < argc; ++i) {
            if (wcscmp(argv[i], L"--device") == 0 && i + 1 < argc) {
                devIndex = i + 1;
                break;
            }
        }
        if (devIndex >= 0) {
            printDeviceVolume(L"Device", argv[devIndex]);
        }

        CoUninitialize();
        if (argv) LocalFree(argv);
        return 0;
    }

    bool setVolume = argv && HasFlag(argc, argv, L"--set-volume");
    if (setVolume) {
        // Diagnostic only: sets a device's master (endpoint) volume to a specific scalar
        // (0.0..1.0). Used once, live, to equalize VB-Cable's and the real device's volume before
        // a WASAPI-loopback-based before/after comparison, after discovering the two devices had
        // very different independent volume slider positions (VB-Cable at 100%, Speakers at 44%)
        // - a real confound for that kind of cross-device comparison, unrelated to the limiter
        // DSP itself.
        EnsureConsoleIfNeeded();
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        int devIndex = -1, scalarIndex = -1;
        for (int i = 1; i < argc; ++i) {
            if (wcscmp(argv[i], L"--device") == 0 && i + 1 < argc) devIndex = i + 1;
            if (wcscmp(argv[i], L"--scalar") == 0 && i + 1 < argc) scalarIndex = i + 1;
        }
        if (devIndex < 0 || scalarIndex < 0) {
            wprintf(L"Usage: --set-volume --device <id> --scalar <0.0..1.0>\n");
            if (argv) LocalFree(argv);
            return 1;
        }
        float targetScalar = static_cast<float>(_wtof(argv[scalarIndex]));
        IMMDeviceEnumerator* enumerator = nullptr;
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
        IMMDevice* device = nullptr;
        HRESULT hr = enumerator ? enumerator->GetDevice(argv[devIndex], &device) : E_FAIL;
        if (enumerator) enumerator->Release();
        if (FAILED(hr) || !device) {
            wprintf(L"GetDevice failed: 0x%08x\n", hr);
            if (argv) LocalFree(argv);
            return 1;
        }
        IAudioEndpointVolume* volume = nullptr;
        hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                               reinterpret_cast<void**>(&volume));
        device->Release();
        if (FAILED(hr) || !volume) {
            wprintf(L"Activate failed: 0x%08x\n", hr);
            if (argv) LocalFree(argv);
            return 1;
        }
        hr = volume->SetMasterVolumeLevelScalar(targetScalar, nullptr);
        float afterScalar = 0.0f;
        volume->GetMasterVolumeLevelScalar(&afterScalar);
        wprintf(L"SetMasterVolumeLevelScalar(%.3f): %ls, readback scalar=%.3f\n", targetScalar,
                SUCCEEDED(hr) ? L"success" : L"FAILED", afterScalar);
        volume->Release();
        CoUninitialize();
        if (argv) LocalFree(argv);
        return 0;
    }

    if (printDefault) {
        // Read-only readback for live testing - deliberately does not touch anything, unlike
        // --test-switch, so it can be run concurrently with a real tray instance to observe its
        // effect on the OS default device without interfering.
        EnsureConsoleIfNeeded();
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::wstring current = hps::GetCurrentDefaultRenderDeviceId();
        hps::VBCableInfo cable = hps::FindVBCable();
        wprintf(L"default=%ls\n", current.c_str());
        wprintf(L"vbcable=%ls found=%ls\n", cable.deviceId.c_str(), cable.found ? L"YES" : L"NO");
        wprintf(L"onVBCable=%ls\n",
                (cable.found && current == cable.deviceId) ? L"YES" : L"NO");
        CoUninitialize();
        if (argv) LocalFree(argv);
        return 0;
    }

    if (testSwitch) {
        EnsureConsoleIfNeeded();
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        // --test-switch --to <deviceId>: one-shot "switch default output to exactly this device
        // id and stop" mode - a recovery tool as much as a test, since VB-Cable's own installer
        // was found live to have already set VB-Cable as the system default output (a stray-
        // default-device situation this app must handle at startup for exactly this reason).
        int toIndex = -1;
        for (int i = 1; i < argc; ++i) {
            if (wcscmp(argv[i], L"--to") == 0 && i + 1 < argc) {
                toIndex = i + 1;
                break;
            }
        }
        if (toIndex >= 0) {
            std::wstring target = argv[toIndex];
            wprintf(L"Switching default output to: %ls\n", target.c_str());
            bool ok = hps::SetDefaultRenderDevice(target);
            Sleep(300);
            std::wstring after = hps::GetCurrentDefaultRenderDeviceId();
            wprintf(L"  SetDefaultRenderDevice reported %ls; readback default is now: %ls\n",
                    ok ? L"success" : L"FAILURE", after.c_str());
            wprintf(L"  Verified: %ls\n", (after == target) ? L"YES" : L"NO");
            CoUninitialize();
            if (argv) LocalFree(argv);
            return 0;
        }

        std::wstring original = hps::GetCurrentDefaultRenderDeviceId();
        wprintf(L"Original default device id: %ls\n", original.c_str());
        if (original.empty()) {
            wprintf(L"Could not determine current default device - aborting test.\n");
        } else {
            hps::VBCableInfo cable = hps::FindVBCable();
            if (!cable.found) {
                wprintf(L"VB-Cable not found (CABLE Input (VB-Audio Virtual Cable)) - install it "
                        L"first.\n");
            } else {
                wprintf(L"VB-Cable device id: %ls\n", cable.deviceId.c_str());
                wprintf(L"Switching default to VB-Cable...\n");
                bool switchHr = hps::SetDefaultRenderDevice(cable.deviceId);
                Sleep(300);
                std::wstring afterSwitch = hps::GetCurrentDefaultRenderDeviceId();
                wprintf(L"  SetDefaultRenderDevice reported %ls; readback default is now: %ls\n",
                        switchHr ? L"success" : L"FAILURE",
                        afterSwitch.c_str());
                wprintf(L"  Verified switched to VB-Cable: %ls\n",
                        (afterSwitch == cable.deviceId) ? L"YES" : L"NO");

                wprintf(L"Switching back to original device...\n");
                bool revertHr = hps::SetDefaultRenderDevice(original);
                Sleep(300);
                std::wstring afterRevert = hps::GetCurrentDefaultRenderDeviceId();
                wprintf(L"  SetDefaultRenderDevice reported %ls; readback default is now: %ls\n",
                        revertHr ? L"success" : L"FAILURE",
                        afterRevert.c_str());
                wprintf(L"  Verified reverted to original: %ls\n",
                        (afterRevert == original) ? L"YES" : L"NO");
            }
        }

        CoUninitialize();
        if (argv) LocalFree(argv);
        return 0;
    }

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

        // Unconditional, every launch - mirrors macOS's revertStrayBlackHoleDefaultOutput()
        // called at AudioMonitor.start(). Not just crash recovery: VB-Cable's own installer was
        // found live to leave VB-Cable as the system default output with no app of ours ever
        // having run yet, so this must run regardless of how the last session ended.
        RevertStrayVBCableDefaultOutput();

        hps::SharedStateServer sharedState;
        sharedState.SetLimiterEnabled(settings.limiterEnabled);
        sharedState.SetHeadroomDb(settings.headroomDb);

        hps::LimiterStatus limiterStatus;
        volatile LONG stopFlag = 0;
        BackgroundContext ctx{&settings, &sharedState, &limiterStatus, &stopFlag};
        HANDLE backgroundThread = CreateThread(nullptr, 0, &BackgroundThreadProc, &ctx, 0, nullptr);

        hps::TrayIcon trayIcon(hInstance, &settings, &sharedState, &limiterStatus);
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
