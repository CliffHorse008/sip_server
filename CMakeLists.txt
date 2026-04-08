cmake_minimum_required(VERSION 3.16)

project(sipserver C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_executable(sipserver
    src/config.c
    src/main.c
    src/net.c
    src/sip_server.c
    src/streamer.c
)

target_include_directories(sipserver PRIVATE include)
target_compile_options(sipserver PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(sipserver PRIVATE _POSIX_C_SOURCE=200809L)
target_link_libraries(sipserver PRIVATE Threads::Threads)
