#include "VolumeCap.h"

#include <algorithm>
#include <cstdio>

#include <initguid.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <windows.h>

namespace hps {
namespace {

// Bluetooth heuristic fallback: many Bluetooth audio drivers leave PKEY_AudioEndpoint_JackSubType
// unset, so a friendly-name substring match is the second signal, combined the way
// docs/windows-port.md recommends ("Windows doesn't have as clean a single transport-type enum
// as macOS's kAudioDevicePropertyTransportType, so this may need a couple of heuristics
// combined").
bool NameLooksBluetooth(const std::wstring& name) {
    auto contains = [&](const wchar_t* needle) { return name.find(needle) != std::wstring::npos; };
    return contains(L"Bluetooth") || contains(L"Hands-Free") || contains(L"Hands Free");
}

std::wstring ReadStringProperty(IPropertyStore* store, REFPROPERTYKEY key) {
    PROPVARIANT var;
    PropVariantInit(&var);
    std::wstring result;
    if (SUCCEEDED(store->GetValue(key, &var)) && var.vt == VT_LPWSTR && var.pwszVal) {
        result = var.pwszVal;
    }
    PropVariantClear(&var);
    return result;
}

DeviceKind ClassifyDevice(IMMDevice* device) {
    IPropertyStore* store = nullptr;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &store)) || !store) {
        return DeviceKind::Unknown;
    }

    DeviceKind kind = DeviceKind::Speakers;

    PROPVARIANT formFactorVar;
    PropVariantInit(&formFactorVar);
    if (SUCCEEDED(store->GetValue(PKEY_AudioEndpoint_FormFactor, &formFactorVar)) &&
        formFactorVar.vt == VT_UI4) {
        auto formFactor = static_cast<EndpointFormFactor>(formFactorVar.ulVal);
        if (formFactor == Headphones || formFactor == Headset) {
            kind = DeviceKind::BuiltInHeadphones;
        }
    }
    PropVariantClear(&formFactorVar);

    std::wstring friendlyName = ReadStringProperty(store, PKEY_Device_FriendlyName);
    std::wstring interfaceName = ReadStringProperty(store, PKEY_DeviceInterface_FriendlyName);
    if (NameLooksBluetooth(friendlyName) || NameLooksBluetooth(interfaceName)) {
        kind = DeviceKind::Bluetooth;
    }

    store->Release();
    return kind;
}

}  // namespace

bool GetDefaultRenderDeviceSnapshot(DeviceSnapshot& outDevice, float& outCurrentDb,
                                     float& outMinDb, float& outMaxDb) {
    outDevice = DeviceSnapshot{};
    outCurrentDb = outMinDb = outMaxDb = 0.0f;

    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                 __uuidof(IMMDeviceEnumerator),
                                 reinterpret_cast<void**>(&enumerator)))) {
        return false;
    }

    IMMDevice* device = nullptr;
    HRESULT hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    enumerator->Release();
    if (FAILED(hr) || !device) {
        return false;
    }

    LPWSTR id = nullptr;
    if (SUCCEEDED(device->GetId(&id)) && id) {
        outDevice.id = id;
        CoTaskMemFree(id);
    }

    IPropertyStore* store = nullptr;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && store) {
        outDevice.friendlyName = ReadStringProperty(store, PKEY_Device_FriendlyName);
        store->Release();
    }

    outDevice.kind = ClassifyDevice(device);
    outDevice.valid = true;

    IAudioEndpointVolume* volume = nullptr;
    hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                           reinterpret_cast<void**>(&volume));
    device->Release();
    if (FAILED(hr) || !volume) {
        return false;
    }

    volume->GetMasterVolumeLevel(&outCurrentDb);

    float minDb = 0.0f, maxDb = 0.0f, stepDb = 0.0f;
    volume->GetVolumeRange(&minDb, &maxDb, &stepDb);
    outMinDb = minDb;
    outMaxDb = maxDb;

    volume->Release();
    return true;
}

