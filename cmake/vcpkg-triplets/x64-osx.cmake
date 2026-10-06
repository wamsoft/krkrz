set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES x86_64)


# [krkrz] ANGLE (OpenGL ES → Metal) だけは dylib で作る。
# SDL3 は GLES コンテキスト生成時に libEGL.dylib / libGLESv2.dylib を実行時に
# dlopen するため、静的ライブラリでは使えない。アプリバンドルの
# Contents/Frameworks に同梱し、エンジンが SDL のヒントで絶対パスを渡す。
if(PORT STREQUAL "angle")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()
