cmake_minimum_required(VERSION 3.16)

project(sipserver C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_library(sipserver STATIC
    src/config.c
    src/ikcp.c
    src/net.c
    src/streamer.c
    src/sip_session.c
    src/sip_server.c
    src/sip_embed.c
)

add_executable(sipserver_upper_push_demo
    example/upper_push_demo.c
)

target_include_directories(sipserver
    PUBLIC include
    PRIVATE src src/internal
)
target_include_directories(sipserver_upper_push_demo PRIVATE include)

target_compile_options(sipserver PRIVATE -Wall -Wextra -Wpedantic)
target_compile_options(sipserver_upper_push_demo PRIVATE -Wall -Wextra -Wpedantic)

target_compile_definitions(sipserver PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_definitions(sipserver_upper_push_demo PRIVATE _POSIX_C_SOURCE=200809L)

target_link_libraries(sipserver PRIVATE Threads::Threads)
target_link_libraries(sipserver_upper_push_demo PRIVATE sipserver Threads::Threads)
