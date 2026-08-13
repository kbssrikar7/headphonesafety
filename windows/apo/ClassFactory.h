#pragma once

#include <Unknwn.h>

class ClassFactory : public IClassFactory {
public:
    ClassFactory();

    // IUnknown
    virtual HRESULT __stdcall QueryInterface(const IID& iid, void** ppv) override;
    virtual ULONG __stdcall AddRef() override;
    virtual ULONG __stdcall Release() override;

    // IClassFactory
    virtual HRESULT __stdcall CreateInstance(IUnknown* pUnknownOuter, const IID& iid,
                                              void** ppv) override;
    virtual HRESULT __stdcall LockServer(BOOL bLock) override;

    static long lockCount;

private:
    long refCount_;
};
