#include "HeadphoneSafetyApo.h"

long HeadphoneSafetyApo::instCount = 0;

// APO_FLAG_INPLACE: the audio engine may pass the same buffer for input and output.
// APO_FLAG_SAMPLESPERFRAME_MUST_MATCH / FRAMESPERSECOND_MUST_MATCH / BITSPERSAMPLE_MUST_MATCH:
// input and output formats must be identical - correct for a pass-through/limiter that only
// changes sample values, never channel count, sample rate, or bit depth.
const CRegAPOProperties<1> HeadphoneSafetyApo::regProperties(
    __uuidof(HeadphoneSafetyApo), L"HeadphoneSafetyApo", L"", 1, 0,
    __uuidof(IAudioProcessingObject),
    (APO_FLAG)(APO_FLAG_SAMPLESPERFRAME_MUST_MATCH | APO_FLAG_FRAMESPERSECOND_MUST_MATCH |
               APO_FLAG_BITSPERSAMPLE_MUST_MATCH | APO_FLAG_INPLACE));

HeadphoneSafetyApo::HeadphoneSafetyApo(IUnknown* pUnkOuter)
    // No "&" here - CRegAPOProperties<N> (unlike a plain APO_REG_PROPERTIES) implicitly converts
    // to the "const APO_REG_PROPERTIES*" the base constructor expects; verified against
    // PotatoAPO's real, compiling usage of the identical pattern rather than assumed.
    : CBaseAudioProcessingObject(regProperties), refCount_(1) {
    pUnkOuter_ = pUnkOuter ? pUnkOuter
                           : reinterpret_cast<IUnknown*>(static_cast<INonDelegatingUnknown*>(this));
    InterlockedIncrement(&instCount);
}

HeadphoneSafetyApo::~HeadphoneSafetyApo() {
    InterlockedDecrement(&instCount);
}

HRESULT __stdcall HeadphoneSafetyApo::QueryInterface(const IID& iid, void** ppv) {
    return pUnkOuter_->QueryInterface(iid, ppv);
}

ULONG __stdcall HeadphoneSafetyApo::AddRef() {
    return pUnkOuter_->AddRef();
}

ULONG __stdcall HeadphoneSafetyApo::Release() {
    return pUnkOuter_->Release();
}

HRESULT __stdcall HeadphoneSafetyApo::GetLatency(HNSTIME* pTime) {
    if (!pTime) return E_POINTER;
    if (!m_bIsLocked) return APOERR_ALREADY_UNLOCKED;
    *pTime = 0;  // pure pass-through / in-place envelope-follower limiter adds no buffering delay
    return S_OK;
}

HRESULT __stdcall HeadphoneSafetyApo::Initialize(UINT32 cbDataSize, BYTE* pbyData) {
    if ((nullptr == pbyData) && (0 != cbDataSize)) return E_INVALIDARG;
    if ((nullptr != pbyData) && (0 == cbDataSize)) return E_POINTER;
    if (cbDataSize != sizeof(APOInitSystemEffects)) return E_INVALIDARG;
    return S_OK;
}

HRESULT __stdcall HeadphoneSafetyApo::LockForProcess(
    UINT32 u32NumInputConnections, APO_CONNECTION_DESCRIPTOR** ppInputConnections,
    UINT32 u32NumOutputConnections, APO_CONNECTION_DESCRIPTOR** ppOutputConnections) {
    // Phase 3 opens the shared-memory IPC mapping here (a non-real-time context the audio engine
    // calls before streaming starts) and caches the pointer for APOProcess to read. Phase 2 has
    // nothing extra to set up.
    return CBaseAudioProcessingObject::LockForProcess(
        u32NumInputConnections, ppInputConnections, u32NumOutputConnections, ppOutputConnections);
}

HRESULT __stdcall HeadphoneSafetyApo::NonDelegatingQueryInterface(const IID& iid, void** ppv) {
    if (iid == __uuidof(IUnknown))
        *ppv = static_cast<INonDelegatingUnknown*>(this);
    else if (iid == __uuidof(IAudioProcessingObject))
        *ppv = static_cast<IAudioProcessingObject*>(this);
    else if (iid == __uuidof(IAudioProcessingObjectRT))
        *ppv = static_cast<IAudioProcessingObjectRT*>(this);
    else if (iid == __uuidof(IAudioProcessingObjectConfiguration))
        *ppv = static_cast<IAudioProcessingObjectConfiguration*>(this);
    else if (iid == __uuidof(IAudioSystemEffects))
        *ppv = static_cast<IAudioSystemEffects*>(this);
    else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    reinterpret_cast<IUnknown*>(*ppv)->AddRef();
    return S_OK;
}

ULONG __stdcall HeadphoneSafetyApo::NonDelegatingAddRef() {
    return InterlockedIncrement(&refCount_);
}

ULONG __stdcall HeadphoneSafetyApo::NonDelegatingRelease() {
    LONG remaining = InterlockedDecrement(&refCount_);
    if (remaining == 0) {
        delete this;
        return 0;
    }
    return static_cast<ULONG>(remaining);
}
