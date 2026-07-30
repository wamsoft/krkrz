
# 共通の include ディレクトリ
set( KRKRZ_INC
common/environ
common/tjs2
common/base
common/extension
common/sound
common/msg
common/utils
common/visual
common/visual/elements
common/visual/gl
common/visual/IA32
common/visual/opengl
common/glad/include
external
)

set( KRKRZ_INC_WIN32_COMMON
win32/vcproj
win32/environ
common/visual/IA32
)

set( KRKRZ_INC_WIN32
win32/vcproj
win32/environ
win32/base
win32/sound
win32/msg
win32/utils
win32/visual
win32/movie
common/visual/IA32
)

set( KRKRZ_SRC 
common/tjs2/tjs.cpp
common/tjs2/tjs.tab.cpp
common/tjs2/tjsArray.cpp
common/tjs2/tjsBinarySerializer.cpp
common/tjs2/tjsByteCodeLoader.cpp
common/tjs2/tjsCompileControl.cpp
common/tjs2/tjsConfig.cpp
common/tjs2/tjsConstArrayData.cpp
common/tjs2/tjsDate.cpp
common/tjs2/tjsdate.tab.cpp
common/tjs2/tjsDateParser.cpp
common/tjs2/tjsDebug.cpp
common/tjs2/tjsDebuggerHook.cpp
common/tjs2/tjsDebuggerSymbols.cpp
common/tjs2/tjsDebuggerCore.cpp
common/tjs2/tjsDictionary.cpp
common/tjs2/tjsDisassemble.cpp
common/tjs2/tjsObjectStats.cpp
common/tjs2/tjsError.cpp
common/tjs2/tjsException.cpp
common/tjs2/tjsGlobalStringMap.cpp
common/tjs2/tjsInterCodeExec.cpp
common/tjs2/tjsInterCodeGen.cpp
common/tjs2/tjsInterface.cpp
common/tjs2/tjsLex.cpp
common/tjs2/tjsMath.cpp
common/tjs2/tjsMessage.cpp
common/tjs2/tjsMT19937ar-cok.cpp
common/tjs2/tjsNamespace.cpp
common/tjs2/tjsNative.cpp
common/tjs2/tjsObject.cpp
common/tjs2/tjsObjectExtendable.cpp
common/tjs2/tjsOctPack.cpp
common/tjs2/tjspp.tab.cpp
common/tjs2/tjsRandomGenerator.cpp
common/tjs2/tjsRegExp.cpp
common/tjs2/tjsScriptBlock.cpp
common/tjs2/tjsScriptCache.cpp
common/tjs2/tjsSnprintf.cpp
common/tjs2/tjsString.cpp
common/tjs2/tjsUtils.cpp
common/tjs2/tjsVariant.cpp
common/tjs2/tjsVariantString.cpp
common/base/BinaryStream.cpp
common/base/CharacterSet.cpp
common/base/EventIntf.cpp
common/base/FileAllocator.cpp
common/base/KrkrzAllocator.cpp
common/base/SoundAllocator.cpp
common/base/PluginIntf.cpp
common/base/PooledAllocator.cpp
common/base/ScriptMgnIntf.cpp
common/base/StorageIntf.cpp
common/base/SysInitIntf.cpp
common/base/SystemIntf.cpp
common/base/TextStream.cpp
common/base/UtilStreams.cpp
common/base/XP3Archive.cpp
common/base/StorageCache.cpp
common/environ/TouchPoint.cpp
common/extension/Extension.cpp
common/msg/MsgIntf.cpp
common/msg/ReadOptionDescUtil.cpp
common/sound/MathAlgorithms.cpp
common/sound/PhaseVocoderDSP.cpp
common/sound/PhaseVocoderFilter.cpp
common/sound/RealFFT.cpp
common/sound/SoundBufferBaseIntf.cpp
common/sound/SoundBufferBaseImpl.cpp
common/sound/WaveIntf.cpp
common/sound/WaveLoopManager.cpp
common/sound/WaveSegmentQueue.cpp
common/sound/OpusCodecDecoder.cpp
common/sound/VorbisCodecDecoder.cpp
common/utils/ClipboardIntf.cpp
common/utils/DAPServer.cpp
common/utils/LogFormat.cpp
common/utils/cp932_uni.cpp
common/utils/DebugIntf.cpp
common/utils/md5.c
common/utils/MiscUtility.cpp
common/utils/ProcessMemory.cpp
common/utils/SystemAllocatorInfo.cpp
common/utils/GlobalAllocStats.cpp
common/utils/AllocTagScope.cpp
common/base/MemoryStatPeriodicDump.cpp
common/base/MemoryOverlay.cpp
common/base/PadOverlay.cpp
common/utils/Random.cpp
common/utils/ThreadIntf.cpp
common/utils/TimerThread.cpp
common/utils/TimerIntf.cpp
common/utils/TVPTimer.cpp
common/utils/uni_cp932.cpp
common/utils/VelocityTracker.cpp
common/utils/WinConsole.cpp
common/visual/BitmapIntf.cpp
common/visual/BitmapLayerTreeOwner.cpp
common/visual/BitmapInfomation.cpp
common/visual/CharacterData.cpp
common/visual/ComplexRect.cpp
common/visual/DrawDevice.cpp
common/visual/ViewportConfig.h
common/visual/FontSystem.cpp
common/visual/FreeType.cpp
common/visual/FreeTypeFontRasterizer.cpp
common/visual/GraphicsLoaderIntf.cpp
common/visual/SimpleImageLoad.cpp
common/visual/GraphicsLoadThread.cpp
common/visual/ImageFunction.cpp
common/visual/LayerBitmapImpl.cpp
common/visual/LayerBitmapIntf.cpp
common/visual/LayerIntf.cpp
common/visual/LayerManager.cpp
common/visual/LayerTreeOwnerImpl.cpp
common/visual/LoadJPEG.cpp
common/visual/LoadPNG.cpp
common/visual/LoadTLG.cpp
common/visual/NullDrawDevice.cpp
common/visual/BitmapDrawDevice.cpp
common/visual/PrerenderedFont.cpp
common/visual/RectItf.cpp
common/visual/SaveTLG5.cpp
common/visual/SaveTLG6.cpp
common/visual/TransIntf.cpp
common/visual/tvpgl.c
common/visual/cpu_detect.cpp
common/visual/VideoOvlIntf.cpp
common/visual/WindowIntf.cpp
common/visual/KeyRepeat.cpp
common/visual/gl/blend_function.cpp
common/visual/gl/ResampleImage.cpp
common/visual/gl/WeightFunctor.cpp
common/base/FuncStubs.cpp
)

