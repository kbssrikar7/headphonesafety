// HeadphoneSafetyApo: the Real-Time Limiter's Windows Audio Processing Object.
//
// Phase 3: LockForProcess opens the shared-memory IPC mapping (SharedStateClient) and resets the
// Limiter for the stream's actual sample rate/channel count - both non-real-time setup.
// APOProcess (ApoProcess.cpp) reads sharedState_ and drives limiter_ on the real-time thread;
// see that file's header comment for the constraints that govern everything it does.
//
// Built directly on the SDK's CBaseAudioProcessingObject (<BaseAudioProcessingObject.h>,
// confirmed present in the plain Windows 11 SDK, not WDK-exclusive) rather than ATL - this
// mirrors PotatoAPO (github.com/Dybios/PotatoAPO), a real, working, minimal APO that attaches to
// real device endpoints (not a sample virtual driver), and avoids requiring the VS "C++ ATL"
// optional component that this project's toolchain does not install.
//
// CLSID AAF92DEA-FFE0-4E91-94A4-39385AD5ECFD - generated fresh for this project, used
// consistently here, in ApoDll.cpp's registration code, and in register/register-apo.ps1.
#pragma once

#include <Unknwn.h>
#include <audioenginebaseapo.h>
#include <BaseAudioProcessingObject.h>

#include "Limiter.h"
#include "SharedStateClient.h"

// COM aggregation support (a client can create this APO as part of a larger aggregate object).
// The audio engine does not normally aggregate APOs, but every real-world reference APO
// implements this pattern, so we mirror it rather than diverge.
class INonDelegatingUnknown {
public:
    virtual HRESULT __stdcall NonDelegatingQueryInterface(const IID& iid, void** ppv) = 0;
    virtual ULONG __stdcall NonDelegatingAddRef() = 0;
    virtual ULONG __stdcall NonDelegatingRelease() = 0;
};

class __declspec(uuid("AAF92DEA-FFE0-4E91-94A4-39385AD5ECFD")) HeadphoneSafetyApo
    : public CBaseAudioProcessingObject,
      public IAudioSystemEffects,
      public INonDelegatingUnknown {
public:
    explicit HeadphoneSafetyApo(IUnknown* pUnkOuter);
    virtual ~HeadphoneSafetyApo();

    // IUnknown (delegating - forwards to the outer object, or to ourselves if not aggregated)
    virtual HRESULT __stdcall QueryInterface(const IID& iid, void** ppv) override;
    virtual ULONG __stdcall AddRef() override;
    virtual ULONG __stdcall Release() override;

    // IAudioProcessingObject / IAudioProcessingObjectConfiguration overrides. Everything not
    // listed here (IsOutputFormatSupported, CalcInputFrames/CalcOutputFrames, Reset, ...) uses
    // CBaseAudioProcessingObject's default implementation.
    virtual HRESULT __stdcall GetLatency(HNSTIME* pTime) override;
    virtual HRESULT __stdcall Initialize(UINT32 cbDataSize, BYTE* pbyData) override;
    virtual HRESULT __stdcall LockForProcess(UINT32 u32NumInputConnections,
                                              APO_CONNECTION_DESCRIPTOR** ppInputConnections,
                                              UINT32 u32NumOutputConnections,
                                              APO_CONNECTION_DESCRIPTOR** ppOutputConnections) override;

    // IAudioProcessingObjectRT - implemented in ApoProcess.cpp, isolated from the rest of this
    // class specifically because it runs on a real-time audio thread inside audiodg.exe and must
    // never block, allocate, take a contended lock, or touch paged memory. See that file's header
    // comment before adding anything here.
    virtual void __stdcall APOProcess(UINT32 u32NumInputConnections,
                                       APO_CONNECTION_PROPERTY** ppInputConnections,
                                       UINT32 u32NumOutputConnections,
                                       APO_CONNECTION_PROPERTY** ppOutputConnections) override;

    // INonDelegatingUnknown (the real implementation IUnknown forwards to)
    virtual HRESULT __stdcall NonDelegatingQueryInterface(const IID& iid, void** ppv) override;
    virtual ULONG __stdcall NonDelegatingAddRef() override;
    virtual ULONG __stdcall NonDelegatingRelease() override;

    static const CRegAPOProperties<1> regProperties;
    static long instCount;

private:
    long refCount_;
    IUnknown* pUnkOuter_;

    // Opened (non-real-time) in LockForProcess; read (real-time-safe plain loads only) in
    // APOProcess. limiter_'s Reset() is also only ever called from LockForProcess.
    hps::SharedStateClient sharedState_;
    hps::Limiter limiter_;
};
