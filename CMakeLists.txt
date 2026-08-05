cmake_minimum_required(VERSION 3.14)
project(zms C)

# MSVC: source/execution charset UTF-8 (avoids C4819 when headers use UTF-8 comments)
if(MSVC)
    add_compile_options(/utf-8 /wd4819)
endif()

set(ZMS_VERSION_MAJOR 0)
set(ZMS_VERSION_MINOR 5)
set(ZMS_VERSION_PATCH 0)

option(ZTK_BUILD_SHARED "Build shared ztk library" OFF)
option(ZTK_BUILD_TESTS "Build zero-tool-kit unit tests" OFF)
option(ZTK_BUILD_EXAMPLES "Build demos (ztk + ZMS)" ON)
option(ZTK_ENABLE_EPOLL "Use epoll poller backend (Linux)" ON)
option(ZTK_ENABLE_WEPOLL "Use wepoll poller backend (Win32)" ON)
option(ZTK_ENABLE_SELECT "Use select poller backend (generic POSIX/RTOS)" ON)
option(ZTK_ALLOW_WIN32_STUB "Build Win32 stub poller (no wepoll) for IDE check" OFF)
option(ZTK_ENABLE_OPENSSL "Enable OpenSSL TLS client (ztk_tls_client)" ON)
option(ZMS_BUILD_SHARED "Build shared ZMS library" OFF)
option(ZMS_ENABLE_SRT "Build SRT transport support (requires libsrt)" OFF)
option(ZMS_BUILD_TESTS "Build ZMS automated tests (Stable SDK lifecycle)" OFF)

# Local overrides (paths, feature toggles) — run tools/configure.ps1 or configure.sh
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/build.local.cmake OPTIONAL)

if (ZTK_BUILD_TESTS)
    include(CTest)
    enable_testing()
endif ()

set(_ZMS_ZTK "${CMAKE_CURRENT_SOURCE_DIR}/3rdpart/zero-tool-kit/CMakeLists.txt")
set(_ZMS_KIT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/3rdpart/zero-media-kit")
if (NOT EXISTS "${_ZMS_ZTK}")
    message(FATAL_ERROR
        "Missing submodule 3rdpart/zero-tool-kit. Run:\n"
        "  git submodule update --init --recursive")
endif ()
if (NOT EXISTS "${_ZMS_KIT_ROOT}/librtp/CMakeLists.txt")
    message(FATAL_ERROR
        "Missing submodule 3rdpart/zero-media-kit. Run:\n"
        "  git submodule update --init --recursive")
endif ()

if(ZTK_ENABLE_OPENSSL AND NOT OPENSSL_ROOT_DIR)
    if(WIN32 AND EXISTS "C:/Program Files/OpenSSL-Win64/include/openssl/ssl.h")
        set(OPENSSL_ROOT_DIR "C:/Program Files/OpenSSL-Win64" CACHE PATH "OpenSSL install prefix")
    endif()
endif()

add_subdirectory(3rdpart/zero-tool-kit)

set(ZMS_KIT "${CMAKE_CURRENT_SOURCE_DIR}/3rdpart/zero-media-kit")
add_subdirectory(${ZMS_KIT}/libmpeg)
add_subdirectory(${ZMS_KIT}/libflv)
add_subdirectory(${ZMS_KIT}/libhls)
add_subdirectory(${ZMS_KIT}/librtp)
add_subdirectory(${ZMS_KIT}/librtsp)
add_subdirectory(${ZMS_KIT}/librtmp)
add_subdirectory(${ZMS_KIT}/libmov)
add_subdirectory(${ZMS_KIT}/libmkv)
add_subdirectory(${ZMS_KIT}/libdash)

# Third-party kit: suppress vendor warnings so ZMS builds clean.
foreach(_zms_3p libflv libdash libhls libmkv libmpeg librtp librtsp librtmp libmov)
    if(TARGET ${_zms_3p})
        if(MSVC)
            target_compile_options(${_zms_3p} PRIVATE /wd4018 /wd4996 /wd4244)
        else()
            target_compile_options(${_zms_3p} PRIVATE -w)
        endif()
    endif()
endforeach()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/zms_sources.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/srt.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/webrtc.cmake)

if(ZMS_ENABLE_WEBRTC AND ZMS_HAVE_LIBICE)
    add_subdirectory(${ZMS_KIT}/libice)
endif()

if (ZMS_ENABLE_WEBRTC)
    list(APPEND ZMS_SOURCES ${ZMS_WEBRTC_SOURCES})
    message(STATUS "ZMS: WebRTC WHEP enabled")
    if(ZMS_HAVE_WEBRTC_DTLS)
        message(STATUS "ZMS: WebRTC DTLS-SRTP enabled (OpenSSL)")
    else()
        message(WARNING "ZMS: WebRTC without OpenSSL — DTLS/SRTP disabled; browser play will not work")
    endif()
endif()

if (ZMS_BUILD_SHARED)
    add_library(zms SHARED ${ZMS_SOURCES})
    target_compile_definitions(zms PRIVATE ZMS_EXPORTS)
else ()
    add_library(zms STATIC ${ZMS_SOURCES})
endif ()

target_include_directories(zms
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${ZMS_KIT}/libmov/source
)

target_link_libraries(zms PUBLIC ztk libmpeg libflv libhls librtp librtsp librtmp libmov libmkv libdash)
if (ZMS_HAVE_SRT)
    target_compile_definitions(zms PUBLIC ZMS_HAVE_SRT=1)
    target_link_libraries(zms PRIVATE ${ZMS_SRT_LIBS})
endif ()
if (ZMS_ENABLE_WEBRTC)
    target_compile_definitions(zms PUBLIC ZMS_ENABLE_WEBRTC=1)
    if(ZMS_HAVE_WEBRTC_DTLS)
        target_compile_definitions(zms PUBLIC ZMS_HAVE_WEBRTC_DTLS=1)
    endif()
    if(ZMS_HAVE_LIBICE)
        target_compile_definitions(zms PRIVATE ZMS_WEBRTC_USE_LIBICE=1)
        target_include_directories(zms PRIVATE
            ${ZMS_KIT}/libice/libice/src
            ${ZMS_KIT}/libice/include)
        if(WIN32)
            target_compile_definitions(zms PRIVATE OS_WINDOWS=1)
        endif()
        target_link_libraries(zms PRIVATE libice)
    endif()
endif()
if(WIN32)
    target_link_libraries(zms PRIVATE ws2_32)
endif()

if(MSVC)
    target_compile_definitions(zms PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

target_compile_definitions(zms PUBLIC
    "ZMS_VERSION_STRING=\"${ZMS_VERSION_MAJOR}.${ZMS_VERSION_MINOR}.${ZMS_VERSION_PATCH}\"")

set_target_properties(zms PROPERTIES
    OUTPUT_NAME zms
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

if (ZTK_BUILD_EXAMPLES AND (ZTK_HAVE_EPOLL OR ZTK_HAVE_WEPOLL OR ZTK_HAVE_SELECT))
    add_subdirectory(examples)
endif ()

add_subdirectory(tests)

install(TARGETS zms
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(DIRECTORY include/zms DESTINATION include)
