cmake_minimum_required(VERSION 3.24.0)

project("sunshine")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SUNSHINE_ANDROID_BOUNDARY_TESTS "Build device-independent Android boundary tests" OFF)
if(SUNSHINE_ANDROID_BOUNDARY_TESTS)
    enable_testing()
    find_package(JNI REQUIRED)
    find_package(Threads REQUIRED)
    add_executable(android_media_tests tests/android_media_tests.cpp platform/android/media_frames.cpp)
    add_executable(android_input_devices_tests tests/android_input_devices_tests.cpp platform/android/input_devices.cpp)
    target_include_directories(android_input_devices_tests PRIVATE ${JNI_INCLUDE_DIRS})
    add_executable(input_state_test tests/input_state_test.cpp)
    target_link_libraries(input_state_test PRIVATE Threads::Threads)
    add_executable(video_queue_test tests/video_queue_test.cpp)
    target_link_libraries(video_queue_test PRIVATE Threads::Threads)
    add_test(NAME video_queue_continuity COMMAND video_queue_test)
    add_test(NAME android_media_boundaries COMMAND android_media_tests)
    add_test(NAME android_input_devices COMMAND android_input_devices_tests)
    add_test(NAME android_input_ordering COMMAND input_state_test)
    return()
endif()

include(dependencies/Boost_Sunshine.cmake)
add_subdirectory(dependencies/moonlight-common-c/enet)
set(SYSTEM_OPENSSL OFF CACHE BOOL "Use bundled OpenSSL" FORCE)
set(BUILD_OPENSSL ON CACHE BOOL "Build verified OpenSSL source" FORCE)
set(CROSS_ANDROID ON CACHE BOOL "Build OpenSSL for Android" FORCE)
set(CROSS OFF CACHE BOOL "Use Android-specific cross compilation" FORCE)
set(OPENSSL_BUILD_VERSION "3.3.2" CACHE STRING "OpenSSL version" FORCE)
set(OPENSSL_BUILD_URL "https://www.openssl.org/source/openssl-3.3.2.tar.gz" CACHE STRING "OpenSSL source" FORCE)
set(OPENSSL_BUILD_HASH "2e8a40b01979afe8be0bbfb3de5dc1c6709fedb46d6c89c10da114ab5fc3d281" CACHE STRING "OpenSSL SHA256" FORCE)
set(OPENSSL_USE_STATIC_LIBS ON CACHE BOOL "Link OpenSSL into Sunshine" FORCE)
set(OPENSSL_MODULES "" CACHE STRING "Retain Sunshine crypto compatibility" FORCE)
set(OPENSSL_ENABLE_TESTS OFF CACHE BOOL "Do not run cross-compiled tests" FORCE)
set(OPENSSL_INSTALL_MAN OFF CACHE BOOL "Do not install manual pages" FORCE)
add_subdirectory(dependencies/openssl-cmake)
add_subdirectory(dependencies/Simple-Web-Server)
add_subdirectory(dependencies/libopus)

add_library(
        sunshine
        SHARED
        sunshine.cpp
        logging.cpp
        network.cpp
        config.cpp
        platform/common.cpp
        platform/android/input_bridge.cpp
        platform/android/media_frames.cpp
        platform/android/capture_renderer.cpp
        crypto.cpp
        dependencies/nanors/rs.c
        stat_trackers.cpp
        globals.cpp
        stream.cpp
        rtsp.cpp
        dependencies/moonlight-common-c/src/RtspParser.c
        audio.cpp
        nvhttp.cpp
        file_handler.cpp
        input.cpp
        video.cpp
        video_colorspace.cpp
)

target_include_directories(sunshine PRIVATE
        dependencies/moonlight-common-c/enet/include
        dependencies/ffmpeg
        dependencies/nanors/deps/obl
        dependencies/nanors
        dependencies/libopus/include
        dependencies
        ${Boost_INCLUDE_DIRS})

target_link_libraries(
        sunshine
        android
        mediandk
        log
        EGL
        GLESv3
        enet
        ${OPUS_LIBRARY}
        ssl
        crypto
        simple-web-server
        ${Boost_LIBRARIES}
) 

add_library(sunshine_input SHARED platform/android/input_devices.cpp)
target_link_libraries(sunshine_input PRIVATE log)