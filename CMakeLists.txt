# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(g7motion VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
find_package(Threads REQUIRED)

add_executable(g7motion
    src/main.cpp
    src/g7pro.cpp
    third_party/ds4linux/daemon/src/virtual_device.cpp
)

target_include_directories(g7motion PRIVATE
    src
    third_party/ds4linux/daemon/include
    third_party/ds4linux/common/include
)

target_link_libraries(g7motion PRIVATE
    PkgConfig::LIBUSB
    Threads::Threads
)

target_compile_options(g7motion PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS g7motion RUNTIME DESTINATION bin)

# Bluetooth rumble bridge: no IMU over BT, but the pad accepts the Xbox
# rumble output report, so re-export it as a virtual pad with FF_RUMBLE.
add_executable(g7bt src/g7bt.cpp)
target_compile_options(g7bt PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS g7bt RUNTIME DESTINATION bin)

# Optional: proves the virtual DS4 is a real SDL3 gyro source.
option(G7MOTION_SDL_PROBE "Build the SDL3 gyro probe" ON)
if(G7MOTION_SDL_PROBE)
    pkg_check_modules(SDL3 IMPORTED_TARGET sdl3)
    if(SDL3_FOUND)
        add_executable(sdl3_probe tools/sdl3_probe.cpp)
        target_link_libraries(sdl3_probe PRIVATE PkgConfig::SDL3)
    else()
        message(STATUS "SDL3 not found; sdl3_probe will not be built")
    endif()
endif()
