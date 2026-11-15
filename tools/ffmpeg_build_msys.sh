#!/usr/bin/env bash
# Cross-compile minimal static FFmpeg (avcodec+avutil+ALAC/AAC) for android arm64-v8a
# inside MSYS2, using the NDK Windows clang toolchain.
set -euo pipefail
export PATH=/usr/bin:/bin:/mingw64/bin:$PATH
export MSYSTEM=MSYS

LOG=/c/Users/zou/Doubao/chats/2026-10-17/new-chat/.devtools/ffmpeg-msys-build.log
mkdir -p /c/Users/zou/Doubao/chats/2026-10-17/new-chat/.devtools
exec > >(tee -a "$LOG") 2>&1
echo "=== [$(date)] ffmpeg MSYS2 build start ==="

ROOT=/c/Users/zou/Doubao/chats/2026-10-17/new-chat
NDKBIN=$ROOT/.devtools/android-sdk/ndk/27.0.12077973/toolchains/llvm/prebuilt/windows-x86_64/bin
WRAP=$ROOT/.devtools/ffmpeg-wrappers
SYSROOT=$NDKBIN/../sysroot
SRC_WIN=$ROOT/AirPlayReceiver/app/src/main/cpp/third_party/ffmpeg
BUILD=$ROOT/.devtools/ffmpeg-build
PREFIX=$ROOT/.devtools/ffmpeg-prebuild

chmod +x "$WRAP/aarch64-clang" "$WRAP/aarch64-clang++"

rm -rf "$BUILD" "$PREFIX"
mkdir -p "$BUILD"
echo "[FF] copying source -> $BUILD ..."
cp -r "$SRC_WIN/." "$BUILD/"
cd "$BUILD"

echo "[FF] configure..."
./configure \
  --prefix="$PREFIX" \
  --target-os=android --arch=aarch64 --enable-cross-compile \
  --cc="$WRAP/aarch64-clang" --cxx="$WRAP/aarch64-clang++" \
  --ar="$NDKBIN/llvm-ar.exe" --nm="$NDKBIN/llvm-nm.exe" \
  --ranlib="$NDKBIN/llvm-ranlib.exe" --strip="$NDKBIN/llvm-strip.exe" \
  --sysroot="$SYSROOT" \
  --enable-pic --disable-asm --disable-x86asm \
  --disable-all --disable-debug --disable-network --disable-autodetect \
  --enable-avcodec --enable-decoder=alac,aac --enable-static --disable-shared

echo "[FF] make -j16..."
make -j16
echo "[FF] make install..."
make install

echo "=== [$(date)] ffmpeg build done ==="
ls -la "$PREFIX/lib"
