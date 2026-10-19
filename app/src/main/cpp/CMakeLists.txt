cmake_minimum_required(VERSION 3.22.1)
project(sampdroid CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(sampdroid SHARED
        jni_bridge.cpp
        samp_query.cpp
        client_rpc.cpp
        client_sync.cpp
        gta_img_archive.cpp
        gta_collision.cpp
        gta_collision_catalog.cpp
        gta_collision_world.cpp
        gta_data_runtime.cpp
        gta_dff_mesh.cpp
        gta_file_source.cpp
        gta_map_manifest.cpp
        gta_txd_texture.cpp
        input_state.cpp
        local_gameplay_bridge.cpp
        local_movement.cpp
        local_vehicle_movement.cpp
        local_player_sync.cpp
        sync_decoder.cpp
        raknet_codec.cpp
        raknet_string_compressor.cpp
        rpc_decoder.cpp
        rpc_state.cpp
        raknet_probe.cpp)

target_compile_options(sampdroid PRIVATE -O2 -fvisibility=hidden -Wall)

find_library(log-lib log)
target_link_libraries(sampdroid ${log-lib})
