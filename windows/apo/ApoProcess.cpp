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
// Phase 3 adds the envelope-follower limiter DSP here and reads the shared-memory IPC state
// (windows/shared/include/hps_shared_state.h) via a plain aligned load of a pointer cached
// earlier in HeadphoneSafetyApo::LockForProcess (a non-real-time context) - keep obeying the
// constraints above when doing so.
#include "HeadphoneSafetyApo.h"

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

            // Pure pass-through for Phase 2 - Phase 3 replaces this copy with the limiter DSP.
            // APO_FLAG_INPLACE (set in regProperties) means the engine may hand us the same
            // buffer for input and output, so guard the copy rather than memcpy unconditionally.
            if (output != input) {
                memcpy(output, input,
                       static_cast<size_t>(validFrameCount) * samplesPerFrame * sizeof(FLOAT32));
            }

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
