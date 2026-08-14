# Standalone offline test for TimeoutRunner - no COM, no audio engine, no elevated action needed.
# Run directly after building: windows\build\tray\tests\timeout_runner_test.exe
add_executable(timeout_runner_test
    timeout_runner_test.cpp
    ../TimeoutRunner.cpp
)
target_include_directories(timeout_runner_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
