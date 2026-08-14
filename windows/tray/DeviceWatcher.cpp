// Every IMMNotificationClient callback here does the absolute minimum: log what changed (for
// diagnostics) and set changeFlag_ via InterlockedExchange, then return immediately. None of them
// ever call IMMDevice/IAudioEndpointVolume/property-store methods against the deviceId passed
// in - hard-won lesson #3 says exactly that kind of query can hang indefinitely against a device
// that's mid-disconnect, and lesson #4 says the fix is to react unconditionally rather than
// re-verify by querying. The next regular poll tick (VolumeCap.cpp's
// GetDefaultRenderDeviceSnapshot, called from main.cpp's loop) is what actually reacts, by
// re-resolving the default endpoint fresh via GetDefaultAudioEndpoint - never by touching the
// specific ID a callback reported.
//
// These callbacks can fire on an arbitrary thread per Microsoft's own IMMNotificationClient
// documentation, so changeFlag_ is accessed only via Interlocked* functions, never a plain bool.
#include "DeviceWatcher.h"

#include <cstdio>

namespace hps {

DeviceWatcher::DeviceWatcher()
    : refCount_(1), changeFlag_(0), enumerator_(nullptr), registered_(false) {
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void**>(&enumerator_));
    if (FAILED(hr) || !enumerator_) {
        enumerator_ = nullptr;
        return;
    }

    hr = enumerator_->RegisterEndpointNotificationCallback(this);
    registered_ = SUCCEEDED(hr);
}

DeviceWatcher::~DeviceWatcher() {
    if (enumerator_) {
        if (registered_) {
            enumerator_->UnregisterEndpointNotificationCallback(this);
        }
        enumerator_->Release();
    }
}

bool DeviceWatcher::ConsumeChangeFlag() {
    return InterlockedExchange(&changeFlag_, 0) != 0;
}

void DeviceWatcher::MarkChanged(const wchar_t* reason) {
    InterlockedExchange(&changeFlag_, 1);
    // stderr, not stdout - this can fire on an arbitrary thread and stdio's buffering behavior
    // across threads is not something to lean on for correctness, just diagnostics.
    fwprintf(stderr, L"[DeviceWatcher] %ls\n", reason);
    fflush(stderr);
}

HRESULT __stdcall DeviceWatcher::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
        *ppvObject = static_cast<IMMNotificationClient*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObject = nullptr;
    return E_NOINTERFACE;
}

ULONG __stdcall DeviceWatcher::AddRef() {
    return InterlockedIncrement(&refCount_);
}

ULONG __stdcall DeviceWatcher::Release() {
    // Never `delete this` here - DeviceWatcher's lifetime is owned by whatever constructed it
    // (a stack/member variable in main.cpp, not a heap object owned via COM refcounting). The
    // initial refCount_ of 1 accounts for that owner; COM's internal AddRef/Release bookkeeping
    // around Register/UnregisterEndpointNotificationCallback adjusts the count on top of that
    // baseline but should never be what frees the object.
    return static_cast<ULONG>(InterlockedDecrement(&refCount_));
}

HRESULT __stdcall DeviceWatcher::OnDeviceStateChanged(LPCWSTR /*deviceId*/, DWORD /*newState*/) {
    MarkChanged(L"OnDeviceStateChanged");
    return S_OK;
}

HRESULT __stdcall DeviceWatcher::OnDeviceAdded(LPCWSTR /*deviceId*/) {
    MarkChanged(L"OnDeviceAdded");
    return S_OK;
}

HRESULT __stdcall DeviceWatcher::OnDeviceRemoved(LPCWSTR /*deviceId*/) {
    MarkChanged(L"OnDeviceRemoved");
    return S_OK;
}

HRESULT __stdcall DeviceWatcher::OnDefaultDeviceChanged(EDataFlow flow, ERole /*role*/,
                                                          LPCWSTR /*defaultDeviceId*/) {
    if (flow == eRender) {
        MarkChanged(L"OnDefaultDeviceChanged (render)");
    }
    return S_OK;
}

HRESULT __stdcall DeviceWatcher::OnPropertyValueChanged(LPCWSTR /*deviceId*/,
                                                          const PROPERTYKEY /*key*/) {
    // Deliberately NOT treated as a re-poll trigger - this fires very frequently for benign
    // property churn (e.g. the volume-level property changing because Volume Cap itself just
    // wrote it) and would set the change flag on nearly every tick, defeating its purpose as a
    // "something structural changed" signal. Device state/add/remove/default-change (above) are
    // the actual signals that matter for Volume Cap's correctness.
    return S_OK;
}

}  // namespace hps
