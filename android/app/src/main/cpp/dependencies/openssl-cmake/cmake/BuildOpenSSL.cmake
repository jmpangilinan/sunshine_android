# MIT License
#
# Copyright (c) 2015-2024 The ViaDuck Project
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
#

# build openssl locally

# includes
include(ProcessorCount)
include(ExternalProject)

# find packages
find_package(Python COMPONENTS Interpreter REQUIRED)
find_package(Perl REQUIRED)

# used to apply various patches to OpenSSL
find_program(PATCH_PROGRAM patch)
if (NOT PATCH_PROGRAM)
    message(FATAL_ERROR "Cannot find patch utility. This is only required for Android cross-compilation but due to script complexity "
                        "the requirement is always enforced")
endif()

# set variables
ProcessorCount(NUM_JOBS)
set(OS "UNIX")
if (NOT OPENSSL_BUILD_URL MATCHES "^https://")
    message(FATAL_ERROR "OPENSSL_BUILD_URL must use HTTPS")
endif()

if (NOT OPENSSL_BUILD_HASH MATCHES "^[0-9a-fA-F]+$")
    message(FATAL_ERROR "OPENSSL_BUILD_HASH must be a verified SHA256 source checksum")
endif()
string(LENGTH "${OPENSSL_BUILD_HASH}" OPENSSL_HASH_LENGTH)
if (NOT OPENSSL_HASH_LENGTH EQUAL 64)
    message(FATAL_ERROR "OPENSSL_BUILD_HASH must contain exactly 64 hexadecimal digits")
endif()
set(OPENSSL_CHECK_HASH URL_HASH SHA256=${OPENSSL_BUILD_HASH})