if (KRKRZ_REPL)
list(APPEND KRKRZ_SRC
	common/utils/REPL.cpp
	# REPL メインスレッド実行キュー (console / file channel 共用) と
	# -replfile= ファイル監視チャネル (エージェント駆動)。
	common/utils/ReplMainQueue.cpp
	common/utils/ReplMainQueue.h
	common/utils/ReplFileChannel.cpp
	common/utils/ReplFileChannel.h
	# abstract unix socket チャネル (Android/Linux)。CLI 引数の無い Android から
	# adb 経由で REPL を叩く。-replsocket=<name> か env KRKRZ_REPL_SOCKET で有効化。
	common/utils/ReplSocketChannel.cpp
	common/utils/ReplSocketChannel.h
	# 画面キャプチャ要求の受け渡し (overlay 込み実画面 → PNG)。
	# Elements 非依存の純粋な REPL 機能であり、 DrawDevice の Show フック
	# (SDLDrawDevice / SDLOGLDrawDevice / 共通の OGLDrawDevice) が
	# KRKRZ_USE_REPL ガード下で参照するため、 KRKRZ_USE_ELEMENTS とは無関係に
	# REPL 有効時は常にコンパイルする (ELEMENTS=OFF + REPL=ON でもリンクできる)。
	common/visual/ScreenCapture.cpp
	common/visual/ScreenCapture.h
)
endif()

