# loopback_capture: a throwaway diagnostic tool (not shipped) used to verify the Real-Time
# Limiter APO is actually reducing gain at the hardware, by capturing WASAPI loopback output to a
# WAV file for offline peak-dB analysis (e.g. via ffmpeg's volumedetect filter).
add_executable(loopback_capture loopback_capture.cpp)
target_link_libraries(loopback_capture PRIVATE ole32)
target_compile_definitions(loopback_capture PRIVATE UNICODE _UNICODE)

# shared_state_dump: a throwaway diagnostic tool (not shipped) that reads the tray<->APO
# shared-memory mapping, including the fields APOProcess writes about what it actually observed.
add_executable(shared_state_dump shared_state_dump.cpp)
target_link_libraries(shared_state_dump PRIVATE hps_shared)
target_compile_definitions(shared_state_dump PRIVATE UNICODE _UNICODE)

# shared_state_write: a throwaway diagnostic tool (not shipped) that writes to the tray<->APO
# shared-memory mapping. Superseded by force_revert_default for live Real-Time Limiter testing
# (Approach B's limiter runs in-process inside the tray, not in a separate APO process, so
# force-disabling it via shared memory no longer makes sense as the watchdog mechanism) - kept
# only because it's still a generically useful shared-state diagnostic alongside
# shared_state_dump.
add_executable(shared_state_write shared_state_write.cpp)
target_link_libraries(shared_state_write PRIVATE hps_shared)
target_compile_definitions(shared_state_write PRIVATE UNICODE _UNICODE)

# force_revert_default: hard-won lesson #6's independent, out-of-process safety net for live
# Approach B testing - makes its own direct IPolicyConfig call to un-stick the OS default render
# device from VB-Cable, with zero dependency on the tray process being alive or responsive.
# Reuses DefaultDeviceSwitcher.cpp/VBCableDetector.cpp directly rather than duplicating the COM
# calls. See watchdog.ps1, which now calls this instead of shared_state_write.
add_executable(force_revert_default force_revert_default.cpp
    ../tray/DefaultDeviceSwitcher.cpp
    ../tray/VBCableDetector.cpp
)
target_include_directories(force_revert_default PRIVATE ../tray)
target_link_libraries(force_revert_default PRIVATE ole32)
target_compile_definitions(force_revert_default PRIVATE UNICODE _UNICODE)
