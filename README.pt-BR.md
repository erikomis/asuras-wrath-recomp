# Asura's Wrath — recompilação estática para PC (macOS)

🇧🇷 **Português (BR)** · [English](README.md) *(a traduzir)*

Port nativo do **Asura's Wrath** (PS3, disco `BLUS30721`, v01.02) para
**macOS/arm64**, feito com recompilação estática: o código PowerPC e SPU do
jogo é traduzido para C/C++ e compilado como um executável nativo, com
gráficos em Metal. Não é um emulador.

**Inclui o caminho para o final verdadeiro do jogo** (a DLC *Episode Pack
Part IV: Nirvana*, episódios 19-22): o port já baixa e instala as DLCs junto
com o jogo — veja [docs/DLC.md](docs/DLC.md).

| | |
| --- | --- |
| Jogo | Asura's Wrath — Capcom / CyberConnect2, 2012 |
| Motor | Unreal Engine 3 (cook PS3) — não o UE3 comum: o "BCGAME" da CC2 |
| Disco | BLUS30721 (USA, EnJaFrDeEsIt), versão 01.02 |
| Plataforma alvo | macOS com Apple silicon, gráficos em Metal |

> [!WARNING]
> **Trabalho em andamento, no começo.** Ainda não chega à gameplay. Não há
> releases: você mesmo compila o jogo a partir do seu próprio disco. O que
> falta está no [ROADMAP.pt-BR.md](ROADMAP.pt-BR.md).

> **Este repositório não contém dados do jogo, firmware, chaves da Sony nem
> nada traduzido a partir deles.** Cada um gera tudo isso localmente a partir
> da própria cópia legítima (e das DLCs que comprou; as URLs do
> `fetch_dlc.sh` são as da CDN pública da Sony para quem tem o jogo, o
> conteúdo continua cifrado com as chaves de quem o instalou). Projeto de fã,
> sem afiliação com a Capcom, a CyberConnect2 ou a Sony Interactive
> Entertainment.

## Como se constrói

Pré-requisitos: o disco (ISO) em `~/Downloads` (ou ajuste o symlink
`game_iso`), Xcode CLT, CMake + Ninja, e o venv de Python compartilhado.

```sh
tools/rebuild.sh            # stage → decrypt → analyse → lift → spu → dlc → build
tools/rebuild.sh dlc        # só as DLCs (fetch + extract + link no VFS)
tools/run.sh                # roda o jogo (monta a ISO só de leitura)
```

Etapas: a ISO é montada apenas para leitura; o EBOOT do disco (SELF key_rev
0x0016) é descriptografado com `tools/fetch_keys.py` + `tools/decrypt_self.py`
(as chaves vêm do key_vault.cpp público do RPCS3, gravadas fora do
repositório, só para você); o toolkit [ps3recomp](https://github.com/sp00nznet/ps3recomp)
analisa e traduz o PPU e o SPU; o CMake/Ninja compila tudo como um
executável nativo com Metal.

## Organização

Mesma organização dos outros ports do autor (Afro Samurai, inFAMOUS 1/2,
Resistance 1/2, Demons' Souls):

```
game/          EBOOT.BIN descriptografado (EBOOT.elf), PARAM.SFO
game_iso/      symlink para a pasta da ISO original (nunca modificada)
dlc/pkg/       os .pkg das DLCs (baixados, não comitados)
dlc/extracted/ as DLCs desempacotadas, por content id
analysis/      a análise do EBOOT (functions.json, imports.json, spu/)
recompiled/    o código C/C++ gerado pelo lifter
port/          main.cpp, stubs.cpp, CMakeLists — o runner nativo
rpcs3/         as árvores /dev_hdd0 e /dev_hdd1 que o jogo vê
tools/         os scripts (rebuild, build, run, probe, dlc, chaves)
docs/          DLC.md e outros
```

## Estado

- [x] Disco inventariado: BLUS30721 v01.02, UE3 da CyberConnect2, Bink,
      middleware UE3 padrão, 6,5 GB de `COOKEDPS3`.
- [x] EBOOT.BIN (SELF key_rev 0x0016) descriptografado sem o RPCS3.
- [x] **EBOOT do patch 1.02 conferido: byte-idêntico ao do disco** (o disco já
      é a versão final do executável — as DLCs exigem 1.02, e já temos).
- [x] As DLCs baixadas da CDN da Sony e desempacotadas: **Part IV: Nirvana
      (final verdadeiro)**, Lost Episodes 1 e 2 (crossover Street Fighter),
      patch 1.02. Linkadas no VFS no layout que o instalador da Sony usa.
- [ ] PPU analisado e traduzido (em andamento).
- [ ] SPU extraído e traduzido.
- [ ] Compila e roda no macOS/arm64 com Metal.
- [ ] Boot → menus → gameplay.

Veja o [ROADMAP.pt-BR.md](ROADMAP.pt-BR.md).