if (KRKRZ_USE_OPENGL)

set( KRKRZ_SRC_OPENGL
common/visual/opengl/OGLDrawDevice.cpp
common/visual/opengl/OGLViewportBackground.h
common/visual/opengl/CanvasIntf.cpp
common/visual/opengl/GLTexture.cpp
common/visual/opengl/GLFrameBufferObject.cpp
common/visual/opengl/GLEffect.cpp
common/visual/opengl/GLEffect.h
common/visual/opengl/GLClip.cpp
common/visual/opengl/GLClip.h
common/visual/opengl/GLShaderUtil.cpp
common/visual/opengl/Matrix32Intf.cpp
common/visual/opengl/Matrix44Intf.cpp
common/visual/opengl/MemoryOverlayGL.cpp
common/visual/opengl/PadOverlayGL.cpp
common/visual/opengl/OffscreenIntf.cpp
common/visual/opengl/OpenGLError.cpp
common/visual/opengl/ShaderProgramIntf.cpp
common/visual/opengl/TextureIntf.cpp
common/visual/opengl/TextureLayerTreeOwner.cpp
common/visual/opengl/VertexBinderIntf.cpp
common/visual/opengl/VertexBufferIntf.cpp
common/glad/src/gles2.c
common/glad/src/egl.c
)

list(APPEND KRKRZ_DEFINES
TVP_USE_OPENGL
)

set( KRKRZ_SRC_WIN32_OPENGL
common/glad/src/egl.c
common/visual/opengl/EGLContext.cpp
win32/visual/OpenGLPlatform.cpp
)

endif()

set( KRKRZ_SRC_WIN32 
win32/environ/ConfigFormUnit.cpp
win32/environ/EmergencyExit.cpp
win32/environ/MouseCursor.cpp
win32/environ/SystemControl.cpp
win32/environ/TVPWindow.cpp
win32/environ/VersionFormUnit.cpp
win32/environ/WindowFormUnit.cpp
win32/environ/WindowsUtil.cpp
win32/base/EventImpl.cpp
win32/base/FileSelector.cpp
win32/base/NativeEventQueue.cpp
win32/base/PluginImpl.cpp
win32/base/SusieArchive.cpp
win32/base/ScriptMgnImpl.cpp
win32/base/StorageImpl.cpp
win32/base/SysInitImpl.cpp
win32/base/SystemImpl.cpp
win32/msg/MsgImpl.cpp
win32/msg/MsgLoad.cpp
win32/msg/ReadOptionDesc.cpp
win32/sound/tvpsnd.c
win32/sound/SoundWinCompat.cpp
win32/utils/ClipboardImpl.cpp
win32/utils/ThreadImpl.cpp
win32/visual/BasicDrawDevice.cpp
win32/visual/BitmapBitsAlloc.cpp
win32/visual/DInputMgn.cpp
win32/visual/DrawDeviceImpl.cpp
win32/visual/GDIFontRasterizer.cpp
win32/visual/GraphicsLoaderImpl.cpp
win32/visual/LayerImpl.cpp
win32/visual/NativeFreeTypeFace.cpp
win32/visual/TVPScreen.cpp
win32/visual/TVPSysFont.cpp
win32/visual/VideoOvlImpl.cpp
win32/visual/VideoPresenterD3D.cpp
win32/visual/VSyncTimingThread.cpp
win32/visual/WindowImpl.cpp
win32/environ/Application.cpp

common/utils/TickCount.cpp
win32/utils/TickCountImpl.cpp

generic/utils/LogImpl.cpp

win32/vcproj/tvpwin32.rc
win32/vcproj/dpi.manifest
)

