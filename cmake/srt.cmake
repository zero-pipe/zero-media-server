# SRT optional transport (libsrt). Included from root CMakeLists.txt.

# Windows: copy srt.dll next to demo exe after build (when ZMS_SRT_ROOT/bin/srt.dll exists).
function(zms_copy_srt_dll target)
    if (NOT WIN32 OR NOT ZMS_SRT_DLL OR NOT EXISTS "${ZMS_SRT_DLL}")
        return()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ZMS_SRT_DLL}" "$<TARGET_FILE_DIR:${target}>"
        COMMENT "Copy srt.dll for ${target}"
        VERBATIM)
endfunction()

if (NOT ZMS_ENABLE_SRT)
    return()
endif ()

set(_ZMS_SRT_FOUND 0)

# Explicit prefix from tools/configure (mainly Windows).
if (ZMS_SRT_ROOT)
    file(TO_CMAKE_PATH "${ZMS_SRT_ROOT}" _zms_srt_root)
    find_path(SRT_INCLUDE_DIR NAMES srt/srt.h PATHS "${_zms_srt_root}/include" NO_DEFAULT_PATH)
    find_library(SRT_LIBRARY NAMES srt libsrt PATHS "${_zms_srt_root}/lib" NO_DEFAULT_PATH)
    if (SRT_INCLUDE_DIR AND SRT_LIBRARY)
        set(_ZMS_SRT_FOUND 1)
        if (NOT TARGET zms_srt_imported)
            add_library(zms_srt_imported UNKNOWN IMPORTED)
        endif ()
        set_target_properties(zms_srt_imported PROPERTIES
            IMPORTED_LOCATION "${SRT_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${SRT_INCLUDE_DIR}")
        message(STATUS "ZMS: SRT from ZMS_SRT_ROOT=${_zms_srt_root}")
    else ()
        message(WARNING "ZMS_SRT_ROOT=${_zms_srt_root} but srt/srt.h or libsrt not found under include/ and lib/")
    endif ()
endif ()

if (NOT _ZMS_SRT_FOUND)
    find_package(PkgConfig QUIET)
    if (PkgConfig_FOUND)
        pkg_check_modules(SRT QUIET IMPORTED_TARGET srt)
        if (SRT_FOUND)
            set(_ZMS_SRT_FOUND 1)
        endif ()
    endif ()
endif ()

if (NOT _ZMS_SRT_FOUND)
    find_path(SRT_INCLUDE_DIR NAMES srt/srt.h)
    find_library(SRT_LIBRARY NAMES srt libsrt)
    if (SRT_INCLUDE_DIR AND SRT_LIBRARY)
        set(_ZMS_SRT_FOUND 1)
        if (NOT TARGET zms_srt_imported)
            add_library(zms_srt_imported UNKNOWN IMPORTED)
        endif ()
        set_target_properties(zms_srt_imported PROPERTIES
            IMPORTED_LOCATION "${SRT_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${SRT_INCLUDE_DIR}")
    endif ()
endif ()

if (NOT _ZMS_SRT_FOUND)
    message(WARNING "ZMS_ENABLE_SRT=ON but libsrt was not found (ZMS_SRT_ROOT, pkg-config srt, or srt/srt.h + libsrt)")
    return()
endif ()

list(APPEND ZMS_SOURCES
    src/live/publish/srt_ingest_session.c
    src/live/play/srt_play_session.c
    src/session/srt/srt_service.c
    src/session/srt/srt_session.c
    src/session/srt/srt_dispatch.c
    src/session/srt/srt_poller.c
)

set(ZMS_HAVE_SRT 1)

if (TARGET PkgConfig::SRT)
    set(ZMS_SRT_LIBS PkgConfig::SRT)
else ()
    set(ZMS_SRT_LIBS zms_srt_imported)
endif ()

if (ZMS_SRT_ROOT)
    file(TO_CMAKE_PATH "${ZMS_SRT_ROOT}" _zms_srt_root)
    set(ZMS_SRT_DLL "${_zms_srt_root}/bin/srt.dll" CACHE INTERNAL "SRT runtime DLL for post-build copy")
endif ()

message(STATUS "ZMS: SRT transport enabled (libsrt found)")
