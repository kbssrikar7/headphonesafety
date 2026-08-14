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
# shared-memory mapping - used by watchdog.ps1 to force-disable the limiter as a dev-time safety
# net (hard-won lesson #6).
add_executable(shared_state_write shared_state_write.cpp)
target_link_libraries(shared_state_write PRIVATE hps_shared)
target_compile_definitions(shared_state_write PRIVATE UNICODE _UNICODE)