# ----------------------------------------------------------------------------
# 画像処理 SIMD ソース群
#
# 以下は CPU アーキテクチャごとに自動取り込みされる:
#  - x86 / x86_64 系       → KRKRZ_SRC_X86_GRAPHICS_SIMD
#  - arm / arm64 系        → KRKRZ_SRC_ARM_GRAPHICS_SIMD
#
# WIN32 専用の Win32 API 依存ファイル(CPU検出 / サウンド SSE)は
# KRKRZ_SRC_WIN32_SSE に残し、従来通り if(WIN32) でのみ取り込む。
#
# 補足: AVX2 / SSSE3 を使うソースには非 MSVC 向けの per-file フラグが必要。
# その設定は本ファイル末尾で set_source_files_properties() している。
# ----------------------------------------------------------------------------

# x86/x86_64 共通の画像処理 SIMD (cross-platform)
set( KRKRZ_SRC_X86_GRAPHICS_SIMD
common/visual/gl/ResampleImageAVX2.cpp
common/visual/gl/ResampleImageSSE2.cpp
common/visual/gl/x86simdutil.cpp
common/visual/gl/x86simdutilAVX2.cpp
common/visual/gl/blend_function_sse2.cpp
common/visual/gl/blend_function_avx2.cpp
common/visual/gl/adjust_color_sse2.cpp
common/visual/gl/boxblur_sse2.cpp
common/visual/gl/colormap_avx2.cpp
common/visual/gl/colorfill_avx2.cpp
common/visual/gl/colorfill_sse2.cpp
common/visual/gl/colormap_sse2.cpp
common/visual/gl/pixelformat_sse2.cpp
common/visual/gl/tlg_sse2.cpp
common/visual/gl/univtrans_sse2.cpp
common/visual/IA32/detect_cpu.cpp
common/visual/IA32/tvpgl_ia32_intf.c
)

# ARM/ARM64 用の画像処理 SIMD
set( KRKRZ_SRC_ARM_GRAPHICS_SIMD
common/visual/gl/blend_function_neon.cpp
common/visual/gl/adjust_color_neon.cpp
common/visual/gl/colormap_neon.cpp
common/visual/gl/colorfill_neon.cpp
common/visual/gl/pixelformat_neon.cpp
)

# Win32 API 固有の補助ファイル。
# per-CPU 親和性スレッドで全コアの features をマスクする Win32 専用の
# 追加検出 + ロギング (Win32 API 依存、本質的に Windows 専用)。
# sound 系の x86 SSE は Phase SB0 で portable 化して KRKRZ_SRC_X86_SOUND_SIMD
# に移動済み。
set( KRKRZ_SRC_WIN32_SSE
win32/environ/DetectCPU.cpp
)

# ----------------------------------------------------------------------------
# sound 系 SIMD (cross-platform)。
# 画像処理 SIMD の KRKRZ_SRC_X86_GRAPHICS_SIMD / KRKRZ_SRC_ARM_GRAPHICS_SIMD
# と同じ構造。Linux/macOS x86_64 でも KRKRZ_TARGET_X86 経由で自動 wired。
# sound NEON は Phase SB2 で実装予定 (現状空 list)。
# ----------------------------------------------------------------------------
set( KRKRZ_SRC_X86_SOUND_SIMD
common/sound/MathAlgorithms_SSE.cpp
common/sound/RealFFT_SSE.cpp
common/sound/xmmlib.cpp
)

set( KRKRZ_SRC_ARM_SOUND_SIMD
common/sound/MathAlgorithms_NEON.cpp
common/sound/RealFFT_NEON.cpp
)

# ----------------------------------------------------------------------------
# アーキテクチャ自動判定
# ----------------------------------------------------------------------------
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _krkrz_arch_lc)
# Emscripten toolchain は CMAKE_SYSTEM_PROCESSOR=x86 を名乗るが、wasm に
# x86 SIMD intrinsics は無いので除外する (C リファレンス実装へフォールバック)
if (NOT EMSCRIPTEN)
if (_krkrz_arch_lc MATCHES "^(x86_64|amd64|x64|i[3-6]86|x86)$")
    set(KRKRZ_TARGET_X86 TRUE)