bool EnforceVolumeCap(double headroomDb, DeviceSnapshot* outDevice, float* outAppliedDb) {
    DeviceSnapshot device;
    float currentDb = 0.0f, minDb = 0.0f, maxDb = 0.0f;
    if (!GetDefaultRenderDeviceSnapshot(device, currentDb, minDb, maxDb)) {
        return false;
    }
    if (outDevice) *outDevice = device;

    bool isHeadphoneLike =
        device.kind == DeviceKind::BuiltInHeadphones || device.kind == DeviceKind::Bluetooth;
    if (!isHeadphoneLike) {
        return false;
    }

    float ceiling = static_cast<float>(std::max<double>(minDb, maxDb - headroomDb));
    if (currentDb <= ceiling) {
        return false;
    }

    // Re-resolve the endpoint by its cached id rather than holding interfaces open across calls -
    // GetDefaultRenderDeviceSnapshot already released its own IMMDevice/IAudioEndpointVolume.
    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                 __uuidof(IMMDeviceEnumerator),
                                 reinterpret_cast<void**>(&enumerator)))) {
        return false;
    }
    IMMDevice* immDevice = nullptr;
    HRESULT hr = enumerator->GetDevice(device.id.c_str(), &immDevice);
    enumerator->Release();
    if (FAILED(hr) || !immDevice) {
        return false;
    }

    IAudioEndpointVolume* volume = nullptr;
    hr = immDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(&volume));
    immDevice->Release();
    if (FAILED(hr) || !volume) {
        return false;
    }

    volume->SetMasterVolumeLevel(ceiling, nullptr);

    // Verify by readback, not by trusting the HRESULT - hard-won lesson #1 (see the plan/doc):
    // a "successful" set call is not proof the change actually took effect.
    float verifyDb = 0.0f;
    volume->GetMasterVolumeLevel(&verifyDb);
    volume->Release();

    bool applied = verifyDb <= ceiling + 0.1f;  // small tolerance for float/step rounding
    if (applied && outAppliedDb) *outAppliedDb = verifyDb;
    return applied;
}

void RunVolumeCapTestLoop(double headroomDb) {
    // Piped stdout (e.g. a test harness capturing output, or `| Tee-Object`) is fully buffered by
    // the CRT by default, so nothing would appear until the buffer fills - and this loop never
    // exits normally, so a process killed at a timeout would lose everything. Flush after every
    // tick instead of relying on buffering/process-exit to do it.
    printf("Volume Cap test loop - headroom %.1f dB. Ctrl+C to stop.\n", headroomDb);
    fflush(stdout);
    while (true) {
        DeviceSnapshot device;
        float currentDb = 0.0f, minDb = 0.0f, maxDb = 0.0f;
        bool ok = GetDefaultRenderDeviceSnapshot(device, currentDb, minDb, maxDb);
        if (!ok || !device.valid) {
            printf("  no default render device\n");
        } else {
            const wchar_t* kindStr = L"other";
            switch (device.kind) {
                case DeviceKind::BuiltInHeadphones:
                    kindStr = L"headphones";
                    break;
                case DeviceKind::Bluetooth:
                    kindStr = L"bluetooth";
                    break;
                case DeviceKind::Speakers:
                    kindStr = L"speakers";
                    break;
                default:
                    break;
            }
            float ceiling = static_cast<float>(std::max<double>(minDb, maxDb - headroomDb));
            float applied = 0.0f;
            bool clamped = EnforceVolumeCap(headroomDb, nullptr, &applied);
            wprintf(L"  %-30ls [%ls] cur=%.1fdB range=[%.1f,%.1f] ceiling=%.1f%ls\n",
                    device.friendlyName.c_str(), kindStr, currentDb, minDb, maxDb, ceiling,
                    clamped ? L" -> CLAMPED" : L"");
        }
        fflush(stdout);
        Sleep(350);
    }
}

}  // namespace hps
