cmake_minimum_required(VERSION 3.22.1)
project("oplplayer")

# Oboe (Gradle prefab 경유)
find_package(oboe REQUIRED CONFIG)

add_library(oplplayer SHARED
    opl_engine.cpp
    jni_bridge.cpp
    ymfm_opl.cpp
    ymfm_adpcm.cpp
    ymfm_pcm.cpp
)

target_include_directories(oplplayer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_options(oplplayer PRIVATE
    -std=c++17
    -O2
    -ffast-math
    -fvisibility=hidden
)

target_link_libraries(oplplayer
    oboe::oboe
    android
    log
)
