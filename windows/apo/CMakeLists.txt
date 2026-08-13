# HeadphoneSafetyApo: the Windows Audio Processing Object COM DLL.
#
# Phase 2: a pure pass-through GFX-equivalent (EFX, see register/register-apo.ps1) APO, built
# directly on the SDK's CBaseAudioProcessingObject rather than ATL (see HeadphoneSafetyApo.h's
# header comment for why). Phase 3 adds the limiter DSP inside ApoProcess.cpp and shared-memory
# IPC wiring, per the plan in C:\Users\kbs\.claude\plans\ok-so-now-you-enumerated-fog.md.
add_library(HeadphoneSafetyApo SHARED
    ApoDll.cpp
    ApoDll.def
    ClassFactory.cpp
    HeadphoneSafetyApo.cpp
    ApoProcess.cpp
)

# AudioBaseProcessingObject.lib / audiomediatypecrt.lib: the compiled implementation behind
# <BaseAudioProcessingObject.h>'s CBaseAudioProcessingObject (the header only declares it - the
# method bodies live in these SDK-provided import libs). Confirmed via PotatoAPO's real, building
# vcxproj (github.com/Dybios/PotatoAPO) rather than assumed.
target_link_libraries(HeadphoneSafetyApo PRIVATE
    hps_shared
    ole32
    AudioBaseProcessingObject
    audiomediatypecrt
)
target_compile_definitions(HeadphoneSafetyApo PRIVATE UNICODE _UNICODE)
