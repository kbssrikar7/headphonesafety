# hps_dsp: the envelope-follower Limiter DSP, shared between windows/apo/ (the parked APO
# approach - see docs/windows-port.md's "Known blocker" - kept building as reference, not
# deleted) and windows/tray/ (LimiterEngine.h/.cpp, the active WASAPI-loopback approach). One
# copy of the algorithm, not two allowed to drift.
add_library(hps_dsp STATIC
    Limiter.cpp
)
# PUBLIC: consumers that link hps_dsp get this directory on their include path, so existing code
# in windows/apo/ (#include "Limiter.h") keeps working unchanged after the move.
target_include_directories(hps_dsp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
