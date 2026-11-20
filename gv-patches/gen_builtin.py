# Copyright (c) 2026 Skitzelles - part of Game Vault's xemu for Xbox Dev Mode.
"""Writes xemu-patches-builtin.inc from the patch files in this folder (8 hex digits + .txt)."""
import re
from pathlib import Path

here = Path(__file__).resolve().parent
out = ["/* made by gv-patches/gen_builtin.py from gv-patches/*.txt: do not edit */"]
for f in sorted(here.glob("*.txt")):
    if not re.fullmatch(r"[0-9A-Fa-f]{8}", f.stem):
        continue
    text = f.read_text(encoding="utf-8").replace("\r\n", "\n")
    lit = "".join(chr(92) + "%03o" % b if b < 32 or b > 126 or b in (34, 92, 63) else chr(b)
                  for b in text.encode("utf-8"))
    out.append('{ 0x%sU, "%s" },' % (f.stem.upper(), lit))
(here.parent / "xemu-patches-builtin.inc").write_text("\n".join(out) + "\n", encoding="utf-8", newline="\n")
print(len(out) - 1, "games")
