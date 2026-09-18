# Copyright 2026 Summon Software Labs.
# SPDX-License-Identifier: Apache-2.0

add_executable(naf_bench naf_bench.cpp)
target_link_libraries(naf_bench PRIVATE NAF::naf $<BUILD_INTERFACE:NAF::warnings>)
target_compile_features(naf_bench PRIVATE cxx_std_20)
if(MSVC)
    target_compile_definitions(naf_bench PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

install(TARGETS naf_bench RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
