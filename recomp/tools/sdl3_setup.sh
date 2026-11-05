#!/bin/sh
# Builds SDL3, which the console reads Switch controllers through, on Linux (the Pi).
# A Mac uses Homebrew's instead: brew install sdl3.
#
#   recomp/tools/sdl3_setup.sh [--prefix DIR]
#
# Debian's own libsdl3 is older than the release pinned here, which is the first with
# Switch 2 controllers (by USB cable only, through libusb). The build has no video or
# sound: the console keeps its own window, and SDL only reads controllers. It installs
# to /usr/local by default, or to any folder, such as ~/.local, which needs no root.
# build_file.py looks in both. Without root, the headers it needs (libudev, libusb) are
# unpacked from Debian's packages into the prefix rather than installed.
set -eu

VERSION=3.4.16
SHA256=7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68
PREFIX=/usr/local
[ "${1:-}" = "--prefix" ] && PREFIX=$2
[ "$(uname -s)" = Linux ] || { echo "On a Mac: brew install sdl3"; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
ARCH=$(dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null || echo "$(uname -m)-linux-gnu")

# What SDL needs to see a controller as it plugs in (udev) and to start a Switch 2 one (libusb).
SUDO=; [ "$(id -u)" = 0 ] || SUDO=sudo
DEPS=$PREFIX/share/sdl3-build-deps
if dpkg -s libudev-dev libusb-1.0-0-dev >/dev/null 2>&1; then
  INCLUDES= LIBRARIES=
elif $SUDO -n true 2>/dev/null; then
  $SUDO apt-get install -y libudev-dev libusb-1.0-0-dev
  INCLUDES= LIBRARIES=
else
  echo "no root: unpacking libudev-dev and libusb-1.0-0-dev into $DEPS"
  mkdir -p "$DEPS"
  (cd "$WORK" && apt-get download libudev-dev libusb-1.0-0-dev && for f in *.deb; do dpkg -x "$f" "$DEPS"; done)
  LIB=$DEPS/usr/lib/$ARCH
  # The packages' links point beside themselves; the libraries are the system's.
  ln -sfn "/usr/lib/$ARCH/libudev.so.1" "$LIB/libudev.so"
  ln -sfn "/usr/lib/$ARCH/libusb-1.0.so.0" "$LIB/libusb-1.0.so"
  rm -f "$LIB/libusb-1.0.a"
  INCLUDES="-I$DEPS/usr/include -I$DEPS/usr/include/libusb-1.0"
  LIBRARIES=$LIB
fi

CMAKE=$(command -v cmake || true)
if [ -z "$CMAKE" ]; then
  echo "no cmake: installing it into $WORK/cmake for this build"
  python3 -m venv "$WORK/cmake" && "$WORK/cmake/bin/pip" -q install cmake
  CMAKE=$WORK/cmake/bin/cmake
fi

cd "$WORK"
curl -sSL -o sdl.tar.gz "https://github.com/libsdl-org/SDL/releases/download/release-$VERSION/SDL3-$VERSION.tar.gz"
echo "$SHA256  sdl.tar.gz" | sha256sum -c -
tar xzf sdl.tar.gz
set -- -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DSDL_UNIX_CONSOLE_BUILD=ON -DSDL_STATIC=OFF \
  -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_AUDIO=OFF -DSDL_VIDEO=OFF -DSDL_RENDER=OFF -DSDL_GPU=OFF \
  -DSDL_CAMERA=OFF -DSDL_DIALOG=OFF -DSDL_TRAY=OFF
if [ -n "$LIBRARIES" ]; then
  set -- "$@" -DCMAKE_C_FLAGS="$INCLUDES" -DCMAKE_LIBRARY_PATH="$LIBRARIES" \
    -DLibUSB_INCLUDE_PATH="$DEPS/usr/include/libusb-1.0" -DLibUSB_LIBRARY="$LIBRARIES/libusb-1.0.so"
fi
"$CMAKE" -S "SDL3-$VERSION" -B build -G Ninja "$@" > configure.log || { tail -30 configure.log; exit 1; }
for feature in SDL_HIDAPI_JOYSTICK SDL_LIBUDEV SDL_HIDAPI_LIBUSB; do
  grep -qE "$feature +\(Wanted: ON\): ON" configure.log || { echo "SDL was configured without $feature"; exit 1; }
done
ninja -C build
if [ -w "$PREFIX" ] || mkdir -p "$PREFIX" 2>/dev/null && [ -w "$PREFIX" ]; then ninja -C build install; else $SUDO ninja -C build install; fi
echo "SDL3 $VERSION is in $PREFIX/lib"
