#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Generate values-zh-rHK / values-zh-rTW strings.xml from values-zh-rCN via OpenCC.

s2hk  : Simplified -> Hong Kong Traditional (香港用語)
s2twp : Simplified -> Taiwan Traditional + phrases (台灣用語)

Only the text content of <string>/<item> elements is converted; XML structure,
format specifiers (%1$s), escapes and comments are preserved byte-for-byte.
"""
import re
import sys
from pathlib import Path
from opencc import OpenCC

SRC = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("values-zh-rCN/strings.xml")

HEADER = """<?xml version="1.0" encoding="utf-8"?>
<!--
  繁體中文（{locale}）· 由 values-zh-rCN 經 OpenCC（{mode}）轉換生成，2026-10-21
  修改文案請改 values-zh-rCN 後重新轉換，或直接手改本文件（勿同時改兩處）。
-->
<resources>"""

STRING_RE = re.compile(r"^(\s*)(<string\b[^>]*>)(.*?)(</string>)\s*$")
ITEM_RE = re.compile(r"^(\s*)(<item>)(.*?)(</item>)\s*$")


def convert(src_text: str, cc: OpenCC, locale: str, mode: str) -> str:
    out_lines = []
    header_done = False
    for line in src_text.splitlines():
        if not header_done:
            # HEADER already carries its own <?xml?> declaration; drop the source one
            if line.lstrip().startswith("<?xml"):
                continue
            if line.strip() == "<resources>":
                out_lines.append(HEADER.format(locale=locale, mode=mode))
                header_done = True
                continue
            out_lines.append(line)
            continue
        m = STRING_RE.match(line) or ITEM_RE.match(line)
        if m:
            indent, open_tag, text, close_tag = m.groups()
            if text:
                text = cc.convert(text)
            out_lines.append(f"{indent}{open_tag}{text}{close_tag}")
        else:
            out_lines.append(line)
    return "\n".join(out_lines) + "\n"


def main() -> None:
    src_text = SRC.read_text(encoding="utf-8")
    for out_name, mode, locale in [
        ("values-zh-rHK/strings.xml", "s2hk", "香港"),
        ("values-zh-rTW/strings.xml", "s2twp", "台灣"),
    ]:
        out_path = SRC.parent.parent / out_name
        out_path.write_text(
            convert(src_text, OpenCC(mode), locale, mode), encoding="utf-8"
        )
        n = out_path.read_text(encoding="utf-8").count("<string ")
        print(f"{out_path}: {n} strings ({mode})")


if __name__ == "__main__":
    main()
