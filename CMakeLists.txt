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

include(CTest)
if(BUILD_TESTING)
    find_program(BASH_EXECUTABLE bash REQUIRED)
    set(TEST_ROOTFS "${CMAKE_CURRENT_BINARY_DIR}/test-rootfs")
    add_test(NAME rootfs.prepare COMMAND "${BASH_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/prepare_rootfs.sh"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/create_rootfs.sh" "${TEST_ROOTFS}")
    set_tests_properties(rootfs.prepare PROPERTIES FIXTURES_SETUP rootfs TIMEOUT 30)
    foreach(test_case IN ITEMS basic_execution arguments exit_code invalid_executable invalid_invocation)
        add_test(NAME milestone1.${test_case}
            COMMAND "${BASH_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_process_execution.sh"
                "$<TARGET_FILE:minicontainer>" "${test_case}" "${TEST_ROOTFS}")
        set_tests_properties(milestone1.${test_case} PROPERTIES
            LABELS "milestone1;process_execution"
            FIXTURES_REQUIRED rootfs
            SKIP_RETURN_CODE 77
            TIMEOUT 10)
    endforeach()
    foreach(test_case IN ITEMS hostname pid1 proc namespaces host_unchanged invalid_hostname)
        add_test(NAME milestone2.${test_case}
            COMMAND "${BASH_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_namespace_isolation.sh"
                "$<TARGET_FILE:minicontainer>" "${test_case}" "${TEST_ROOTFS}")
        set_tests_properties(milestone2.${test_case} PROPERTIES
            LABELS "milestone2;namespace_isolation"
            FIXTURES_REQUIRED rootfs
            SKIP_RETURN_CODE 77
            TIMEOUT 10)
    endforeach()
    foreach(test_case IN ITEMS root cwd host_hidden proc devices invalid_rootfs)
        add_test(NAME milestone3.${test_case} COMMAND "${BASH_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_rootfs.sh"
            "$<TARGET_FILE:minicontainer>" "${test_case}" "${TEST_ROOTFS}")
        set_tests_properties(milestone3.${test_case} PROPERTIES
            LABELS "milestone3;rootfs" FIXTURES_REQUIRED rootfs
            SKIP_RETURN_CODE 77 TIMEOUT 10)
    endforeach()
endif()
