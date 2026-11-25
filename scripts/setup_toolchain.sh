#!/usr/bin/env bash
set -euo pipefail

# TSINGTAO EXPRESS '96 — macOS Apple Silicon PlayStation 1 Toolchain Setup
# Installs:
#   1. mipsel-none-elf-gcc + binutils (via Homebrew if available, else compiled natively into toolchain/install)
#   2. Lameguy64/PSn00bSDK (headers, libraries, elf2x, CMake toolchain) -> toolchain/install
#   3. Lameguy64/mkpsxiso (BIN/CUE disc image packer) -> toolchain/install
#   4. Open-source PAL SCEE license sector -> disc/license_pal.dat

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TC_SRC="${ROOT_DIR}/toolchain/src"
TC_INSTALL="${ROOT_DIR}/toolchain/install"
NCPU="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

mkdir -p "${TC_SRC}" "${TC_INSTALL}"

echo "=== [1/4] Checking host prerequisites (cmake, ninja, git) ==="
for tool in cmake ninja git python3; do
	if ! command -v "${tool}" >/dev/null 2>&1; then
		echo "Missing ${tool}; installing via Homebrew..."
		brew install "${tool}"
	fi
done

echo "=== [2/4] Ensuring mipsel-none-elf-gcc toolchain ==="
if [ -x "${TC_INSTALL}/bin/mipsel-none-elf-gcc" ]; then
	echo "Found local mipsel-none-elf-gcc at ${TC_INSTALL}/bin/mipsel-none-elf-gcc"
elif command -v mipsel-none-elf-gcc >/dev/null 2>&1; then
	echo "Found system mipsel-none-elf-gcc at $(command -v mipsel-none-elf-gcc)"
else
	echo "Attempting Homebrew install of pcsx-redux/mips/mipsel-none-elf-gcc..."
	brew tap pcsx-redux/mips >/dev/null 2>&1 || true
	brew trust --formula pcsx-redux/mips/mipsel-none-elf-binutils >/dev/null 2>&1 || true
	brew trust --formula pcsx-redux/mips/mipsel-none-elf-gcc >/dev/null 2>&1 || true
	if ! HOMEBREW_NO_AUTO_UPDATE=1 brew install pcsx-redux/mips/mipsel-none-elf-gcc; then
		echo "Homebrew source build unavailable on this macOS CLT version; building mipsel-none-elf binutils + gcc locally..."
		brew install gnu-sed gmp mpfr libmpc
		TMP_BUILD="/tmp/psx-tc-build"
		TMP_OUT="/tmp/psx-tc-out"
		mkdir -p "${TMP_BUILD}"
		cd "${TMP_BUILD}"

		BINUTILS_VER="2.43.1"
		GCC_VER="14.2.0"

		if [ ! -f "binutils-${BINUTILS_VER}.tar.xz" ]; then
			curl -fSL "https://ftpmirror.gnu.org/gnu/binutils/binutils-${BINUTILS_VER}.tar.xz" -o "binutils-${BINUTILS_VER}.tar.xz"
		fi
		if [ ! -f "gcc-${GCC_VER}.tar.xz" ]; then
			curl -fSL "https://ftpmirror.gnu.org/gnu/gcc/gcc-${GCC_VER}/gcc-${GCC_VER}.tar.xz" -o "gcc-${GCC_VER}.tar.xz"
		fi

		rm -rf "binutils-${BINUTILS_VER}" build-binutils "gcc-${GCC_VER}" build-gcc "${TMP_OUT}"
		tar -xf "binutils-${BINUTILS_VER}.tar.xz"
		mkdir -p build-binutils && cd build-binutils
		"../binutils-${BINUTILS_VER}/configure" \
			--target=mipsel-none-elf \
			--prefix="${TMP_OUT}" \
			--with-system-zlib \
			--disable-nls \
			--disable-werror \
			--disable-gdb \
			--disable-sim \
			--disable-gprofng
		make -j"${NCPU}" MAKEINFO=true
		make MAKEINFO=true install-strip

		cd "${TMP_BUILD}"
		tar -xf "gcc-${GCC_VER}.tar.xz"
		mkdir -p build-gcc && cd build-gcc
		export PATH="$(brew --prefix gnu-sed)/libexec/gnubin:${TMP_OUT}/bin:${PATH}"
		"../gcc-${GCC_VER}/configure" \
			--target=mipsel-none-elf \
			--prefix="${TMP_OUT}" \
			--with-system-zlib \
			--with-gmp="$(brew --prefix gmp)" \
			--with-mpfr="$(brew --prefix mpfr)" \
			--with-mpc="$(brew --prefix libmpc)" \
			--without-isl \
			--disable-nls \
			--disable-threads \
			--disable-shared \
			--disable-libssp \
			--disable-libstdcxx-pch \
			--disable-libgomp \
			--disable-werror \
			--without-headers \
			--disable-hosted-libstdcxx \
			--enable-languages=c,c++
		make -j"${NCPU}" MAKEINFO=true all-gcc
		make MAKEINFO=true install-strip-gcc
		make -j"${NCPU}" MAKEINFO=true all-target-libgcc
		make MAKEINFO=true install-strip-target-libgcc

		cp -R "${TMP_OUT}/." "${TC_INSTALL}/"
		rm -rf "${TMP_BUILD}" "${TMP_OUT}"
	fi
