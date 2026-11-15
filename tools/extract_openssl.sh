#!/usr/bin/env bash
set -euo pipefail
export MSYSTEM=MSYS
ROOT=/c/Users/zou/Doubao/chats/2026-10-17/new-chat
gz=$ROOT/.devtools/downloads/openssl-3.4.4-official.tar.gz
d=$ROOT/.devtools/openssl-src-official
tar -tzf "$gz" > /dev/null
echo "TAR_OK"
mkdir -p "$d"
tar -xzf "$gz" -C "$d"
echo "EXTRACT_OK"
echo "configure: $([ -f "$d/openssl-openssl-3.4.4/Configure" ] && echo yes || echo no)"
