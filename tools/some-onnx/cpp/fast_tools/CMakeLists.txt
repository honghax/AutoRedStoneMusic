cmake_minimum_required(VERSION 3.16)
project(fast_tools LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(fast_core STATIC
    midi_io.cpp
    nbt_writer.cpp
    gen_machine.cpp
)
target_include_directories(fast_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

if(MSVC)
    target_compile_options(fast_core PRIVATE /O2 /EHsc /utf-8 /W3)
endif()

foreach(tool fast_midi_to_nbt fast_arrange_multilane fast_merge_midis)
    add_executable(${tool} ${tool}.cpp)
    target_link_libraries(${tool} PRIVATE fast_core)
    if(MSVC)
        target_compile_options(${tool} PRIVATE /O2 /EHsc /utf-8 /W3)
    endif()
endforeach()
