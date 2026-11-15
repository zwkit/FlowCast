#!/usr/bin/env bash
# Cross-compile OpenSSL 3.4.4 (static libcrypto) for Android arm64-v8a inside
# MSYS2, using the NDK Windows clang via wrapper tools. Mirrors openssl-cmake's
# android build options so the resulting layout matches ByproductsOpenSSL.
set -euo pipefail
export PATH=/usr/bin:/bin:/mingw64/bin:$PATH
export MSYSTEM=MSYS

ROOT=/c/Users/zou/Doubao/chats/2026-10-17/new-chat
LOG=$ROOT/.devtools/openssl-msys-build.log
mkdir -p $ROOT/.devtools
exec > >(tee -a "$LOG") 2>&1
echo "=== [$(date)] openssl MSYS2 build start ==="

NDK=$ROOT/.devtools/android-sdk/ndk/27.0.12077973
CCDIR=$ROOT/.devtools/openssl-cc
SRC_FROM=$ROOT/AirPlayReceiver/app/.cxx/Debug/25624n4j/arm64-v8a/third_party/openssl-cmake/openssl-prefix/src/openssl
SRC=$ROOT/.devtools/openssl-build
PREFIX=$ROOT/.devtools/openssl-prebuild

chmod +x "$CCDIR"/*
rm -rf "$SRC" "$PREFIX"
mkdir -p "$SRC"
echo "[OSSL] copying source -> $SRC ..."
cp -r "$SRC_FROM/." "$SRC/"
cd "$SRC"

export ANDROID_NDK_ROOT="$NDK"
export ANDROID_NDK="$NDK"
export CROSS_COMPILE="$CCDIR/"
export PATH="$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin:$CCDIR:$PATH"

echo "[OSSL] Configure..."
# OpenSSL 3.4 removed most legacy no-* options; keep only accepted ones.
# Static build (no-shared) -> libcrypto.a
perl ./Configure android-arm64 -D__ANDROID_API__=24 --libdir=lib \
  no-tests no-shared --prefix=/usr/local

echo "[OSSL] make -j16..."
make -j16
echo "[OSSL] make install_sw DESTDIR=$PREFIX"
make install_sw DESTDIR="$PREFIX"

echo "=== [$(date)] openssl build done ==="
ls -la "$PREFIX/usr/local/lib" 2>/dev/null || true
