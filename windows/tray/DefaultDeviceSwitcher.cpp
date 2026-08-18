#include "DefaultDeviceSwitcher.h"

// DEFINE_GUID in PolicyConfig.h only allocates real storage (rather than an extern declaration)
// in the one translation unit that includes <initguid.h> first - this is that one place for
// IID_IPolicyConfig/CLSID_CPolicyConfigClient.
#include <initguid.h>
#include "PolicyConfig.h"

#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <audioclient.h>

namespace hps {

bool SetDefaultRenderDevice(const std::wstring& deviceId) {
    if (deviceId.empty()) return false;

    IPolicyConfig* policyConfig = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_CPolicyConfigClient, nullptr, CLSCTX_ALL, IID_IPolicyConfig,
                                   reinterpret_cast<void**>(&policyConfig));
    if (FAILED(hr) || !policyConfig) return false;

    bool allOk = true;
    // All three roles - a partial switch leaves some apps routed wrong.
    const ERole roles[] = {eConsole, eMultimedia, eCommunications};
    for (ERole role : roles) {
        HRESULT setHr = policyConfig->SetDefaultEndpoint(deviceId.c_str(), role);
        if (FAILED(setHr)) allOk = false;
    }

    policyConfig->Release();
    return allOk;
}

std::wstring GetCurrentDefaultRenderDeviceId() {
    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                 __uuidof(IMMDeviceEnumerator),
                                 reinterpret_cast<void**>(&enumerator)))) {
        return std::wstring();
    }

    IMMDevice* device = nullptr;
    HRESULT hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    enumerator->Release();
    if (FAILED(hr) || !device) return std::wstring();

    LPWSTR id = nullptr;
    std::wstring result;
    if (SUCCEEDED(device->GetId(&id)) && id) {
        result = id;
        CoTaskMemFree(id);
    }
    device->Release();
    return result;
}

std::wstring GetFriendlyName(const std::wstring& deviceId) {
    if (deviceId.empty()) return std::wstring();

    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                 __uuidof(IMMDeviceEnumerator),
                                 reinterpret_cast<void**>(&enumerator)))) {
        return std::wstring();
    }

    IMMDevice* device = nullptr;
    HRESULT hr = enumerator->GetDevice(deviceId.c_str(), &device);
    enumerator->Release();
    if (FAILED(hr) || !device) return std::wstring();

    std::wstring result;
    IPropertyStore* store = nullptr;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && store) {
        PROPVARIANT var;
        PropVariantInit(&var);
        if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &var)) && var.vt == VT_LPWSTR &&
            var.pwszVal) {
            result = var.pwszVal;
        }
        PropVariantClear(&var);
        store->Release();
    }
    device->Release();
    return result;
}

bool IsLikelyBluetoothVoiceEndpoint(const std::wstring& deviceId) {
    if (deviceId.empty()) return false;

    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                 __uuidof(IMMDeviceEnumerator),
                                 reinterpret_cast<void**>(&enumerator)))) {
        return false;
    }
    IMMDevice* device = nullptr;
    HRESULT hr = enumerator->GetDevice(deviceId.c_str(), &device);
    enumerator->Release();
    if (FAILED(hr) || !device) return false;

    IAudioClient* audioClient = nullptr;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                           reinterpret_cast<void**>(&audioClient));
    device->Release();
    if (FAILED(hr) || !audioClient) return false;

    WAVEFORMATEX* mixFormat = nullptr;
    bool isVoiceEndpoint = false;
    if (SUCCEEDED(audioClient->GetMixFormat(&mixFormat)) && mixFormat) {
        // See the header comment: real A2DP codecs never go below 32000 Hz, HFP's CVSD/mSBC
        // codecs run at 8000/16000 Hz.
        isVoiceEndpoint = mixFormat->nSamplesPerSec < 32000;
        CoTaskMemFree(mixFormat);
    }
    audioClient->Release();
    return isVoiceEndpoint;
}

}  // namespace hps
