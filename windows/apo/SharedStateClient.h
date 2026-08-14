// Reader-side (apo/, HeadphoneSafetyApo COM DLL) of the tray<->APO shared-memory IPC contract
// defined in windows/shared/include/hps_shared_state.h. OpenFileMappingW/MapViewOfFile happen
// only in TryOpen(), called from HeadphoneSafetyApo::LockForProcess - a non-real-time context.
// APOProcess (ApoProcess.cpp) only ever does plain aligned reads through the pointer this class
// caches; it never calls TryOpen() or any Win32 API itself.
#pragma once

#include <hps_shared_state.h>

namespace hps {

class SharedStateClient {
public:
    SharedStateClient();
    ~SharedStateClient();

    // Attempts to open the mapping created by the tray. Safe to call even if the tray hasn't
    // started yet (or never starts, or a future run already opened it) - IsOpen() reports
    // whether it succeeded. Idempotent: a second call after a successful open is a no-op.
    void TryOpen();

    bool IsOpen() const { return view_ != nullptr; }

    // Real-time-safe: plain aligned loads of the mapped struct's fields, no syscalls. Return
    // safe "disabled" defaults when not open, so ApoProcess.cpp can treat "no tray running yet"
    // the same as "limiter disabled" without a separate branch.
    bool IsLimiterEnabled() const;
    double GetHeadroomDb() const;

    // Diagnostic-only writes, real-time-safe (InterlockedExchange, same as SharedStateServer's
    // writers) - let a separate read-only tool observe what APOProcess actually saw, since
    // APOProcess itself cannot log. See hps_shared_state.h's field comments. No-op if not open.
    void MarkProcessRan(bool sawOpenState, bool observedLimiterEnabled);

private:
    HANDLE mapping_;
    ApoSharedState* view_;
    bool canWrite_ = false;
};

}  // namespace hps
