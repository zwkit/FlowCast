#!/usr/bin/env bash
# Diagnose an NDK toolchain dir: file types, clang versions, wrapper contents.
# Usage: ./diag_ndk.sh [NDK_TOOLCHAIN_BIN]
#   NDK_TOOLCHAIN_BIN defaults to $ANDROID_NDK/toolchains/llvm/prebuilt/*/bin
#   (override with the first arg or the NDK_BIN env var).
set -u
if [ $# -ge 1 ]; then
    B="$1"
elif [ -n "${NDK_BIN:-}" ]; then
    B="$NDK_BIN"
elif [ -n "${ANDROID_NDK:-}" ] || [ -n "${ANDROID_HOME:-}" ]; then
    NDK_ROOT="${ANDROID_NDK:-${ANDROID_HOME:-}/ndk}"
    B=$(ls -d "$NDK_ROOT"/*/toolchains/llvm/prebuilt/*/bin 2>/dev/null | head -1)
else
    echo "usage: $0 [NDK_TOOLCHAIN_BIN]  (or set NDK_BIN / ANDROID_NDK)" >&2
    exit 2
fi
if [ ! -d "$B" ]; then
    echo "toolchain bin dir not found: $B" >&2
    exit 1
fi
echo "toolchain: $B"
echo "== file types =="
file "$B/clang" "$B/clang-18" "$B/llvm-ar" 2>&1
echo "== run clang-18 =="
"$B/clang-18" --version 2>&1 | head -1
echo "== run clang (symlink) =="
"$B/clang" --version 2>&1 | head -1
echo "== wrapper head =="
head -8 "$B/aarch64-linux-android24-clang" 2>&1
echo "== ls clang entries =="
ls -la "$B/clang" "$B/clang-18" 2>&1
