SHELL = /bin/bash

ifeq ($(VCPKG_ROOT),)
$(error Variables VCPKG_ROOT not set correctly.)
endif

ifeq ($(shell type cygpath >& /dev/null && echo true),true)
FIXPATH = cygpath -ma
else
FIXPATH = realpath
endif

# Detect OS and set default PRESET accordingly
ifeq ($(OS),Windows_NT)
	PRESET?=x64-windows
else
	UNAME_S := $(shell uname -s)
	UNAME_M := $(shell uname -m)
	ifeq ($(UNAME_S),Linux)
		ifeq ($(UNAME_M),aarch64)
			PRESET?=arm64-linux
		else
			PRESET?=x64-linux
		endif
	else ifeq ($(UNAME_S),Darwin)
		ifeq ($(UNAME_M),arm64)
			PRESET?=arm64-osx
		else
			PRESET?=x64-osx
		endif
	else
		PRESET?=x64-windows
	endif
endif

BUILD_TYPE?=Release
CMAKEOPT?=
INSTALL_PREFIX?=install

BUILD_PATH=$(shell cmake --preset $(PRESET) -N | grep BUILD_DIR | sed 's/.*BUILD_DIR="\(.*\)"/\1/')
# CMake 4.x の `--preset -N` は変数を表示しないので、 取れなければプリセット既定の build/<preset>
ifeq ($(strip $(BUILD_PATH)),)
BUILD_PATH=build/$(PRESET)
endif

.PHONY: prebuild build clean install run test

all: build

# cmake 処理実行
# CMAKEOPT で引数定義追加
prebuild:
	cmake --preset $(PRESET) ${CMAKEOPT}
# ビルド実行
build:
	cmake --build $(BUILD_PATH) --config $(BUILD_TYPE)

clean:
	cmake --build $(BUILD_PATH) --config $(BUILD_TYPE) --target clean

install:
	cmake --install $(BUILD_PATH) --config $(BUILD_TYPE) --prefix $(INSTALL_PREFIX)

# SIMD parity テスト のビルド + 実行
# simd_parity: 画像処理 SIMD (SSE2/AVX2/NEON) を C リファレンスと byte-exact
#              (一部 tolerance) で比較
# sound_parity: sound SIMD (SSE/NEON) を C リファレンスと tolerance 比較
# どちらも SDL3 / OpenGL / vcpkg 非依存のスタンドアロンテスト。
test:
	cmake --build $(BUILD_PATH) --config $(BUILD_TYPE) --target krkrz_simd_parity_test
	cmake --build $(BUILD_PATH) --config $(BUILD_TYPE) --target krkrz_sound_parity_test
	ctest --test-dir $(BUILD_PATH) -C $(BUILD_TYPE) -R parity --output-on-failure

# WIN版用ルール
ifeq ($(findstring windows,$(PRESET)),windows)

DATAPATH?=data
DATAPATH_ABS=$(shell $(FIXPATH) "$(DATAPATH)")

ifeq (x86,$(findstring x86,$(PRESET)))
PLUGINS_SRC_DIR=plugin
PLUGINS_DST_DIR=$(BUILD_PATH)/$(BUILD_TYPE)/plugin
EXEFILE=$(BUILD_PATH)/$(BUILD_TYPE)/krkrz.exe
endif

ifeq (x64,$(findstring x64,$(PRESET)))
PLUGINS_SRC_DIR=plugin64
PLUGINS_DST_DIR=$(BUILD_PATH)/$(BUILD_TYPE)/plugin64
EXEFILE=$(BUILD_PATH)/$(BUILD_TYPE)/krkrz64.exe
endif

PLUGINS = $(patsubst $(PLUGINS_SRC_DIR)/%.dll, $(PLUGINS_DST_DIR)/%.dll, $(wildcard $(PLUGINS_SRC_DIR)/*.dll))

$(PLUGINS_DST_DIR)/%.dll : $(PLUGINS_SRC_DIR)/%.dll
	@mkdir -p `dirname $@`
	cp $< $@

$(EXEFILE): build

run: $(EXEFILE) $(PLUGINS)
	$(EXEFILE) $(DATAPATH_ABS)

endif
