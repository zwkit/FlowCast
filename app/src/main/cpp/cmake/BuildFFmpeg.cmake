# build minimal static ffmpeg (libavcodec + libavutil + ALAC + AAC decoders) and expose under ffmpeg::avcodec and ffmpeg::avutil

include(ExternalProject)
include(ProcessorCount)

# ---- Windows local build: use the pre-cross-compiled FFmpeg static libs ----
# (produced via MSYS2 + NDK Windows clang; see tools/ffmpeg_build_msys.sh). If the
# prebuilt libs are present we import them directly and skip the source
# ExternalProject build (which needs a POSIX shell/make that Windows lacks).
set(_ff_prebuilt ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg-prebuild/${ANDROID_ABI})
if(EXISTS ${_ff_prebuilt}/lib/libavcodec.a AND EXISTS ${_ff_prebuilt}/lib/libavutil.a)
    message(STATUS "FFmpeg: using prebuilt static libs from ${_ff_prebuilt}")
    file(MAKE_DIRECTORY ${_ff_prebuilt}/include)
    # placeholder target so add_dependencies(airplay_native ffmpeg_ep) keeps working
    add_custom_target(ffmpeg_ep ALL)
    add_library(ffmpeg::avutil STATIC IMPORTED)
    set_target_properties(ffmpeg::avutil PROPERTIES
        IMPORTED_LOCATION ${_ff_prebuilt}/lib/libavutil.a
        INTERFACE_INCLUDE_DIRECTORIES ${_ff_prebuilt}/include)
    add_library(ffmpeg::avcodec STATIC IMPORTED)
    set_target_properties(ffmpeg::avcodec PROPERTIES
        IMPORTED_LOCATION ${_ff_prebuilt}/lib/libavcodec.a
        INTERFACE_INCLUDE_DIRECTORIES ${_ff_prebuilt}/include
        INTERFACE_LINK_LIBRARIES ffmpeg::avutil)
    return()
endif()
# ---- end prebuilt branch ----

# Neither prebuilt libs nor FFmpeg source present: give an actionable error
# instead of a confusing ExternalProject failure. The full source tree is not
# tracked in git (see .gitignore); fetch it when a source build is required:
#   git clone --depth 1 https://github.com/FFmpeg/FFmpeg.git \
#       app/src/main/cpp/third_party/ffmpeg
if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg/configure)
    message(FATAL_ERROR
        "BuildFFmpeg: no prebuilt libs for ABI '${ANDROID_ABI}' at "
        "${_ff_prebuilt}, and FFmpeg source is missing at "
        "third_party/ffmpeg (not tracked in git). Either restore the prebuilt "
        "static libs (tools/ffmpeg_build_msys*.sh) or fetch the FFmpeg source:\n"
        "  git clone --depth 1 https://github.com/FFmpeg/FFmpeg.git "
        "app/src/main/cpp/third_party/ffmpeg")
endif()

if(ANDROID_ABI STREQUAL "arm64-v8a")
    set(_ffmpeg_arch aarch64)
    set(_ffmpeg_triple aarch64-linux-android)
elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
    set(_ffmpeg_arch arm)
    set(_ffmpeg_triple armv7a-linux-androideabi)
    set(_ffmpeg_cpu --cpu=armv7-a)
elseif(ANDROID_ABI STREQUAL "x86_64")
    set(_ffmpeg_arch x86_64)
    set(_ffmpeg_triple x86_64-linux-android)
elseif(ANDROID_ABI STREQUAL "x86")
    set(_ffmpeg_arch x86)
    set(_ffmpeg_triple i686-linux-android)
else()
    message(FATAL_ERROR "BuildFFmpeg: unhandled ABI ${ANDROID_ABI}")
endif()

# NDK's per-API clang wrappers live next to the generic clang driver
get_filename_component(_ffmpeg_tc "${CMAKE_C_COMPILER}" DIRECTORY)

ProcessorCount(_ffmpeg_jobs)
if(_ffmpeg_jobs EQUAL 0)
    set(_ffmpeg_jobs 4)
endif()

set(FFMPEG_INSTALL ${CMAKE_BINARY_DIR}/ffmpeg)

# instrument ffmpeg with the same sanitizers; built with --disable-asm
set(_ffmpeg_sanitize "")
if(SANITIZE)
    string(REPLACE ";" " " _ff_scflags "${SANITIZE_CFLAGS}")
    string(REPLACE ";" " " _ff_sldflags "${SANITIZE_LDFLAGS}")
    set(_ffmpeg_sanitize "--extra-cflags=${_ff_scflags}" "--extra-ldflags=${_ff_sldflags}")
endif()

ExternalProject_Add(ffmpeg_ep
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg
    DOWNLOAD_COMMAND ""
    PREFIX ${CMAKE_BINARY_DIR}/ffmpeg-ep
    INSTALL_DIR ${FFMPEG_INSTALL}
    CONFIGURE_COMMAND <SOURCE_DIR>/configure
        --prefix=<INSTALL_DIR>
        --target-os=android --arch=${_ffmpeg_arch} ${_ffmpeg_cpu} --enable-cross-compile
        --cc=${_ffmpeg_tc}/${_ffmpeg_triple}${AIRPLAY_ANDROID_API}-clang
        --cxx=${_ffmpeg_tc}/${_ffmpeg_triple}${AIRPLAY_ANDROID_API}-clang++
        --ar=${_ffmpeg_tc}/llvm-ar --nm=${_ffmpeg_tc}/llvm-nm
        --ranlib=${_ffmpeg_tc}/llvm-ranlib --strip=${_ffmpeg_tc}/llvm-strip
        --sysroot=${CMAKE_SYSROOT}
        --enable-pic --disable-asm --disable-x86asm
        --disable-all --disable-debug --disable-network --disable-autodetect
        --enable-avcodec --enable-decoder=alac,aac --enable-static --disable-shared
        ${_ffmpeg_sanitize}
    BUILD_COMMAND make -j${_ffmpeg_jobs}
    INSTALL_COMMAND make install
    BUILD_BYPRODUCTS ${FFMPEG_INSTALL}/lib/libavcodec.a ${FFMPEG_INSTALL}/lib/libavutil.a
    LOG_CONFIGURE TRUE
    LOG_BUILD TRUE
    LOG_INSTALL TRUE
)

file(MAKE_DIRECTORY ${FFMPEG_INSTALL}/include)

add_library(ffmpeg::avutil STATIC IMPORTED)
set_target_properties(ffmpeg::avutil PROPERTIES
    IMPORTED_LOCATION ${FFMPEG_INSTALL}/lib/libavutil.a
    INTERFACE_INCLUDE_DIRECTORIES ${FFMPEG_INSTALL}/include
)
add_library(ffmpeg::avcodec STATIC IMPORTED)
set_target_properties(ffmpeg::avcodec PROPERTIES
    IMPORTED_LOCATION ${FFMPEG_INSTALL}/lib/libavcodec.a
    INTERFACE_INCLUDE_DIRECTORIES ${FFMPEG_INSTALL}/include
    INTERFACE_LINK_LIBRARIES ffmpeg::avutil
)
