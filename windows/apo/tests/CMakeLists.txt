# Standalone offline test for the Limiter DSP - no COM, no audio engine, no elevated action
# needed. Run directly after building: windows\build\apo\tests\limiter_test.exe
add_executable(limiter_test
    limiter_test.cpp
    ../Limiter.cpp
)
target_include_directories(limiter_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
