cmake_minimum_required(VERSION 3.22.1)
project(pocketsoccer)

add_library(app-glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c
)

add_library(pocketsoccer SHARED
    main.cpp
    game_rules.cpp
    game_world.cpp
    renderer.cpp
    wifi_session.cpp
)

target_compile_features(pocketsoccer PRIVATE cxx_std_17)
target_compile_options(pocketsoccer PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(pocketsoccer PRIVATE
    ${ANDROID_NDK}/sources/android/native_app_glue
)
target_link_libraries(pocketsoccer app-glue android log EGL GLESv2)
