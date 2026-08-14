// IMMNotificationClient wrapper that sets a thread-safe "please re-poll" flag when the render
// device topology changes, WITHOUT ever touching the specific device ID passed into a callback.
//
// Hard-won lessons #3/#4 (docs/windows-port.md, from the macOS build): querying a device
// mid-disconnect can hang the calling thread indefinitely, and a device-list-change listener
// should react unconditionally (assume something changed, let the next regular poll re-resolve
// via GetDefaultAudioEndpoint) rather than re-verify by querying the changed device itself. See
// DeviceWatcher.cpp's callback implementations for how this is applied.
//
// Construct once at process startup (after CoInitializeEx) and keep it alive for the process
// lifetime; the notification subscription is undone in the destructor.
#pragma once

#include <windows.h>
#include <mmdeviceapi.h>

namespace hps {

class DeviceWatcher : public IMMNotificationClient {
public:
    DeviceWatcher();
    ~DeviceWatcher();

    DeviceWatcher(const DeviceWatcher&) = delete;
    DeviceWatcher& operator=(const DeviceWatcher&) = delete;

    bool IsRegistered() const { return registered_; }

    // True if a device-list-change callback has fired since the last call to this method (which
    // resets it to false). Thread-safe - these callbacks can fire on an arbitrary COM thread per
    // Microsoft's own IMMNotificationClient documentation, not necessarily the thread that
    // constructed this object.
    bool ConsumeChangeFlag();

    // IUnknown
    HRESULT __stdcall QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG __stdcall AddRef() override;
    ULONG __stdcall Release() override;

    // IMMNotificationClient
    HRESULT __stdcall OnDeviceStateChanged(LPCWSTR deviceId, DWORD newState) override;
    HRESULT __stdcall OnDeviceAdded(LPCWSTR deviceId) override;
    HRESULT __stdcall OnDeviceRemoved(LPCWSTR deviceId) override;
    HRESULT __stdcall OnDefaultDeviceChanged(EDataFlow flow, ERole role,
                                              LPCWSTR defaultDeviceId) override;
    HRESULT __stdcall OnPropertyValueChanged(LPCWSTR deviceId, const PROPERTYKEY key) override;

private:
    void MarkChanged(const wchar_t* reason);

    LONG refCount_;
    volatile LONG changeFlag_;
    IMMDeviceEnumerator* enumerator_;
    bool registered_;
};

}  // namespace hps
