// shared_state_write: a throwaway diagnostic tool, NOT part of the shipped app. Writes a single
// field into the tray<->APO shared-memory mapping. Built specifically for
// windows/tools/watchdog.ps1's use case (force-disable the limiter as a dev-time safety net, per
// hard-won lesson #6 in docs/windows-port.md) - only ever writes limiterEnabled, never touches
// headroomDbTimes100 or anything else.
//
// Opens the mapping with write access, which only succeeds if this process is running as the
// same user who owns it (see hps_shared_state.h's HPS_MAPPING_SDDL: owner gets full control,
// everyone else read-only) - the normal case for a developer running this from their own
// terminal while their own tray process is also running.
//
// Usage: shared_state_write.exe <0|1>
#include <windows.h>
#include <hps_shared_state.h>

#include <cstdio>
#include <cstdlib>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        fwprintf(stderr, L"Usage: %s <0|1>  (sets limiterEnabled)\n", argv[0]);
        return 1;
    }
    int value = _wtoi(argv[1]);

    HANDLE mapping = OpenFileMappingW(FILE_MAP_WRITE, FALSE, HPS_MAPPING_NAME);
    if (!mapping) {
        fwprintf(stderr,
                  L"OpenFileMappingW failed: %lu (mapping does not exist, or this process isn't "
                  L"the owning user - has the tray run?)\n",
                  GetLastError());
        return 1;
    }

    void* mapped = MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(ApoSharedState));
    if (!mapped) {
        fwprintf(stderr, L"MapViewOfFile failed: %lu\n", GetLastError());
        CloseHandle(mapping);
        return 1;
    }

    ApoSharedState* state = reinterpret_cast<ApoSharedState*>(mapped);
    InterlockedExchange(&state->limiterEnabled, value ? 1 : 0);
    wprintf(L"limiterEnabled set to %d\n", value ? 1 : 0);

    UnmapViewOfFile(mapped);
    CloseHandle(mapping);
    return 0;
}
