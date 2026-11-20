Game patches built into xemu (xemu-patches.c). One file per game, named after its title ID: 54430003.txt.
After changing a file here run: python gv-patches/gen_builtin.py  (writes xemu-patches-builtin.inc, commit both).
A file of the same name in LocalState\xemu\xemu\patches (testing) or next to xemu.exe\patches is used instead.
