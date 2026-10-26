cmake_minimum_required(VERSION 3.21)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if ("${CMAKE_SYSTEM_NAME}" STREQUAL "iOS" OR IOS)
    set(CMAKE_OSX_ARCHITECTURES "arm64")
else()
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()

project(gd-rtc VERSION 0.1.0)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(${PROJECT_NAME} SHARED ${SOURCES})

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable to point to Geode")
else()
    message(STATUS "Found Geode: $ENV{GEODE_SDK}")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

# WebRTC data channels carry all high-frequency gameplay packets peer-to-peer.
# Firebase remains the low-frequency room directory and signaling transport only.
include(FetchContent)
set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_DEPS_LIBS OFF CACHE BOOL "" FORCE)
set(NO_MEDIA ON CACHE BOOL "" FORCE)
set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
set(WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)

# Android's NDK does not ship OpenSSL. Build a portable TLS backend as part
# of the mod so the same WebRTC dependency works on both target platforms.
set(USE_MBEDTLS ON CACHE BOOL "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    mbedtls
    GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
    GIT_TAG v3.6.4
    GIT_SHALLOW TRUE
    GIT_SUBMODULES_RECURSE TRUE
)
# The Geode Windows toolchain reports an MSVC simulation ID while using
# clang, so let Mbed TLS select its actual Clang compile flags.
set(_gd_rtc_simulate_id "${CMAKE_C_SIMULATE_ID}")
set(CMAKE_C_SIMULATE_ID "")
FetchContent_MakeAvailable(mbedtls)
set(CMAKE_C_SIMULATE_ID "${_gd_rtc_simulate_id}")
# The Geode Windows toolchain invokes clang.exe, not clang-cl. MSVC-style
# /W and /wd flags are invalid for C sources in libdatachannel's dependencies.
# This mod has no C sources, so leave third-party C compilation on clang defaults.
set(CMAKE_C_FLAGS "")
set(CMAKE_C_FLAGS "" CACHE STRING "" FORCE)
if(TARGET mbedtls AND NOT TARGET MbedTLS::MbedTLS)
    add_library(MbedTLS::MbedTLS ALIAS mbedtls)
endif()
# libdatachannel requires DTLS-SRTP declarations from Mbed TLS even when
# this mod uses data channels only. Enable the feature consistently in both
# the Mbed TLS library and downstream libdatachannel compilation.
if(TARGET mbedtls)
    target_compile_definitions(mbedtls PUBLIC MBEDTLS_SSL_DTLS_SRTP)
endif()

FetchContent_Declare(
    libdatachannel
    GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
    GIT_TAG v0.24.5
    GIT_SHALLOW TRUE
    GIT_SUBMODULES_RECURSE TRUE
)
FetchContent_MakeAvailable(libdatachannel)

setup_geode_mod(${PROJECT_NAME})
target_link_libraries(${PROJECT_NAME} LibDataChannel::LibDataChannelStatic)
