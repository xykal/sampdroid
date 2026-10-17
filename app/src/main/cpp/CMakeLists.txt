cmake_minimum_required(VERSION 3.22.1)
project(sampdroid CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(sampdroid SHARED
        jni_bridge.cpp
        samp_query.cpp
        raknet_probe.cpp)

target_compile_options(sampdroid PRIVATE -O2 -fvisibility=hidden -Wall)

find_library(log-lib log)
target_link_libraries(sampdroid ${log-lib})
