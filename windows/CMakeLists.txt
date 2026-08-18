cmake_minimum_required(VERSION 3.21)
project(HeadphoneSafetyWindows LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "windows/ only builds on Windows (targets WASAPI/COM/APO APIs).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Shared IPC contract (hps_shared_state.h) consumed by both apo/ and tray/.
add_library(hps_shared INTERFACE)
target_include_directories(hps_shared INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/shared/include)

add_subdirectory(shared_dsp)
add_subdirectory(apo)
add_subdirectory(tray)
add_subdirectory(tools)
