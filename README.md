# Asura's Wrath — static recompilation for PC (macOS)

🌐 **English** · [Português (BR)](README.pt-BR.md)

A native port of **Asura's Wrath** (PS3, disc `BLUS30721`, v01.02) to
**macOS/arm64**, built with static recompilation: the game's PowerPC and SPU
code is translated to C/C++ and compiled into a native executable, with
graphics on Metal. It is not an emulator.

**The path to the game's true ending is included**: the port fetches and
installs the add-on content alongside the game — the *Episode Pack Part IV:
Nirvana* (episodes 19-22), which is where the story actually ends. See
[docs/DLC.md](docs/DLC.md) (in Portuguese).

| | |
| --- | --- |
| Game | Asura's Wrath — Capcom / CyberConnect2, 2012 |
| Engine | Unreal Engine 3 (PS3 cook) — CC2's "BCGAME" build |
| Disc | BLUS30721 (USA, EnJaFrDeEsIt), version 01.02 |
| Target | Apple-silicon macOS, Metal graphics |

> [!WARNING]
> **Early work in progress.** It does not reach gameplay yet. There are no
> releases: you build the game yourself from your own disc. What is missing
> is in [ROADMAP.pt-BR.md](ROADMAP.pt-BR.md) (Portuguese).

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

Built on [ps3recomp](https://github.com/sp00nznet/ps3recomp), with the layout
and the tools of the author's other ports (Afro Samurai, inFAMOUS 1/2,
Resistance 1/2, Demons' Souls). The documentation is written in Portuguese
first; English translations of the README and roadmap will follow.
