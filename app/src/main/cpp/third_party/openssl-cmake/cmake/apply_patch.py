#!/usr/bin/env python
"""Apply a unified diff read from stdin to the current directory.
Used as the ExternalProject PATCH_COMMAND for OpenSSL builds on this project.
Emulates `patch -p1 --forward -r - < file`; arguments are ignored.
"""
import sys
import patch  # pip install patch

def main():
    data = sys.stdin.buffer.read()
    if not data.strip():
        sys.exit(0)
    ps = patch.fromstring(data)
    if ps is None:
        print("patch: failed to parse diff", file=sys.stderr)
        sys.exit(1)
    try:
        ok = ps.apply()
    except Exception as e:
        print("patch: apply error:", e, file=sys.stderr)
        sys.exit(1)
    sys.exit(0 if ok else 1)

if __name__ == "__main__":
    main()
