# Copyright (c) 2026 Skitzelles - part of Game Vault's xemu for Xbox Dev Mode.
"""Turns the community widescreen / 60 fps IPS patches (for default.xbe) into gv-patches files.

    python gv-patches/import_ips.py <mods folder> [--gv-list <xbox-patches.txt to update>]

<mods folder> holds <TITLE ID>[.<VERSION>]/<name>.ips and a patches.txt naming the ones to take, with
"<title>/<file> <size> <FNV-1a 64>" lines (the list xbased ships: the Xbox graphical patches collections, made and
tested on the North American releases). Only the IPS files (community work) are taken, never xbased's own mod.txt
code (GPL-3, xemu is GPL-2). Each patch becomes a section of <TITLE ID>.txt with "region 1" (USA certificate),
"version" when the folder names one, and "file 0xOFFSET -> bytes" lines (at most 32 bytes each).
A widescreen.dat in <mods folder> (OgXHD's widescreen database, from icup321's XboxGraphicalPatches: ":<title id>"
then pairs of lines, the bytes to find in default.xbe and their replacement) adds a "find old -> new" widescreen
patch for each game in it that has no widescreen IPS patch; xemu searches the running .xbe for those bytes.
Files written by an earlier run (marked below) are replaced; hand-made files are kept and get the new sections added
only when they don't have them yet.
"""
import re
import sys
from pathlib import Path

MARK = "# made by import_ips.py"
here = Path(__file__).resolve().parent


