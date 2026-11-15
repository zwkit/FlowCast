#!/usr/bin/env bash
# Cross-compile a minimal static FFmpeg (avcodec + avutil + ALAC/AAC decoders)
# for Android arm64-v8a, using the Linux NDK inside WSL.
# Output lands in $PREFIX (libavcodec.a / libavutil.a / include) and is then
# copied back to Windows for the existing Gradle/CMake build to link against.
set -euo pipefail

LOG=/home/zw/ffbuild/build.log
mkdir -p /home/zw/ffbuild
exec > >(tee -a "$LOG") 2>&1

echo "=== [$(date)] ffmpeg WSL build start ==="

# Use NDK r27 (27.0.12077973) to exactly match the Windows NDK that builds
# the rest of the native code. Extract with 7z so symlinks (clang->clang-N)
# are restored correctly.
NDK_URL="https://dl.google.com/android/repository/android-ndk-r27-linux.zip"
NDK_ZIP="/home/zw/ffbuild/android-ndk-r27-linux.zip"
NDK_DIR="/home/zw/ffbuild/android-ndk-r27"

# 1) NDK download & extract (idempotent, into user-writable dir)
if [ ! -d "$NDK_DIR" ]; then
  if [ ! -f "$NDK_ZIP" ]; then
    echo "[NDK] downloading..."
    curl -fSL -o "$NDK_ZIP" "$NDK_URL"
  fi
  echo "[NDK] extracting via 7z -> $NDK_DIR ..."
  7z x -y -o/home/zw/ffbuild "$NDK_ZIP" >/dev/null
  echo "[NDK] source.properties:"
  grep -E 'Pkg.Revision|Pkg.Desc' "$NDK_DIR/source.properties" || true
else
  echo "[NDK] already present at $NDK_DIR"
fi

TC="$NDK_DIR/toolchains/llvm/prebuilt/linux-x86_64/bin"
SYSROOT="$NDK_DIR/toolchains/llvm/prebuilt/linux-x86_64/sysroot"

# 2) copy ffmpeg source into the Linux filesystem (configure must run in-tree)
SRC_WIN="/mnt/c/Users/zou/Doubao/chats/2026-10-17/new-chat/AirPlayReceiver/app/src/main/cpp/third_party/ffmpeg"
FFSRC="/home/zw/ffbuild/ffmpeg-src"
PREFIX="/home/zw/ffbuild/ffmpeg-install"
rm -rf "$FFSRC" "$PREFIX"
mkdir -p "$FFSRC"
echo "[FF] copying source from drvfs -> $FFSRC ..."
cp -r "$SRC_WIN/." "$FFSRC/"
cd "$FFSRC"

# sanity: NDK clang must actually be executable (symlink restored)
echo "[FF] sanity clang check..."
"$TC/clang" --version | head -1
test -x "$TC/aarch64-linux-android24-clang" && echo "[FF] wrapper OK"

# 3) configure -- flags mirror BuildFFmpeg.cmake (aarch64, API level 24)
echo "[FF] configure..."
./configure \
  --prefix="$PREFIX" \
  --target-os=android --arch=aarch64 --enable-cross-compile \
  --cc="$TC/aarch64-linux-android24-clang" \
  --cxx="$TC/aarch64-linux-android24-clang++" \
  --ar="$TC/llvm-ar" --nm="$TC/llvm-nm" \
  --ranlib="$TC/llvm-ranlib" --strip="$TC/llvm-strip" \
  --sysroot="$SYSROOT" \
  --enable-pic --disable-asm --disable-x86asm \
  --disable-all --disable-debug --disable-network --disable-autodetect \
  --enable-avcodec --enable-decoder=alac,aac --enable-static --disable-shared

# 4) build & install
echo "[FF] make -j$(nproc)..."
make -j"$(nproc)"
echo "[FF] make install..."
make install

echo "=== [$(date)] ffmpeg build done ==="
ls -la "$PREFIX/lib"
