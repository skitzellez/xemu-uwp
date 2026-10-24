<p align="center">
  <img src="https://raw.githubusercontent.com/skitzellez/xbox-game-vault/main/GameVault/art/logo-banner.png" alt="Game Vault" width="640">
</p>

<h1 align="center">xemu for Xbox Dev Mode</h1>

<p align="center">
  <b>The original Xbox emulator, running on an Xbox Series X|S in Developer Mode.</b><br>
  Made to work hand in hand with <a href="https://github.com/skitzellez/xbox-game-vault">Game Vault</a>: pick a game, it plays.<br>
  <b>Test builds: expect rough edges, and tell us about them.</b>
</p>

<p align="center">
  <a href="https://github.com/skitzellez/xemu-uwp/releases"><img alt="Latest release" src="https://img.shields.io/github/v/release/skitzellez/xemu-uwp?include_prereleases&label=release&color=107C10"></a>
  <a href="https://github.com/skitzellez/xemu-uwp/releases"><img alt="Downloads" src="https://img.shields.io/github/downloads/skitzellez/xemu-uwp/total?color=0B7DDA"></a>
  <img alt="Platform: Xbox Dev Mode" src="https://img.shields.io/badge/platform-Xbox%20Dev%20Mode-107C10?logo=xbox&logoColor=white">
  <img alt="License: GPL-2.0" src="https://img.shields.io/badge/license-GPL--2.0-blue">
  <a href="https://www.patreon.com/Skitzelles"><img alt="Patreon" src="https://img.shields.io/badge/Patreon-Skitzelles-F96854?logo=patreon&logoColor=white"></a>
</p>

---

> [!IMPORTANT]
> This is an **unofficial port**. It is not part of the [xemu project](https://xemu.app) and is not supported by
> it, so please **don't report problems with it to xemu**. Open an issue here instead.

> [!NOTE]
> **No games, Microsoft BIOS files or keys are included.** A free, open-source BIOS
> ([SpiritBoot](https://github.com/Mainkill1/SpiritBoot)) comes along so games start without anything of yours;
> online play needs your own Xbox files.

## ✨ Highlights

| | |
|---|---|
| 🎮 **Just press play** | Start a game in Game Vault and it boots. No settings file, no BIOS hunting to get started. |
| 💾 **Saves on your USB stick** | Game Vault keeps a copy of the Xbox hard disk in the `Game Vault` folder on your stick and updates only what changed, in seconds. |
| ⚡ **Smooth start-up** | Graphics are prepared ahead, so the Xbox logo and the parts you have seen before play without hitches. |
| ⏸️ **Quick save states** | View + RB saves, View + LB loads, at any moment. |
| 🌐 **Online through Insignia** | The Xbox Live replacement works, with your own Xbox BIOS and the dashboard (which xemu installs from a game disc by itself). |
| 📝 **Sessions you can read back** | Each log starts with a plain summary and a timeline (Home screen and back, speed drops and their cause, how it ended), then one line per second and every button press. Small screenshots every 10 seconds and at every event. A crash report says in words what went wrong and where. |

## 🚀 Getting started

The easy way is the all-in-one setup from **[Game Vault](https://github.com/skitzellez/xbox-game-vault#installation)**:
it installs Game Vault, xemu and the other emulators in one go from a Windows PC.

By hand:
1. Download `Xemu_<version>_x64.appx` from [Releases](https://github.com/skitzellez/xemu-uwp/releases).
2. Install it through the Xbox **Device Portal** (Add → choose the file).
3. In **Dev Home**, highlight **xemu**, press **Menu → View details**, and set **App type** to **Game**.
   As an *App* it gets too little memory to run games.

## 🎮 Controls

| Buttons | Action |
|---|---|
| View + RB | Quick save |
| View + LB | Quick load |
| Hold View + Menu | Quit back to Game Vault |

## 🐞 Reporting a problem

[Open an issue](https://github.com/skitzellez/xemu-uwp/issues/new) with the log. With Game Vault, it is in
**`Game Vault\logs`** on your USB stick (`Xbox - <game>.txt`, with the summary at the top). Say which console you
have, which game, and what happened.

## 📚 More

- **[README-UWP.md](README-UWP.md)**: what was changed for the Xbox, what works, the performance work and the
  open problems.
- **[Game Vault](https://github.com/skitzellez/xbox-game-vault)**: the launcher this is made for.

## ❤️ Support

The port is free and stays free. If you'd like to help keep the test console running:
**[patreon.com/Skitzelles](https://www.patreon.com/Skitzelles)**. Please support the xemu project too:
[xemu.app](https://xemu.app).

## 📜 Credits and licence

- **[xemu](https://xemu.app)** by Matt Borgerson and contributors, built on QEMU. All the emulation is theirs.
- Xbox port by **[Skitzelles](https://github.com/skitzellez)**, written with an AI assistant (Claude).
- **New graphics** (the Direct3D 12 renderer and presenter) by **[danprice142](https://github.com/danprice142/xemu-uwp)**,
  from his own Xbox port of xemu.
- [XboxEmulationHub](https://github.com/XboxEmulationHub) for SDL3-uwp and mesa-uwp;
  [SpiritBoot](https://github.com/Mainkill1/SpiritBoot) for the free BIOS.

GPL-2.0, like xemu. The original xemu README is in [README.rst](README.rst) and at [xemu.app](https://xemu.app).
