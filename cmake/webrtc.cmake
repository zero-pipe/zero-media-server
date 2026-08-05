option(ZMS_ENABLE_WEBRTC "Build WebRTC WHEP/WHIP (ICE-lite; DTLS when OpenSSL available)" OFF)

if(ZMS_ENABLE_WEBRTC)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/libice.cmake)
    set(ZMS_WEBRTC_SOURCES
        src/webrtc/session/webrtc_service.c
        src/webrtc/session/webrtc_session.c
        src/webrtc/session/webrtc_sdp.c
        src/webrtc/session/webrtc_whep_route.c
        src/webrtc/session/webrtc_dispatch.c
        src/webrtc/session/webrtc_stun.c
        src/webrtc/session/webrtc_dtls.c
        src/webrtc/session/webrtc_srtp.c
        src/webrtc/session/webrtc_media_gateway.c
        src/webrtc/session/webrtc_rtcp.c
        src/webrtc/session/webrtc_rtp_twcc.c
        src/webrtc/session/webrtc_whip_route.c
        src/webrtc/whep/whep_play_session.c
        src/webrtc/whip/webrtc_whip_ingress.c
        src/webrtc/session/webrtc_ice.c
    )
    if(ZMS_HAVE_LIBICE)
        list(APPEND ZMS_WEBRTC_SOURCES
            src/webrtc/session/webrtc_ice_port.c
        )
    endif()
    if(ZTK_HAVE_OPENSSL)
        set(ZMS_HAVE_WEBRTC_DTLS 1)
    endif()
endif()
