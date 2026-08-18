// force_revert_default: an independent, out-of-process safety net for live Real-Time Limiter
// testing (hard-won lesson #6, docs/windows-port.md). Replaces the old shared_state_write.exe +
// watchdog.ps1 pair from the Approach A (APO) design: that mechanism forced the limiter off by
// writing limiterEnabled=0 into shared memory for HeadphoneSafetyApo.dll to notice - but Approach
// B's limiter runs in-process inside HeadphoneSafetyTray.exe itself, so if that process is hung
// (the exact failure mode this tool exists to cover), it will not be reading shared memory either.
//
// This tool makes its own direct IPolicyConfig::SetDefaultEndpoint call to switch the OS default
// render device away from VB-Cable, with zero dependency on the tray process being alive or
// responsive at all - it does not talk to the tray process in any way. Reuses
// DefaultDeviceSwitcher.cpp/VBCableDetector.cpp unmodified (compiled directly into this tool)
// rather than re-implementing the same COM calls a second time.
//
// Usage: this tool has no memory of its own "last known real device" - like the tray's own
// startup check (RevertStrayVBCableDefaultOutput in main.cpp), it falls back to "any active
// render device that isn't VB-Cable's own endpoint" rather than requiring a specific target
// device to be passed in.
#include <windows.h>
#include <mmdeviceapi.h>
#include <initguid.h>

#include <cstdio>
#include <string>

#include "../tray/DefaultDeviceSwitcher.h"
#include "../tray/VBCableDetector.h"

int wmain() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    hps::VBCableInfo cable = hps::FindVBCable();
    if (!cable.found) {
        wprintf(L"force_revert_default: VB-Cable not found - nothing to revert.\n");
        CoUninitialize();
        return 1;
    }

    std::wstring current = hps::GetCurrentDefaultRenderDeviceId();
    wprintf(L"force_revert_default: current default = %ls\n", current.c_str());
    if (current != cable.deviceId) {
        wprintf(L"force_revert_default: default is not VB-Cable - nothing to do.\n");
        CoUninitialize();
        return 0;
    }

    wprintf(L"force_revert_default: default is VB-Cable (%ls) - reverting.\n",
            cable.deviceId.c_str());

    IMMDeviceEnumerator* enumerator = nullptr;
    bool reverted = false;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator),
                                    reinterpret_cast<void**>(&enumerator)))) {
        IMMDeviceCollection* collection = nullptr;
        if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection)) &&
            collection) {
            UINT count = 0;
            collection->GetCount(&count);
            for (UINT i = 0; i < count && !reverted; ++i) {
                IMMDevice* candidate = nullptr;
                if (FAILED(collection->Item(i, &candidate)) || !candidate) continue;
                LPWSTR idStr = nullptr;
                if (SUCCEEDED(candidate->GetId(&idStr)) && idStr) {
                    std::wstring candidateId = idStr;
                    CoTaskMemFree(idStr);
                    if (candidateId != cable.deviceId) {
                        bool ok = hps::SetDefaultRenderDevice(candidateId);
                        Sleep(300);
                        std::wstring after = hps::GetCurrentDefaultRenderDeviceId();
                        reverted = (after == candidateId);
                        wprintf(L"force_revert_default: switched to %ls (SetDefaultRenderDevice "
                                L"%ls, readback verified: %ls)\n",
                                candidateId.c_str(), ok ? L"succeeded" : L"FAILED",
                                reverted ? L"YES" : L"NO");
                    }
                }
                candidate->Release();
            }
            collection->Release();
        }
        enumerator->Release();
    }

    CoUninitialize();
    if (!reverted) {
        wprintf(L"force_revert_default: could not find any non-VB-Cable device to revert to.\n");
        return 1;
    }
    return 0;
}
