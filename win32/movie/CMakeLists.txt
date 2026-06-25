cmake_minimum_required(VERSION 3.16)

# UTF-8 source
add_compile_options("$<$<AND:$<C_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:C>>:/utf-8>")
add_compile_options("$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:CXX>>:/utf-8>")
add_compile_options("$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:CXX>>:/Zc:__cplusplus>")

# ----------------------------------------------------
# DirectShow BaseClasses
# ----------------------------------------------------

set(BASECLASSES_FOLDER ../../external/baseclasses)

add_library( baseclasses 
    ${BASECLASSES_FOLDER}/amextra.cpp
    ${BASECLASSES_FOLDER}/amfilter.cpp
    ${BASECLASSES_FOLDER}/amvideo.cpp
    ${BASECLASSES_FOLDER}/arithutil.cpp
    ${BASECLASSES_FOLDER}/combase.cpp
    ${BASECLASSES_FOLDER}/cprop.cpp
    ${BASECLASSES_FOLDER}/ctlutil.cpp
    ${BASECLASSES_FOLDER}/ddmm.cpp
    ${BASECLASSES_FOLDER}/dllentry.cpp
    ${BASECLASSES_FOLDER}/dllsetup.cpp
    ${BASECLASSES_FOLDER}/mtype.cpp
    ${BASECLASSES_FOLDER}/outputq.cpp
    ${BASECLASSES_FOLDER}/perflog.cpp
    ${BASECLASSES_FOLDER}/pstream.cpp
    ${BASECLASSES_FOLDER}/pullpin.cpp
    ${BASECLASSES_FOLDER}/refclock.cpp
    ${BASECLASSES_FOLDER}/renbase.cpp
    ${BASECLASSES_FOLDER}/schedule.cpp
    ${BASECLASSES_FOLDER}/seekpt.cpp
    ${BASECLASSES_FOLDER}/source.cpp
    ${BASECLASSES_FOLDER}/strmctl.cpp
    ${BASECLASSES_FOLDER}/sysclock.cpp
    ${BASECLASSES_FOLDER}/transfrm.cpp
    ${BASECLASSES_FOLDER}/transip.cpp
    ${BASECLASSES_FOLDER}/videoctl.cpp
    ${BASECLASSES_FOLDER}/vtrans.cpp
    ${BASECLASSES_FOLDER}/winctrl.cpp
    ${BASECLASSES_FOLDER}/winutil.cpp
    ${BASECLASSES_FOLDER}/wxdebug.cpp
    ${BASECLASSES_FOLDER}/wxlist.cpp
    ${BASECLASSES_FOLDER}/wxutil.cpp
)

target_include_directories( baseclasses PUBLIC
    ${BASECLASSES_FOLDER}
)

target_compile_definitions( baseclasses PRIVATE
	_LIB
	WIN32
	UNICODE
	_UNICODE
)

# ----------------------------------------------------
# webp movieplayer (for layer)
# ----------------------------------------------------

add_subdirectory( ../../external/movie-player movie-player)

# ----------------------------------------------------
# krmovie plugin
# ----------------------------------------------------

add_library( krmovie SHARED
    ../../tp_stub/tp_stub.cpp
    asyncio.cpp
    asyncrdr.cpp
    BufferRenderer.cpp
    CDemuxOutputPin.cpp
    CDemuxSource.cpp
    CMediaSeekingProxy.cpp
    CVMRCustomAllocatorPresenter9.cpp
    CWMAllocator.cpp
    CWMBuffer.cpp
    CWMReader.cpp
    DShowException.cpp
    dslayerd.cpp
    dsmixer.cpp
    dsmovie.cpp
    dsoverlay.cpp
    krlmovie.cpp
    krmmovie.cpp
    krmovie.cpp
    MFByteStream.cpp
    MFPlayer.cpp
    PlayWindow.cpp
    IBufferRenderer_i.c
    IRendererBufferAccess_i.c
    IRendererBufferVideo_i.c
    resource.h
    krmovie.rc
    webplayer.cpp
    # libvorbis/libogg が要求する sound_malloc 等を malloc 直行で満たすスタブ。
    # 本体 (krkrz exe) は common/base/SoundAllocator.cpp を使うので別リンク単位。
    sound_alloc_stub.c
)

target_include_directories( krmovie PRIVATE 
    ../../tp_stub/
    ../
    ../visual
    ../vcproj
    ../../external/movie-player/extlibs/libyuv/include
)

target_compile_definitions( krmovie PRIVATE
	_LIB
	WIN32
	_WIN32
    UNICODE
    _UNICODE
)

target_link_libraries( krmovie PUBLIC
	baseclasses
    movieplayer
    xaudio2
)
