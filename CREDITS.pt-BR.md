# Créditos

🇧🇷 **Português (BR)** · [English](CREDITS.md)

Este port só existe por causa do trabalho abaixo. Obrigado a todos.

## Toolkit

- **[ps3recomp](https://github.com/sp00nznet/ps3recomp)** — Ned Heller
  ([@sp00nznet](https://github.com/sp00nznet)) e
  [colaboradores](https://github.com/sp00nznet/ps3recomp/blob/master/CONTRIBUTORS.md).
  O runtime, as bibliotecas HLE, os lifters de PPU e SPU, o backend Metal e o
  template de projeto de que `main.cpp`, `stubs.cpp` e `CMakeLists.txt` deste
  port partem. MIT. As correções locais estão em [`patches/`](patches/).

## Chaves

- **[RPCS3](https://github.com/RPCS3/rpcs3)** — as chaves de decifragem do
  SELF são lidas na hora da compilação do `key_vault.cpp` público dele, só
  para decifrar o seu próprio disco; nada fica guardado neste repositório.

## Outros ports do autor

- [Resistance: Fall of Man](https://github.com/erikomis/resistance-recomp) e
  [inFAMOUS 2](https://github.com/erikomis/infamous2-recomp): a organização,
  as ferramentas e o `tools/decrypt_self.py` vêm deles.

## Jogo

Asura's Wrath © Capcom, desenvolvido pela CyberConnect2, lançado em 2012.
Projeto de fã, sem afiliação com a Capcom, a CyberConnect2 ou a Sony
Interactive Entertainment, e não inclui nenhuma parte do jogo.
