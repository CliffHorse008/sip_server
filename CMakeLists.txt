cmake_minimum_required(VERSION 3.16)

project(sipserver C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(Threads REQUIRED)

function(target_enable_strict_warnings target)
    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
        -Wformat=2
        -Wshadow
        -Wconversion
        -Wsign-conversion
        -Wcast-align
        -Wcast-qual
        -Wstrict-prototypes
        -Wmissing-prototypes
        -Wold-style-definition
        -Wundef
        -Wwrite-strings
        -Wvla
        -Wswitch-enum
    )
endfunction()

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

target_enable_strict_warnings(sipserver)
target_enable_strict_warnings(sipserver_upper_push_demo)

set_source_files_properties(src/ikcp.c PROPERTIES
    COMPILE_OPTIONS "-w"
)

target_compile_definitions(sipserver PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_definitions(sipserver_upper_push_demo PRIVATE _POSIX_C_SOURCE=200809L)

target_link_libraries(sipserver PRIVATE Threads::Threads)
target_link_libraries(sipserver_upper_push_demo PRIVATE sipserver Threads::Threads)
