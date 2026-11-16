cmake_minimum_required(VERSION 3.22)
project(test_package LANGUAGES C CXX)

find_package(LanceC CONFIG REQUIRED)

add_executable(test_package test_package.cpp)
target_compile_features(test_package PRIVATE cxx_std_17)
target_link_libraries(test_package PRIVATE LanceC::lance_c)

add_executable(test_package_c test_package.c)
target_compile_features(test_package_c PRIVATE c_std_11)
target_link_libraries(test_package_c PRIVATE LanceC::lance_c)
