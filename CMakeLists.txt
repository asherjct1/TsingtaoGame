cmake_minimum_required(VERSION 3.21)

project(
	tsingtao_express_96
	LANGUAGES    C ASM
	VERSION      1.0.0
	DESCRIPTION  "TSINGTAO EXPRESS '96 (PlayStation 1 PAL)"
)

# Output PS-X executable as build/SLES_096.00 (matching disc/SYSTEM.CNF)
# alongside build/SLES_096.elf and build/SLES_096.map.
set(PSN00BSDK_EXECUTABLE_SUFFIX ".00")

set(GAME_SOURCES
	src/main.c
	src/gfx.c
	src/room3d.c
	src/roguelike.c
	src/input.c
	src/audio.c
	src/memcard.c
	src/game.c
	src/bonus.c
	assets/gen/tim_blobs.c
	assets/gen/sfx_blobs.c
)

psn00bsdk_add_executable(SLES_096 GPREL ${GAME_SOURCES})

target_include_directories(SLES_096 PRIVATE
	${CMAKE_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
	${CMAKE_SOURCE_DIR}/assets/gen
)

target_compile_options(SLES_096 PRIVATE
	-std=gnu11
	-Wall
	-Wextra
	-O2
	-fno-strict-aliasing
)

# Multi-track PAL BIN/CUE disc image target (Track 01 Data + Track 02 Red Book CD-DA)
if(MKPSXISO AND NOT MKPSXISO STREQUAL "MKPSXISO-NOTFOUND")
	set(ISO_BIN "${CMAKE_BINARY_DIR}/tsingtao_express_96.bin")
	set(ISO_CUE "${CMAKE_BINARY_DIR}/tsingtao_express_96.cue")

	add_custom_command(
		OUTPUT "${ISO_BIN}" "${ISO_CUE}"
		COMMAND "${MKPSXISO}" -y
			-o "${ISO_BIN}"
			-c "${ISO_CUE}"
			"${CMAKE_SOURCE_DIR}/disc/tsingtao_pal.xml"
		DEPENDS
			SLES_096
			"${CMAKE_SOURCE_DIR}/disc/SYSTEM.CNF"
			"${CMAKE_SOURCE_DIR}/disc/license_pal.dat"
			"${CMAKE_SOURCE_DIR}/disc/tsingtao_pal.xml"
			"${CMAKE_SOURCE_DIR}/assets/raw/bgm.wav"
		COMMENT "Packaging PAL multi-track PS1 disc image (BIN/CUE)"
		VERBATIM
	)

	add_custom_target(iso
		DEPENDS "${ISO_BIN}" "${ISO_CUE}"
	)
endif()
