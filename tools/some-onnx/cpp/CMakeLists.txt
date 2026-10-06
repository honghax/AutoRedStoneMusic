cmake_minimum_required(VERSION 3.20)
project(audio_to_midi LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ORT_ROOT "" CACHE PATH "Path to the ONNX Runtime C++ SDK")
if(NOT ORT_ROOT)
    message(FATAL_ERROR "Set ORT_ROOT to the ONNX Runtime C++ SDK directory")
endif()

add_executable(audio_to_midi src/main.cpp)
target_include_directories(audio_to_midi PRIVATE "${ORT_ROOT}/include")
target_link_libraries(audio_to_midi PRIVATE "${ORT_ROOT}/lib/onnxruntime.lib")

if(MSVC)
    target_compile_options(audio_to_midi PRIVATE /O2 /EHsc /utf-8)
    target_link_options(audio_to_midi PRIVATE "/LIBPATH:${ORT_ROOT}/lib")
    get_filename_component(_MSVC_ROOT "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(_MSVC_ROOT "${_MSVC_ROOT}/../../.." ABSOLUTE)
    target_link_options(audio_to_midi PRIVATE "/LIBPATH:${_MSVC_ROOT}/lib/x64")
    file(GLOB _WIN_KITS "C:/Program Files (x86)/Windows Kits/10/Lib/*")
    foreach(_kit ${_WIN_KITS})
        if(EXISTS "${_kit}/um/x64")
            target_link_options(audio_to_midi PRIVATE "/LIBPATH:${_kit}/um/x64" "/LIBPATH:${_kit}/ucrt/x64")
        endif()
    endforeach()
endif()

add_custom_command(TARGET audio_to_midi POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ORT_ROOT}/lib/onnxruntime.dll"
        "$<TARGET_FILE_DIR:audio_to_midi>"
)