endif()
if (_krkrz_arch_lc MATCHES "^(aarch64|arm64|armv[0-9]|arm)")
    set(KRKRZ_TARGET_ARM TRUE)
endif()
endif()
unset(_krkrz_arch_lc)

# 画像処理 + sound SIMD を共通ソースリストに追加 (OS 非依存、アーキテクチャに応じて)
if (KRKRZ_TARGET_X86)
    list(APPEND KRKRZ_SRC ${KRKRZ_SRC_X86_GRAPHICS_SIMD})
    list(APPEND KRKRZ_SRC ${KRKRZ_SRC_X86_SOUND_SIMD})
endif()
if (KRKRZ_TARGET_ARM)
    list(APPEND KRKRZ_SRC ${KRKRZ_SRC_ARM_GRAPHICS_SIMD})
    list(APPEND KRKRZ_SRC ${KRKRZ_SRC_ARM_SOUND_SIMD})
endif()

# ----------------------------------------------------------------------------
# 拡張命令 per-file コンパイルフラグ
#  - MSVC は SSE2/SSSE3 の intrinsics を /arch なしでも受け付けるが、AVX2 は
#    /arch:AVX2 が必要。GCC/Clang は SSSE3 / AVX2 ともに per-file の -m フラグ
#    が必要。runtime dispatch で実際に呼ばれる CPU は事前にチェックされるので、
#    ファイル単位で命令を有効化しても安全。
# ----------------------------------------------------------------------------
set(_krkrz_avx2_files
    common/visual/gl/blend_function_avx2.cpp
    common/visual/gl/ResampleImageAVX2.cpp
    common/visual/gl/x86simdutilAVX2.cpp
    common/visual/gl/colormap_avx2.cpp
    common/visual/gl/colorfill_avx2.cpp
)
if (MSVC)
    set(_krkrz_avx2_flags "/arch:AVX2")
    set(_krkrz_ssse3_flags "")
else()
    set(_krkrz_avx2_flags "-mavx2;-mfma")
    set(_krkrz_ssse3_flags "-mssse3")
endif()

if (KRKRZ_TARGET_X86)
    foreach(_f ${_krkrz_avx2_files})
        set_source_files_properties(${_f} PROPERTIES COMPILE_OPTIONS "${_krkrz_avx2_flags}")
    endforeach()
    if (_krkrz_ssse3_flags)
        # blend_function_sse2.cpp / pixelformat_sse2.cpp には SSSE3 runtime-dispatch 経路が含まれている
        set_source_files_properties(
            common/visual/gl/blend_function_sse2.cpp
            common/visual/gl/pixelformat_sse2.cpp
            PROPERTIES COMPILE_OPTIONS "${_krkrz_ssse3_flags}")
    endif()
endif()

unset(_krkrz_avx2_files)
unset(_krkrz_avx2_flags)
unset(_krkrz_ssse3_flags)


set( KRKRZ_INC_GENERIC
generic/base
generic/environ
generic/msg
generic/utils
generic/visual
)

set( KRKRZ_SRC_GENERIC
common/sound/SoundDecodeThread.cpp
common/sound/SoundEventThread.cpp
common/sound/QueueSoundBufferImpl.cpp
generic/base/DrawDeviceImpl.cpp
generic/base/EventImpl.cpp
generic/base/PluginImpl.cpp
generic/base/ScriptMgnImpl.cpp
generic/base/StorageImpl.cpp
generic/base/SysInitImpl.cpp
generic/base/SystemImpl.cpp
generic/environ/Application.cpp
generic/environ/FontSystemBase.cpp
generic/environ/WindowForm.cpp
generic/environ/JoyPad.cpp
generic/msg/MsgImpl.cpp
generic/msg/MsgLoad.cpp
generic/msg/ReadOptionDesc.cpp
generic/utils/ClipboardImpl.cpp
generic/visual/BitmapBitsAlloc.cpp
generic/visual/LayerImpl.cpp
generic/visual/VideoOvlImpl.cpp
generic/visual/WindowImpl.cpp
)

