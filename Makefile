# TSINGTAO EXPRESS '96 — PlayStation 1 (PAL) Top-Level Makefile

TOOLCHAIN    := toolchain/install
SDK_CMAKE    := $(TOOLCHAIN)/lib/libpsn00b/cmake/sdk.cmake
BUILD_DIR    := build

MIPS_SIZE    := $(TOOLCHAIN)/bin/mipsel-none-elf-size
MKPSXISO     := $(TOOLCHAIN)/bin/mkpsxiso

ASSETS_PY    := $(shell if [ -x "tools/.assets_venv/bin/python" ]; then echo "tools/.assets_venv/bin/python"; else echo "python3"; fi)
AUDIO_PY     := $(shell if [ -x "tools/.audio_venv/bin/python" ]; then echo "tools/.audio_venv/bin/python"; else echo "python3"; fi)

.PHONY: all toolchain assets audio configure build iso size sim clean run help

all: assets audio iso

help:
	@echo "TSINGTAO EXPRESS '96 (PS1 PAL) Build Targets:"
	@echo "  make toolchain  - Install/build mipsel-none-elf-gcc, PSn00bSDK, and mkpsxiso"
	@echo "  make assets     - Fetch genuine Tsingtao photos/logo & compile .TIM assets"
	@echo "  make audio      - Fetch Red Book CD-DA BGM & synthesize .VAG SPU SFX"
	@echo "  make build      - Cross-compile PS1 PAL executable (build/SLES_096.00)"
	@echo "  make iso        - Package PAL BIN/CUE disc image (build/tsingtao_express_96.bin/.cue)"
	@echo "  make size       - Inspect MIPS ELF RAM footprint"
	@echo "  make sim        - Build & run fast multi-threaded C balance simulator"
	@echo "  make clean      - Remove build/ directory"
	@echo "  make run        - Launch build/tsingtao_express_96.cue in an available PS1 emulator"

toolchain:
	@./scripts/setup_toolchain.sh

assets:
	@"$(ASSETS_PY)" tools/fetch_real_assets.py
	@"$(ASSETS_PY)" tools/build_assets.py

audio:
	@"$(AUDIO_PY)" tools/fetch_bgm.py
	@"$(AUDIO_PY)" tools/build_audio.py

$(BUILD_DIR)/build.ninja: CMakeLists.txt
	@if [ ! -f "$(SDK_CMAKE)" ]; then \
		echo "ERROR: PSn00bSDK not installed at $(TOOLCHAIN). Run 'make toolchain' first."; \
		exit 1; \
	fi
	@PATH="$$PWD/$(TOOLCHAIN)/bin:$$PATH" cmake -S . -B "$(BUILD_DIR)" -G Ninja \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_TOOLCHAIN_FILE="$$PWD/$(SDK_CMAKE)" \
		-DPSN00BSDK_TC="$$PWD/$(TOOLCHAIN)"

configure: $(BUILD_DIR)/build.ninja

build: $(BUILD_DIR)/build.ninja
	@PATH="$$PWD/$(TOOLCHAIN)/bin:$$PATH" cmake --build "$(BUILD_DIR)" --target SLES_096
	@if [ -x "$(MIPS_SIZE)" ]; then \
		echo ""; \
		echo "=== PS1 Main RAM Footprint (2 MB total RAM) ==="; \
		"$(MIPS_SIZE)" "$(BUILD_DIR)/SLES_096.elf"; \
	elif command -v mipsel-none-elf-size >/dev/null 2>&1; then \
		echo ""; \
		echo "=== PS1 Main RAM Footprint (2 MB total RAM) ==="; \
		mipsel-none-elf-size "$(BUILD_DIR)/SLES_096.elf"; \
	fi

iso: build
	@if [ ! -f "assets/raw/bgm.wav" ]; then \
		echo "ERROR: assets/raw/bgm.wav missing. Run 'make audio' or place a 44.1kHz stereo WAV there."; \
		exit 1; \
	fi
	@PATH="$$PWD/$(TOOLCHAIN)/bin:$$PATH" cmake --build "$(BUILD_DIR)" --target iso
	@ls -lh "$(BUILD_DIR)/SLES_096.00" "$(BUILD_DIR)/tsingtao_express_96.bin" "$(BUILD_DIR)/tsingtao_express_96.cue"

size: build

sim:
	@mkdir -p "$(BUILD_DIR)"
	cc -O3 -std=gnu11 -Wall -Wextra -pthread -Isrc -Iassets/gen \
		tools/balance_sim.c src/game.c src/roguelike.c src/bonus.c \
		-lm -o "$(BUILD_DIR)/balance_sim"
	@"$(BUILD_DIR)/balance_sim" $(SIM_ARGS)

clean:
	@rm -rf "$(BUILD_DIR)"

run: iso
	@if [ -d "/Applications/DuckStation.app" ]; then \
		open -a DuckStation "$(BUILD_DIR)/tsingtao_express_96.cue"; \
	elif command -v mednafen >/dev/null 2>&1; then \
		mednafen "$(BUILD_DIR)/tsingtao_express_96.cue"; \
	elif [ -d "/Applications/RetroArch.app" ]; then \
		open -a RetroArch "$(BUILD_DIR)/tsingtao_express_96.cue"; \
	else \
		echo "No PS1 emulator found automatically. Open $$PWD/$(BUILD_DIR)/tsingtao_express_96.cue in DuckStation, Mednafen, or RetroArch (Beetle PSX / SwanStation)."; \
	fi
