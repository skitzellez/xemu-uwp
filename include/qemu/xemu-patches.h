/* Copyright (c) 2026 Skitzelles - part of Game Vault's xemu for Xbox Dev Mode. xemu game patches (xemu-patches.c). */
#ifndef XEMU_PATCHES_H
#define XEMU_PATCHES_H

/* where patch files are: own (LocalState\xemu\xemu\) and shipped (next to xemu.exe); each holds a patches folder */
void xemu_patches_set_dirs(const char *own, const char *shipped);
/* patches= from Game Vault's link: "id,id", "none", "all", or NULL for each patch's default */
void xemu_patches_set_link_choice(const char *choice);
/* checks the running game and writes its patches; call about twice a second with the main loop lock held */
void xemu_patches_tick(void);

#endif
