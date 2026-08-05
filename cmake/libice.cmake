# libice â€?vendored under 3rdpart/zero-media-kit/libice (ireader/sdk subset).

set(_ZMS_LIBICE_VENDOR "${CMAKE_CURRENT_SOURCE_DIR}/3rdpart/zero-media-kit/libice")

if(NOT ZMS_LIBICE_ROOT)
    set(ZMS_LIBICE_ROOT "${_ZMS_LIBICE_VENDOR}" CACHE PATH "Path to ireader libice tree (default: zero-media-kit/libice)")
endif()

option(ZMS_WEBRTC_USE_LIBICE "Use vendored libice for WebRTC ICE/STUN/TURN" ON)

if(ZMS_WEBRTC_USE_LIBICE)
    if(NOT EXISTS "${ZMS_LIBICE_ROOT}/libice/include/ice-agent.h")
        message(WARNING "ZMS_WEBRTC_USE_LIBICE=ON but libice not found at ${ZMS_LIBICE_ROOT}; WebRTC ICE disabled")
        set(ZMS_WEBRTC_USE_LIBICE OFF CACHE BOOL "Use vendored libice for WebRTC ICE/STUN/TURN" FORCE)
    else()
        set(ZMS_HAVE_LIBICE 1)
        message(STATUS "ZMS: WebRTC libice enabled (${ZMS_LIBICE_ROOT})")
    endif()
endif()
