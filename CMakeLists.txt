cmake_minimum_required(VERSION 3.20)
project(dodio_audio_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

execute_process(
        COMMAND node -p "require('node-addon-api').include_dir"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE NODE_ADDON_API_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
)

execute_process(
        COMMAND node -p "process.versions.node"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE NODE_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
)

set(NODE_GYP_NODE_HEADERS "$ENV{LOCALAPPDATA}/node-gyp/Cache/${NODE_VERSION}/include/node")

add_library(dodio_audio_ide STATIC
        native/test.cpp
        native/audio_player.cpp
)

target_include_directories(dodio_audio_ide PRIVATE
        ${CMAKE_SOURCE_DIR}/native
        ${CMAKE_SOURCE_DIR}/${NODE_ADDON_API_DIR}
        ${NODE_GYP_NODE_HEADERS}
)

target_compile_definitions(dodio_audio_ide PRIVATE
        NAPI_DISABLE_CPP_EXCEPTIONS
)
