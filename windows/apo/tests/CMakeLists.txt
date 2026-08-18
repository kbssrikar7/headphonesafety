# Standalone offline test for the Limiter DSP - no COM, no audio engine, no elevated action
# needed. Run directly after building: windows\build\apo\tests\limiter_test.exe
add_executable(limiter_test
    limiter_test.cpp
)
target_link_libraries(limiter_test PRIVATE hps_dsp)
