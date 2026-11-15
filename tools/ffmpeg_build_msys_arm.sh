#!/usr/bin/env bash
# Cross-compile minimal static FFmpeg (avcodec+avutil+ALAC/AAC) for android
# armeabi-v7a inside MSYS2, using the NDK Windows clang toolchain.
set -euo pipefail
export PATH=/usr/bin:/bin:/mingw64/bin:$PATH
export MSYSTEM=MSYS

ROOT=/c/Users/zou/Doubao/chats/2026-10-17/new-chat
LOG=$ROOT/.devtools/ffmpeg-msys-arm-build.log
mkdir -p $ROOT/.devtools
exec > >(tee -a "$LOG") 2>&1
echo "=== [$(date)] ffmpeg armeabi-v7a MSYS2 build start ==="

NDKBIN=$ROOT/.devtools/android-sdk/ndk/27.0.12077973/toolchains/llvm/prebuilt/windows-x86_64/bin
WRAP=$ROOT/.devtools/ffmpeg-wrappers
SYSROOT=$NDKBIN/../sysroot
SRC_WIN=$ROOT/AirPlayReceiver/app/src/main/cpp/third_party/ffmpeg
BUILD=$ROOT/.devtools/ffmpeg-build-arm
PREFIX=$ROOT/.devtools/ffmpeg-prebuild-arm

chmod +x "$WRAP/armv7a-clang" "$WRAP/armv7a-clang++"

rm -rf "$BUILD" "$PREFIX"
mkdir -p "$BUILD"
echo "[FF-arm] copying source -> $BUILD ..."
cp -r "$SRC_WIN/." "$BUILD/"
cd "$BUILD"

echo "[FF-arm] configure..."
./configure \
  --prefix="$PREFIX" \
  --target-os=android --arch=arm --cpu=armv7-a --enable-cross-compile \
  --cc="$WRAP/armv7a-clang" --cxx="$WRAP/armv7a-clang++" \
  --ar="$NDKBIN/llvm-ar.exe" --nm="$NDKBIN/llvm-nm.exe" \
  --ranlib="$NDKBIN/llvm-ranlib.exe" --strip="$NDKBIN/llvm-strip.exe" \
  --sysroot="$SYSROOT" \
  --enable-pic --disable-asm --disable-x86asm \
  --disable-all --disable-debug --disable-network --disable-autodetect \
  --enable-avcodec --enable-decoder=alac,aac --enable-static --disable-shared

echo "[FF-arm] make -j16..."
make -j16
echo "[FF-arm] make install..."
make install

echo "=== [$(date)] ffmpeg armeabi-v7a build done ==="
ls -la "$PREFIX/lib"
