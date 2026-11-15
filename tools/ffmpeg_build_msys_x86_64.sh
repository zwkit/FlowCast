#!/usr/bin/env bash
# Cross-compile minimal static FFmpeg (avcodec+avutil+ALAC/AAC) for android
# x86_64 inside MSYS2, using the NDK Windows clang toolchain.
set -euo pipefail
export PATH=/usr/bin:/bin:/mingw64/bin:$PATH
export MSYSTEM=MSYS

ROOT=/c/Users/zou/Doubao/chats/2026-10-17/new-chat
LOG=$ROOT/.devtools/ffmpeg-msys-x86_64-build.log
mkdir -p $ROOT/.devtools
exec > >(tee -a "$LOG") 2>&1
echo "=== [$(date)] ffmpeg x86_64 MSYS2 build start ==="

NDKBIN=$ROOT/.devtools/android-sdk/ndk/27.0.12077973/toolchains/llvm/prebuilt/windows-x86_64/bin
WRAP=$ROOT/.devtools/ffmpeg-wrappers
SYSROOT=$NDKBIN/../sysroot
SRC_WIN=$ROOT/AirPlayReceiver/app/src/main/cpp/third_party/ffmpeg
BUILD=$ROOT/.devtools/ffmpeg-build-x86_64
PREFIX=$ROOT/.devtools/ffmpeg-prebuild-x86_64

chmod +x "$WRAP/x86_64-clang" "$WRAP/x86_64-clang++"

rm -rf "$BUILD" "$PREFIX"
mkdir -p "$BUILD"
echo "[FF-x86_64] copying source -> $BUILD ..."
cp -r "$SRC_WIN/." "$BUILD/"
cd "$BUILD"

echo "[FF-x86_64] configure..."
./configure \
  --prefix="$PREFIX" \
  --target-os=android --arch=x86_64 --cpu=x86-64 --enable-cross-compile \
  --cc="$WRAP/x86_64-clang" --cxx="$WRAP/x86_64-clang++" \
  --ar="$NDKBIN/llvm-ar.exe" --nm="$NDKBIN/llvm-nm.exe" \
  --ranlib="$NDKBIN/llvm-ranlib.exe" --strip="$NDKBIN/llvm-strip.exe" \
  --sysroot="$SYSROOT" \
  --enable-pic --disable-asm --disable-x86asm \
  --disable-all --disable-debug --disable-network --disable-autodetect \
  --enable-avcodec --enable-decoder=alac,aac --enable-static --disable-shared

echo "[FF-x86_64] make -j16..."
make -j16
echo "[FF-x86_64] make install..."
make install

echo "=== [$(date)] ffmpeg x86_64 build done ==="
ls -la "$PREFIX/lib"
