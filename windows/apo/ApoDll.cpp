// COM DLL entry points for HeadphoneSafetyApo.
//
// DllRegisterServer/DllUnregisterServer here handle ONLY the generic COM class registration
// (CLSID -> InprocServer32 mapping, exactly like any other in-process COM server). Attaching
// this APO to a specific device's audio pipeline (writing its CLSID into that endpoint's
// FxProperties, and the DisableProtectedAudioDG unsigned-APO bypass) is a separate, deliberately
// elevated, endpoint-specific step done by register/register-apo.ps1 - not by regsvr32. See that
// script for why: FxProperties writes are system-wide, target one endpoint at a time, and need
// to be backed up/reversible per endpoint, which doesn't fit the DllRegisterServer model.
//
// Deliberately does NOT call the SDK's RegisterAPO()/UnregisterAPO() helpers (declared in
// audioenginebaseapo.h): those back the APO discovery/enumeration API (EnumerateAPOs), which
// this project does not use, and pulling them in would require linking against an import
// library that was not identified with confidence during Phase 2's research. Plain COM
// registration (this file) plus register-apo.ps1's direct FxProperties write is a complete and
// simpler path, matching how PotatoAPO's DllRegisterServer is structured (though PotatoAPO also
// calls RegisterAPO() - we're deliberately more conservative here to keep the build's dependency
// surface small and verified).
#include <windows.h>

#include <string>

#include "ClassFactory.h"
#include "HeadphoneSafetyApo.h"

namespace {
HINSTANCE g_hModule = nullptr;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID) {
    if (dwReason == DLL_PROCESS_ATTACH) {
        g_hModule = reinterpret_cast<HINSTANCE>(hModule);
    }
    return TRUE;
}

STDAPI DllCanUnloadNow() {
    return (HeadphoneSafetyApo::instCount == 0 && ClassFactory::lockCount == 0) ? S_OK : S_FALSE;
}

STDAPI DllGetClassObject(const CLSID& clsid, const IID& iid, void** ppv) {
    if (clsid != __uuidof(HeadphoneSafetyApo)) return CLASS_E_CLASSNOTAVAILABLE;

    ClassFactory* factory = new ClassFactory();
    if (factory == nullptr) return E_OUTOFMEMORY;

    HRESULT hr = factory->QueryInterface(iid, ppv);
    factory->Release();
    return hr;
}

STDAPI DllRegisterServer() {
    wchar_t filename[1024];
    GetModuleFileNameW(g_hModule, filename, sizeof(filename) / sizeof(wchar_t));

    wchar_t* guidStr = nullptr;
    StringFromCLSID(__uuidof(HeadphoneSafetyApo), &guidStr);
    std::wstring guidString(guidStr);
    CoTaskMemFree(guidStr);

    HKEY keyHandle;
    RegCreateKeyExW(HKEY_LOCAL_MACHINE, (L"SOFTWARE\\Classes\\CLSID\\" + guidString).c_str(), 0,
                     nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &keyHandle, nullptr);
    const wchar_t* value = L"HeadphoneSafetyApo";
    RegSetValueExW(keyHandle, L"", 0, REG_SZ, reinterpret_cast<const BYTE*>(value),
                    static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
    RegCloseKey(keyHandle);

    RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                     (L"SOFTWARE\\Classes\\CLSID\\" + guidString + L"\\InprocServer32").c_str(), 0,
                     nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &keyHandle, nullptr);
    value = filename;
    RegSetValueExW(keyHandle, L"", 0, REG_SZ, reinterpret_cast<const BYTE*>(value),
                    static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
    value = L"Both";
    RegSetValueExW(keyHandle, L"ThreadingModel", 0, REG_SZ, reinterpret_cast<const BYTE*>(value),
                    static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
    RegCloseKey(keyHandle);

    return S_OK;
}

STDAPI DllUnregisterServer() {
    wchar_t* guidStr = nullptr;
    StringFromCLSID(__uuidof(HeadphoneSafetyApo), &guidStr);
    std::wstring guidString(guidStr);
    CoTaskMemFree(guidStr);

    RegDeleteKeyExW(HKEY_LOCAL_MACHINE,
                     (L"SOFTWARE\\Classes\\CLSID\\" + guidString + L"\\InprocServer32").c_str(),
                     KEY_WOW64_64KEY, 0);
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, (L"SOFTWARE\\Classes\\CLSID\\" + guidString).c_str(),
                     KEY_WOW64_64KEY, 0);
    return S_OK;
}
