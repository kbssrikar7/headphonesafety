#include "SharedStateClient.h"

namespace hps {

SharedStateClient::SharedStateClient() : mapping_(nullptr), view_(nullptr) {}

SharedStateClient::~SharedStateClient() {
    if (view_) {
        UnmapViewOfFile(view_);
    }
    if (mapping_) {
        CloseHandle(mapping_);
    }
}

void SharedStateClient::TryOpen() {
    if (view_) return;  // already open

    // Try read+write first (needed for the diagnostic fields below); fall back to read-only if
    // that's denied - audiodg.exe's access against the mapping's DACL (see hps_shared_state.h,
    // HPS_MAPPING_SDDL) depends on whether it's running as the same user token as the tray (gets
    // the Owner ACE's full control) or a distinct principal (gets only the Everyone ACE's read).
    // Never assume which without checking - this is exactly what's being diagnosed right now.
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, HPS_MAPPING_NAME);
    DWORD desiredAccess = FILE_MAP_READ | FILE_MAP_WRITE;
    if (!mapping) {
        mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, HPS_MAPPING_NAME);
        desiredAccess = FILE_MAP_READ;
    }
    if (!mapping) {
        // Expected, not an error: the tray may not have started yet, or may never start. See
        // hps_shared_state.h and ApoProcess.cpp for how "not open" is treated as "limiter
        // disabled, stay in pass-through" - this is the known, accepted stream-rebuild-timing
        // limitation documented in the approved plan.
        return;
    }

    void* mapped = MapViewOfFile(mapping, desiredAccess, 0, 0, sizeof(ApoSharedState));
    if (!mapped) {
        CloseHandle(mapping);
        return;
    }

    ApoSharedState* state = reinterpret_cast<ApoSharedState*>(mapped);
    if (state->magic != static_cast<LONG>(HPS_SHARED_MAGIC) ||
        state->structVersion != static_cast<LONG>(HPS_SHARED_VERSION)) {
        UnmapViewOfFile(mapped);
        CloseHandle(mapping);
        return;
    }

    mapping_ = mapping;
    view_ = state;
    canWrite_ = (desiredAccess & FILE_MAP_WRITE) != 0;
}

bool SharedStateClient::IsLimiterEnabled() const {
    if (!view_) return false;
    return view_->limiterEnabled != 0;
}

double SharedStateClient::GetHeadroomDb() const {
    if (!view_) return 0.0;
    return static_cast<double>(view_->headroomDbTimes100) / 100.0;
}

void SharedStateClient::MarkProcessRan(bool sawOpenState, bool observedLimiterEnabled) {
    if (!view_ || !canWrite_) return;
    InterlockedExchange(&view_->apoSharedStateWasOpen, sawOpenState ? 1 : 0);
    InterlockedExchange(&view_->apoObservedLimiterEnabled, observedLimiterEnabled ? 1 : 0);
    InterlockedIncrement(&view_->apoProcessCallCount);
}

}  // namespace hps
