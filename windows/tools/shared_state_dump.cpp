// shared_state_dump: a throwaway diagnostic tool, NOT part of the shipped app. Opens the
// tray<->APO shared-memory mapping read-only and prints its contents, including the
// apoSharedStateWasOpen/apoObservedLimiterEnabled/apoProcessCallCount diagnostic fields that
// APOProcess writes on every call - lets us see exactly what the live APO instance running
// inside audiodg.exe actually observed, since APOProcess itself cannot log.
#include <windows.h>
#include <hps_shared_state.h>

#include <cstdio>

int wmain() {
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, HPS_MAPPING_NAME);
    if (!mapping) {
        fwprintf(stderr, L"OpenFileMappingW failed: %lu (mapping does not exist - has the tray run?)\n",
                  GetLastError());
        return 1;
    }

    void* mapped = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(ApoSharedState));
    if (!mapped) {
        fwprintf(stderr, L"MapViewOfFile failed: %lu\n", GetLastError());
        CloseHandle(mapping);
        return 1;
    }

    ApoSharedState* state = reinterpret_cast<ApoSharedState*>(mapped);
    wprintf(L"magic: 0x%08x (expect 0x%08x)\n", state->magic, HPS_SHARED_MAGIC);
    wprintf(L"structVersion: %d (expect %d)\n", state->structVersion, HPS_SHARED_VERSION);
    wprintf(L"limiterEnabled: %d\n", state->limiterEnabled);
    wprintf(L"headroomDbTimes100: %d (%.2f dB)\n", state->headroomDbTimes100,
            state->headroomDbTimes100 / 100.0);
    wprintf(L"heartbeatTickCount: %llu\n", state->heartbeatTickCount);
    wprintf(L"apoSharedStateWasOpen: %d\n", state->apoSharedStateWasOpen);
    wprintf(L"apoObservedLimiterEnabled: %d\n", state->apoObservedLimiterEnabled);
    wprintf(L"apoProcessCallCount: %d\n", state->apoProcessCallCount);

    UnmapViewOfFile(mapped);
    CloseHandle(mapping);
    return 0;
}
