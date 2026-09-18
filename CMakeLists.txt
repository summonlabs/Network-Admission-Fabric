# Network Admission Fabric
# Copyright 2026 Summon Software Labs.
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20)

project(NetworkAdmissionFabric
    VERSION 1.0.0
    DESCRIPTION "Vendor-neutral C++20 runtime for generation-bound network admission decisions"
    HOMEPAGE_URL "https://github.com/summonlabs/Network-Admission-Fabric"
    LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(NAF_IS_TOP_LEVEL ON)
else()
    set(NAF_IS_TOP_LEVEL OFF)
endif()

option(NAF_BUILD_TESTS "Build the Network Admission Fabric test suite" ${NAF_IS_TOP_LEVEL})
option(NAF_BUILD_TOOLS "Build the nafd coordinator and nafctl client tools" ${NAF_IS_TOP_LEVEL})
option(NAF_BUILD_BENCHMARKS "Build the synthetic admission benchmark" ${NAF_IS_TOP_LEVEL})
option(NAF_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(NAF_ENABLE_ASAN "Build with AddressSanitizer where the toolchain supports it" OFF)
option(NAF_ENABLE_STATIC_ANALYSIS "Run the compiler's static analyser (MSVC /analyze)" OFF)

if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 20)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

# ---------------------------------------------------------------------------
# Warnings
# ---------------------------------------------------------------------------
add_library(naf_warnings INTERFACE)
add_library(NAF::warnings ALIAS naf_warnings)

if(MSVC)
    target_compile_options(naf_warnings INTERFACE
        /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /EHsc
    )
    if(NAF_WARNINGS_AS_ERRORS)
        target_compile_options(naf_warnings INTERFACE /WX)
    endif()
    if(NAF_ENABLE_ASAN)
        target_compile_options(naf_warnings INTERFACE /fsanitize=address /Zi /Od)
        target_link_options(naf_warnings INTERFACE /INCREMENTAL:NO /DEBUG)
    endif()
    if(NAF_ENABLE_STATIC_ANALYSIS)
        target_compile_options(naf_warnings INTERFACE /analyze /analyze:external-)
    endif()
else()
    target_compile_options(naf_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
        -Wold-style-cast -Wnon-virtual-dtor -Wcast-align -Wunused -Woverloaded-virtual
        -Wnull-dereference -Wdouble-promotion -Wformat=2)
    if(NAF_WARNINGS_AS_ERRORS)
        target_compile_options(naf_warnings INTERFACE -Werror)
    endif()
    if(NAF_ENABLE_ASAN)
        target_compile_options(naf_warnings INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(naf_warnings INTERFACE -fsanitize=address,undefined)
    endif()
endif()

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
set(NAF_SOURCES
    src/core/status.cpp
    src/core/identity.cpp
    src/core/file.cpp
    src/model/quantity.cpp
    src/model/state.cpp
    src/model/request.cpp
    src/model/policy.cpp
    src/authority/authority.cpp
    src/engine/decision.cpp
    src/engine/ledger.cpp
    src/engine/history.cpp
    src/engine/engine.cpp
    src/engine/admission.cpp
    src/durable/journal.cpp
    src/durable/records.cpp
    src/ipc/stream.cpp
    src/ipc/frame.cpp
    src/ipc/endpoint.cpp
    src/ipc/protocol.cpp
    src/ipc/server.cpp
    src/ipc/client.cpp
)

set(NAF_PUBLIC_HEADERS
    include/naf/version.hpp
    include/naf/naf.hpp
    include/naf/core/bytes.hpp
    include/naf/core/checked.hpp
    include/naf/core/crc32c.hpp
    include/naf/core/file.hpp
    include/naf/core/hash.hpp
    include/naf/core/identity.hpp
    include/naf/core/limits.hpp
    include/naf/core/status.hpp
    include/naf/model/policy.hpp
    include/naf/model/quantity.hpp
    include/naf/model/request.hpp
    include/naf/model/state.hpp
    include/naf/authority/authority.hpp
    include/naf/engine/decision.hpp
    include/naf/engine/engine.hpp
    include/naf/engine/history.hpp
    include/naf/engine/ledger.hpp
    include/naf/durable/journal.hpp
    include/naf/durable/records.hpp
    include/naf/durable/recovery.hpp
    include/naf/ipc/client.hpp
    include/naf/ipc/endpoint.hpp
    include/naf/ipc/frame.hpp
    include/naf/ipc/protocol.hpp
    include/naf/ipc/server.hpp
    include/naf/ipc/stream.hpp
)

add_library(naf STATIC ${NAF_SOURCES})
add_library(NAF::naf ALIAS naf)

target_include_directories(naf
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(naf PRIVATE $<BUILD_INTERFACE:NAF::warnings>)  # build-time only; not exported
target_compile_features(naf PUBLIC cxx_std_20)

set_target_properties(naf PROPERTIES
    OUTPUT_NAME naf
    POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    target_compile_definitions(naf PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ---------------------------------------------------------------------------
# Install / package
# ---------------------------------------------------------------------------
install(TARGETS naf
    EXPORT NAFTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/naf DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES LICENSE README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/NAF OPTIONAL)

set(NAF_FORMAT_REVISION 1)
set(NAF_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/NAF)

install(EXPORT NAFTargets
    FILE NAFTargets.cmake
    NAMESPACE NAF::
    DESTINATION ${NAF_CMAKE_DIR})

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/NAFConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/NAFConfig.cmake
    INSTALL_DESTINATION ${NAF_CMAKE_DIR})

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/NAFConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/NAFConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/NAFConfigVersion.cmake
    DESTINATION ${NAF_CMAKE_DIR})

# ---------------------------------------------------------------------------
# Components
# ---------------------------------------------------------------------------
if(NAF_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(NAF_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(NAF_BUILD_BENCHMARKS)
    add_subdirectory(bench)
endif()

message(STATUS "Network Admission Fabric ${PROJECT_VERSION}")
message(STATUS "  C++ standard        : ${CMAKE_CXX_STANDARD}")
message(STATUS "  warnings as errors  : ${NAF_WARNINGS_AS_ERRORS}")
message(STATUS "  AddressSanitizer    : ${NAF_ENABLE_ASAN}")
message(STATUS "  tests               : ${NAF_BUILD_TESTS}")
message(STATUS "  tools               : ${NAF_BUILD_TOOLS}")
message(STATUS "  benchmarks          : ${NAF_BUILD_BENCHMARKS}")
