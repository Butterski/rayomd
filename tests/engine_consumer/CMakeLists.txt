# SPDX-License-Identifier: Apache-2.0

# Builds a program against rayomd::engine the way another front end does: this repository added
# with add_subdirectory, so only the engine library is built.
cmake_minimum_required(VERSION 3.16)
project(rayomd_engine_consumer LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(../.. rayomd EXCLUDE_FROM_ALL)
add_executable(engine-consumer main.cpp)
target_link_libraries(engine-consumer PRIVATE rayomd::engine)