set(KRKRZ_PUBLIC_HEADER 
common/visual/tvpinputdefs.h
common/visual/MoviePlayer.h
generic/krkrz.h
generic/base/LocalFileSystem.h
generic/environ/WindowFormEvent.h
generic/environ/VirtualKey.h
)
list(TRANSFORM KRKRZ_PUBLIC_HEADER PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)

if (WIN32)
	list(APPEND KRKRZ_INC_GENERIC ${KRKRZ_INC_WIN32_COMMON})
	list(APPEND KRKRZ_SRC_GENERIC ${KRKRZ_SRC_WIN32_SSE})
endif()

set(KRKRZ_SRC_SDL3
	sdl3/base/FileImpl.cpp
	sdl3/base/SDL3KirikiriIOStream.cpp
	sdl3/base/SDL3KirikiriIOStream.h
	sdl3/base/SDL3KirikiriStorage.cpp
	sdl3/base/SDL3KirikiriStorage.h
	sdl3/base/storage.cpp
	sdl3/environ/app.cpp
	sdl3/environ/app.h
	sdl3/environ/form.cpp
#	sdl3/environ/joystick.cpp
	sdl3/environ/key.cpp
	sdl3/environ/main.cpp
	sdl3/environ/pad.cpp
	sdl3/utils/LogImpl.cpp
	sdl3/utils/TickCount.cpp
	sdl3/visual/SDLDrawDevice.cpp
	sdl3/visual/SDLDrawDevice.h
	sdl3/visual/MemoryOverlayRender.cpp
	sdl3/visual/MemoryOverlayRender.h
	sdl3/visual/PadOverlayRender.cpp
	sdl3/visual/PadOverlayRender.h
	sdl3/visual/PostRenderCallback.cpp
	sdl3/visual/PostRenderCallback.h
	sdl3/visual/SDLTextureUpdateRect.h
)

if (KRKRZ_USE_OPENGL)
list(APPEND KRKRZ_SRC_SDL3
	sdl3/visual/SDLOGLDrawDevice.cpp
	sdl3/visual/SDLOGLDrawDevice.h
	sdl3/visual/SDLOGLTextureUpdateRect.h
)
endif()

set(KRKRZ_INC_SDL3
	sdl3/base
	sdl3/environ
	sdl3/utils
	sdl3/visual
)

set(KRKRZ_LIB_SDL3
	SDL3::SDL3
)

