cmake_minimum_required(VERSION 3.10)
project(spacemap LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(map
    map.cpp
    scanner.cpp
    stats.cpp
    output.cpp
    tui.cpp
    mft.cpp
    tips.cpp
)

target_link_libraries(map PRIVATE kernel32)
target_compile_definitions(map PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(map PRIVATE -finput-charset=UTF-8)

if(MINGW)
    target_link_options(map PRIVATE -municode -static)
endif()
