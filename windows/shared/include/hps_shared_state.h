// Shared-memory IPC contract between the tray/control process (writer) and the
// HeadphoneSafetyApo COM DLL (reader), which loads inside the isolated audiodg.exe audio-engine
// process rather than the tray's own process. See windows/README.md "How it works: IPC" for the
// full design rationale (naming/session-scoping, security descriptor, why no lock is needed).
//
// (Kept ASCII-only throughout this file - MSVC's default source-encoding handling has bitten this
// project once already via em-dashes in comments; see build.ps1's history for context.)
//
// There is exactly one writer (the tray, on toggle/preset-change - not a hot path) and one class
// of reader (APOProcess, on the real-time audio thread). Every field is a naturally-aligned 32-
// or 64-bit word, so plain interlocked/volatile access is atomic on x86/x64 without a lock; the
// reader only needs eventually-consistent values (a one-buffer-stale headroom value is inaudible).
#pragma once

#include <windows.h>

#define HPS_MAPPING_NAME    L"Local\\HeadphoneSafetyApoControl"
// Owner (the tray process) gets full control; everyone else gets read-only. audiodg.exe reads
// this at ordinary integrity in the same session - Mandatory Integrity Control allows read-down
// by default, so no integrity-label ACE is needed, only this DACL.
#define HPS_MAPPING_SDDL    L"D:(A;;GA;;;OW)(A;;GR;;;WD)"

#define HPS_SHARED_MAGIC    0x48504153u  // 'HPAS'
#define HPS_SHARED_VERSION  1u

#pragma pack(push, 4)
typedef struct ApoSharedState {
    LONG magic;                    // HPS_SHARED_MAGIC; sanity-checked once at APO Initialize
    LONG structVersion;            // HPS_SHARED_VERSION; future-proofs a layout change
    volatile LONG limiterEnabled;          // 0/1; InterlockedExchange on write
    volatile LONG headroomDbTimes100;      // fixed-point, e.g. 1000 = 10.00 dB; avoids float tearing
    volatile LONG64 heartbeatTickCount;    // GetTickCount64(), written periodically; informational
    // Diagnostic-only fields, written by APOProcess itself (real-time-safe InterlockedExchange, no
    // different than any other field here) so a separate read-only tool can observe what the live
    // APO instance actually sees, since APOProcess cannot log/printf. Added while debugging a case
    // where the limiter had no audible/measurable effect despite the offline DSP test passing -
    // this makes "is the IPC connection actually open" and "what did APOProcess last read"
    // directly inspectable from outside audiodg.exe instead of guessed at.
    volatile LONG apoSharedStateWasOpen;   // 0/1; last SharedStateClient::IsOpen() seen by APOProcess
    volatile LONG apoObservedLimiterEnabled;  // 0/1; last IsLimiterEnabled() value APOProcess read
    volatile LONG apoProcessCallCount;     // increments every APOProcess call; proves it's running at all
} ApoSharedState;
#pragma pack(pop)
