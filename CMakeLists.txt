cmake_minimum_required(VERSION 3.20)

if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    set(platform_help "Configure and build this project inside a Linux environment.")
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
        set(platform_help
            "On macOS, enter your Lima VM first:\n"
            "  limactl shell minicontainer\n"
            "  cd ${CMAKE_CURRENT_SOURCE_DIR}\n"
            "  cmake -S . -B build\n"
            "  cmake --build build")
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

add_executable(minicontainer
    src/main.cpp
    src/cli.cpp
    src/container.cpp
    src/container_init.cpp
    src/rootfs.cpp
    src/process_signals.cpp
    src/cgroup.cpp
    src/config.hpp
    src/container_init.hpp
    src/rootfs.hpp
    src/process_signals.hpp
    src/cli.hpp
    src/container.hpp
    src/cgroup.hpp
)

target_compile_features(minicontainer PRIVATE cxx_std_20)
set_target_properties(minicontainer PROPERTIES CXX_EXTENSIONS OFF)

# Expose the GNU/Linux APIs used by clone(), pipe2(), and related calls.
target_compile_definitions(minicontainer PRIVATE _GNU_SOURCE)
target_include_directories(minicontainer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(minicontainer PRIVATE -Wall -Wextra)
endif()
