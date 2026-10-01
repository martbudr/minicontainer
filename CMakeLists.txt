cmake_minimum_required(VERSION 3.20)

if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    set(platform_help "Configure and build this project inside a Linux environment.")
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
        set(platform_help
            "On macOS, enter your Lima VM first:\n"
            "  limactl shell minicontainer\n"
            "  cd /Users/mbudr/Documents/minicontainer\n"
            "  cmake -S . -B build-linux\n"
            "  cmake --build build-linux")
        string(JOIN "" platform_help ${platform_help})
    endif()
    message(FATAL_ERROR
        "minicontainer requires a Linux build host.\n"
        "Detected host: ${CMAKE_HOST_SYSTEM_NAME}\n"
        "The runtime uses Linux clone(); native builds and cross-compilation "
        "from other operating systems are unsupported.\n"
        "${platform_help}")
endif()

project(MiniContainer CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR
        "minicontainer requires a Linux target.\n"
        "Detected target: ${CMAKE_SYSTEM_NAME}\n"
        "Remove the non-Linux toolchain selection and configure for Linux.")
endif()

message(STATUS "minicontainer: Linux host and target confirmed; using clone() runtime")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(minicontainer
        src/main.cpp
)
