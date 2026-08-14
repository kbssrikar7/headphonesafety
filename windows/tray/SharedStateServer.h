// Writer-side (tray/, HeadphoneSafetyTray.exe) of the tray<->APO shared-memory IPC contract
// defined in windows/shared/include/hps_shared_state.h. Creates the named mapping and exposes
// setters that do atomic InterlockedExchange writes.
//
// Construction never throws and never blocks the tray's core Volume Cap function even if mapping
// creation fails (e.g. a security/policy restriction on some machine) - IsOpen() reports success,
// and every setter silently no-ops when not open.
#pragma once

#include <hps_shared_state.h>

namespace hps {

class SharedStateServer {
public:
    SharedStateServer();
    ~SharedStateServer();

    bool IsOpen() const { return view_ != nullptr; }

    void SetLimiterEnabled(bool enabled);
    void SetHeadroomDb(double headroomDb);
    void Heartbeat();

private:
    HANDLE mapping_;
    ApoSharedState* view_;
};

}  // namespace hps
