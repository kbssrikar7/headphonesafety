#include "SharedStateServer.h"

#include <sddl.h>

namespace hps {

SharedStateServer::SharedStateServer() : mapping_(nullptr), view_(nullptr) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(HPS_MAPPING_SDDL, SDDL_REVISION_1,
                                                                &sd, nullptr)) {
        return;  // degraded mode - IsOpen() stays false, tray keeps running Volume Cap regardless
    }

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;

    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0,
                                         sizeof(ApoSharedState), HPS_MAPPING_NAME);
    // Check GetLastError() immediately after the call it actually describes - a later Win32 call
    // (MapViewOfFile below) can reset it, so this must happen right here, not after mapping the
    // view.
    bool alreadyExisted = mapping && (GetLastError() == ERROR_ALREADY_EXISTS);
    LocalFree(sd);
    if (!mapping) {
        return;
    }

    void* mapped = MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(ApoSharedState));
    if (!mapped) {
        CloseHandle(mapping);
        return;
    }

    ApoSharedState* state = reinterpret_cast<ApoSharedState*>(mapped);
    if (!alreadyExisted) {
        // Only initialize on first creation - if a mapping from a still-running prior instance
        // already existed, its live values (limiterEnabled, headroomDbTimes100) should not be
        // reset out from under whatever's currently reading them.
        state->magic = static_cast<LONG>(HPS_SHARED_MAGIC);
        state->structVersion = static_cast<LONG>(HPS_SHARED_VERSION);
        state->limiterEnabled = 0;
        state->headroomDbTimes100 = 1000;  // 10.00dB, matches Settings::headroomDb's default
        state->heartbeatTickCount = 0;
        state->apoSharedStateWasOpen = 0;
        state->apoObservedLimiterEnabled = 0;
        state->apoProcessCallCount = 0;
    }

    mapping_ = mapping;
    view_ = state;
}

SharedStateServer::~SharedStateServer() {
    if (view_) {
        UnmapViewOfFile(view_);
    }
    if (mapping_) {
        CloseHandle(mapping_);
    }
}

void SharedStateServer::SetLimiterEnabled(bool enabled) {
    if (!view_) return;
    InterlockedExchange(&view_->limiterEnabled, enabled ? 1 : 0);
}

void SharedStateServer::SetHeadroomDb(double headroomDb) {
    if (!view_) return;
    LONG fixedPoint = static_cast<LONG>(headroomDb * 100.0 + 0.5);
    InterlockedExchange(&view_->headroomDbTimes100, fixedPoint);
}

void SharedStateServer::Heartbeat() {
    if (!view_) return;
    InterlockedExchange64(&view_->heartbeatTickCount, static_cast<LONG64>(GetTickCount64()));
}

}  // namespace hps
