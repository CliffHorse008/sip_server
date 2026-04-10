cmake_minimum_required(VERSION 3.16)

project(sipserver C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_library(sipserver_config STATIC
    src/config.c
)

add_library(sipserver_net STATIC
    src/net.c
)

add_library(sipserver_media STATIC
    src/streamer.c
)

add_library(sipserver_session STATIC
    src/sip_server.c
)

add_library(sipserver_embed STATIC
    src/sip_embed.c
)

add_executable(sipserver
    src/main.c
)

target_include_directories(sipserver_config PUBLIC include)
target_include_directories(sipserver_net PUBLIC include)
target_include_directories(sipserver_media PUBLIC include)
target_include_directories(sipserver_session PUBLIC include)
target_include_directories(sipserver_embed PUBLIC include)
target_include_directories(sipserver PRIVATE include)

target_compile_options(sipserver_config PRIVATE -Wall -Wextra -Wpedantic)
target_compile_options(sipserver_net PRIVATE -Wall -Wextra -Wpedantic)
target_compile_options(sipserver_media PRIVATE -Wall -Wextra -Wpedantic)
target_compile_options(sipserver_session PRIVATE -Wall -Wextra -Wpedantic)
target_compile_options(sipserver_embed PRIVATE -Wall -Wextra -Wpedantic)
target_compile_options(sipserver PRIVATE -Wall -Wextra -Wpedantic)

target_compile_definitions(sipserver_config PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_definitions(sipserver_net PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_definitions(sipserver_media PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_definitions(sipserver_session PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_definitions(sipserver_embed PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_definitions(sipserver PRIVATE _POSIX_C_SOURCE=200809L)

target_link_libraries(sipserver_media PUBLIC sipserver_config PRIVATE sipserver_net Threads::Threads)
target_link_libraries(sipserver_session PUBLIC sipserver_config sipserver_media PRIVATE sipserver_net Threads::Threads)
target_link_libraries(sipserver_embed PUBLIC sipserver_config sipserver_media sipserver_session PRIVATE Threads::Threads)
target_link_libraries(sipserver PRIVATE sipserver_embed Threads::Threads)
