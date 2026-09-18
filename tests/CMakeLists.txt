# Copyright 2026 Summon Software Labs.
# SPDX-License-Identifier: Apache-2.0

set(NAF_TEST_SUPPORT ${CMAKE_CURRENT_SOURCE_DIR}/support/test_main.cpp)

function(naf_add_test target)
    add_executable(${target} ${ARGN} ${NAF_TEST_SUPPORT})
    target_link_libraries(${target} PRIVATE NAF::naf $<BUILD_INTERFACE:NAF::warnings>)
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/support)
    target_compile_features(${target} PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    add_test(NAME ${target} COMMAND ${target})
endfunction()

naf_add_test(naf_unit_core unit_core.cpp)
naf_add_test(naf_unit_journal unit_journal.cpp)
naf_add_test(naf_engine_basic engine_basic.cpp)
naf_add_test(naf_engine_identity engine_identity.cpp)
naf_add_test(naf_property property_engine.cpp)
naf_add_test(naf_adversarial adversarial.cpp)
naf_add_test(naf_persistence persistence.cpp)
naf_add_test(naf_concurrency concurrency.cpp)

if(TARGET nafd)
    naf_add_test(naf_multiprocess multiprocess.cpp)
    target_compile_definitions(naf_multiprocess PRIVATE
        NAF_NAFD_EXECUTABLE="$<TARGET_FILE:nafd>")
endif()
