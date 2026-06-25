# external/sound-codecs/CMakeLists.txt
#
# FetchContent-based build of libogg + libvorbis with their internal
# memory allocators redirected to krkrz's SoundAllocator (sound_malloc etc.).
#
# Targets defined here (with alias names matching the vcpkg find_package output,
# so downstream `target_link_libraries(... Ogg::ogg Vorbis::vorbis)` calls work
# without changes):
#   - Ogg::ogg
#   - Vorbis::vorbis
#   - Vorbis::vorbisfile
#   - Vorbis::vorbisenc
#
# Allocator hook:
#   Every .c file in libogg / libvorbis is force-included with
#   hooks/sound_alloc_hook.h (via -include or /FI). That header re-defines
#   the four `_ogg_*` macros from <ogg/os_types.h> to forward to sound_*.
#   The libogg / libvorbis source tree itself is not modified.

include(FetchContent)

# ---------------------------------------------------------------------------
# libogg
# ---------------------------------------------------------------------------
FetchContent_Declare(
    krkrz_ogg
    GIT_REPOSITORY https://github.com/xiph/ogg.git
    GIT_TAG        v1.3.6
    EXCLUDE_FROM_ALL
)

# libogg のテスト/examples を抑制
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(INSTALL_CMAKE_PACKAGE_MODULE OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(krkrz_ogg)

# 上流 CMakeLists が定義する target はそのまま "ogg"。alias を整える。
if (NOT TARGET Ogg::ogg)
    if (TARGET ogg)
        add_library(Ogg::ogg ALIAS ogg)
    endif()
endif()

# ---------------------------------------------------------------------------
# libvorbis
# ---------------------------------------------------------------------------
FetchContent_Declare(
    krkrz_vorbis
    GIT_REPOSITORY https://github.com/xiph/vorbis.git
    GIT_TAG        v1.3.7
    EXCLUDE_FROM_ALL
)

# libvorbis は同梱の FindOgg.cmake (find_library / find_path + REQUIRED_VARS=
# OGG_LIBRARY,OGG_INCLUDE_DIR の検証) を使う。FetchContent でビルドした libogg
# はまだディスク上の .lib として存在しないので、これらの cache 変数を先に
# 立てて find_library / find_path がスキップされるように仕向ける。
# Ogg::ogg は ALIAS で既存なので、FindOgg.cmake は IMPORTED target の再生成を
# スキップする (`if(NOT TARGET Ogg::ogg)` ガードあり)。下流の
# target_link_libraries(vorbis PUBLIC Ogg::ogg) は ALIAS 経由で実 target "ogg"
# にディスパッチされ、ビルド時にリンクが成立する。
get_target_property(_krkrz_ogg_include_dirs ogg INTERFACE_INCLUDE_DIRECTORIES)
set(Ogg_FOUND        TRUE                          CACHE BOOL     "" FORCE)
set(OGG_FOUND        TRUE                          CACHE BOOL     "" FORCE)
set(OGG_INCLUDE_DIR  "${_krkrz_ogg_include_dirs}"  CACHE PATH     "" FORCE)
set(OGG_INCLUDE_DIRS "${_krkrz_ogg_include_dirs}"  CACHE PATH     "" FORCE)
# OGG_LIBRARY / OGG_LIBRARIES の値はプレースホルダ。実際のリンクは ALIAS
# Ogg::ogg → target "ogg" 経由で行うので、ここの文字列値は使われない。
set(OGG_LIBRARY      "ogg"                         CACHE FILEPATH "" FORCE)
set(OGG_LIBRARIES    "ogg"                         CACHE FILEPATH "" FORCE)

FetchContent_MakeAvailable(krkrz_vorbis)

if (NOT TARGET Vorbis::vorbis)
    if (TARGET vorbis)
        add_library(Vorbis::vorbis ALIAS vorbis)
    endif()
endif()
if (NOT TARGET Vorbis::vorbisfile)
    if (TARGET vorbisfile)
        add_library(Vorbis::vorbisfile ALIAS vorbisfile)
    endif()
endif()
if (NOT TARGET Vorbis::vorbisenc)
    if (TARGET vorbisenc)
        add_library(Vorbis::vorbisenc ALIAS vorbisenc)
    endif()
endif()

# ---------------------------------------------------------------------------
# Allocator hook injection
# ---------------------------------------------------------------------------
# sound_alloc_hook.h は ogg/os_types.h を取り込んでから _ogg_* マクロを上書き
# するので、ヘッダ検索路に "include/" (sound_alloc.h) を、強制 include 用に
# "hooks/" を、それぞれ libogg/libvorbis のコンパイル時に通す必要がある。
set(_krkrz_sound_codecs_inc
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/hooks"
)

function(_krkrz_inject_sound_alloc_hook tgt)
    if (NOT TARGET ${tgt})
        return()
    endif()
    target_include_directories(${tgt} PRIVATE ${_krkrz_sound_codecs_inc})
    if (MSVC)
        target_compile_options(${tgt} PRIVATE /FI sound_alloc_hook.h)
    else()
        target_compile_options(${tgt} PRIVATE -include sound_alloc_hook.h)
    endif()
endfunction()

_krkrz_inject_sound_alloc_hook(ogg)
_krkrz_inject_sound_alloc_hook(vorbis)
_krkrz_inject_sound_alloc_hook(vorbisfile)
_krkrz_inject_sound_alloc_hook(vorbisenc)
