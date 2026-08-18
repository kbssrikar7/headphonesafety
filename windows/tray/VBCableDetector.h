// Finds VB-Audio Virtual Cable's render endpoint ("CABLE Input (VB-Audio Virtual Cable)"),
// analogous in role to macOS's BlackHoleDetector.swift. VB-Cable is a required, separately
// installed prerequisite (like BlackHole on macOS) - not bundled with this app.
#pragma once

#include <string>

namespace hps {

struct VBCableInfo {
    std::wstring deviceId;
    bool found = false;
};

// Enumerates active render endpoints and returns the one whose friendly name matches VB-Cable's
// known "CABLE Input (VB-Audio Virtual Cable)" string. found=false if not present/installed.
VBCableInfo FindVBCable();

}  // namespace hps
