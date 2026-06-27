cmake_minimum_required(VERSION 3.22.1)
project("tun2socks" LANGUAGES C)

add_library(tun2socks SHARED
    tun2socks_jni.c
    tun2socks_engine.c
    rate_limiter.c
)

target_include_directories(tun2socks PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

find_library(log-lib log)
target_link_libraries(tun2socks ${log-lib})

# 16 KB page-size alignment for 64-bit targets (Android 15+ requirement)
target_link_options(tun2socks PRIVATE "-Wl,-z,max-page-size=16384")

target_compile_options(tun2socks PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