# ExternalProject stamps, not the existence of a partial install, own rebuilds.
    if (NOT OPENSSL_BUILD_VERSION)
        message(FATAL_ERROR "You must specify OPENSSL_BUILD_VERSION!")
    endif()

    if (WIN32 AND NOT CROSS)
        # yep, windows needs special treatment, but neither cygwin nor msys, since they provide an UNIX-like environment
        
        if (MINGW)
            set(OS "WIN32")
            message(WARNING "Building on windows is experimental")
            
            find_program(MSYS_BASH "bash.exe" PATHS "C:/Msys/" "C:/MinGW/msys/" PATH_SUFFIXES "/1.0/bin/" "/bin/"
                    DOC "Path to MSYS installation")
            if (NOT MSYS_BASH)
                message(FATAL_ERROR "Specify MSYS installation path")
            endif(NOT MSYS_BASH)
            
            set(MINGW_MAKE ${CMAKE_MAKE_PROGRAM})
            message(WARNING "Assuming your make program is a sibling of your compiler (resides in same directory)")
        elseif(NOT (CYGWIN OR MSYS))
            message(FATAL_ERROR "Unsupported compiler infrastructure")
        endif(MINGW)
        
        set(MAKE_PROGRAM ${CMAKE_MAKE_PROGRAM})
    elseif(NOT UNIX)
        message(FATAL_ERROR "Unsupported platform")
    else()
        # for OpenSSL we can only use GNU make, no exotic things like Ninja (MSYS always uses GNU make)
        find_program(MAKE_PROGRAM NAMES gmake make REQUIRED)
    endif()

    # on windows we need to replace path to perl since CreateProcess(..) cannot handle unix paths
    if (WIN32 AND NOT CROSS)
        set(PERL_PATH_FIX_INSTALL sed -i -- 's/\\/usr\\/bin\\/perl/perl/g' Makefile)
    else()
        set(PERL_PATH_FIX_INSTALL true)
    endif()

    # CROSS and CROSS_ANDROID cannot both be set (because of internal reasons)
    if (CROSS AND CROSS_ANDROID)
        # if user set CROSS_ANDROID and CROSS we assume he wants CROSS_ANDROID, so set CROSS to OFF
        set(CROSS OFF)
    endif()

    if (CROSS_ANDROID)
        set(OS "LINUX_CROSS_ANDROID")
    endif()

    # python helper script for corrent building environment
    set(BUILD_ENV_TOOL ${Python_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/building_env.py
        --bash "${MSYS_BASH}" --make "${MINGW_MAKE}" --envfile "${CMAKE_CURRENT_BINARY_DIR}/buildenv.txt" ${OS})

    # user-specified modules
    separate_arguments(CONFIGURE_OPENSSL_MODULES UNIX_COMMAND "${OPENSSL_MODULES}")

    # additional configure script parameters
    set(CONFIGURE_OPENSSL_PARAMS --libdir=lib)
    if (OPENSSL_DEBUG_BUILD)
        list(APPEND CONFIGURE_OPENSSL_PARAMS no-asm -g3 -O0 -fno-omit-frame-pointer -fno-inline-functions)
    endif()
    if (OPENSSL_RPATH)
        list(APPEND CONFIGURE_OPENSSL_PARAMS "-Wl,-rpath=${OPENSSL_RPATH}")
    endif()
    
    # set install command depending of choice on man page generation
    if (OPENSSL_INSTALL_MAN)
        set(INSTALL_OPENSSL_MAN "install_docs")
    endif()
    
    # disable building tests
    if (NOT OPENSSL_ENABLE_TESTS)
        set(CONFIGURE_OPENSSL_MODULES ${CONFIGURE_OPENSSL_MODULES} no-tests)
        set(COMMAND_TEST "true")
    endif()

    # cross-compiling
    if (CROSS)
        set(COMMAND_CONFIGURE ./Configure ${CONFIGURE_OPENSSL_PARAMS} --cross-compile-prefix=${CROSS_PREFIX} ${CROSS_TARGET} 
            ${CONFIGURE_OPENSSL_MODULES} --prefix=/usr/local/)
        set(COMMAND_TEST "true")
    elseif(CROSS_ANDROID)
        # required environment configuration is already set (by e.g. ndk) so no need to fiddle around with all the OpenSSL options ...
        if (NOT ANDROID)
            message(FATAL_ERROR "Use NDK cmake toolchain or cmake android autoconfig")
        endif()
        
        # arch options
        if (CMAKE_ANDROID_ARCH_ABI STREQUAL "armeabi-v7a")
            set(OPENSSL_PLATFORM "arm")
        elseif (CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
            set(OPENSSL_PLATFORM "arm64")
        elseif (CMAKE_ANDROID_ARCH_ABI STREQUAL "x86")
            set(OPENSSL_PLATFORM "x86")
        elseif (CMAKE_ANDROID_ARCH_ABI STREQUAL "x86_64")
            set(OPENSSL_PLATFORM "x86_64")
        else()
            message(FATAL_ERROR "Unsupported Android OpenSSL ABI: ${CMAKE_ANDROID_ARCH_ABI}")
        endif()
        
        # collect options to pass via ENV to openssl configure
        # OpenSSL 3.3.2 locates clang/llvm-ar on PATH inside the NDK and
        # chooses its API-suffixed clang wrapper from __ANDROID_API__.
        # NDK r28 has no legacy GCC toolchain directories.
        set(FORWARD_ANDROID_NDK_ROOT "${CMAKE_ANDROID_NDK}")
        if (NOT FORWARD_ANDROID_NDK_ROOT)
            set(FORWARD_ANDROID_NDK_ROOT "${ANDROID_NDK}")
        endif()
        get_filename_component(OPENSSL_TOOLCHAIN_BIN "${CMAKE_C_COMPILER}" DIRECTORY)
        set(FORWARD_PATH "${OPENSSL_TOOLCHAIN_BIN}")
        set(FORWARD_CC clang)
        set(FORWARD_AR llvm-ar)
        set(FORWARD_RANLIB llvm-ranlib)
        set(FORWARD_CFLAGS "${CMAKE_C_FLAGS} -fPIC -Qunused-arguments")
        set(FORWARD_CXXFLAGS "${CMAKE_CXX_FLAGS} -fPIC -Qunused-arguments")
        set(FORWARD_LDFLAGS "${CMAKE_SHARED_LINKER_FLAGS}")
        # Dependencies may shadow CMAKE_SYSTEM_VERSION (e.g. with 1).
        # Prefer the NDK's resolved platform, then its explicit android-N value.
        if (ANDROID_PLATFORM_LEVEL MATCHES "^[0-9]+$")
            set(OPENSSL_ANDROID_API "${ANDROID_PLATFORM_LEVEL}")
        elseif (ANDROID_PLATFORM MATCHES "^android-([0-9]+)$")
            set(OPENSSL_ANDROID_API "${CMAKE_MATCH_1}")
        elseif (ANDROID_NATIVE_API_LEVEL MATCHES "^[0-9]+$")
            set(OPENSSL_ANDROID_API "${ANDROID_NATIVE_API_LEVEL}")
        else()
            set(OPENSSL_ANDROID_API "${CMAKE_SYSTEM_VERSION}")
        endif()
        if (NOT OPENSSL_ANDROID_API MATCHES "^[0-9]+$" OR OPENSSL_ANDROID_API LESS 21)
            message(FATAL_ERROR "Android OpenSSL requires a valid NDK platform API level (21+)")
        endif()
        list(APPEND CONFIGURE_OPENSSL_PARAMS "-D__ANDROID_API__=${OPENSSL_ANDROID_API}")
        
        set(COMMAND_CONFIGURE ./Configure android-${OPENSSL_PLATFORM} ${CONFIGURE_OPENSSL_PARAMS} ${CONFIGURE_OPENSSL_MODULES})
        set(COMMAND_TEST "true")
    else()                   # detect host system automatically
        set(COMMAND_CONFIGURE ./config ${CONFIGURE_OPENSSL_PARAMS} ${CONFIGURE_OPENSSL_MODULES})
        
        if (NOT COMMAND_TEST)
            set(COMMAND_TEST ${BUILD_ENV_TOOL} <SOURCE_DIR> -- ${MAKE_PROGRAM} test)
        endif()
    endif()

    # build OPENSSL_PATCH_COMMAND
    # These vendor patches only modify tests, which Android does not run.
    if (OPENSSL_ENABLE_TESTS AND NOT CROSS_ANDROID)
        include(PatchOpenSSL)
    endif()
    set(OPENSSL_CONFIGURE_PREFIX)
    if (CROSS_ANDROID AND CMAKE_ANDROID_ARCH_ABI STREQUAL "armeabi-v7a")
        if (NOT OPENSSL_BUILD_VERSION STREQUAL "3.3.2")
            message(FATAL_ERROR "Review ARM capability visibility patch for this OpenSSL version")
        endif()
        set(OPENSSL_CONFIGURE_PREFIX ${CMAKE_COMMAND}
            -DOPENSSL_SOURCE_DIR=<SOURCE_DIR>
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PatchAndroidArmcap.cmake" COMMAND)
    endif()

    # add openssl target
    ExternalProject_Add(openssl
        URL "${OPENSSL_BUILD_URL}"
        ${OPENSSL_CHECK_HASH}
        TLS_VERIFY TRUE
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        UPDATE_COMMAND ""

        CONFIGURE_COMMAND ${OPENSSL_CONFIGURE_PREFIX} ${BUILD_ENV_TOOL} <SOURCE_DIR> -- ${COMMAND_CONFIGURE}
        ${OPENSSL_PATCH_COMMAND}

        BUILD_COMMAND ${BUILD_ENV_TOOL} <SOURCE_DIR> -- ${MAKE_PROGRAM} -j ${NUM_JOBS}
        BUILD_BYPRODUCTS ${OPENSSL_BYPRODUCTS}

        TEST_BEFORE_INSTALL 1
        TEST_COMMAND ${COMMAND_TEST}

        INSTALL_COMMAND ${BUILD_ENV_TOOL} <SOURCE_DIR> -- ${PERL_PATH_FIX_INSTALL}
        COMMAND ${BUILD_ENV_TOOL} <SOURCE_DIR> -- ${MAKE_PROGRAM} DESTDIR=${OPENSSL_PREFIX} install_sw ${INSTALL_OPENSSL_MAN}
        # Ninja already knows the installed artifacts through BUILD_BYPRODUCTS.

        LOG_INSTALL 1
    )

    # write all "FORWARD_" variables with escaped quotes to file, is picked up by python script
    get_cmake_property(_variableNames VARIABLES)
    foreach (_variableName ${_variableNames})
        if (_variableName MATCHES "^FORWARD_")
            string(REPLACE "FORWARD_" "" _envName ${_variableName})
            string(REPLACE "\"" "\\\"" _envValue "${${_variableName}}")
            set(OUT_FILE "${OUT_FILE}${_envName}=\"${_envValue}\"\n")
        endif()
    endforeach()
    set(OPENSSL_ENV_FILE "${CMAKE_CURRENT_BINARY_DIR}/buildenv.txt")
    if (EXISTS "${OPENSSL_ENV_FILE}")
        file(READ "${OPENSSL_ENV_FILE}" OPENSSL_OLD_ENV)
    endif()
    if (NOT EXISTS "${OPENSSL_ENV_FILE}" OR NOT OPENSSL_OLD_ENV STREQUAL OUT_FILE)
        file(WRITE "${OPENSSL_ENV_FILE}" "${OUT_FILE}")
    endif()
    ExternalProject_Add_StepDependencies(openssl configure
        "${OPENSSL_ENV_FILE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/building_env.py")
    if (OPENSSL_CONFIGURE_PREFIX)
        ExternalProject_Add_StepDependencies(openssl configure
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PatchAndroidArmcap.cmake")
    endif()