def fnv1a64(b):
    h = 0xCBF29CE484222325
    for x in b:
        h = ((h ^ x) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def ips_records(data):
    if data[:5] != b"PATCH":
        raise ValueError("not an IPS file")
    i, out = 5, []
    while True:
        if data[i:i + 3] == b"EOF":
            return out
        off = int.from_bytes(data[i:i + 3], "big")
        size = int.from_bytes(data[i + 3:i + 5], "big")
        i += 5
        if size == 0:  # run of one byte
            n = int.from_bytes(data[i:i + 2], "big")
            out.append((off, bytes([data[i + 2]]) * n))
            i += 3
        else:
            out.append((off, data[i:i + size]))
            i += size


KINDS = [  # file name (lower) -> id, name, description
    (r"^60 fps", "60fps", "60 FPS", "Runs the game at 60 frames a second. Some parts may run too fast or glitch."),
    (r"^120 fps", "120fps", "120 FPS", "Runs the game at 120 frames a second. Some parts may run too fast or glitch."),
    (r"^widescreen.*rendering", "widescreen", "Widescreen 16:9",
     "Draws the game in 16:9 to fill the TV. Some screens may show drawing glitches."),
    (r"^widescreen", "widescreen", "Widescreen 16:9", "Draws the game in 16:9 to fill the TV, without stretching."),
    (r"^480p", "480p", "480p", "Turns on the game's 480p mode for a sharper picture."),
    (r"^draw distance", "draw-distance", "Draw Distance x4", "Draws far away scenery four times as far."),
    (r"^invert camera", "invert-camera", "Invert Camera", "Turns the camera's up and down the other way."),
    (r"^force high lod", "high-lod", "High Detail Models", "Always uses the most detailed models."),
]


def kind_of(stem):
    for rx, pid, name, desc in KINDS:
        if re.search(rx, stem.lower()):
            return pid, name, desc
    return None


def main():
    mods = Path(sys.argv[1])
    gv_list = Path(sys.argv[sys.argv.index("--gv-list") + 1]) if "--gv-list" in sys.argv else None
    games = {}  # title -> list of (pid, name, desc, version, records, source)
    skipped = []
    for row in (mods / "patches.txt").read_text(encoding="utf-8").splitlines():
        if not row.strip() or row.startswith("#"):
            continue
        m = re.fullmatch(r"(\S+)/(.+\.ips) (\d+) ([0-9a-f]{16})", row.strip())
        if not m:
            skipped.append((row, "line not understood"))
            continue
        folder, fname, size, h = m.groups()
        f = mods / folder / fname
        data = f.read_bytes()
        if len(data) != int(size) or fnv1a64(data) != int(h, 16):
            skipped.append((row, "file differs from its list line"))
            continue
        stem = fname[:-4]
        if re.search(r"\(apply to (?!default\.xbe)", stem.lower()):
            skipped.append((row, "for another .xbe of the game"))
            continue
        k = kind_of(re.sub(r"\s*\(.*\)$", "", stem) if not stem.lower().startswith("widescreen") else stem)
        if not k:
            skipped.append((row, "unknown kind"))
            continue
        title, _, ver = folder.partition(".")
        recs = ips_records(data)
        games.setdefault(title.upper(), []).append((k[0], k[1], k[2], ver, recs, folder + "/" + fname))

    wsdb = mods / "widescreen.dat"
    db_added = 0
    if wsdb.exists():
        cur, prev, db = None, None, {}
        for row in wsdb.read_text(encoding="utf-8", errors="replace").splitlines():
            row = row.strip()
            if not row or row.startswith("'"):
                continue
            if row.startswith(":"):
                cur, prev = row[1:].upper(), None
                if cur == "END":
                    break
                db.setdefault(cur, [])
                continue
            if cur is None:
                continue
            if prev is None:
                prev = row
                continue
            if re.fullmatch(r"([0-9A-Fa-f]{2})+", prev) and re.fullmatch(r"([0-9A-Fa-f]{2})+", row)                     and len(prev) == len(row) and prev != row:
                if (prev, row) not in db[cur]:
                    db[cur].append((prev, row))
            else:
                skipped.append((cur, "widescreen.dat pair not understood"))
            prev = None
        for title, pairs in sorted(db.items()):
            if not pairs or not re.fullmatch(r"[0-9A-F]{8}", title):
                continue
            if any(k[0].startswith("widescreen") for k in games.get(title, [])):
                skipped.append((title, "widescreen.dat: has a widescreen IPS patch already"))
                continue
            games.setdefault(title, []).append(("widescreen", "Widescreen 16:9",
                                                "Draws the game in 16:9 to fill the TV, without stretching.",
                                                "", pairs, "widescreen.dat"))
            db_added += 1

    gv_rows = []
    for title, plist in sorted(games.items()):
        out = [MARK + " from the Xbox graphical patches collections (community IPS patches for default.xbe and OgXHD's widescreen database,",
               "# made and tested on the North American releases; list as shipped by xbased, github.com/crazyriddler/xbased)",
               "title " + title, ""]
        used = set()
        for pid, name, desc, ver, recs, src in plist:
            base, n = pid, 2
            while pid in used:
                pid = "%s-%d" % (base, n)
                n += 1
            used.add(pid)
            out.append("[%s] %s" % (pid, name))
            out.append("# " + src)
            out.append("default off")
            if src == "widescreen.dat":  # found by its bytes: any release that has them
                for old, new in recs:
                    out.append("find %s -> %s" % (" ".join(old[i:i + 2].upper() for i in range(0, len(old), 2)),
                                                  " ".join(new[i:i + 2].upper() for i in range(0, len(new), 2))))
                out.append("")
                gv_rows.append("%s	%s	%s	%s" % (title, pid, name, desc))
                continue
            out.append("region 1")
            if ver:
                out.append("version 0x%08X" % int(ver))
            for off, b in recs:
                for i in range(0, len(b), 32):
                    out.append("file 0x%08X -> %s" % (off + i, " ".join("%02X" % x for x in b[i:i + 32])))
            out.append("")
            gv_rows.append("%s\t%s\t%s\t%s" % (title, pid, name, desc))
        path = here / (title + ".txt")
        if path.exists() and not path.read_text(encoding="utf-8").startswith(MARK):
            old = path.read_text(encoding="utf-8")
            add = "\n".join(l for l in out[4:])
            have = set(re.findall(r"^\[([^\]]+)\]", old, re.M))
            if any(p in have for p in used):
                skipped.append((title, "hand-made file already has one of these ids: not merged"))
                continue
            path.write_text(old.rstrip("\n") + "\n\n# from the Xbox graphical patches collections\n" + add + "\n",
                            encoding="utf-8", newline="\n")
        else:
            path.write_text("\n".join(out), encoding="utf-8", newline="\n")
    print(len(games), "games,", len(gv_rows), "patches,", db_added, "of them from widescreen.dat")
    for r, why in skipped:
        print("skipped:", r, "-", why)
    if gv_list:
        keep = [l for l in gv_list.read_text(encoding="utf-8").splitlines() if not re.match(r"[0-9A-F]{8}\t", l)
                or l.split("\t")[0] not in games]
        gv_list.write_text("\n".join(keep + gv_rows) + "\n", encoding="utf-8", newline="\n")
        print("updated", gv_list)


main()
