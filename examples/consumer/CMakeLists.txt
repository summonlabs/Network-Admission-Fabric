# Copyright 2026 Summon Software Labs.
# SPDX-License-Identifier: Apache-2.0
#
# Independent downstream consumer. This project is deliberately separate from the
# Network Admission Fabric build tree: it only uses the installed package.
cmake_minimum_required(VERSION 3.20)
project(NAFConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(NAF 1.0 REQUIRED)

add_executable(naf_consumer main.cpp)
target_link_libraries(naf_consumer PRIVATE NAF::naf)

message(STATUS "NAF consumer built against NAF ${NAF_VERSION}")
