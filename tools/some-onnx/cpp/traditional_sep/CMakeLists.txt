cmake_minimum_required(VERSION 3.20)
project(traditional_sep LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(traditional_sep main.cpp)
target_include_directories(traditional_sep PRIVATE ../eigen)

if(MSVC)
    target_compile_options(traditional_sep PRIVATE /O2 /EHsc /utf-8)
else()
    target_compile_options(traditional_sep PRIVATE -O2 -Wall -Wextra)
endif()
