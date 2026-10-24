# xemu on Xbox Series X|S Dev Mode (UWP) - experimental port

**UWP port by [Skitzelles](https://github.com/skitzellez).**

This branch (`uwp`) makes [xemu](https://github.com/xemu-project/xemu), the original Xbox emulator, run as a UWP
app on an Xbox Series X in Dev Mode. It is based on upstream master `478b4f4`. The port's own changes are
inside `#ifdef XEMU_UWP`; on top of that the branch carries a few fixes taken from open xemu pull requests
(listed under "Taken from others") and one generic fix in `block/file-win32.c`.

**Support the project:** [patreon.com/Skitzelles](https://www.patreon.com/Skitzelles). Builds stay free; memberships keep the dev PC and test Xbox running.

**Current build: 0.9.30.0** ([releases](https://github.com/skitzellez/xemu-uwp/releases)).

## Controls added for the console

| Buttons | What |
| --- | --- |
| **View + RB** | Save a quick state (one per game, kept inside the virtual hard disk) |
| **View + LB** | Load it. Not in the first seconds after starting a game: wait for the title screen |
| **View + Menu**, held two seconds | Quit and return to the launcher |

## Game compatibility

Retail games boot, play with sound and the controller, and many run at full speed; heavy scenes can still
fall below it (see "What does not work"). Results are not listed per game here. Please report what you find.

**This code was written by an AI assistant (Claude) working with the repository owner.** It is published as a
fork so that others can read it, test it, and take ideas from it. xemu's own rules (`CONTRIBUTING.md`, "Use of
AI Tooling") permit AI-assisted contributions when the submitter understands, has tested and can defend every
line and names the model used; nothing here has been reviewed to that standard or offered to xemu. QEMU and SDL
decline AI-generated contributions, so the generic QEMU and SDL changes are not for those projects.

An earlier, separate UWP port of xemu exists: [rodrigoandrigo/xemu](https://github.com/rodrigoandrigo/xemu)
(branch `UWP`), with its own interface, networking and releases.

No BIOS, MCPX ROM, hard disk image or game is included. You need your own, as with desktop xemu.

Packaging scripts, the Device Portal tools and the launcher live in the companion repository
[xbox-game-vault](https://github.com/skitzellez/xbox-game-vault) (folder `XemuUWP`).

## Setup on the console

Take `Xemu_x64.appx` from the [releases page](https://github.com/skitzellez/xemu-uwp/releases) (early test
builds), or build it yourself (see "Building"). Then:

1. Xbox Series X|S in Dev Mode, Device Portal switched on, a profile signed in.
2. Install `Xemu_x64.appx` through the Device Portal (`https://<console>:11443`, "Add").
3. In Dev Home, open the app's details and set **App type** to **Game**. As "App" it has about 145 MB of
   memory and games will not run.
4. System files (0.5.0.0 on): the easy way is the Game Vault launcher, whose first-run screen finds your
   boot ROM and BIOS on the USB drive, fetches the xemu project's free blank hard disk if you have none, and
   puts them in the publisher's shared folder `games\xbox-system`, where xemu looks by itself. No `xemu.toml`
   is needed. By hand instead: upload `mcpx_1.0.bin`, `bios.bin` and `xbox_hdd.qcow2` (see
   https://xemu.app/docs/required-files/) to the app's `LocalState\xemu\xemu` folder with the Device Portal;
   they are found there as well. An `xemu.toml` that names files which exist takes precedence over both.
5. Games: disc images (XISO or full dumps, `.iso`) on an NTFS USB drive on which app packages have full
   control (`icacls X:\ /grant "*S-1-15-2-1:(OI)(CI)F"`). Either set `dvd_path` in `xemu.toml`, or start
   games from the Game Vault launcher, which passes the disc with a link.
6. Start xemu. `uwp_debug.log` in the same folder shows progress, frame rates and profiler lines.

## Contributing

Test reports, fixes and ideas are welcome: open an issue or a pull request against the `uwp` branch. The
rules (no games or BIOS files, say what you tested, AI-assisted work is fine if disclosed) are in
[CONTRIBUTING.md](https://github.com/skitzellez/xbox-game-vault/blob/main/CONTRIBUTING.md) of the companion
repository. Keep changes inside `#ifdef XEMU_UWP`.

## What works (seen on a Series X, app type "Game")

- Boots the BIOS, the dashboard and retail games; output through Mesa's OpenGL-on-D3D12 (GL 4.6).
- Sound, through an XAudio2 bridge (`ui/xemu-uwp-audio.c`).
- Controller on port 1 (Windows.Gaming.Input through SDL).
- 1080p and 4K output.
- Starting a disc from another app with a link: `xemu:?disc=<escaped path>&launchOnExit=<uri>`.
- Full disc dumps (redump style) play without converting: the game partition is found and mounted at its offset.
- Homebrew used for testing: `nxdk_pgraph_tests`, `xemu-perf-tests`.
- Quitting: the link `xemu:?quit=1` ends the emulator and the launcher's `launchOnExit` link is opened
  (seen working with Game Vault). Holding **View + Menu** (Back + Start) for two seconds does the same.
- Quick save states (0.6.2.0 on): saved and loaded on the console from the controller and through the links
  `xemu:?state=save` and `xemu:?state=1` (the latter loads 45 s after start). They are ordinary xemu
  snapshots in the hard disk image.
- No settings file needed (0.5.0.0 on): the boot ROM, BIOS and hard disk are found in the launcher's shared
  folder or next to where `xemu.toml` would be. NAT networking is on by default.
- Online (0.8.6.0): with the built-in NAT networking, Insignia's Setup Assistant
  ([insignia.live](https://insignia.live), the replacement for the original Xbox Live service) runs, registers
  the emulated console and reports "All checks passed". Creating an account and playing a match have not been
  tried yet.
- Dashboard from a game disc the user owns (0.8.9.0): online sign-up needs Microsoft's dashboard on the hard
  disk, and the free hard disk image has none. Many Xbox Live games carry its installer (`dashupdate.xbe`).
  When the game being started has one and the hard disk has no dashboard, or an older one, its files are
  unpacked onto C: before the console boots (`ui/xemu-uwp-dash.c`: XDVDFS, the installer's file list, LZX
  through libmspack, FATX; about a second; every XBE's section digests are checked first). `dash=0` in the
  launch link turns it off. Nothing of Microsoft's is shipped. Tried with one game's disc image so far.
- Buttons can be pressed from outside for testing: the launch link takes `press=down,down,a` (also `wait` and
  a number of milliseconds). `XemuUWP/tools/xlink.ps1` in the companion repository sends such links and takes
  a screenshot.
- A failed internal check (assert) no longer vanishes without trace: `uwp_debug.log` gets the message and a
  module+offset backtrace (`ABORT` lines), to be looked up with `nm -n` on the unstripped exe.
- When the console sends the app to the background, the virtual hard disk is flushed and the settings are
  saved, so a suspended app that is ended later does not lose saves (0.4.3.0).
- With `[net] enable = true` (NAT) in `xemu.toml` the emulator starts and runs; no game was tried online.
- Free BIOS (0.9.1.0): without a BIOS of your own, games start with the bundled
  [SpiritBoot](https://github.com/Mainkill1/SpiritBoot) BIOS (offline only), on a hard disk of its own.
- Sets itself up (0.9.2.0): your own boot ROM and BIOS are used when found; otherwise the free BIOS. A
  missing hard disk is made from the copyright-free blank image in the package. Paths picked this way are
  not written into `xemu.toml`.
- Smooth start (0.9.20.0): the Xbox logo animation and a game's first scenes no longer freeze while
  graphics are prepared; see "Shader warm-up" below. The first start of each game records what it needs.

## What does not work, or is unfinished

- Movies that are decoded with MMX/SSE in software run at a few frames a second in at least one game. The
  emulated CPU is the limit (same reports exist for desktop xemu, issues #2938, #1266, #1232).
- One racing game stays near 30 fps in races although the renderer is not saturated; see "Open leads".
- In heavy scenes the emulated CPU is the limit (Halo 2, a combat room: one core flat out, about 30 % of
  it in block lookup and dispatch). See "Open leads".
- A state loaded while the emulated console is still booting can trip an assert in the DSP interpreter
  (`read_memory_p`); loaded later it works. Cause not understood.
- A new scene seen for the first time in a run can still pause briefly while its textures are uploaded
  and its graphics are compiled; scenes from a game's first 30 seconds are prepared ahead (shader warm-up).
- Disc swap while xemu is running is written but untested.
- Disc images are opened with `CreateFileFromAppW` since 0.4.0.0, which should make FAT32 / exFAT USB drives
  work without the NTFS + permissions step. **Not yet tried on a FAT32 drive**; NTFS still works.
- No real shader cache between runs: the Mesa UWP build has no disk cache and reports no program binary
  formats, so xemu's own shader cache stays empty. The shader warm-up (below) covers each game's first
  30 seconds; later scenes still compile on first use. `MESA_SHADER_CACHE_DIR` is set, ready for a Mesa build
  that has the cache.
- Debug leftovers are still in: `UWP_MARK` markers (`system/vl.c`, `hw/xbox/xbox.c`,
  `hw/xbox/mcpx/apu/monitor.c`), a listing of `*.iso` files on all drives at start-up (`ui/xemu.c`, it runs
  before the main loop and can delay start-up on a slow drive), `pgraph read/write` trace lines and two lock
  probes.
- Only a handful of games were tried. Expect breakage.

## Taken from others

Fixes from open pull requests on xemu-project/xemu, applied here as they are, with thanks to their authors:
#3072 (EmmanuelARB) audio voice processor: skip a segment that is not ready instead of asserting;
#2929 (flowa1911you) raise the PGRAPH interrupt from a bottom half; #2941 (xionic) lock-free read of the
polled fence register; #2930 (flowa1911you) consume inline vertex arrays in bulk; #3028 (EqUiNoX-Labs) keep
whole-page code invalidation off the running page; #2957 (EriKWDev) delay the DVD read-completion interrupt.

### From the other UWP port

[rodrigoandrigo/xemu](https://github.com/rodrigoandrigo/xemu) (branch `UWP`) was read after this port was
published. Ideas adopted from it, with thanks: the APU frame thread raises its interrupt through a bottom half
instead of taking the big QEMU lock, and the `MESA_SHADER_CACHE_DIR` setting. Its brokered-storage design was
the prompt for the simpler `CreateFileFromAppW` route used here. The GPU read-back fixes listed under
"Performance work" are not in that port at the time of writing (its 1.0.0.114 ran one game's demo levels at
23-28 fps on the same console where this port holds 55-60).

### From danprice142's port

The **New graphics** engine is the Direct3D 12 renderer (`hw/xbox/nv2a/pgraph/d3d12/`) and presenter
(`ui/d3d12-presenter.*`) written by [danprice142](https://github.com/danprice142/xemu-uwp) for his Xbox port of
xemu, with thanks. Our changes to it are in the git history. Two speed-ups were also first seen there: the TLB
reset that skips untouched entries (`accel/tcg/cputlb.c`) and fewer shader variants for unused settings
(`hw/xbox/nv2a/pgraph/glsl/psh.c`).

## What had to change to run in a UWP app on the console

| Problem on the console | What this branch does |
| --- | --- |
| No Win32 windows; the app needs a CoreWindow | Entry point is `WinMain` -> `SDL_RunApp`, with a UWP build of SDL3 |
| `VirtualAlloc` cannot give writable + executable memory | TCG code buffer from `VirtualAllocFromApp` / `VirtualProtectFromApp`, resolved at run time (`include/qemu/xemu-uwp.h`, `tcg/region.c`, `util/osdep.c`) |
| `GetDefaultCommConfigA` does not exist (the app would not start) | Skipped (`chardev/char-win.c`) |
| Only one window and no pbuffers in Mesa's UWP build | `gloffscreen` reuses the single window's context |
| SDL audio initialisation deadlocks off the main thread (WASAPI activation never completes) | The APU writes to a device-less `SDL_AudioStream`; a Win32 thread feeds XAudio2 (`xaudio2_9.dll`, loaded at run time) |
| `SDL_CreateThread` fails from MinGW code in an AppContainer | Plain `CreateThread` for the audio feeder |
| DXGI present path is desktop only | Stubbed (`ui/xui/win32-dxgi-present.cc`) |
| stderr is lost | Progress log `uwp_debug.log` in `LocalState\xemu\xemu` |

Generic fix: `block/file-win32.c` did not check the result of `GetDiskFreeSpace`; an uninitialised sector size
led to an assert.

## Performance work (OpenGL on D3D12 specific)

On this GL-on-D3D12 stack a `glReadPixels` surface read-back costs about 2 ms, and games can cause ten a frame.
The changes keep data on the GPU:

- Whole-surface `SRCCOPY` image blits stay on the GPU (`gl/blit.c`, `pgraph_gl_surface_copy_to`).
- A render target may serve as level 0 of a mipmapped texture.
- Surface-to-texture uses `glCopyImageSubData` when formats match, and allocates texture storage once.
- Cube maps whose six faces are render targets are copied face by face on the GPU (`gl/texture.c`).
- Depth surfaces sampled as textures (16 and 24 bit, formats 0x30, 0x2E, 0x12) are copied or drawn on the GPU.
- A depth surface larger than the colour target stays bound instead of being read back and rebuilt.
- `glFlush` instead of `glFinish` at flip.
- Vertex data without waiting for the GPU (0.6.2.0, `gl/vertex.c`): upstream patches a GL buffer that mirrors
  all of VRAM with `glBufferSubData` before a draw. That buffer is always referenced by queued draws, and
  Mesa's D3D12 driver answers a write to a busy buffer by flushing and waiting. The pages a draw needs are
  now copied to the next free part of a 64 MB stream buffer through an unsynchronized mapping, with a table of
  the VRAM ranges it already holds; inline arrays and inline buffers go the same way. Halo 2's menu went
  from 23-30 fps to a steady 29-30.
- Index buffers are created `GL_DYNAMIC_DRAW`, not `GL_STATIC_DRAW` (0.8.0.0, `gl/draw.c`). Mesa's D3D12
  driver cannot map a static buffer directly (`can_map_directly` in `d3d12_resource.cpp`), and it has to read
  the index buffer back for every indexed draw of a primitive type D3D12 lacks (triangle fans, through
  `util_primconvert`): it copies the buffer on the GPU, flushes and waits for the GPU. About a hundred waits a
  frame in Halo 2; the profiler showed them as `NtGdiDdDDIWaitForSynchronizationObjectFromCpu` under
  `glDrawElements`. A dynamic buffer is read in place.
- A colour target whose memory is being reused for another target is not copied back to guest RAM first
  (0.8.1.0, `uwp_evict_without_download` in `gl/surface.c`): when the new target starts at the same address
  with another pitch, or the two ranges overlap only partly. Halo 2 alternates 128x128 and 160x120 targets
  over one pool, four read-backs a frame. A target lying wholly inside another, or keeping its pitch, is
  still copied back. If a game shows a wrong picture after this, this rule is the first suspect.
- Fixed in 0.8.6.0: a program that draws its screen with the CPU into memory overlapping an old GPU surface
  was shown shifted (the display took any surface containing the display address). A surface the GPU is not
  drawing to that starts more than two lines away from the display address is now dropped
  (`uwp_display_surface` in `gl/display.c`).
- Fixed in 0.5.4.0: the shortcut that skips a depth surface over a recent colour image left a depth-only
  pass with nothing bound (`assert(r->color_binding || r->zeta_binding)`, Halo 2 while loading a level). It
  now applies only while a colour target is bound.

- Texture cache of 4096 entries instead of 512 (0.9.9.0, `gl/texture.c`): busy scenes with many
  compressed textures were rebuilding the same textures every frame.
- Cube maps with only some faces drawn on the GPU are no longer copied through memory (0.9.9.0).

### Shader warm-up (0.9.20.0)

The console's D3D12 driver compiles each shader for the complete pipeline (target formats, vertex layout,
texture kinds, blend, depth, stencil and raster state) at the first draw that uses it, and nothing survives
between runs. Every start therefore froze the Xbox logo animation for 0.1 to 1.4 s several times.

`gl/shaders.c` now records the first draw of each shader in the first 30 s of a run, per disc, in
`uwp_warm_<hash>.bin` next to the log (shader state plus that pipeline setup). The next start replays those
draws off screen, with zero-area triangles, on three threads with GL contexts of their own; the game's first
draw waits for them. The screen stays black about a second and a half longer, the logo animation then runs
without the freezes, and the game is reached as fast as before. A list from another build is discarded.

Measured on the console: one platformer's attract demo went from 20-28 fps to 59-60; another game's demo from
29-30 to 59-60 with read-backs at zero.

`target/i386/ops_sse.h` also has native host versions of the busiest MMX/SSE helpers (`UWP_NATIVE_SSE`). They
are correct but did not change the frame rate of the slow movies: the cost is the helper calls, not the bodies.

### Built-in profiler

Every 3 s `uwp_debug.log` gets a `prof` line: CPU % per thread (vcpu, pfifo, ui), wall % per probe
(`UWP_PROF_BEGIN/END`), nv2a counters per frame, read-backs by cause, and now and then a short event trace
and the hottest guest code blocks. `tools/sum.py` and `tools/sym.py` in the companion repository print it.

## Open leads

- The renderer thread still spends about 7 % in `NtGdiDdDDICreateAllocation` / `DestroyAllocation2`:
  buffers being created and destroyed all the time (the 64 MB vertex stream buffer is replaced when full;
  Mesa's own upload buffers may add to it). A small pool of stream buffers would end that.
- How the waits were found, for the next one: the sampler logs, for the renderer thread, the export each
  sample outside xemu.exe is in (`pfifo outside xemu, by export`), the first caller inside
  `libgallium_wgl.dll` and the first caller in xemu.exe. `skipdraws=1` in the launch link and the timers
  `PF_GLDRAW`, `PF_FLUSH_DRAW`, `PF_SURF_UPD`, `PF_BIND_SHADER` separate renderer cost from CPU cost.
- The emulated CPU spends about 30 % of its time in block lookup and dispatch in Halo 2 (`cpu_exec_loop`,
  `helper_lookup_tb_ptr`, `qht_lookup_custom`): something keeps emptying the jump cache or the TLB.
- The 30 fps racing game: about ten PGRAPH register accesses per frame from the vcpu wait on the renderer's
  locks (2-5 ms a frame). They are the guest's handler for a PGRAPH interrupt (bit 20) raised once a frame;
  `pgraph_write` takes `pfifo.lock` and `pg->lock` while the pfifo thread holds them.
- Slow software-decoded movies: runs of register-only MMX/SSE instructions could be executed natively as one
  stub per run instead of one helper call per instruction. Not started.
- `gen_enter_mmx` emits nine stores per MMX instruction with hard FPU on; the SSE/MMX helpers are declared
  without `TCG_CALL_NO_RWG`.

## Building

Needs MSYS2 (UCRT64, gcc), Visual Studio 2022 Build Tools with the UWP workload, the SDL3 UWP fork
(`SDL3-uwp`, branch `uwp-3.4.16` plus the local fixes) built with CMake for `WindowsStore`, and Mesa's UWP
build **26.2.2** from XboxEmulationHub/mesa-uwp (`opengl32.dll`, `libgallium_wgl.dll`, `z-1.dll`) plus
`dxil.dll`. Older Mesa UWP builds crash when a GL context is made current off the UI thread.

```
# inside MSYS2 UCRT64; PKG_CONFIG_PATH must reach a sdl3.pc that points at the UWP SDL3.lib
mkdir build-uwp && cd build-uwp
../configure --extra-cflags='-DXBOX=1 -DXEMU_UWP=1 -Wno-error -lIphlpapi -lCrypt32' --target-list=i386-softmmu -Db_lto=true -Dx86_version=3
make -j qemu-system-i386w.exe
```

The executable is then marked `editbin /APPCONTAINER`, packed with its DLLs and a manifest that declares
`codeGeneration`, `broadFileSystemAccess` and `expandedResources`, and signed; `XemuUWP/package.sh` in the
companion repository does that. On the console set the app type to **Game** (about 840 MB instead of 145 MB).

## Licence

Same as xemu / QEMU: GPL version 2. See `COPYING`. The UWP port changes are Copyright (c) 2026 Skitzelles.
Under the GPL you must keep the copyright notices, mark your own changes as changes, and pass on the source
with any copy you distribute. Please keep the credit to Skitzelles and to the xemu project.
