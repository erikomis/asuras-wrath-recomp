# Credits

🌐 **English** · [Português (BR)](CREDITS.pt-BR.md)

This port only exists because of the work below. Thank you all.

## Toolkit

- **[ps3recomp](https://github.com/sp00nznet/ps3recomp)** — Ned Heller
  ([@sp00nznet](https://github.com/sp00nznet)) and
  [contributors](https://github.com/sp00nznet/ps3recomp/blob/master/CONTRIBUTORS.md).
  The runtime, the HLE libraries, the PPU and SPU lifters, the Metal backend,
  and the project template this port's `main.cpp`, `stubs.cpp` and
  `CMakeLists.txt` start from. MIT. The local fixes are in
  [`patches/`](patches/).

## Keys

- **[RPCS3](https://github.com/RPCS3/rpcs3)** — the SELF decryption keys are
  read at build time from its public `key_vault.cpp`, only to decrypt your own
  disc; nothing is stored in this repository.

## Other ports of the author

- [Resistance: Fall of Man](https://github.com/erikomis/resistance-recomp) and
  [inFAMOUS 2](https://github.com/erikomis/infamous2-recomp): the layout, the
  tools and `tools/decrypt_self.py` come from them.

## Game

Asura's Wrath © Capcom, developed by CyberConnect2, published in 2012. This
fan project is not affiliated with Capcom, CyberConnect2 or Sony Interactive
Entertainment, and includes no part of the game.
