// Volume Cap: WASAPI IAudioEndpointVolume polling that clamps the default render endpoint's
// volume to (max - headroomDb) whenever that endpoint is headphone-like. See
// docs/windows-port.md, "Feature 1: Volume Cap" for the source design, and
// linux/src/volume_cap.rs for the reference implementation this mirrors.
#pragma once

#include <string>

namespace hps {

enum class DeviceKind {
    Unknown,
    Speakers,
    BuiltInHeadphones,
    Bluetooth,
};

struct DeviceSnapshot {
    std::wstring id;
    std::wstring friendlyName;
    DeviceKind kind = DeviceKind::Unknown;
    bool valid = false;
};

// Reads the current default render endpoint's classification and volume range/level (in dB).
// Returns false if there is no default render endpoint (e.g. no audio devices present).
bool GetDefaultRenderDeviceSnapshot(DeviceSnapshot& outDevice, float& outCurrentDb,
                                     float& outMinDb, float& outMaxDb);

// If the current default render endpoint is headphone-like and currently above
// (max - headroomDb), clamps it down to that ceiling and verifies the change by reading the
// level back (hard-won lesson #1: a successful SetMasterVolumeLevel call is not proof it took
// effect). Returns true if a clamp was applied and verified this call.
bool EnforceVolumeCap(double headroomDb, DeviceSnapshot* outDevice = nullptr,
                       float* outAppliedDb = nullptr);

// Blocks forever, polling every ~350ms and logging device/kind/current-dB/ceiling/clamp-action
// to stdout each tick. Used by --test-cap for headless verification before the tray UI exists.
void RunVolumeCapTestLoop(double headroomDb);

}  // namespace hps
