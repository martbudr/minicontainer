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

include(CTest)
if(BUILD_TESTING)
    find_program(BASH_EXECUTABLE bash REQUIRED)

    add_executable(test_workload tests/workload.cpp)
    target_compile_features(test_workload PRIVATE cxx_std_20)
    set_target_properties(test_workload PROPERTIES CXX_EXTENSIONS OFF)

    set(test_rootfs "${CMAKE_CURRENT_BINARY_DIR}/test-rootfs")
    add_test(NAME prepare_test_rootfs
        COMMAND "${BASH_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/prepare_rootfs.sh"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/create_rootfs.sh"
            "${test_rootfs}" "$<TARGET_FILE:test_workload>")
    set_tests_properties(prepare_test_rootfs PROPERTIES
        FIXTURES_SETUP test_rootfs
        TIMEOUT 60)

    function(add_runtime_tests script label)
        foreach(test_case IN LISTS ARGN)
            set(test_name "${label}_${test_case}")
            add_test(NAME "${test_name}"
                COMMAND "${BASH_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/${script}"
                    "$<TARGET_FILE:minicontainer>" "${test_case}" "${test_rootfs}")
            set_tests_properties("${test_name}" PROPERTIES
                FIXTURES_REQUIRED test_rootfs
                LABELS "${label}"
                SKIP_RETURN_CODE 77
                TIMEOUT 30
                # Shared rootfs mounts and host-wide cgroup checks must not overlap.
                RESOURCE_LOCK minicontainer_integration)
        endforeach()
    endfunction()

    add_runtime_tests(test_process_execution.sh milestone1
        basic_execution arguments exit_code invalid_executable invalid_invocation)
    add_runtime_tests(test_namespace_isolation.sh milestone2
        hostname pid1 proc namespaces host_unchanged invalid_hostname)
    add_runtime_tests(test_rootfs.sh milestone3
        root cwd host_hidden proc devices invalid_rootfs)
    add_runtime_tests(test_control.sh milestone4
        limits cpu memory pids sigint sigterm escalation parent_death zombies
        descendants signal_exit invalid_limits startup_failure)
endif()
