#include "VBCableDetector.h"

#include <initguid.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>

namespace hps {
namespace {

// Confirmed live on this project's dev machine via Get-PnpDevice against a real VB-Cable
// install - not assumed from documentation alone.
const wchar_t kVBCableFriendlyName[] = L"CABLE Input (VB-Audio Virtual Cable)";

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

}  // namespace

VBCableInfo FindVBCable() {
    VBCableInfo info;

    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                 __uuidof(IMMDeviceEnumerator),
                                 reinterpret_cast<void**>(&enumerator)))) {
        return info;
    }

    IMMDeviceCollection* collection = nullptr;
    HRESULT hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    enumerator->Release();
    if (FAILED(hr) || !collection) return info;

    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        IMMDevice* device = nullptr;
        if (FAILED(collection->Item(i, &device)) || !device) continue;

        IPropertyStore* store = nullptr;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && store) {
            std::wstring name = ReadStringProperty(store, PKEY_Device_FriendlyName);
            store->Release();
            if (name == kVBCableFriendlyName) {
                LPWSTR id = nullptr;
                if (SUCCEEDED(device->GetId(&id)) && id) {
                    info.deviceId = id;
                    info.found = true;
                    CoTaskMemFree(id);
                }
                device->Release();
                break;
            }
        }
        device->Release();
    }

    collection->Release();
    return info;
}

}  // namespace hps
