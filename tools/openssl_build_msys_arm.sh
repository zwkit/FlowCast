#!/usr/bin/env bash
# Cross-compile OpenSSL 3.4.4 (static libcrypto) for Android armeabi-v7a inside
# MSYS2, using the NDK Windows clang (detected from the NDK prebuilt bin).
set -euo pipefail
export PATH=/usr/bin:/bin:/mingw64/bin:$PATH
export MSYSTEM=MSYS

ROOT=/c/Users/zou/Doubao/chats/2026-10-17/new-chat
LOG=$ROOT/.devtools/openssl-msys-arm-build.log
mkdir -p $ROOT/.devtools
exec > >(tee -a "$LOG") 2>&1
echo "=== [$(date)] openssl armeabi-v7a MSYS2 build start ==="

NDK=$ROOT/.devtools/android-sdk/ndk/27.0.12077973
CCDIR=$ROOT/.devtools/openssl-cc
SRC_FROM=$ROOT/.devtools/openssl-build
SRC=$ROOT/.devtools/openssl-build-arm
PREFIX=$ROOT/.devtools/openssl-prebuild-arm

chmod +x "$CCDIR"/*
rm -rf "$SRC" "$PREFIX"
mkdir -p "$SRC"
echo "[OSSL-arm] copying source -> $SRC ..."
cp -r "$SRC_FROM/." "$SRC/"
cd "$SRC"
# clear prior (arm64) configure artifacts before re-configuring for arm
if [ -f Makefile ]; then make clean >/dev/null 2>&1 || true; make distclean >/dev/null 2>&1 || true; fi
rm -f configdata.pm configdata.txt builddata.pm 2>/dev/null || true
find . -name '*.o' -delete 2>/dev/null || true
rm -f libcrypto.a libssl.a 2>/dev/null || true

export ANDROID_NDK_ROOT="$NDK"
export ANDROID_NDK="$NDK"
export PATH="$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin:$CCDIR:$PATH"

# OpenSSL 3.4: legacy no-* options removed; static build (no-shared) -> libcrypto.a.
# no-asm avoids arm64 asm objects carried over from the old arm64 tree.
perl ./Configure android-arm -D__ANDROID_API__=24 --libdir=lib \
  no-tests no-shared no-asm --prefix=/usr/local

make -j16
make install_sw DESTDIR="$PREFIX"

echo "=== [$(date)] openssl armeabi-v7a build done ==="
ls -la "$PREFIX/usr/local/lib" 2>/dev/null || true
