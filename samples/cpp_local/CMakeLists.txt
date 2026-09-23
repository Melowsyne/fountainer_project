# Copyright (c) 2026 Melowsyne Unipessoal Lda. All rights reserved.
#
# Sample and load programs on the public API of the fountainer_client_framework.
# The framework is included as a subproject (EXCLUDE_FROM_ALL: only the
# libraries are built, not its tests/examples).
#
#   cmake -S . -B build -DFOUNTAINER_FRAMEWORK_DIR=/pfad/zum/fountainer_client_framework
#   cmake --build build --target local_access stress_local
cmake_minimum_required(VERSION 3.20)
project(fountainer_samples LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(FOUNTAINER_FRAMEWORK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../fountainer_client_framework"
    CACHE PATH "Quellordner des fountainer_client_framework")
set(FOUNTAINER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(${FOUNTAINER_FRAMEWORK_DIR} framework EXCLUDE_FROM_ALL)

add_executable(local_access local_access.cpp)
target_link_libraries(local_access PRIVATE fountainer::client)

add_executable(stress_local stress_local.cpp)
target_link_libraries(stress_local PRIVATE fountainer::client)
