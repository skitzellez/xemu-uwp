# Copyright (c) 2026 Skitzelles - part of Game Vault, https://github.com/skitzellez/xbox-game-vault
# PolyForm Noncommercial License 1.0.0: not for sale, and this notice stays in every copy.
#
# xemu as a TOP layer (it updates itself, like Game Vault 0.16.1.844+): links the very objects and libraries of
# build-rel's qemu-system-i386w.exe into XemuTop.dll instead (-shared). The base app (XemuUWP/layers) loads it and
# calls its export XemuTopMain (ui/xemu.c). Run after mk.sh has built the program:
#   python mk_top.py            -> build-rel/XemuTop.dll
import os
import re
import shlex
import subprocess
import sys

here = os.path.dirname(os.path.abspath(__file__))
build = os.path.join(here, "build-rel")
text = open(os.path.join(build, "build.ninja"), encoding="utf-8", errors="replace").read()
m = re.search(r"^build qemu-system-i386w\.exe: cpp_LINKER_RSP (.*?)\n((?: +\S.*\n)*)", text, re.M)
if not m:
    sys.exit("link rule not found in build.ninja")
inputs = m.group(1).split(" | ")[0].split(" || ")[0]
vars_ = dict(re.findall(r"^ +(\w+) = (.*)$", m.group(2), re.M))
if not vars_.get("LINK_ARGS"):
    sys.exit("LINK_ARGS not found")
args = vars_.get("ARGS", "")
link_args = vars_.get("LINK_ARGS", "")
rsp = " ".join([args, "-shared", "-o", "XemuTop.dll", inputs, link_args])
open(os.path.join(build, "XemuTop.dll.rsp"), "w", encoding="utf-8").write(rsp)
env = dict(os.environ)
env["PATH"] = r"C:\msys64\ucrt64\bin;C:\msys64\usr\bin;" + env.get("PATH", "")
r = subprocess.run([r"C:\msys64\ucrt64\bin\c++.exe", "-m64", "@XemuTop.dll.rsp"], cwd=build, env=env)
if r.returncode:
    sys.exit("link failed")
print("built", os.path.join(build, "XemuTop.dll"), os.path.getsize(os.path.join(build, "XemuTop.dll")))
