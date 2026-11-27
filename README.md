# RL-NX: Rogue Leader for Nintendo Switch

Star Wars Rogue Squadron II: Rogue Leader (GameCube, USA, `GSWE64` revision 0) running natively on a
Nintendo Switch as a homebrew `.nro`.

It is a Switch port of [Red Two](https://github.com/gminustwo/red_two) by gminustwo, a static
recompilation of the game: the game's own PowerPC code is translated to C++ ahead of time and runs on a
runtime that stands in for the GameCube's hardware. Graphics go through
[Aurora](https://github.com/encounter/aurora) onto deko3d (the Switch's native GPU API); the audio DSP
is emulated in software. **No game data is included.** You need your own disc image of the USA release.

> Status: playable on hardware in handheld mode at 720p. Frame rate depends on the level: menus and
> light scenes are at or near 60 fps, the busiest levels run well below it. Speed is the open work; the
> full log of what was done, what was measured and what is left is [`README.PORT.md`](README.PORT.md).
> Docked output (900p / 1080p) exists but is less tested.

## What you need

- A Nintendo Switch running custom firmware with the Homebrew Menu (hbmenu).
- **Title takeover.** Start the homebrew by holding **R** while launching a game, not from the Album.
  Applet mode leaves too little memory for the game's 256 MiB address space, and the port says so on
  screen if it detects it.
- Your own disc image of Rogue Leader (USA, rev 0): `.iso`, `.gcm`, `.rvz` or `.wia`.

## Installing

Copy `RL-NX.nro` and your disc image into the same folder on the SD card:

```
sdmc:/switch/RL-NX/RL-NX.nro
sdmc:/switch/RL-NX/<your Rogue Leader disc>.rvz      (any name; one image only)
```

Start it from the Homebrew Menu under title takeover. The first start builds the shader cache, which takes
a little longer than later starts. The game creates its other folders itself:

| Folder | What is in it |
|---|---|
| `config/` | `menu.cfg` (the in-game menu's saved settings), Aurora's `input.json`, an optional `boot.cfg` |
| `cache/` | shader and pipeline caches, downloaded updates |
| `logs/` | `log.txt` for the last run (see *Reporting a problem*) |
| `saves/` | the memory card image |
| `mods/hd/` | optional HD texture pack (below) |

Saves are a GameCube memory card image in `saves/`. A `.raw` card from Dolphin or Nintendont can be copied
there as it is.

## The `-` menu

Tap **Minus** to open the options panel over the game and tap it again to close it. While it is open the
game sees no controller input.

- **Up / Down** moves, **A** switches the row.
- **L / R** (or **Left / Right**) changes the tab when the Debug tab is shown.

**Game tab**

| Row | What it does |
|---|---|
| FRAME RATE | shows the frame counter at the top of the screen |
| INVERT FLIGHT | flips the stick's up/down while you are flying a craft (off by default) |
| RUMBLE | controller vibration on or off (on by default) |
| HD TEXTURES | uses the HD texture pack, if one is installed (below) |
| GAME LOGGING | adds detail to the log for bug reports (off by default) |
| UPDATE | looks for a newer release on GitHub and installs it |

The **Debug** tab (performance and diagnostic switches) is hidden by default. Add the line
`R2_DEBUG_MENU=1` to `sdmc:/switch/RL-NX/config/boot.cfg` to show it. Normal play needs no `boot.cfg`.

Settings are remembered between runs. Delete `config/menu.cfg` to go back to the defaults.

## HD textures

RL-NX can load a Dolphin-format texture pack from `sdmc:/switch/RL-NX/mods/hd/`.

**Credit goes to General_Han_Solo for the HD texture pack.** Nothing was changed in the textures except
down-sampling them to a size that performs well on the Switch 1. The conversion tool,
[`switch/tools/hd_pack_shrink.py`](switch/tools/hd_pack_shrink.py), is in this repository: run it on the
original pack with a different preset if you want a different size trade-off, for example higher
resolution.

- Down-sampled pack for the Switch (download): <https://mega.nz/file/IMdn2JQC#6VKreFPqE9n9Oyxu58Kpo3mcaDSFdwHkkxvephdvlzg>
- Original pack: <https://emulationking.com/nintendo/gamecube/games/starwarsroguesquadroniirogueleader/texturepacks/generalhansolo/>
- Dolphin Emulator Forum thread: <https://forums.dolphin-emu.org/Thread-star-wars-rogue-leader-rebel-strike-jedi-knight-23rd-march-2026>

To use it, unpack the pack's `GSWE64` folder into `sdmc:/switch/RL-NX/mods/hd/`, then switch **HD TEXTURES**
on in the `-` menu.

## Updating

The **UPDATE** row in the Game tab asks GitHub for the latest release of
[digdat0/RogueLeaderNX](https://github.com/digdat0/RogueLeaderNX/releases). If there is a newer release it
offers to download it; if there is none, nothing is offered. After the download it asks to restart and
starts the new version by itself. You can also copy a new `RL-NX.nro` over the old one by hand.

## Reporting a problem

Open an issue on the [Issues](../../issues/new/choose) page. Please include:

- the version (shown in the log's first line),
- what happened, in which mission or menu, and whether it happens every time,
- **the log**: `sdmc:/switch/RL-NX/logs/log.txt` from the session where it happened (copy it before
  starting the game again, because each start replaces it). Turn **GAME LOGGING** on in the `-` menu first
  if you can, since it adds detail. The log shows paths from your SD card but nothing else personal,
- a screenshot or a short video for anything you can see.

## Building

Requirements: devkitPro with devkitA64 and the Switch portlibs, CMake 3.25+, Ninja, Git Bash on Windows
(or any POSIX shell), and your own disc image. The first build recompiles the game from your disc into
`build-switch/gen/`, which is yours alone and is never committed or shared.

```bash
bash switch/tools/prepare-refs.sh   # once: fetches the reference trees
bash switch/build-dawn.sh           # once
bash switch/build-sdl.sh            # once

RL_NX_DEKO=1 bash switch/build.sh 8     # -> build-switch/RL-NX.nro (deko3d, the console build)
```

`switch/README.md` covers the toolchain and a black-screen checklist, `AGENTS.md` is the working brief
(build, test and measurement recipes), and `README.PORT.md` is the full port log.

## How it works

`src/recompiler` finds the game's functions in `main.dol` and writes them out as C++. `src/runtime` is the
GameCube's hardware at register level (CPU state and MMU, threads on host fibers, video, disc, audio DSP,
ARAM, memory card, controllers). GX commands go to Aurora, which `switch/deko` renders with deko3d and
`switch/uam` compiles shaders for the Switch's GPU. The Switch layer (`switch/src`) holds the SD card
layout, logging, audio output, the menu and the updater.

## Credits

- **Red Two** by gminustwo, the static recompilation this port is built on.
- **Aurora** by encounter, the GX renderer.
- **General_Han_Solo**, the HD texture pack (see above).
- **davy** on SteamGridDB, the Rogue Leader icon art (https://www.steamgriddb.com/grid/544663).
- Switch homebrew groundwork from KartPadNX, Melee-NX and SwitchWaker (notices in `switch/licenses/`).
- devkitPro, libnx, deko3d, uam and the Switch homebrew community.

The port was made by digdat0. The previous README for upstream Red Two on PC is kept in
[`docs/README.RedTwoPC.md`](docs/README.RedTwoPC.md).

## Legal

This repository's own code is under the licences in `LICENSE` and `switch/licenses/`: Red Two and Aurora
are MIT, deko3d is zlib, Dawn is Apache-2.0, and the Switch layer is GPL-3.0 with MPL-2.0 file-level pieces.
It contains no code or assets from the game. Star Wars and Rogue Leader are trademarks of Lucasfilm Ltd.;
Nintendo Switch and GameCube are trademarks of Nintendo. This project is not affiliated with or endorsed
by them.
