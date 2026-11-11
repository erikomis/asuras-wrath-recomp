# Roadmap

🌐 **English** · [Português (BR)](ROADMAP.pt-BR.md)

Status of the native Asura's Wrath (`BLUS30721`, v01.02) port to macOS/arm64
and what comes next. Same layout as the roadmaps of the author's other ports.
`[x]` = done and verified; `[ ]` = to do. The full technical diary (causes,
addresses, probes) is kept in Portuguese in [ROADMAP.pt-BR.md](ROADMAP.pt-BR.md);
this page is a summary.

## Status at a glance (2026-11-11)

- **Episode 1 gameplay reached in a headless probe:** a 470 s probe with no
  window and scripted input goes through the title, creates or loads the save,
  enters NEW GAME, plays the opening of Episode 1, opens the pause menu and
  reaches the first QTEs with no error in the log (reported by the developer).
  **Not verified:** audio (it does not work), the QTEs' response, the rest of
  the episode, and running in a window after these fixes.
- **Five causes were fixed on the way:** the title froze because the runtime
  injected a "job done" event per SPURS job into a queue the game uses for its
  block allocator (213 MB gone, then the UE3 out-of-memory handler held the
  allocator lock); an intermittent freeze (3 boots in 10) from the append-only
  "ShaderPatching" job chain being re-run by every `cellSpursRunJobChain`; a
  SIGBUS loading Episode 1 from a 373-case `switch` the lifter cut at 256
  (fixed in the lifter); a stall after the map and HUD from another title's
  CRI task special cases hitting image id 22, which is PhysX here
  (`SPU_CRI_IMAGE=0`); and unpredictable probes because a connected physical
  pad moved the menus (`PAD_NO_CONTROLLER=1`).
- **The disc is already the final executable:** the official 1.02 patch's
  EBOOT is byte-identical to the disc's (SHA-256 checked), so there is no
  second executable to recompile.
- **The add-on content is fetched and linked** into the VFS (Part IV: Nirvana,
  episodes 19-22, the true ending; the Lost Episodes; the patch). The game has
  never loaded it yet.
- **Toolkit:** ps3recomp in a local worktree, with the patches in
  [`patches/`](patches/) (apply in the order listed in its README).

## Done

- [x] Disc inventoried: UE3 (CyberConnect2's `BCGAME`), Bink, six languages,
      6.5 GB of `COOKEDPS3`.
- [x] `EBOOT.BIN` decrypted without RPCS3 (`tools/decrypt_self.py`,
      `tools/fetch_keys.py`; keys come from RPCS3's public source and are kept
      outside the repository).
- [x] PPU analysed and lifted; 27 SPU images extracted and lifted, plus one
      extra Edge Geometry job and the Bink job.
- [x] Builds and starts on macOS/arm64 with Metal.
- [x] Boot, first-run install, loading videos and logos; the title menu renders.
- [x] Title, save and NEW GAME; opening of Episode 1 and first QTEs in a
      headless probe.

## Next steps

### Gameplay
- [ ] Audio (the MultiStream task runs a codec loaded at run time by the
      interpreter and rejects size-0 transfers).
- [ ] Run in a window; check the "screen cutting" seen in the menu there.
- [ ] QTEs responsive; Episode 1 to the end, then on to Episode 18.
- [ ] The CC2 look (cel shading, outlines, saturated colours) checked scene by
      scene.

### The true ending (Part IV: Nirvana)
- [ ] `PS3-BCGAME.INI.EDAT` decrypted at run time (the toolkit does it; to
      confirm in practice).
- [ ] Episodes 19-22 load and play, up to *A Life Well Lived*.

### Stability and performance
- [ ] A steady 30 fps (the PS3 target), measured.
- [ ] Long sessions without hangs or leaks; a clean quit.
- [ ] `cellSpursQueueDetachLv2EventQueue` now implemented in the toolkit patch:
      re-check the unresolved imports on Episode 1 loading.
- [ ] `GCM_USER_CMD_QUEUE=1` was left on without being the cause of anything.

## After it is playable

- [ ] Higher internal resolution and anti-aliasing that keeps the cel shading.
- [ ] 60 fps as an experimental mode (careful with QTEs and timers).
- [ ] Controls: dead zones and remapping.
- [ ] A desktop launcher like the Resistance ports'.
- [ ] Windows and Linux.

## Out of scope for now

- Online features and trophies.
- PSN avatar and cosmetic packs (only the episodes matter).

## Giving back

- [ ] `augment_functions.py` with a per-port TOC in the upstream lifter.
- [ ] Whatever else the title asks of the runtime, each fix generic and tested.
