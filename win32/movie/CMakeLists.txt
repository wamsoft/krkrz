cmake_minimum_required(VERSION 3.16)

# UTF-8 source
add_compile_options("$<$<AND:$<C_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:C>>:/utf-8>")
add_compile_options("$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:CXX>>:/utf-8>")
add_compile_options("$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:CXX>>:/Zc:__cplusplus>")

# ----------------------------------------------------
# webp movieplayer (for layer)
# ----------------------------------------------------

add_subdirectory( ../../external/movie-player movie-player)

# ----------------------------------------------------
# krmovie plugin
# ----------------------------------------------------
# DirectShow / EVR(MediaSession) スタックは Track V-D で全撤去。動画は全て
#   webm     = movie-player (libvpx)     → D3D11 present
#   mpg/mpeg = pl_mpeg (MPEG-1)          → D3D11 present
#   その他   = MF SourceReader (RGB32)   → D3D11 present / レイヤバッファ
# の CPU デコード + D3D11 経路に統一。baseclasses / DirectShow への依存は無い。

# Track V-A: krmovie は exe へ静的統合するため STATIC ライブラリ化。
#  - tp_stub.cpp (namespace krkrz_plugin の import 側) は krmovie 内に保持
#    (exe の実 TVP* とは別名前空間で衝突せず、V2Link で pointer 初期化)。
#  - sound_alloc_stub.c は撤去: exe の common/base/SoundAllocator.cpp が
#    sound_malloc 等を提供するため、静的リンクすると二重定義になる。
#  - krmovie.rc / resource.h (DLL 版 About リソース) は撤去。
add_library( krmovie STATIC
    ../../tp_stub/tp_stub.cpp
    krlmovie.cpp
    krmovie.cpp
    LayerVideoBase.cpp
    MFSourceReaderVideo.cpp
    Mpeg1Video.cpp
    MediaEngineVideo.cpp
    D3D11OverlayWindow.cpp
    webplayer.cpp
)

target_include_directories( krmovie PRIVATE
    ../../tp_stub/
    ../
    ../visual
    ../vcproj
    ../../external/movie-player/extlibs/libyuv/include
    ../../external/pl_mpeg
    # Track V-E: MediaEngineVideo.cpp が VideoPresenter.h 経由で参照する common ヘッダ
    # (tjsCommHead.h=common/tjs2 / ComplexRect.h=common/visual 等)。
    ../../common/tjs2
    ../../common/base
    ../../common/environ
    ../../common/utils
    ../../common/msg
    ../../common/visual
    ../../external
)

target_compile_definitions( krmovie PRIVATE
	_LIB
	WIN32
	_WIN32
    UNICODE
    _UNICODE
    # Track V-E: VideoPresenter.h の presenter インターフェース定義 (#ifdef __WINVER__) を
    # 有効化する。krmovie は Windows 専用なので __WINVER__ を定義する。
    __WINVER__
)

target_link_libraries( krmovie PUBLIC
    movieplayer
    xaudio2
)
