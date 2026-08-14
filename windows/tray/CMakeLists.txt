# HeadphoneSafetyTray: the tray/control app (Win32 Shell_NotifyIcon, no real-time constraints).
#
# Phase 1 built Volume Cap polling (VolumeCap.cpp) + HKCU-backed prefs (Settings.cpp) + a
# --test-cap CLI mode. Phase 3 adds SharedStateServer.cpp, the writer side of the IPC mapping
# that lets this process control the limiter running inside HeadphoneSafetyApo.dll. Phase 5 adds
# the full TrayIcon.cpp Shell_NotifyIcon menu. See
# C:\Users\kbs\.claude\plans\ok-so-now-you-enumerated-fog.md.
add_executable(HeadphoneSafetyTray WIN32
    main.cpp
    Settings.cpp
    VolumeCap.cpp
    SharedStateServer.cpp
)

target_link_libraries(HeadphoneSafetyTray PRIVATE hps_shared ole32 shell32)
target_compile_definitions(HeadphoneSafetyTray PRIVATE UNICODE _UNICODE)