# Elements (汎用ダイアログ機構)。
# KRKRZ_USE_ELEMENTS=OFF で完全に除外可能。OFF のときは KRKRZ_HAS_ELEMENTS
# マクロが立たないので、consumer 側 (DrawDevice / WindowIntf / ScriptMgnIntf /
# Application 等) は #ifdef で全部抜ける。
if (KRKRZ_USE_ELEMENTS)
	set(KRKRZ_SRC_ELEMENTS
		common/visual/elements/DialogEventHandler.h
		common/visual/elements/DialogRenderer.h
		common/visual/elements/ElementsDialogManager.cpp
		common/visual/elements/ElementsDialogManager.h
		common/visual/elements/ElementsUserConfig.h
		common/visual/elements/DialogIntf.cpp
		common/visual/elements/DialogIntf.h
		common/visual/elements/StoragesResourceLoader.cpp
		common/visual/elements/StoragesResourceLoader.h
		common/visual/elements/VariantJsonUtil.cpp
		common/visual/elements/VariantJsonUtil.h
		# SDL 拡張プラグイン向け C ABI サービス (tp_stub プラグインから
		# TVPGetSDLDialogAPI で取得。 softkey 等が使用)
		common/visual/elements/DialogPluginService.cpp
		common/visual/elements/tp_dialog_service.h
	)
	# OpenGL ES 直接版 DrawDevice (tTVPOGLDrawDevice / tTVPSDLOGLDrawDevice) 共用の
	# dialog overlay レンダラ。GL ヘッダを include するため OpenGL ビルド時のみ。
	if (KRKRZ_USE_OPENGL)
		list(APPEND KRKRZ_SRC_ELEMENTS
			common/visual/opengl/OGLDialogRenderer.cpp
			common/visual/opengl/OGLDialogRenderer.h
		)
	endif()
	# Phase 3: SDL3 用 renderer アダプタ / Phase 7: UserConfig (SDL3 専用)
	# Phase 6c: 独立 SDL_Window モーダルランナー (SDL3 専用、 TJS Dialog.showModal* と
	# UserConfig から共用)。
	list(APPEND KRKRZ_SRC_SDL3
		${KRKRZ_SRC_ELEMENTS}
		sdl3/visual/SDLDialogRenderer.cpp
		sdl3/visual/SDLDialogRenderer.h
		sdl3/visual/SDLElementsUserConfig.cpp
		sdl3/visual/SDLElementsModalRunner.cpp
		sdl3/visual/SDLElementsModalRunner.h
	)
	# エージェント駆動制御 API (REPL / -replfile から入力注入・キャプチャ・
	# ダイアログ制御)。 dialogs()/dialogClick()/dialogTree() 等が
	# ElementsDialogManager を直接呼ぶため Elements 依存。 よって ELEMENTS と
	# REPL の両方が有効なときだけビルドする (ScriptMgnIntf の Agent 登録も
	# #if defined(KRKRZ_HAS_ELEMENTS) && defined(KRKRZ_USE_REPL) で二重ガード)。
	# 画面キャプチャ実体 (ScreenCapture.cpp) は Elements 非依存なので上の
	# KRKRZ_REPL ブロック (KRKRZ_SRC) 側に移動済み。
	if (KRKRZ_REPL)
		list(APPEND KRKRZ_SRC_SDL3
			sdl3/environ/AgentControlIntf.cpp
			sdl3/environ/AgentControlIntf.h
		)
	endif()
	list(APPEND KRKRZ_LIB_SDL3 cycfi::elements)
	# elements_modal は elements リポ管轄 (external/elements/external/elements_modal)
	# で add_subdirectory(external/elements) 経由でビルドされる。 CMakeLists.txt が
	# その直後で krkrz64 にリンクする (TARGET 存在を確認できるタイミング)。
	# このファイル (sources.cmake) は include 順の都合で elements_modal target が
	# まだ存在しない段階で評価されるため、ここで append しても TARGET 判定で抜ける。

	# ElementsDialogManager.cpp / SDLElementsUserConfig.cpp / SDLElementsModalRunner.cpp
	# は <elements.hpp> または <elements_modal/modal.h> を include して std::variant
	# 等を使うため C++20 必須。 DialogIntf.cpp も showModal* で SDLElementsModalRunner
	# を呼ぶが、 自身は elements の型を露出しないので C++17 のままで OK。
	# StoragesResourceLoader.cpp は <elements/support/font.hpp> 経由で
	# cycfi::elements::register_font を呼ぶため同様に C++20。
	set_source_files_properties(
		common/visual/elements/ElementsDialogManager.cpp
		common/visual/elements/StoragesResourceLoader.cpp
		sdl3/visual/SDLElementsUserConfig.cpp
		sdl3/visual/SDLElementsModalRunner.cpp
		PROPERTIES CXX_STANDARD 20
	)
endif()

