// REAL-TIME THREAD ONLY.
//
// This file contains ONLY the APOProcess callback. The audio engine calls this on a real-time
// audio thread inside audiodg.exe, once per buffer, for as long as this APO's endpoint is
// streaming. Per Microsoft's own APO documentation this code must be nonblocking and
// nonpageable:
//   - never allocate (no `new`, no growing std::vector, no anything that can call into the heap)
//   - never take a lock that could be contended by a non-real-time thread
//   - never call anything that can block: file I/O, registry access, most Win32 APIs, printf/log
//   - never touch paged memory
// Phase 3 (this file): reads sharedState_ (opened earlier, non-real-time, in
// HeadphoneSafetyApo::LockForProcess) via plain aligned loads only - IsLimiterEnabled()/
// GetHeadroomDb() never touch a syscall - and drives limiter_ (envelope-follower state that was
// Reset() in that same non-real-time call). When the shared state isn't open (tray not running
// yet - a known, accepted limitation, see hps_shared_state.h) or the limiter is disabled, this
// stays a pure memcpy pass-through exactly like Phase 2.
#include "HeadphoneSafetyApo.h"

#include "Limiter.h"

#pragma AVRT_CODE_BEGIN
void __stdcall HeadphoneSafetyApo::APOProcess(UINT32 u32NumInputConnections,
                                               APO_CONNECTION_PROPERTY** ppInputConnections,
                                               UINT32 u32NumOutputConnections,
                                               APO_CONNECTION_PROPERTY** ppOutputConnections) {
    UNREFERENCED_PARAMETER(u32NumInputConnections);
    UNREFERENCED_PARAMETER(u32NumOutputConnections);

    switch (ppInputConnections[0]->u32BufferFlags) {
        case BUFFER_VALID: {
            FLOAT32* input = reinterpret_cast<FLOAT32*>(ppInputConnections[0]->pBuffer);
            FLOAT32* output = reinterpret_cast<FLOAT32*>(ppOutputConnections[0]->pBuffer);
            UINT32 validFrameCount = ppInputConnections[0]->u32ValidFrameCount;
            UINT32 samplesPerFrame = GetSamplesPerFrame();

            // APO_FLAG_INPLACE (set in regProperties) means the engine may hand us the same
            // buffer for input and output, so guard the copy rather than memcpy unconditionally.
            if (output != input) {
                memcpy(output, input,
                       static_cast<size_t>(validFrameCount) * samplesPerFrame * sizeof(FLOAT32));
            }

            // sharedState_.IsLimiterEnabled()/GetHeadroomDb() are plain aligned loads of a
            // pointer already cached in LockForProcess - real-time-safe, no syscalls. If the
            // mapping never opened (tray not running yet), IsLimiterEnabled() returns false and
            // this stays the pure pass-through above, matching Phase 2's behavior exactly.
            bool limiterEnabled = sharedState_.IsLimiterEnabled();
            if (limiterEnabled) {
                float ceiling = hps::HeadroomDbToLinearCeiling(sharedState_.GetHeadroomDb());
                limiter_.Process(output, validFrameCount, samplesPerFrame, ceiling);
            }
            // Diagnostic-only, real-time-safe (see hps_shared_state.h) - lets a separate tool
            // observe what this running APO instance actually saw, since this function can't log.
            sharedState_.MarkProcessRan(sharedState_.IsOpen(), limiterEnabled);

            ppOutputConnections[0]->u32ValidFrameCount = validFrameCount;
            ppOutputConnections[0]->u32BufferFlags = ppInputConnections[0]->u32BufferFlags;
            break;
        }
        case BUFFER_SILENT:
        default:
            ppOutputConnections[0]->u32ValidFrameCount = ppInputConnections[0]->u32ValidFrameCount;
            ppOutputConnections[0]->u32BufferFlags = ppInputConnections[0]->u32BufferFlags;
            break;
    }
}
#pragma AVRT_CODE_END
