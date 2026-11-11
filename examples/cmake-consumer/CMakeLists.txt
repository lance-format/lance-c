# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

cmake_minimum_required(VERSION 3.22)
project(lance_c_consumer LANGUAGES CXX)

find_package(LanceC 0.1 REQUIRED)

add_executable(consumer main.cpp)
target_compile_features(consumer PRIVATE cxx_std_17)
target_link_libraries(consumer PRIVATE LanceC::lance_c)

add_executable(acceptance acceptance.cpp)
target_compile_features(acceptance PRIVATE cxx_std_17)
target_link_libraries(acceptance PRIVATE LanceC::lance_c)
