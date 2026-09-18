# Copyright 2026 Summon Software Labs.
# SPDX-License-Identifier: Apache-2.0

add_executable(nafd nafd.cpp)
target_link_libraries(nafd PRIVATE NAF::naf $<BUILD_INTERFACE:NAF::warnings>)
target_compile_features(nafd PRIVATE cxx_std_20)
if(MSVC)
    target_compile_definitions(nafd PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

add_executable(nafctl nafctl.cpp)
target_link_libraries(nafctl PRIVATE NAF::naf $<BUILD_INTERFACE:NAF::warnings>)
target_compile_features(nafctl PRIVATE cxx_std_20)
if(MSVC)
    target_compile_definitions(nafctl PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

install(TARGETS nafd nafctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
