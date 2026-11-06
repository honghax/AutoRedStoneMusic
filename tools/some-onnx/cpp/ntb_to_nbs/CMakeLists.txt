cmake_minimum_required(VERSION 3.16)
project(ntb_to_nbs LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(ntb_to_nbs
    main.cpp
    nbt.cpp
    inflate_gzip.cpp
)

if(MSVC)
    target_compile_options(ntb_to_nbs PRIVATE /W3)
else()
    target_compile_options(ntb_to_nbs PRIVATE -Wall -Wextra)
endif()
