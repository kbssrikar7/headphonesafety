#include "ClassFactory.h"

#include "HeadphoneSafetyApo.h"

long ClassFactory::lockCount = 0;

ClassFactory::ClassFactory() : refCount_(1) {}

HRESULT __stdcall ClassFactory::QueryInterface(const IID& iid, void** ppv) {
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IClassFactory))
        *ppv = static_cast<IClassFactory*>(this);
    else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    reinterpret_cast<IUnknown*>(*ppv)->AddRef();
    return S_OK;
}

ULONG __stdcall ClassFactory::AddRef() {
    return InterlockedIncrement(&refCount_);
}

ULONG __stdcall ClassFactory::Release() {
    LONG remaining = InterlockedDecrement(&refCount_);
    if (remaining == 0) {
        delete this;
        return 0;
    }
    return static_cast<ULONG>(remaining);
}

HRESULT __stdcall ClassFactory::CreateInstance(IUnknown* pUnknownOuter, const IID& iid,
                                                void** ppv) {
    if (pUnknownOuter != nullptr && iid != __uuidof(IUnknown)) return E_NOINTERFACE;

    HeadphoneSafetyApo* apo = new HeadphoneSafetyApo(pUnknownOuter);
    if (apo == nullptr) return E_OUTOFMEMORY;

    HRESULT hr = apo->NonDelegatingQueryInterface(iid, ppv);
    apo->NonDelegatingRelease();
    return hr;
}

HRESULT __stdcall ClassFactory::LockServer(BOOL bLock) {
    if (bLock)
        InterlockedIncrement(&lockCount);
    else
        InterlockedDecrement(&lockCount);
    return S_OK;
}
