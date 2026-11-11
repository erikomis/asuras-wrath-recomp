# Asura's Wrath — static recompilation for PC (macOS)

🌐 **English** · [Português (BR)](README.pt-BR.md)

A native port of **Asura's Wrath** (PS3, disc `BLUS30721`, v01.02) to
**macOS/arm64**, built with static recompilation: the game's PowerPC and SPU
code is translated to C/C++ and compiled into a native executable, with
graphics on Metal. It is not an emulator.

**The path to the game's true ending is included**: the port fetches and
installs the add-on content alongside the game — the *Episode Pack Part IV:
Nirvana* (episodes 19-22), which is where the story actually ends. See
[docs/DLC.md](docs/DLC.md) (in Portuguese). Whether those episodes load
and play has not been tried yet.

| | |
| --- | --- |
| Game | Asura's Wrath — Capcom / CyberConnect2, 2012 |
| Engine | Unreal Engine 3 (PS3 cook) — CC2's "BCGAME" build |
| Disc | BLUS30721 (USA, EnJaFrDeEsIt), version 01.02 |
| Target | Apple-silicon macOS, Metal graphics |

> [!WARNING]
> **Early alpha, not playable to the end.** In headless probes (no window) the
> game reaches the title screen, creates or loads a save, enters NEW GAME,
> plays the opening of Episode 1 and gets to the first QTEs with no error in
> the log (reported by the developer). **Audio does not work**, and playing
> through a window, the QTEs' response and the rest of the episode have not
> been verified. There are no releases: you build the game yourself from your
> own disc. What is missing is in [ROADMAP.md](ROADMAP.md).

> **This repository contains no game data, no DLC, no firmware, no Sony keys
> and nothing translated from them.** Everyone generates all of that locally
> from their own legitimate copy (and fetches their own add-on content; the
> URLs in `fetch_dlc.sh` are Sony's public CDN for this title ID, and the
> packages stay encrypted with the buyer's licence). A fan project, not
> affiliated with Capcom, CyberConnect2 or Sony Interactive Entertainment.

## Building

Prerequisites: the disc (ISO) in `~/Downloads` (or adjust the `game_iso`
symlink), Xcode CLT, CMake + Ninja, and the shared Python venv.

```sh
tools/rebuild.sh            # stage → decrypt → analyse → lift → spu → dlc → build
tools/rebuild.sh dlc        # just the DLC (fetch + extract + link into the VFS)
tools/run.sh                # run the game (mounts the ISO read-only)
```

## Status

| | |
| --- | --- |
| Disc inventory, EBOOT decrypted, PPU and SPU lifted, builds with Metal | ✅ |
| Boot, first-run install, legal screens and loading videos | ✅ |
| Title screen, save, NEW GAME | ✅ in headless probes |
| Opening of Episode 1, pause menu, first QTEs | 🧪 reached in a 470 s headless probe; the rest of the episode not checked |
| Playing in a window | 🧪 not verified after the latest fixes |
| Audio | ❌ does not work yet |
| Add-on episodes (Part IV: Nirvana) | 🧪 fetched and linked into the VFS; never loaded |

## Layout

| Path | What it is |
| --- | --- |
| [`port/`](port/) | The game-specific code: boot, stubs, SPURS queues, CMake |
| [`patches/`](patches/) | The ps3recomp toolkit patches ([notes](patches/README.md), in Portuguese) |
| [`tools/`](tools/) | Rebuild, decrypt, DLC, run and probe scripts |
| [`docs/`](docs/) | DLC notes and the game manifest (Portuguese) |

Built on [ps3recomp](https://github.com/sp00nznet/ps3recomp), with the layout
and the tools of the author's other ports ([Resistance: Fall of Man](https://github.com/erikomis/resistance-recomp),
[inFAMOUS 2](https://github.com/erikomis/infamous2-recomp), among others). See
[CREDITS.md](CREDITS.md). The technical diary is in Portuguese
([ROADMAP.pt-BR.md](ROADMAP.pt-BR.md)); the roadmap has an English summary.

## License

The port's own code is MIT ([LICENSE](LICENSE)); ps3recomp keeps its own
licence. Asura's Wrath belongs to Capcom and CyberConnect2; no part of the
game is included.