list(APPEND KRKRZ_SRC_WIN32
	common/sound/AudioStream.cpp
	# Phase1-2: WaveSoundBuffer を miniaudio(QueueSoundBuffer) 化するため WINVER にも追加
	common/sound/SoundDecodeThread.cpp
	common/sound/SoundEventThread.cpp
	common/sound/QueueSoundBufferImpl.cpp
)
# KRKRZ_AUDIO_PLATFORM_OVERRIDE が立っている場合 (例: NX/Ounce) は
# 機種専用の iTVPAudioStream 実装を使うので、miniaudio + SDL3 audio device 経由の
# generic 経路 (common/sound/AudioStream.cpp + sdl3/sound/audio.cpp) は除外する。
if (NOT KRKRZ_AUDIO_PLATFORM_OVERRIDE)
	list(APPEND KRKRZ_SRC_SDL3
		common/sound/AudioStream.cpp
		sdl3/sound/audio.cpp
	)
	if (KRKRZ_VARIANT STREQUAL "SDL")
		list(APPEND KRKRZ_DEFINES
			# miniaudio dont use device io
			MA_NO_DEVICE_IO
		)
	endif()
endif()



# KRKRZ_USE_MOVIE=OFF のとき VideoOverlay 実装をスタブ化する
# (movie-player 非依存。TVPCreateMoviePlayer は常に nullptr を返す)
# wasm は movie-player を移植せず、ブラウザネイティブ <video> 実装を使う
# (KRKRZ_USE_MOVIE と無関係に常時有効。外部依存なし)
if(EMSCRIPTEN)
	set(KRKRZ_SRC_MOVIE sdl3/base/WebMoviePlayer.cpp)
elseif(KRKRZ_USE_MOVIE)
	set(KRKRZ_SRC_MOVIE generic/app/movie.cpp)
else()
	set(KRKRZ_SRC_MOVIE generic/app/movie_null.cpp)
endif()

if(WIN32)
	list(APPEND KRKRZ_SRC_SDL3
		sdl3/environ/stdapp.cpp
		${KRKRZ_SRC_MOVIE}
		generic/app/winres.cpp
		win32/utils/ThreadImpl.cpp
	)
elseif(APPLE)
	list(APPEND KRKRZ_SRC_SDL3
		sdl3/environ/stdapp.cpp
		${KRKRZ_SRC_MOVIE}
		sdl3/base/resource.cpp
		sdl3/utils/ThreadImpl.cpp
	)
elseif(ANDROID)
	list(APPEND KRKRZ_SRC_SDL3
		sdl3/environ/stdapp.cpp
		${KRKRZ_SRC_MOVIE}
		generic/app/andres.cpp
		sdl3/utils/ThreadImpl.cpp
	)
elseif(EMSCRIPTEN)
	# wasm: リソースは既定で外部 (MEMFS /resource へ preload、file:// 読み)。
	# その場合 resource:// メディア (objres.cpp = resource_table 消費側) は不要。
	# KRKRZ_EMSCRIPTEN_EMBED_RESOURCE=ON のときのみ埋め込み版 objres.cpp を使う
	# (resource_table は CMakeLists.txt の Bin2C 生成物が提供)。
	list(APPEND KRKRZ_SRC_SDL3
		sdl3/environ/stdapp.cpp
		${KRKRZ_SRC_MOVIE}
		sdl3/utils/ThreadImpl.cpp
	)
	if(KRKRZ_EMSCRIPTEN_EMBED_RESOURCE)
		list(APPEND KRKRZ_SRC_SDL3 generic/app/objres.cpp)
	endif()
	# devtools コンソールから TJS を評価する REPL ブリッジ (MASTER では除外)
	if(NOT MASTER)
		list(APPEND KRKRZ_SRC_SDL3
			common/utils/ReplWasmBridge.cpp
		)
	endif()
	# web:// ストレージメディア (fetch でバラファイルをオンデマンド取得)
	list(APPEND KRKRZ_SRC_SDL3
		sdl3/base/HttpStorageMedia.cpp
	)
elseif(UNIX)
	list(APPEND KRKRZ_SRC_SDL3
		sdl3/environ/stdapp.cpp
		${KRKRZ_SRC_MOVIE}
		generic/app/objres.cpp
		sdl3/utils/ThreadImpl.cpp
	)
endif()