fi

export PATH="${TC_INSTALL}/bin:${PATH}"
MIPS_GCC="$(command -v mipsel-none-elf-gcc)"
MIPS_TC_PREFIX="$(cd "$(dirname "${MIPS_GCC}")/.." && pwd)"
echo "Using MIPS GCC: ${MIPS_GCC} ($("${MIPS_GCC}" --version | head -n 1))"

echo "=== [3/4] Cloning & building mkpsxiso + PSn00bSDK into ${TC_INSTALL} ==="
if [ ! -d "${TC_SRC}/mkpsxiso/.git" ]; then
	git clone --depth 1 --recursive https://github.com/Lameguy64/mkpsxiso.git "${TC_SRC}/mkpsxiso"
fi
cmake -S "${TC_SRC}/mkpsxiso" -B "${TC_SRC}/mkpsxiso/build" -G Ninja \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="${TC_INSTALL}"
cmake --build "${TC_SRC}/mkpsxiso/build"
cmake --install "${TC_SRC}/mkpsxiso/build"

if [ ! -d "${TC_SRC}/PSn00bSDK/.git" ]; then
	git clone --depth 1 --recursive https://github.com/Lameguy64/PSn00bSDK.git "${TC_SRC}/PSn00bSDK"
fi
# GCC 14+ does not forward -I to gas for lowercase .s files; ensure ASM receives -Wa,-I
if ! grep -q "COMPILE_LANGUAGE:ASM" "${TC_SRC}/PSn00bSDK/libpsn00b/cmake/flags.cmake"; then
	python3 - "${TC_SRC}/PSn00bSDK/libpsn00b/cmake/flags.cmake" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
s = s.replace("\t$<$<COMPILE_LANGUAGE:CXX>:", "\t$<$<COMPILE_LANGUAGE:ASM>:\n\t\t-Wa,-I${PSN00BSDK_INCLUDE}\n\t>\n\t$<$<COMPILE_LANGUAGE:CXX>:", 1)
open(p, "w").write(s)
PY
fi
cmake -S "${TC_SRC}/PSn00bSDK" -B "${TC_SRC}/PSn00bSDK/build" -G Ninja \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="${TC_INSTALL}" \
	-DPSN00BSDK_TC="${MIPS_TC_PREFIX}" \
	-DSKIP_EXAMPLES=ON
cmake --build "${TC_SRC}/PSn00bSDK/build"
cmake --install "${TC_SRC}/PSn00bSDK/build"

echo "=== [4/4] Ensuring open-source PAL license sector (disc/license_pal.dat) ==="
if [ ! -f "${ROOT_DIR}/disc/license_pal.dat" ]; then
	python3 - "${ROOT_DIR}/disc/license_pal.dat" <<'PY'
import sys
data = bytearray(12 * 2336)
for s in range(12):
    off = s * 2336
    submode = 0x89 if s == 11 else 0x08
    data[off:off+8] = bytes([0x00, 0x00, submode, 0x00, 0x00, 0x00, submode, 0x00])
lic = b"          Licensed  by          Sony Computer Entertainment Euro pe "
assert len(lic) == 68
data[4 * 2336 + 8 : 4 * 2336 + 8 + len(lic)] = lic
data[5 * 2336 + 8 : 5 * 2336 + 20] = bytes([0x41,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00])
with open(sys.argv[1], "wb") as f:
    f.write(data)
PY
fi

echo "=== Toolchain ready! ==="
echo "  Compiler : ${MIPS_GCC}"
echo "  PSn00bSDK: ${TC_INSTALL}/lib/libpsn00b/cmake/sdk.cmake"
echo "  mkpsxiso : ${TC_INSTALL}/bin/mkpsxiso"
