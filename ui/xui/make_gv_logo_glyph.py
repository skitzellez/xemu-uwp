# Copyright (c) 2026 Skitzelles - part of Game Vault, https://github.com/skitzellez/xbox-game-vault
# PolyForm Noncommercial License 1.0.0: not for sale, and this notice stays in every copy.
# Makes gv-logo-glyph.h: the Game Vault logo (GameVault/art/logo.svg, the same shapes) as 64 x 64 RGBA, see-through
# background, for the "Return to Game Vault" icon in xemu's menu (font-manager.cc puts it in the font as a glyph).
# Run: python make_gv_logo_glyph.py   (needs Pillow)
import os
from PIL import Image, ImageDraw

S = 8                      # draw big, then shrink: smooth edges
N = 64
img = Image.new("RGBA", (N * S, N * S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
k = N * S / 100.0          # logo.svg is 100 x 100
blue, dark = (0x40, 0xBF, 0xFF, 255), (0x16, 0x24, 0x3A, 255)


def p(x, y):
    return (x * k, y * k)


# the cartridge outline: M24 10 H76 Q84 10 84 18 V74 H78 V90 H22 V74 H16 V18 Q16 10 24 10 Z (stroke 6)
outline = [p(24, 10), p(76, 10), p(80, 11), p(83, 14), p(84, 18), p(84, 74), p(78, 74), p(78, 90), p(22, 90),
           p(22, 74), p(16, 74), p(16, 18), p(17, 14), p(20, 11), p(24, 10)]
d.line(outline, fill=blue, width=int(6 * k), joint="curve")
for x in (30, 39, 48, 57, 66):   # the cartridge's contacts
    d.rounded_rectangle([p(x, 80), p(x + 5, 87)], radius=k, fill=blue)
# the vault wheel
r = 19 * k
d.ellipse([50 * k - r, 44 * k - r, 50 * k + r, 44 * k + r], fill=dark, outline=blue, width=int(5 * k))
for x2, y2 in ((50, 18), (72.5, 57), (27.5, 57)):
    d.line([p(50, 44), p(x2, y2)], fill=blue, width=int(5.5 * k))
    rr = 2.75 * k
    d.ellipse([x2 * k - rr, y2 * k - rr, x2 * k + rr, y2 * k + rr], fill=blue)
rr = 6.5 * k
d.ellipse([50 * k - rr, 44 * k - rr, 50 * k + rr, 44 * k + rr], fill=blue)

small = img.resize((N, N), Image.LANCZOS)
data = small.tobytes()
here = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(here, "gv-logo-glyph.h"), "w", newline="\n") as f:
    f.write("/* Made by make_gv_logo_glyph.py from GameVault/art/logo.svg: the Game Vault logo, 64 x 64 RGBA. */\n")
    f.write("/* Copyright (c) 2026 Skitzelles - part of Game Vault. PolyForm Noncommercial License 1.0.0. */\n")
    f.write("#pragma once\nstatic const int gv_logo_glyph_size = %d;\nstatic const unsigned char gv_logo_glyph_rgba[] = {\n" % N)
    for i in range(0, len(data), 24):
        f.write("    " + ", ".join("0x%02x" % b for b in data[i:i + 24]) + ",\n")
    f.write("};\n")
small.resize((256, 256), Image.NEAREST).save(os.path.join(here, "gv-logo-glyph-preview.png"))
print("wrote gv-logo-glyph.h")
