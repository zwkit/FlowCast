#!/usr/bin/env bash
# Cross-compile OpenSSL 3.4.4 (static libcrypto) for Android x86_64 inside
# MSYS2, using the NDK Windows clang (detected from the NDK prebuilt bin).
set -euo pipefail
export PATH=/usr/bin:/bin:/mingw64/bin:$PATH
export MSYSTEM=MSYS

ROOT=/c/Users/zou/Doubao/chats/2026-10-17/new-chat
LOG=$ROOT/.devtools/openssl-msys-x86_64-build.log
mkdir -p $ROOT/.devtools
exec > >(tee -a "$LOG") 2>&1
echo "=== [$(date)] openssl x86_64 MSYS2 build start ==="

NDK=$ROOT/.devtools/android-sdk/ndk/27.0.12077973
CCDIR=$ROOT/.devtools/openssl-cc
NDKBIN=$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin
SRC_FROM=$ROOT/.devtools/openssl-src-official/openssl-3.4.4
SRC=$ROOT/.devtools/openssl-build-x86_64c
PREFIX=$ROOT/.devtools/openssl-prebuild-x86_64

chmod +x "$CCDIR"/*
# OpenSSL android-x86_64 detection requires x86_64-linux-android-gcc/g++ inside the NDK prebuilt bin
cp -f "$CCDIR/x86_64-linux-android-gcc" "$NDKBIN/"
cp -f "$CCDIR/x86_64-linux-android-g++" "$NDKBIN/"
chmod +x "$NDKBIN/x86_64-linux-android-gcc" "$NDKBIN/x86_64-linux-android-g++"

rm -rf "$SRC" "$PREFIX"
mkdir -p "$SRC"
echo "[OSSL-x86_64] copying source -> $SRC ..."
cp -r "$SRC_FROM/." "$SRC/"
cd "$SRC"
# clear prior (arm64) configure artifacts before re-configuring for x86_64
if [ -f Makefile ]; then make clean >/dev/null 2>&1 || true; make distclean >/dev/null 2>&1 || true; fi
rm -f configdata.pm configdata.txt builddata.pm 2>/dev/null || true
find . -name '*.o' -delete 2>/dev/null || true
rm -f libcrypto.a libssl.a 2>/dev/null || true

export ANDROID_NDK_ROOT="$NDK"
export ANDROID_NDK="$NDK"
export PATH="$NDKBIN:$CCDIR:$PATH"

# OpenSSL 3.4: legacy no-* options removed; static build (no-shared) -> libcrypto.a.
# no-asm keeps it pure C (no cross-arch asm objects carried over).
perl ./Configure android-x86_64 -D__ANDROID_API__=24 --libdir=lib \
  no-tests no-shared no-asm --prefix=/usr/local

# MSYS2 fork() is unreliable on Windows (cygheap errors); build at low
# parallelism and retry so partial compiles continue incrementally.
set +e
rc=1
for i in 1 2 3 4 5 6 7 8 9 10; do
  make -j4
  rc=$?
  if [ $rc -eq 0 ]; then break; fi
  echo "[OSSL-x86_64] make attempt $i failed (rc=$rc), retrying incrementally..."
done
set -e
[ $rc -eq 0 ] || { echo "[OSSL-x86_64] make failed after all retries"; exit 1; }
make install_sw DESTDIR="$PREFIX"

echo "=== [$(date)] openssl x86_64 build done ==="
ls -la "$PREFIX/usr/local/lib" 2>/dev/null || true
