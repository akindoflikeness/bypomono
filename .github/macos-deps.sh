#!/usr/bin/env bash
# Builds the macOS dependencies as static libraries from source into $1.
#
# Homebrew bottles are built for the runner's OS, so anything linked against
# them refuses to launch on an older macOS. Building here with one deployment
# target and linking the archives in means the app and the plugin run on every
# macOS from $MACOS_MIN up, and neither bundle carries a dylib.
set -euo pipefail

PREFIX="${1:?usage: macos-deps.sh PREFIX}"
MACOS_MIN=12.0

SDL2_VERSION=2.32.8
FREETYPE_VERSION=2.13.3
LIBPNG_VERSION=1.6.44
ZLIB_VERSION=1.3.1

export MACOSX_DEPLOYMENT_TARGET="$MACOS_MIN"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"

fetch() {
	echo "==> fetching $2"
	curl -fsSL --retry 3 -o "$1" "$2"
	tar xf "$1"
}

cmake_common=(
	-DCMAKE_BUILD_TYPE=Release
	-DCMAKE_INSTALL_PREFIX="$PREFIX"
	-DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOS_MIN"
	-DCMAKE_POSITION_INDEPENDENT_CODE=ON
	-DCMAKE_PREFIX_PATH="$PREFIX"
)

build() {
	local dir="$1"; shift
	echo "==> building $dir"
	cmake -S "$dir" -B "$dir/_b" "${cmake_common[@]}" "$@"
	cmake --build "$dir/_b" -j"$(sysctl -n hw.ncpu)"
	cmake --install "$dir/_b"
}

# zlib's CMake install always drops a dylib beside the archive, so use its
# own configure, which honours --static.
fetch zlib.tar.gz "https://github.com/madler/zlib/releases/download/v$ZLIB_VERSION/zlib-$ZLIB_VERSION.tar.gz"
echo "==> building zlib-$ZLIB_VERSION"
(
	cd "zlib-$ZLIB_VERSION"
	CFLAGS="-O2 -fPIC -mmacosx-version-min=$MACOS_MIN" ./configure --prefix="$PREFIX" --static
	make -j"$(sysctl -n hw.ncpu)"
	make install
)

# libpng and freetype are pointed straight at the archives just installed so
# find_package cannot reach a Homebrew dylib instead.
fetch libpng.tar.gz "https://github.com/pnggroup/libpng/archive/refs/tags/v$LIBPNG_VERSION.tar.gz"
build "libpng-$LIBPNG_VERSION" \
	-DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_FRAMEWORK=OFF \
	-DPNG_TESTS=OFF -DPNG_TOOLS=OFF \
	-DZLIB_LIBRARY="$PREFIX/lib/libz.a" -DZLIB_INCLUDE_DIR="$PREFIX/include"

fetch freetype.tar.xz "https://download.savannah.gnu.org/releases/freetype/freetype-$FREETYPE_VERSION.tar.xz"
build "freetype-$FREETYPE_VERSION" \
	-DBUILD_SHARED_LIBS=OFF \
	-DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON -DFT_DISABLE_BZIP2=ON \
	-DFT_REQUIRE_ZLIB=ON -DFT_REQUIRE_PNG=ON \
	-DZLIB_LIBRARY="$PREFIX/lib/libz.a" -DZLIB_INCLUDE_DIR="$PREFIX/include" \
	-DPNG_LIBRARY="$PREFIX/lib/libpng16.a" -DPNG_PNG_INCLUDE_DIR="$PREFIX/include"

# real SDL2, not Homebrew's sdl2-compat shim, which reaches SDL3 by dlopen and
# so cannot be linked statically at all.
fetch SDL2.tar.gz "https://github.com/libsdl-org/SDL/releases/download/release-$SDL2_VERSION/SDL2-$SDL2_VERSION.tar.gz"
build "SDL2-$SDL2_VERSION" \
	-DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_STATIC_PIC=ON \
	-DSDL_TEST=OFF -DSDL_LIBSAMPLERATE=OFF

echo "==> checking $PREFIX"
for a in libSDL2.a libfreetype.a libpng16.a libz.a; do
	[ -f "$PREFIX/lib/$a" ] || { echo "error: $PREFIX/lib/$a was not built" >&2; exit 1; }
	bad="$(otool -l "$PREFIX/lib/$a" | awk '$1 == "minos" {print $2}' | sort -u | grep -vx "$MACOS_MIN" || true)"
	[ -z "$bad" ] || { echo "error: $a declares minos $bad, expected $MACOS_MIN" >&2; exit 1; }
done
if ls "$PREFIX"/lib/*.dylib >/dev/null 2>&1; then
	echo "error: shared libraries were installed into $PREFIX/lib:" >&2
	ls -l "$PREFIX"/lib/*.dylib >&2
	exit 1
fi
echo "==> ok"
